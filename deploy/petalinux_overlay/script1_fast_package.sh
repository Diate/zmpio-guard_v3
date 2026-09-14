#!/usr/bin/env bash
set -euo pipefail

# ============================================================================
# PetaLinux build/package script with CPU1 IPC support (wakes CPU1 before
# handing off to Linux).
#
# Key design points:
#   1. boot.cmd loads image.ub and issues "bootm" at the correct address
#      (kernel_addr_r=0x2000000, taken from `strings images/linux/u-boot.elf`).
#      image.ub is a FIT image that already bundles the FDT
#      (fdt-system-top.dtb), so a single bootm is sufficient; the DTB does
#      not need to be loaded separately.
#   2. custom.wks partitions /boot using --source bootimg-partition so its
#      contents come from build/wic_boot_files/.
#   3. Setuid bits for su/sudo/passwd are fixed at build time via a
#      petalinux-image-minimal bbappend (ROOTFS_POSTPROCESS_COMMAND), inside
#      do_rootfs's pseudo-context, so nothing needs to be patched on the
#      board afterward. WIC packaging uses PetaLinux's default rootfs state
#      (no separate fakeroot state or manual --rootfs-dir).
# ============================================================================

# ----------------------------- Environment configuration -----------------------------
PETALINUX_SETTINGS="/petalinux/settings.sh"
BASE_DIR="/petalinux/Pentalinux"
PROJECT_NAME="zynq_openamp"
PROJECT_DIR="${BASE_DIR}/${PROJECT_NAME}"
SHARE_DIR="${BASE_DIR}/share"
HW_DIR="${SHARE_DIR}"
DTS_TEMPLATE="${SHARE_DIR}/system-user-openamp-template.dtsi"
CLOCK_INIT_SCRIPT="${SHARE_DIR}/zmpio-enable-cpu1-clocks"
CLOCK_INIT_SERVICE="${SHARE_DIR}/zmpio-clock-init.service"

# ---------------------------------------------------------------------------
# The Linux client stack.
#
# zmpiod is the SINGLE reader of the ABI v2 TX ring (REQ-STR-002) and the
# only thing that can receive CPU1's feature stream.  This rootfs runs
# classic sysvinit, so zmpiod is supervised by an /etc/inittab respawn
# entry rather than a systemd unit.  See
# deploy/petalinux_overlay/zmpiod-supervise.
#
# zmpiod is the ONLY process permitted to read the ABI v2 TX ring: the ring is
# single-producer/single-consumer, so a second reader would silently steal
# feature records (REQ-STR-002).
# ---------------------------------------------------------------------------
ZMPIOD_BIN="${SHARE_DIR}/zmpiod"
ZMPIOCTL_BIN="${SHARE_DIR}/zmpioctl"
ZMPIOD_INIT="${SHARE_DIR}/zmpiod.init"
ZMPIOD_SUPERVISE="${SHARE_DIR}/zmpiod-supervise"
ZMPIO_UDEV_RULES="${SHARE_DIR}/zmpio-uio.rules"
ZMPIO_CAPTURE_DIR="/var/log/zmpio"
LOG_DIR="${BASE_DIR}/logs"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_FILE="${LOG_DIR}/openamp_build_${TIMESTAMP}.log"
FAST_STORAGE_BASE="${PROJECT_DIR}/.plnx-cache"
MIN_FREE_GB=12

# Rootfs and bootargs configuration
ROOTFS_DEVNODE="/dev/mmcblk0p2"
# uio_pdrv_genirq.of_id=generic-uio: uio_pdrv_genirq does not auto-bind to the
# "generic-uio" compatible string used by zmpio_doorbell_0/zmpio_shm in
# system-user-openamp-template.dtsi; without this, zmpiod's zmpio_open() spins
# forever on status=-1 and the ABI v2 TX ring fills up once nothing is
# draining it (CPU1 then reports "feature stream drop ... reason=-3" once
# the ring hits capacity).
ROOTFS_BOOTARGS="console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait maxcpus=1 clk_ignore_unused uio_pdrv_genirq.of_id=generic-uio"
DTS_TARGET="${PROJECT_DIR}/project-spec/meta-user/recipes-bsp/device-tree/files/system-user.dtsi"

# Key input files
OCM_READER_SRC="${SHARE_DIR}/ocm_ipc_reader.c"       # Reader source, if present
CPU1_ELF="${SHARE_DIR}/app_freertos.elf"             # CPU1 ELF image
CPU1_BIN="app_freertos.bin"                          # Binary produced from the ELF

# RAM addresses used by U-Boot (confirmed via `strings images/linux/u-boot.elf`)
UBOOT_KERNEL_ADDR="0x2000000"    # kernel_addr_r (image.ub is a FIT image, FDT included)
CPU1_LOAD_ADDR="0x18000000"      # CPU1's reserved-memory region (OCM/DDR)
CPU1_SCRATCH_ADDR="0x10000000"   # Scratch region OUTSIDE reserved-memory: the binary is
                                  # loaded here first, then copied to CPU1_LOAD_ADDR with
                                  # cp.b (U-Boot's `load` command refuses to write directly
                                  # into reserved-memory, but `cp.b` is not blocked)

# Path of the .img produced from the wic/.direct output (set by convert_wic_to_img)
WIC_IMG_PATH=""

# --- Memory layout: CPU1 region kept separate from the shared-memory IPC region ---
# CPU1 (FreeRTOS code/stack/heap): 16 MB at 0x18000000
CPU1_RESERVED_SIZE="0x01000000"
# Shared-memory IPC (bidirectional ring buffer): 4 MB, immediately after the CPU1 region
SHARED_MEM_BASE="0x19000000"
SHARED_MEM_SIZE="0x00400000"

# Control flags
FROM_SCRATCH=0
DEEP_CLEAN=0
POST_BUILD_SOFT_CLEAN=1
REGEN_BOOT_ONLY=0
DTB_ONLY=0

# Fast-package modes: reuse already-built artifacts and never invoke petalinux-build.
FAST_PACKAGE_CPU1_IPC=0
FAST_PACKAGE_IPC_ONLY=0
ROOTFS_TAR_SOURCE=""
CPU1_PACKAGE_MODE="update"

# ============================================================================
# Usage/help
# ============================================================================
usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --from-scratch        Clean project runtime state before build (keep downloads/sstate)
  --deep-clean          Full clean before build (also remove downloads/sstate)
  --no-post-clean       Keep tmp/host cache after successful build
  --regen-boot-only     Skip petalinux-config/build; only regenerate boot.scr
                         and repackage BOOT.BIN/WIC/img. Use this when only
                         boot.cmd changed or app_freertos.elf was replaced,
                         with no change to kernel/rootfs/hardware. Requires
                         at least one prior successful build.
  --dtb-only            Reapply the device tree from the template, rebuild
                         device-tree + image.ub, then repackage
                         BOOT.BIN/WIC/img. Use this when ONLY
                         deploy/petalinux_overlay/system-user-openamp-template.dtsi
                         changed. Skips the full petalinux-config sequence
                         (--get-hw-description, --silentconfig, -c rootfs),
                         so it is faster than a full build, but it still runs
                         petalinux-build -- unlike --regen-boot-only, because
                         the DTB lives inside the FIT image.ub and
                         --regen-boot-only does NOT rebuild image.ub.
  --fast-package-cpu1-ipc
                         Skip petalinux-build. Rebuild app_freertos.bin from
                         ${CPU1_ELF}, refresh the zmpio stack in rootfs.tar.gz,
                         then repackage the WIC image.
  --fast-package-ipc-only
                         Skip petalinux-build. Only refresh the zmpio stack in
                         the rootfs and repackage the WIC image; keep the CPU1
                         binary already cached at images/linux/${CPU1_BIN}.
  -h, --help            Show this help

