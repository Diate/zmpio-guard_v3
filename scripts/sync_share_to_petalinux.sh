#!/usr/bin/env bash
# Copy the staged Windows artifacts into the PetaLinux project's share folder.
# The staged Windows share is mounted at /media/sf_share by default.
# SOURCE_SHARE_DIR can override that source when required.

set -euo pipefail

SOURCE_SHARE_DIR="${SOURCE_SHARE_DIR:-/media/sf_share}"
PETA_SHARE_DIR="${PETA_SHARE_DIR:-/petalinux/Pentalinux/share}"

files=(
  "app_freertos.elf"
  "cpu1_bitstream.bit"
  "cpu1_wrapper.xsa"
  "system-user-openamp-template.dtsi"
  "zmpio-enable-cpu1-clocks"
  "zmpio-clock-init.service"
  # The Linux client stack and its sysvinit supervision. zmpiod is the single
  # reader of the ABI v2 TX ring.
  "zmpiod"
  "zmpioctl"
  "zmpiod.init"
  "zmpiod-supervise"
  "zmpio-uio.rules"
  "script1_fast_package.sh"
  "sync_share_to_petalinux.sh"
)

# Step 6 (docs/PLAN_BUOC_6.md): a Windows checkout with core.autocrlf=true
# stages every text file here with CRLF, which breaks bash's own
# shebang/`set -euo pipefail` parsing on the scripts in this list outright
# ("$'\r': command not found") and is silently wrong for anything a
# Linux-side tool (dtc, systemd, udev) parses byte-for-byte. .gitattributes
# now pins eol=lf for these paths so a fresh checkout should not hit this,
# but strip \r here too as a second line of defense independent of the
# Windows side's git config -- this runs on the Linux VM, not affected by
# whatever produced the staged copy.
binary_files=("app_freertos.elf" "cpu1_bitstream.bit" "cpu1_wrapper.xsa" "zmpiod" "zmpioctl")

is_binary() {
  local name="$1"
  local candidate
  for candidate in "${binary_files[@]}"; do
    [[ "${candidate}" == "${name}" ]] && return 0
  done
  return 1
}

usage() {
  cat <<'EOF'
Usage: sync_share_to_petalinux.sh [--dry-run]

Environment variables:
  SOURCE_SHARE_DIR  Staging share from Windows (default: /media/sf_share)
  PETA_SHARE_DIR    PetaLinux share folder (default: /petalinux/Pentalinux/share)
EOF
}

dry_run=false
case "${1:-}" in
  "") ;;
  --dry-run) dry_run=true ;;
  --help|-h) usage; exit 0 ;;
  *) echo "ERROR: Unknown option: $1" >&2; usage >&2; exit 2 ;;
esac

for name in "${files[@]}"; do
  if [[ ! -f "${SOURCE_SHARE_DIR}/${name}" ]]; then
    echo "ERROR: Missing staged file: ${SOURCE_SHARE_DIR}/${name}" >&2
    exit 1
  fi
done

if [[ -e "${PETA_SHARE_DIR}" && ! -d "${PETA_SHARE_DIR}" ]]; then
  echo "ERROR: PETA_SHARE_DIR is not a directory: ${PETA_SHARE_DIR}" >&2
  exit 1
fi

if [[ "${dry_run}" == true ]]; then
  printf '[DRY RUN] %s -> %s\n' "${SOURCE_SHARE_DIR}" "${PETA_SHARE_DIR}"
  printf '[DRY RUN] Would replace: %s\n' "${files[*]}"
  exit 0
fi

mkdir -p -- "${PETA_SHARE_DIR}"

for name in "${files[@]}"; do
  destination="${PETA_SHARE_DIR}/${name}"
  if [[ -d "${destination}" ]]; then
    echo "ERROR: Refusing to replace directory: ${destination}" >&2
    exit 1
  fi

  # Remove only the managed files; unrelated PetaLinux share files stay intact.
  rm -f -- "${destination}"
  case "${name}" in
    zmpiod|zmpioctl|zmpiod.init|zmpiod-supervise|zmpio-enable-cpu1-clocks|script1_fast_package.sh|sync_share_to_petalinux.sh)
      install -m 0755 -- "${SOURCE_SHARE_DIR}/${name}" "${destination}"
      ;;
    *)
      install -m 0644 -- "${SOURCE_SHARE_DIR}/${name}" "${destination}"
      ;;
  esac

  if ! is_binary "${name}"; then
    sed -i 's/\r$//' -- "${destination}"
  fi

  printf '[INFO] Copied %s\n' "${name}"
done

sync

echo "[SUCCESS] PetaLinux share is updated: ${PETA_SHARE_DIR}"