Default behavior:
  - Keeps downloads + sstate for faster soft rebuilds
  - Removes tmp/host cache after successful build to save disk
  - A normal build installs lrzsz (rz/sz) into the root filesystem.
    Fast-package and regen-only modes reuse the existing rootfs and therefore
    require one normal build after this script update.
EOF
}

# ============================================================================
# Command-line argument parsing
# ============================================================================
parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --from-scratch)   FROM_SCRATCH=1; shift ;;
      --deep-clean)     FROM_SCRATCH=1; DEEP_CLEAN=1; shift ;;
      --no-post-clean)  POST_BUILD_SOFT_CLEAN=0; shift ;;
      --regen-boot-only) REGEN_BOOT_ONLY=1; shift ;;
      --dtb-only)       DTB_ONLY=1; shift ;;
      --fast-package-cpu1-ipc) FAST_PACKAGE_CPU1_IPC=1; shift ;;
      --fast-package-ipc-only) FAST_PACKAGE_IPC_ONLY=1; shift ;;
      -h|--help)        usage; exit 0 ;;
      *) echo "ERROR: Unknown option: $1" >&2; usage; exit 1 ;;
    esac
  done
}

# ============================================================================
# Utility functions
# ============================================================================
check_free_space() {
  local path="$1" min_gb="${2:-$MIN_FREE_GB}"
  local avail_kb req_kb
  avail_kb=$(df -Pk "$path" | awk 'NR==2 {print $4}')
  req_kb=$((min_gb * 1024 * 1024))
  if [[ "${avail_kb}" -lt "${req_kb}" ]]; then
    echo "ERROR: Low disk space at ${path}. Need at least ${min_gb} GB free." >&2
    return 1
  fi
}

cleanup_stale_server() {
  pkill -f 'bitbake|bitbake-server|cooker|Parser-' >/dev/null 2>&1 || true
  rm -f build/bitbake.sock build/bitbake.lock build/bitbake-cookerdaemon.log
}

clean_project_runtime() {
  echo "[WARN] Cleaning project runtime state" | tee -a "${LOG_FILE}"
  cleanup_stale_server
  rm -rf build/cache build/tmp build/wic build/wic-tmp build/wic_boot_files
  rm -f build/bitbake.sock build/bitbake.lock build/bitbake-cookerdaemon.log
  rm -rf "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/host-tmp"
  mkdir -p "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/host-tmp"
}

clean_project_deep() {
  echo "[WARN] Performing deep clean (including downloads & sstate)" | tee -a "${LOG_FILE}"
  clean_project_runtime
  rm -rf "${FAST_STORAGE_BASE}/downloads" "${FAST_STORAGE_BASE}/sstate-cache"
  mkdir -p "${FAST_STORAGE_BASE}/downloads" "${FAST_STORAGE_BASE}/sstate-cache"
}

post_build_clean() {
  echo "[INFO] Soft cleanup after build (keep downloads/sstate)" | tee -a "${LOG_FILE}"
  rm -rf "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/host-tmp"
  rm -rf build/cache build/wic build/wic-tmp build/wic_boot_files
  mkdir -p "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/host-tmp"
}

resolve_sf_share_dir() {
  [[ -n "${SF_SHARE_DIR:-}" ]] && { echo "$SF_SHARE_DIR"; return 0; }
  local candidates=(
    "/media/sf_share"
  )
  for d in "${candidates[@]}"; do
    [[ -d "$d" ]] && { echo "$d"; return 0; }
  done
  return 1
}

copy_artifacts_to_sf_share() {
  local sf_dir artifact
  if ! sf_dir=$(resolve_sf_share_dir); then
    echo "[ERROR] sf_share not found. Mount /media/sf_share or set SF_SHARE_DIR." \
      | tee -a "${LOG_FILE}"
    return 1
  fi

  if [[ -z "${WIC_IMG_PATH:-}" || ! -f "${WIC_IMG_PATH}" ]]; then
    echo "[ERROR] Final .img was not created; refusing to finish without a share copy." \
      | tee -a "${LOG_FILE}"
    return 1
  fi

  mkdir -p "$sf_dir"
  for artifact in "${WIC_IMG_PATH}" images/linux/*.wic images/linux/*.direct; do
    [[ -f "${artifact}" ]] || continue
    cp -f "${artifact}" "${sf_dir}/$(basename "${artifact}")"
    echo "[INFO] Copied ${artifact} -> ${sf_dir}" | tee -a "${LOG_FILE}"
  done
}

# Renames the .direct/.wic output of petalinux-package --wic to .img. These
# are already raw disk images, so this is a plain copy/rename rather than a
# format conversion -- the result can be dd'd straight to an SD card.
convert_wic_to_img() {
  local latest_wic
  latest_wic=$(ls -t build/wic-tmp/*.direct build/wic-tmp/*.wic \
                     images/linux/*.direct images/linux/*.wic 2>/dev/null | head -n1 || true)

  if [[ -z "${latest_wic}" ]]; then
    echo "[WARN] No .direct/.wic file found to convert to .img" | tee -a "${LOG_FILE}"
    return 0
  fi

  local img_out="images/linux/${PROJECT_NAME}-sdimage.img"
  cp -f "${latest_wic}" "${img_out}"
  echo "[INFO] Converted ${latest_wic} -> ${img_out}" | tee -a "${LOG_FILE}"

  WIC_IMG_PATH="${img_out}"
}

# ============================================================================
# CPU1 / IPC support functions
# ============================================================================

# Creates a kernel config fragment enabling CONFIG_DEVMEM.
prepare_kernel_fragment() {
  local fragment_dir="${PROJECT_DIR}/project-spec/meta-user/recipes-kernel/linux/linux-xlnx"
  mkdir -p "$fragment_dir"
  cat > "${fragment_dir}/devmem.cfg" <<EOF
CONFIG_DEVMEM=y
EOF
  echo "[INFO] Kernel fragment devmem.cfg created" | tee -a "${LOG_FILE}"

  # ZMPIO_UIO_NODES (system-user-openamp-template.dtsi) needs uio_pdrv_genirq
  # built in/as a module AND bound via the of_id= kernel command line arg
  # below -- CONFIG_UIO_PDRV_GENIRQ alone does not auto-bind to "generic-uio".
  # Without both the bootarg and this fragment, zmpiod sticks in
  # "zmpio_open failed (status=-1), retrying" for the whole capture. Setting
  # =y here is a safe no-op if it was already on.
  cat > "${fragment_dir}/uio.cfg" <<EOF
CONFIG_UIO=y
CONFIG_UIO_PDRV_GENIRQ=y
EOF
  echo "[INFO] Kernel fragment uio.cfg created" | tee -a "${LOG_FILE}"
}

# Creates a petalinux-image-minimal bbappend that fixes the setuid bits for
# su/sudo/passwd inside do_rootfs's own pseudo-context, avoiding an LD_PRELOAD
# conflict with wic that a separate fakeroot state would otherwise cause.
prepare_rootfs_bbappend() {
  local bbappend_dir="${PROJECT_DIR}/project-spec/meta-user/recipes-images/images"
  local bbappend_file="${bbappend_dir}/petalinux-image-minimal.bbappend"

  mkdir -p "${bbappend_dir}"

  local postprocess="fix_setuid_bits; install_zmpio_clock_init; install_zmpiod; "

  cat > "${bbappend_file}" <<EOF
ROOTFS_POSTPROCESS_COMMAND += "${postprocess}"

fix_setuid_bits () {
    if [ -f \${IMAGE_ROOTFS}/bin/su ]; then
        chown root:root \${IMAGE_ROOTFS}/bin/su
        chmod u+s \${IMAGE_ROOTFS}/bin/su
    fi
    if [ -f \${IMAGE_ROOTFS}/usr/bin/sudo ]; then
        chown root:root \${IMAGE_ROOTFS}/usr/bin/sudo
        chmod u+s \${IMAGE_ROOTFS}/usr/bin/sudo
    fi
    if [ -f \${IMAGE_ROOTFS}/usr/bin/passwd ]; then
        chown root:root \${IMAGE_ROOTFS}/usr/bin/passwd
        chmod u+s \${IMAGE_ROOTFS}/usr/bin/passwd
    fi
}

install_zmpio_clock_init () {
    install -d \${IMAGE_ROOTFS}/usr/sbin
    install -m 0755 "${CLOCK_INIT_SCRIPT}" \
        \${IMAGE_ROOTFS}/usr/sbin/zmpio-enable-cpu1-clocks

    install -d \${IMAGE_ROOTFS}/lib/systemd/system
    install -m 0644 "${CLOCK_INIT_SERVICE}" \
        \${IMAGE_ROOTFS}/lib/systemd/system/zmpio-clock-init.service
    install -d \${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants
    ln -snf /lib/systemd/system/zmpio-clock-init.service \
        \${IMAGE_ROOTFS}/etc/systemd/system/multi-user.target.wants/zmpio-clock-init.service

    install -d \${IMAGE_ROOTFS}/etc/init.d \${IMAGE_ROOTFS}/etc/rcS.d
    ln -snf /usr/sbin/zmpio-enable-cpu1-clocks \
        \${IMAGE_ROOTFS}/etc/init.d/zmpio-clock-init
    ln -snf ../init.d/zmpio-clock-init \
        \${IMAGE_ROOTFS}/etc/rcS.d/S20zmpio-clock-init
}

install_zmpiod () {
    if [ ! -f "${ZMPIOD_BIN}" ]; then
        bbwarn "zmpiod not found at ${ZMPIOD_BIN}; image will have no feature-stream consumer"
        return 0
    fi

    install -d \${IMAGE_ROOTFS}/usr/bin \${IMAGE_ROOTFS}/usr/sbin
    install -m 0755 "${ZMPIOD_BIN}" \${IMAGE_ROOTFS}/usr/bin/zmpiod
    if [ -f "${ZMPIOCTL_BIN}" ]; then
        install -m 0755 "${ZMPIOCTL_BIN}" \${IMAGE_ROOTFS}/usr/bin/zmpioctl
    fi
    install -m 0755 "${ZMPIOD_SUPERVISE}" \${IMAGE_ROOTFS}/usr/sbin/zmpiod-supervise

    install -d \${IMAGE_ROOTFS}/etc/init.d
    install -m 0755 "${ZMPIOD_INIT}" \${IMAGE_ROOTFS}/etc/init.d/zmpiod

    # The group the socket and the UIO nodes are gated on (zmpio_socket_proto.h,
    # zmpio-uio.rules).  Created here rather than assumed: without it the
    # policy gate silently degrades to "whoever can reach the socket".
    if ! grep -q '^zmpio:' \${IMAGE_ROOTFS}/etc/group 2>/dev/null; then
        echo 'zmpio:x:1500:' >> \${IMAGE_ROOTFS}/etc/group
    fi

    install -d \${IMAGE_ROOTFS}/etc/udev/rules.d
    if [ -f "${ZMPIO_UDEV_RULES}" ]; then
        install -m 0644 "${ZMPIO_UDEV_RULES}" \
            \${IMAGE_ROOTFS}/etc/udev/rules.d/99-zmpio-uio.rules
    fi

    # ZMPIO_CAPTURE_DIR (zmpio_capture.c) is intentionally NOT created here.
    # /var/log on this rootfs is a symlink to /var/volatile/log, populated
    # fresh from tmpfs by populate-volatile at boot -- at rootfs-assembly time
    # the symlink target does not exist yet, so "install -d .../var/log/zmpio"
    # hits mkdir() EEXIST on the dangling symlink itself and fails do_rootfs.
    # zmpiod's own zmpio_capture_open() already does
    # "mkdir(capture->dir, 0755)" with EEXIST tolerance the first time it
    # opens a capture file, which is also just the correct place for this:
    # /var/volatile/log is recreated empty on every boot regardless of what
    # the build-time rootfs image contained.

    # sysvinit respawn entry -- THIS is what restarts the daemon within a
    # second of a crash or kill -9 (recovery-time gate, <= 2 s).  An
    # rcS.d symlink starts it once and never looks again, so there is
    # deliberately no S9xzmpiod link here.  Ordered after zmpio-clock-init
    # only in the sense that the supervisor retries until the UIO nodes
    # exist (zmpiod.c's zmpio_open() backoff).
    if ! grep -q 'zmpiod-supervise' \${IMAGE_ROOTFS}/etc/inittab 2>/dev/null; then
        echo 'zmpd:2345:respawn:/usr/sbin/zmpiod-supervise' \
            >> \${IMAGE_ROOTFS}/etc/inittab
    fi
}
EOF

  echo "[INFO] Created ${bbappend_file}" | tee -a "${LOG_FILE}"
}

# Builds ocm_ipc_reader and adds its recipe to the rootfs.
build_ocm_reader() {
  if [[ ! -f "${OCM_READER_SRC}" ]]; then
    echo "[WARN] ${OCM_READER_SRC} not found, skipping ocm_ipc_reader build" | tee -a "${LOG_FILE}"
    return 0
  fi

  local cross_prefix="arm-linux-gnueabihf-"
  if ! command -v "${cross_prefix}gcc" &>/dev/null; then
    cross_prefix="arm-none-eabi-"
  fi

  local bin_name="ocm_ipc_reader"
  ${cross_prefix}gcc -O2 -static -o "${bin_name}" "${OCM_READER_SRC}" 2>&1 | tee -a "${LOG_FILE}"

  local recipe_dir="${PROJECT_DIR}/project-spec/meta-user/recipes-apps/ocm-ipc/files"
  mkdir -p "${recipe_dir}"
  cp "${bin_name}" "${recipe_dir}/"

  cat > "${PROJECT_DIR}/project-spec/meta-user/recipes-apps/ocm-ipc/ocm-ipc.bb" <<EOF
SUMMARY = "OCM IPC reader"
LICENSE = "CLOSED"
SRC_URI = "file://${bin_name}"
S = "\${WORKDIR}"
do_install() {
    install -d \${D}/usr/bin
    install -m 0755 ${bin_name} \${D}/usr/bin/
}
FILES_\${PN} += "/usr/bin/${bin_name}"
EOF
  echo "[INFO] ocm_ipc_reader built and recipe added" | tee -a "${LOG_FILE}"
}

# Creates boot.scr (U-Boot script) that wakes CPU1 before booting Linux.
create_boot_script() {
  local cmd_file="${PROJECT_DIR}/boot.cmd"
  local scr_file="${PROJECT_DIR}/images/linux/boot.scr"

  cat > "$cmd_file" <<EOF
# Boot script for Zynq - wake CPU1 first, then boot Linux
#
# Handoff sequence per XAPP1078: copy -> flush -> publish address -> flush.
# The two "dcache flush" calls are NOT optional. U-Boot runs with the D-cache
# and MMU enabled, while CPU1 leaves the BootROM with caches and MMU OFF and
# reads DDR/OCM directly. Without the flushes, both the FreeRTOS image at
# ${CPU1_LOAD_ADDR} and the handoff word at 0xFFFFFFF0 can still be sitting in
# CPU0's L1/L2 when CPU1 goes looking for them -- CPU1 then either spins
# forever on a stale 0 or branches into a partially written image. That is a
# non-deterministic failure by construction, so absence of a crash on any one
# boot proves nothing.
#
# There is deliberately no explicit SEV here. The previous "smc #0" was not
# one: U-Boot has no such command, it printed "Unknown command 'smc' - try
# 'help'" on every boot and the script simply carried on to the next line (see
# docs/architecture/evidence/step6_uart1_clockgate_20260909_1842_uart0.log).
# CPU1 still starts because its BootROM loop is "wfe; read 0xFFFFFFF0; branch
# if non-zero", and ARMv7 Linux issues SEV from every arch_spin_unlock() --
# so the first event arrives the moment the kernel starts scheduling. That is
# what the ~1 s delay between "Starting kernel ..." and CPU1's own banner in
# the captured log actually is. Publishing the address before bootm (as here)
# is therefore the ordering the wake-up depends on; do not move the mw.l after
# bootm.
echo "Loading CPU1 binary from SD (into scratch region, bypassing the reserved-memory check)..."
load mmc 0:1 ${CPU1_SCRATCH_ADDR} app_freertos.bin
cp.b ${CPU1_SCRATCH_ADDR} ${CPU1_LOAD_ADDR} \${filesize}
dcache flush
mw.l 0xFFFFFFF0 ${CPU1_LOAD_ADDR}
dcache flush

echo "CPU1 handoff published at 0xFFFFFFF0. Loading Linux image..."
load mmc 0:1 ${UBOOT_KERNEL_ADDR} image.ub
bootm ${UBOOT_KERNEL_ADDR}
EOF

  if command -v mkimage &>/dev/null; then
    mkimage -C none -A arm -T script -d "$cmd_file" "$scr_file" 2>&1 | tee -a "${LOG_FILE}"
  elif [[ -f "${PETALINUX}/tools/linux-i386/mkimage" ]]; then
    "${PETALINUX}/tools/linux-i386/mkimage" -C none -A arm -T script -d "$cmd_file" "$scr_file"
  else
    echo "ERROR: mkimage not found. Please install u-boot-tools." >&2
    exit 1
  fi
  echo "[INFO] boot.scr generated at ${scr_file}" | tee -a "${LOG_FILE}"
}

# Converts app_freertos.elf -> .bin and copies it into boot_files.
#
# Overlays the zmpio client stack onto the base rootfs: zmpiod, zmpioctl, their
# sysvinit supervision, the UIO udev rule, and the capture directory. zmpiod is
# the single reader of the ABI v2 TX ring (REQ-STR-002).
#
prepare_rootfs_with_zmpio_stack() {
  local base_rootfs="images/linux/rootfs.tar.gz"
  local work_dir="build/fast_package/rootfs_zmpio_stack"
  local raw_tar="${work_dir}/rootfs.base.tar"
  local append_tar="${work_dir}/zmpio_stack.tar"
  local stage_dir="${work_dir}/stage"
  local output_tar="${work_dir}/rootfs-with-zmpio-stack.tar.gz"
  local -a members=()

  [[ -f "${base_rootfs}" ]] || {
    echo "ERROR: ${base_rootfs} not found. Run a full PetaLinux build once first." >&2
    return 1
  }
  [[ -f "${CLOCK_INIT_SCRIPT}" ]] || {
    echo "ERROR: ${CLOCK_INIT_SCRIPT} not found." >&2
    return 1
  }
  [[ -f "${CLOCK_INIT_SERVICE}" ]] || {
    echo "ERROR: ${CLOCK_INIT_SERVICE} not found." >&2
    return 1
  }
  [[ -f "${ZMPIOD_BIN}" ]] || {
    echo "ERROR: ${ZMPIOD_BIN} not found. Cross-build software/linux and copy" >&2
    echo "       zmpiod/zmpioctl into ${SHARE_DIR} first -- without the daemon the" >&2
    echo "       image has no consumer for CPU1's feature stream." >&2
    return 1
  }
  [[ -f "${ZMPIOD_INIT}" ]] || {
    echo "ERROR: ${ZMPIOD_INIT} not found." >&2
    return 1
  }
  [[ -f "${ZMPIOD_SUPERVISE}" ]] || {
    echo "ERROR: ${ZMPIOD_SUPERVISE} not found." >&2
    return 1
  }

  rm -rf "${work_dir}"
  mkdir -p \
    "${stage_dir}/usr/bin" \
    "${stage_dir}/usr/sbin" \
    "${stage_dir}/lib/systemd/system" \
    "${stage_dir}/etc/systemd/system/multi-user.target.wants" \
    "${stage_dir}/etc/init.d" \
    "${stage_dir}/etc/rcS.d" \
    "${stage_dir}/etc/udev/rules.d" \
    "${stage_dir}${ZMPIO_CAPTURE_DIR}"

  # Preserve every existing rootfs archive entry and append replacements for
  # just the paths below. A later tar entry wins when WIC extracts the
  # tarball, which is what makes this an overlay rather than a rebuild.
  gzip -dc "${base_rootfs}" > "${raw_tar}"

  install -m 0755 "${CLOCK_INIT_SCRIPT}" "${stage_dir}/usr/sbin/zmpio-enable-cpu1-clocks"
  install -m 0644 "${CLOCK_INIT_SERVICE}" "${stage_dir}/lib/systemd/system/zmpio-clock-init.service"
  ln -snf /lib/systemd/system/zmpio-clock-init.service \
    "${stage_dir}/etc/systemd/system/multi-user.target.wants/zmpio-clock-init.service"
  ln -snf /usr/sbin/zmpio-enable-cpu1-clocks \
    "${stage_dir}/etc/init.d/zmpio-clock-init"
  ln -snf ../init.d/zmpio-clock-init \
    "${stage_dir}/etc/rcS.d/S20zmpio-clock-init"
  members+=(
    ./usr/sbin/zmpio-enable-cpu1-clocks
    ./lib/systemd/system/zmpio-clock-init.service
    ./etc/systemd/system/multi-user.target.wants/zmpio-clock-init.service
    ./etc/init.d/zmpio-clock-init
    ./etc/rcS.d/S20zmpio-clock-init
  )

  install -m 0755 "${ZMPIOD_BIN}" "${stage_dir}/usr/bin/zmpiod"
  install -m 0755 "${ZMPIOD_SUPERVISE}" "${stage_dir}/usr/sbin/zmpiod-supervise"
  install -m 0755 "${ZMPIOD_INIT}" "${stage_dir}/etc/init.d/zmpiod"
  members+=(
    ./usr/bin/zmpiod
    ./usr/sbin/zmpiod-supervise
    ./etc/init.d/zmpiod
    ".${ZMPIO_CAPTURE_DIR}"
  )
  if [[ -f "${ZMPIOCTL_BIN}" ]]; then
    install -m 0755 "${ZMPIOCTL_BIN}" "${stage_dir}/usr/bin/zmpioctl"
    members+=(./usr/bin/zmpioctl)
  fi
  if [[ -f "${ZMPIO_UDEV_RULES}" ]]; then
    install -m 0644 "${ZMPIO_UDEV_RULES}" \
      "${stage_dir}/etc/udev/rules.d/99-zmpio-uio.rules"
    members+=(./etc/udev/rules.d/99-zmpio-uio.rules)
  fi

  tar --create --file="${append_tar}" --numeric-owner --owner=0 --group=0 \
    -C "${stage_dir}" "${members[@]}"
  tar --concatenate --file="${raw_tar}" "${append_tar}"
  gzip -n -c "${raw_tar}" > "${output_tar}"

  ROOTFS_TAR_SOURCE="${output_tar}"
  echo "[INFO] Prepared fast rootfs overlay: ${ROOTFS_TAR_SOURCE}" | tee -a "${LOG_FILE}"
  echo "[INFO] NOTE: the fast overlay cannot append to /etc/inittab (it can only" \
       "replace whole files); after the first boot with this image, add" \
       "'zmpd:2345:respawn:/usr/sbin/zmpiod-supervise' to /etc/inittab and run" \
       "'init q', or use a full petalinux-build image where install_zmpiod()" \
       "does it at rootfs time." | tee -a "${LOG_FILE}"
}

# Builds a fresh app_freertos.bin and keeps a persistent copy under
# images/linux/. That persistent copy is what lets --fast-package-ipc-only
# leave the CPU1 binary untouched.
prepare_cpu1_binary() {
  if [[ ! -f "${CPU1_ELF}" ]]; then
    echo "ERROR: ${CPU1_ELF} not found; cannot create ${CPU1_BIN}" >&2
    return 1
  fi

  local objcopy_cmd=""
  if command -v arm-none-eabi-objcopy &>/dev/null; then
    objcopy_cmd="arm-none-eabi-objcopy"
  elif command -v arm-linux-gnueabihf-objcopy &>/dev/null; then
    objcopy_cmd="arm-linux-gnueabihf-objcopy"
  else
    echo "ERROR: objcopy not found" >&2
    return 1
  fi

  local cached_bin="images/linux/${CPU1_BIN}"
  mkdir -p build/wic_boot_files images/linux
  "${objcopy_cmd}" -O binary "${CPU1_ELF}" "${cached_bin}" 2>&1 | tee -a "${LOG_FILE}"
  cp -f "${cached_bin}" "build/wic_boot_files/${CPU1_BIN}"
  echo "[INFO] Converted ${CPU1_ELF} -> ${cached_bin}" | tee -a "${LOG_FILE}"
}

# Copies back the already-cached CPU1 binary only; does not read the ELF or
# update CPU1.
prepare_cached_cpu1_binary() {
  local cached_bin="images/linux/${CPU1_BIN}"
  [[ -f "${cached_bin}" ]] || {
    echo "ERROR: ${cached_bin} not found. Run --fast-package-cpu1-ipc once first," >&2
    echo "       or place the exact current CPU1 binary at that path." >&2
    return 1
  }

  mkdir -p build/wic_boot_files
  cp -f "${cached_bin}" "build/wic_boot_files/${CPU1_BIN}"
  echo "[INFO] Preserved cached CPU1 binary ${cached_bin}" | tee -a "${LOG_FILE}"
}

# Ensures the device tree carries the correct configuration
# (reserved-memory, CPU1 disabled, bootargs).
ensure_device_tree() {
  local dtsi="${DTS_TARGET}"
  mkdir -p "$(dirname "$dtsi")"
  if [[ ! -f "$dtsi" ]]; then
    cp "${DTS_TEMPLATE}" "$dtsi"
    echo "[INFO] Copied device tree template to ${dtsi}" | tee -a "${LOG_FILE}"
  elif ! cmp -s "${DTS_TEMPLATE}" "$dtsi"; then
    # The template under deploy/petalinux_overlay/ is this repo's source of
    # truth for this file (see CLAUDE.md). Refresh from the template whenever
    # it differs from the project's system-user.dtsi -- the append-with-marker
    # blocks below only cover the handful of fragments they name, not
    # arbitrary template changes, so leaving a stale system-user.dtsi in place
    # would silently drop any other template edit (e.g. &uart1 ownership)
    # from every subsequent build.
    local backup="${dtsi}.bak.$(date +%Y%m%d_%H%M%S)"
    cp "$dtsi" "$backup"
    cp "${DTS_TEMPLATE}" "$dtsi"
    echo "[INFO] Device tree template differs from ${dtsi}; refreshed from template" | tee -a "${LOG_FILE}"
    echo "[INFO] Previous device tree saved as ${backup}" | tee -a "${LOG_FILE}"
  else
    echo "[INFO] ${dtsi} already matches the template" | tee -a "${LOG_FILE}"
  fi

  if ! grep -q "reserved-memory" "$dtsi"; then
    echo "[INFO] Adding reserved-memory (cpu1_reserved + ipc_shared) to ${dtsi}" | tee -a "${LOG_FILE}"
    cat >> "$dtsi" <<EOF

/ {
    reserved-memory {
        #address-cells = <1>;
        #size-cells = <1>;
        ranges;

        cpu1_reserved: cpu1@18000000 {
            no-map;
            reg = <${CPU1_LOAD_ADDR} ${CPU1_RESERVED_SIZE}>;
        };

        ipc_shared: shared@19000000 {
            no-map;
            reg = <${SHARED_MEM_BASE} ${SHARED_MEM_SIZE}>;
        };
    };
};

&cpus {
    cpu@1 {
        status = "disabled";
    };
};
EOF
  fi

  # An existing system-user.dtsi may predate CPU1 peripheral ownership.
  # Append a marked final override so Linux never binds SPI0 or AXI IIC0,
  # both of which are accessed directly by CPU1 FreeRTOS.
  if ! grep -q "ZMPIO_CPU1_PERIPHERAL_OWNERSHIP" "$dtsi"; then
    cat >> "$dtsi" <<'EOF'

/* ZMPIO_CPU1_PERIPHERAL_OWNERSHIP: final CPU1 ownership override. */
&spi0 {
    status = "disabled";
};

&axi_iic_0 {
    status = "disabled";
};
EOF
    echo "[INFO] Disabled Linux ownership of CPU1 SPI0 and AXI IIC0" | tee -a "${LOG_FILE}"
  fi

  if grep -q 'bootargs[[:space:]]*=' "$dtsi"; then
    sed -i 's|bootargs[[:space:]]*=.*;|bootargs = "'"${ROOTFS_BOOTARGS}"'";|' "$dtsi"
  else
    cat >> "$dtsi" <<EOF
/ {
    chosen {
        bootargs = "${ROOTFS_BOOTARGS}";
    };
};
EOF
  fi
  echo "[INFO] Device tree ${dtsi} is ready" | tee -a "${LOG_FILE}"
}

# Ensures the rootfs config selects ext4 and the correct SD devnode.
ensure_rootfs_ext4_config() {
  local cfg="project-spec/configs/config"
  sed -i '/^CONFIG_SUBSYSTEM_ROOTFS_EXT4=/d' "$cfg"
  sed -i '/^CONFIG_SUBSYSTEM_SDROOT_DEVNODE=/d' "$cfg"
  sed -i '/^#oe_silentonly CONFIG_SUBSYSTEM_ROOTFS_INITRAMFS is not set$/d' "$cfg"
  {
    echo 'CONFIG_SUBSYSTEM_ROOTFS_EXT4=y'
    echo '#oe_silentonly CONFIG_SUBSYSTEM_ROOTFS_INITRAMFS is not set'
    echo "CONFIG_SUBSYSTEM_SDROOT_DEVNODE=\"${ROOTFS_DEVNODE}\""
  } >> "$cfg"
  echo "[INFO] Rootfs config updated (ext4, devnode ${ROOTFS_DEVNODE})" | tee -a "${LOG_FILE}"
}

# Registers and selects lrzsz for the rootfs. user-rootfsconfig makes the
# package appear in the PetaLinux menu; rootfs_config is the actual selection
# BitBake uses when building the image. Both files are kept in sync so no
# manual selection via `petalinux-config -c rootfs` is required.
ensure_lrzsz_rootfs_config() {
  local user_rootfs_cfg="project-spec/meta-user/conf/user-rootfsconfig"
  local rootfs_cfg="project-spec/configs/rootfs_config"

  mkdir -p "$(dirname "${user_rootfs_cfg}")" "$(dirname "${rootfs_cfg}")"
  touch "${user_rootfs_cfg}" "${rootfs_cfg}"

  # user-rootfsconfig contains bare CONFIG symbols, not Kconfig assignments.
  sed -i '/^[[:space:]]*CONFIG_lrzsz[[:space:]]*$/d' "${user_rootfs_cfg}"
  printf '\nCONFIG_lrzsz\n' >> "${user_rootfs_cfg}"

  sed -i '/^CONFIG_lrzsz=/d; /^# CONFIG_lrzsz is not set$/d' "${rootfs_cfg}"
  printf '\nCONFIG_lrzsz=y\n' >> "${rootfs_cfg}"

  echo "[INFO] Enabled lrzsz in rootfs (rz/sz will be included in the image)" \
    | tee -a "${LOG_FILE}"
}

# Prepare a local temporary workspace for petalinux-package --wic.
# This is intentionally independent of petalinux-build so fast and regen modes
# can package existing artifacts after the normal build cache was cleaned.
prepare_wic_tmpdir() {
  local project_config="project-spec/configs/config"
  local bitbake_tmp="${FAST_STORAGE_BASE}/tmp"
  local host_tmp="${FAST_STORAGE_BASE}/host-tmp"
  local conf

  [[ -f "${project_config}" ]] || {
    echo "[ERROR] ${project_config} not found; cannot configure WIC TMPDIR." \
      | tee -a "${LOG_FILE}"
    return 1
  }

  mkdir -p "${bitbake_tmp}" "${host_tmp}"

  if grep -q '^CONFIG_TMP_DIR_LOCATION=' "${project_config}"; then
    sed -i 's|^CONFIG_TMP_DIR_LOCATION=.*|CONFIG_TMP_DIR_LOCATION="'"${bitbake_tmp}"'"|' \
      "${project_config}"
  else
    printf '\nCONFIG_TMP_DIR_LOCATION="%s"\n' "${bitbake_tmp}" >> "${project_config}"
  fi

  for conf in build/conf/local.conf project-spec/meta-user/conf/petalinuxbsp.conf; do
    [[ -f "${conf}" ]] || continue
    if grep -q '^TMPDIR = ' "${conf}"; then
      sed -i 's|^TMPDIR = ".*"|TMPDIR = "'"${bitbake_tmp}"'"|' "${conf}"
    else
      printf '\nTMPDIR = "%s"\n' "${bitbake_tmp}" >> "${conf}"
    fi
  done

  export TMPDIR="${host_tmp}"
  echo "[INFO] WIC TMPDIR configured locally: ${bitbake_tmp}" | tee -a "${LOG_FILE}"
}

# Packages BOOT files + WIC + converts to .img. Kept as a separate function
# so --regen-boot-only and the full build flow share the same logic.
package_boot_and_wic() {
  prepare_wic_tmpdir
  echo "[INFO] Packaging BOOT.BIN" | tee -a "${LOG_FILE}"
  local bitstream=""
  if [[ -f "images/linux/system.bit" ]]; then
    bitstream="images/linux/system.bit"
  elif [[ -f "${SHARE_DIR}/cpu1_bitstream.bit" ]]; then
    bitstream="${SHARE_DIR}/cpu1_bitstream.bit"
  else
    echo "WARN: No FPGA bitstream found. Packaging without bitstream." | tee -a "${LOG_FILE}"
  fi
  if [[ -n "$bitstream" ]]; then
    petalinux-package --boot --fsbl images/linux/zynq_fsbl.elf --fpga "$bitstream" --u-boot --force 2>&1 | tee -a "${LOG_FILE}"
  else
    petalinux-package --boot --fsbl images/linux/zynq_fsbl.elf --u-boot --force 2>&1 | tee -a "${LOG_FILE}"
  fi

  create_boot_script

  rm -rf build/wic_boot_files && mkdir -p build/wic_boot_files
  cp images/linux/BOOT.BIN images/linux/image.ub images/linux/boot.scr images/linux/system.dtb build/wic_boot_files/ 2>/dev/null || true
  if [[ "${CPU1_PACKAGE_MODE}" == "preserve" ]]; then
    prepare_cached_cpu1_binary
  else
    prepare_cpu1_binary
  fi

  # petalinux-package --wic reads rootfs.tar.gz from --images-dir.
  # Fast modes supply an archive with only the zmpio stack overlaid.
  local rootfs_tar="${ROOTFS_TAR_SOURCE:-images/linux/rootfs.tar.gz}"
  if [[ -f "${rootfs_tar}" ]]; then
    cp -f "${rootfs_tar}" "build/wic_boot_files/rootfs.tar.gz"
    echo "[INFO] Copied ${rootfs_tar} into build/wic_boot_files" | tee -a "${LOG_FILE}"
  else
    echo "[ERROR] Rootfs archive not found: ${rootfs_tar}" | tee -a "${LOG_FILE}"
    exit 1
  fi

  for f in BOOT.BIN image.ub boot.scr "${CPU1_BIN}" rootfs.tar.gz; do
    if [[ ! -f "build/wic_boot_files/${f}" ]]; then
      echo "[ERROR] Missing file '${f}' in build/wic_boot_files -- the resulting WIC image will fail to boot!" | tee -a "${LOG_FILE}"
    fi
  done

  local boot_files_abs="${PROJECT_DIR}/build/wic_boot_files"

  cat > build/custom.wks <<EOF
part /boot --source bootimg-partition --fstype=vfat --label boot --active --align 2048 --fixed-size 128M
part /     --source rootfs --fstype=ext4 --label root --align 2048 --fixed-size 512M
EOF

  echo "[INFO] Creating wic image using custom.wks..." | tee -a "${LOG_FILE}"
  petalinux-package --wic \
    --images-dir "${boot_files_abs}" \
    --bootfiles "BOOT.BIN image.ub boot.scr ${CPU1_BIN}" \
    --outdir images/linux \
    -w build/custom.wks \
    2>&1 | tee -a "${LOG_FILE}"

  convert_wic_to_img
  verify_setuid_on_image
}

# Verifies setuid/owner bits directly on the freshly built .img, without
# requiring a board. When su/passwd is a symlink, the real target is followed
# and checked instead, because the kernel always reports a symlink's own mode
# as 0777 regardless of whether its target has the setuid bit set.
verify_setuid_on_image() {
  if ! command -v debugfs &>/dev/null; then
    echo "[INFO] debugfs is not available; skipping setuid verification on the image." | tee -a "${LOG_FILE}"
    return 0
  fi
  local img="${WIC_IMG_PATH:-}"
  if [[ -z "$img" || ! -f "$img" ]]; then
    echo "[WARN] No .img found to verify setuid on." | tee -a "${LOG_FILE}"
    return 0
  fi

  local part_line start_sector sector_size=512 offset_bytes
  local extracted="/tmp/rootfs_verify_$$.ext4"

  part_line=$(fdisk -lu "$img" 2>/dev/null | awk '/Linux$/ {print; exit}')
  if [[ -z "$part_line" ]]; then
    echo "[WARN] Could not identify the ext4 partition on ${img}; skipping setuid verification." | tee -a "${LOG_FILE}"
    return 0
  fi

  start_sector=$(echo "$part_line" | awk '{print $2}')
  offset_bytes=$(( start_sector * sector_size ))

  echo "[INFO] Extracting rootfs partition (offset ${offset_bytes} bytes, sector ${start_sector}) to check setuid..." | tee -a "${LOG_FILE}"
  if ! dd if="$img" of="$extracted" bs=512 skip="$start_sector" 2>/dev/null; then
    echo "[WARN] Extracting the rootfs partition with dd failed; skipping setuid verification." | tee -a "${LOG_FILE}"
    return 0
  fi

  echo "[INFO] Verifying setuid/owner on ${img} (rootfs partition, offset ${offset_bytes}):" | tee -a "${LOG_FILE}"

  # Stats one path; if it is a symlink, follows the real target (up to 3
  # hops, to bound a symlink loop) and stats that target instead.
  local seen_paths=""
  stat_follow_symlink() {
    local path="$1" depth="${2:-0}"
    if [[ "$depth" -ge 3 ]]; then
      echo "    [WARN] Too many nested symlinks; stopping at: ${path}" | tee -a "${LOG_FILE}"
      return 0
    fi

    local result
    result=$(debugfs -R "stat ${path}" "${extracted}" 2>/dev/null)

    if [[ -z "$result" ]]; then
      echo "  ${path}: could not be read (it may not exist)" | tee -a "${LOG_FILE}"
      return 0
    fi

    if echo "$result" | grep -q "Type: symlink"; then
      local target
      target=$(echo "$result" | grep -oP '(?<=Fast link dest: ")[^"]*' || true)
      if [[ -z "$target" ]]; then
        echo "  ${path}: is a symlink but its target could not be read (possibly a slow symlink)" | tee -a "${LOG_FILE}"
        return 0
      fi
      # Normalize a relative target to an absolute path.
      if [[ "$target" != /* ]]; then
        local dir
        dir=$(dirname "$path")
        target="${dir}/${target}"
      fi
      echo "  ${path} -> symlinks to: ${target}" | tee -a "${LOG_FILE}"
      stat_follow_symlink "$target" $((depth + 1))
    else
      echo "  ${path} (actual target):" | tee -a "${LOG_FILE}"
      echo "$result" | grep -E 'User:|Mode:' | sed 's/^/    /' | tee -a "${LOG_FILE}"
    fi
  }

  for f in /usr/bin/sudo /bin/su /usr/bin/passwd; do
    stat_follow_symlink "$f" 0
  done

  rm -f "${extracted}"
}

# ============================================================================
# Main entry point
# ============================================================================
main() {
  parse_args "$@"

  mkdir -p "${LOG_DIR}"

  [[ -f "${PETALINUX_SETTINGS}" ]] || { echo "ERROR: ${PETALINUX_SETTINGS} not found" >&2; exit 1; }
  [[ -d "${HW_DIR}" ]] || { echo "ERROR: Hardware directory ${HW_DIR} missing" >&2; exit 1; }
  [[ -f "${DTS_TEMPLATE}" ]] || { echo "ERROR: ${DTS_TEMPLATE} missing" >&2; exit 1; }
  [[ -f "${CLOCK_INIT_SCRIPT}" ]] || { echo "ERROR: ${CLOCK_INIT_SCRIPT} missing" >&2; exit 1; }
  [[ -f "${CLOCK_INIT_SERVICE}" ]] || { echo "ERROR: ${CLOCK_INIT_SERVICE} missing" >&2; exit 1; }

  if [[ -z "${PETALINUX:-}" ]]; then
    echo "[INFO] Sourcing PetaLinux settings..." | tee -a "${LOG_FILE}"
    source "${PETALINUX_SETTINGS}" 2>&1 | tee -a "${LOG_FILE}"
  fi

  if [[ ! -d "${PROJECT_DIR}" ]]; then
    if [[ "${FAST_PACKAGE_CPU1_IPC}" -eq 1 || "${FAST_PACKAGE_IPC_ONLY}" -eq 1 ]]; then
      echo "ERROR: ${PROJECT_DIR} does not exist; fast packaging requires an existing build." >&2
      exit 1
    fi
    echo "[INFO] Creating project ${PROJECT_NAME}" | tee -a "${LOG_FILE}"
    cd "${BASE_DIR}"
    petalinux-create -t project -n "${PROJECT_NAME}" --template zynq 2>&1 | tee -a "${LOG_FILE}"
  fi
  cd "${PROJECT_DIR}"

  # -------- REGEN-BOOT-ONLY MODE: skip config/build, only repackage --------
  if [[ "${FAST_PACKAGE_CPU1_IPC}" -eq 1 && "${FAST_PACKAGE_IPC_ONLY}" -eq 1 ]]; then
    echo "ERROR: Select only one fast-package option." >&2
    exit 1
  fi

  if [[ "${FAST_PACKAGE_CPU1_IPC}" -eq 1 || "${FAST_PACKAGE_IPC_ONLY}" -eq 1 ]]; then
    [[ -f "images/linux/image.ub" ]] || {
      echo "ERROR: images/linux/image.ub not found. Run a full PetaLinux build once first." >&2
      exit 1
    }

    if [[ "${FAST_PACKAGE_CPU1_IPC}" -eq 1 ]]; then
      echo "[INFO] Fast package: update CPU1 binary + zmpio stack; skip petalinux-build." | tee -a "${LOG_FILE}"
      CPU1_PACKAGE_MODE="update"
    else
      echo "[INFO] Fast package: update zmpio stack only; preserve cached CPU1 binary; skip petalinux-build." | tee -a "${LOG_FILE}"
      CPU1_PACKAGE_MODE="preserve"
    fi

    prepare_rootfs_with_zmpio_stack
    mkdir -p build/wic
    package_boot_and_wic
    copy_artifacts_to_sf_share

    echo "[INFO] Fast WIC/img packaging completed. Log: ${LOG_FILE}" | tee -a "${LOG_FILE}"
    exit 0
  fi

  if [[ "${DTB_ONLY}" -eq 1 ]]; then
    if [[ "${REGEN_BOOT_ONLY}" -eq 1 ]]; then
      echo "ERROR: --dtb-only and --regen-boot-only are mutually exclusive." >&2
      exit 1
    fi
    echo "[INFO] --dtb-only: applying device tree from template, skipping petalinux-config" | tee -a "${LOG_FILE}"
    [[ -f "images/linux/image.ub" ]] || { echo "ERROR: images/linux/image.ub does not exist. A full build must be run at least once first." >&2; exit 1; }

    ensure_device_tree

    export TMPDIR="${FAST_STORAGE_BASE}/host-tmp"
    mkdir -p "$TMPDIR"

    # -c device-tree alone only refreshes images/linux/system.dtb. The DTB this
    # board actually boots is the "fdt-system-top.dtb" subimage INSIDE the FIT
    # images/linux/image.ub, so the image itself has to be rebuilt afterwards
    # or the new device tree never reaches the target.
    echo "[INFO] petalinux-build -c device-tree" | tee -a "${LOG_FILE}"
    petalinux-build -c device-tree 2>&1 | tee -a "${LOG_FILE}"
    echo "[INFO] petalinux-build (regenerating image.ub with the new DTB)" | tee -a "${LOG_FILE}"
    petalinux-build 2>&1 | tee -a "${LOG_FILE}"

    mkdir -p build/wic
    package_boot_and_wic
    copy_artifacts_to_sf_share
    echo "[INFO] --dtb-only complete. Log: ${LOG_FILE}" | tee -a "${LOG_FILE}"
    exit 0
  fi

  if [[ "${REGEN_BOOT_ONLY}" -eq 1 ]]; then
    echo "[INFO] --regen-boot-only: skipping petalinux-config/build" | tee -a "${LOG_FILE}"
    [[ -f "images/linux/image.ub" ]] || { echo "ERROR: images/linux/image.ub does not exist. A full build must be run at least once first." >&2; exit 1; }
    mkdir -p build/wic
    package_boot_and_wic
    copy_artifacts_to_sf_share
    echo "[INFO] Regen boot + WIC complete. Log: ${LOG_FILE}" | tee -a "${LOG_FILE}"
    exit 0
  fi

  # -------- CLEAN (run before the disk-space check) --------
  if [[ "${DEEP_CLEAN}" -eq 1 ]]; then
    clean_project_deep
  elif [[ "${FROM_SCRATCH}" -eq 1 ]]; then
    clean_project_runtime
  fi
  cleanup_stale_server

  # -------- DISK SPACE CHECK (after cleaning) --------
  mkdir -p "${FAST_STORAGE_BASE}"
  check_free_space "${FAST_STORAGE_BASE}" "${MIN_FREE_GB}"

  echo "[INFO] Importing hardware from ${HW_DIR}" | tee -a "${LOG_FILE}"
  petalinux-config --get-hw-description="${HW_DIR}" --silentconfig 2>&1 | tee -a "${LOG_FILE}" || {
    echo "[ERROR] petalinux-config failed. Trying to recover..." | tee -a "${LOG_FILE}"
    clean_project_runtime
    petalinux-config --get-hw-description="${HW_DIR}" --silentconfig 2>&1 | tee -a "${LOG_FILE}"
  }

  ensure_rootfs_ext4_config

  petalinux-config --silentconfig 2>&1 | tee -a "${LOG_FILE}" || {
    clean_project_runtime
    petalinux-config --silentconfig 2>&1 | tee -a "${LOG_FILE}"
  }

  ensure_lrzsz_rootfs_config
  echo "[INFO] Applying rootfs configuration (including lrzsz)" | tee -a "${LOG_FILE}"
  petalinux-config -c rootfs --silentconfig 2>&1 | tee -a "${LOG_FILE}"
  if ! grep -qx 'CONFIG_lrzsz=y' project-spec/configs/rootfs_config; then
    echo "ERROR: lrzsz was not accepted by the PetaLinux rootfs configuration." >&2
    echo "       Verify that the lrzsz recipe is available in the configured layers." >&2
    exit 1
  fi

  mkdir -p "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/downloads" "${FAST_STORAGE_BASE}/sstate-cache"
  sed -i 's|^CONFIG_TMP_DIR_LOCATION=.*|CONFIG_TMP_DIR_LOCATION="'"${FAST_STORAGE_BASE}"'/tmp"|' project-spec/configs/config

  for conf in build/conf/local.conf project-spec/meta-user/conf/petalinuxbsp.conf; do
    [[ -f "$conf" ]] || continue
    sed -i 's|^TMPDIR = ".*"|TMPDIR = "'"${FAST_STORAGE_BASE}"'/tmp"|' "$conf"
    sed -i 's|^DL_DIR = ".*"|DL_DIR = "'"${FAST_STORAGE_BASE}"'/downloads"|' "$conf"
    if grep -q '^SSTATE_DIR = ' "$conf"; then
      sed -i 's|^SSTATE_DIR = ".*"|SSTATE_DIR = "'"${FAST_STORAGE_BASE}"'/sstate-cache"|' "$conf"
    else
      printf '\nSSTATE_DIR = "'%s'/sstate-cache"\n' "${FAST_STORAGE_BASE}" >> "$conf"
    fi
    if ! grep -q '^EXTRA_IMAGE_FEATURES.*debug-tweaks' "$conf"; then
      echo 'EXTRA_IMAGE_FEATURES += "debug-tweaks"' >> "$conf"
    fi
  done

  # ========== IPC PREPARATION STEPS ==========
  prepare_kernel_fragment
  prepare_rootfs_bbappend
  # build_ocm_reader
  ensure_device_tree

  # ========== BUILD ==========
  echo "[INFO] Starting petalinux-build" | tee -a "${LOG_FILE}"
  export TMPDIR="${FAST_STORAGE_BASE}/host-tmp"
  mkdir -p "$TMPDIR"
  if ! petalinux-build 2>&1 | tee -a "${LOG_FILE}"; then
    echo "[ERROR] petalinux-build failed. Attempting recovery..." | tee -a "${LOG_FILE}"
    clean_project_runtime
    petalinux-build 2>&1 | tee -a "${LOG_FILE}"
  fi

  # ========== PACKAGING (BOOT.BIN + WIC + img + verify) ==========
  package_boot_and_wic

  # Every successful package must be exported to the configured share.
  copy_artifacts_to_sf_share

  # Soft clean after the build
  if [[ "${POST_BUILD_SOFT_CLEAN}" -eq 1 ]]; then
    post_build_clean
  fi
  if [[ "${DEEP_CLEAN}" -eq 1 ]]; then
        echo "[INFO] Running post-build intensive cleanup..." | tee -a "${LOG_FILE}"
        cleanup_stale_server
        rm -rf build/tmp build/cache build/wic build/wic-tmp build/wic_boot_files
        rm -rf "${FAST_STORAGE_BASE}/tmp" "${FAST_STORAGE_BASE}/host-tmp"
    fi

  echo "[INFO] Build completed successfully. Log: ${LOG_FILE}" | tee -a "${LOG_FILE}"
}

# ============================================================================
# Run main
# ============================================================================
main "$@"
