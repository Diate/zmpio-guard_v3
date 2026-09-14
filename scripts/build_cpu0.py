#!/usr/bin/env python3
"""Build the ZMPIO CPU0 bare-metal firmware with Vitis Unified 2023.2.

Companion to build_cpu1.py, for the standalone_ps7_cortexa9_0 domain /
cpu0_application component. Adds the lwip213 library to the domain (needed
for network/ping support), regenerates the domain BSP, then builds the app.

Run through ``vitis -s`` (normally via build_cpu0.bat), not a system Python
interpreter.
"""

from __future__ import print_function

import argparse
import hashlib
import os
import shutil
import sys
from pathlib import Path

import vitis_metadata


PLATFORM_NAME = "platform_dual"
DOMAIN_NAME = "standalone_ps7_cortexa9_0"
APP_NAME = "cpu0_application"
DOMAIN_LIBRARIES = ("lwip213",)

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
WORKSPACE = PROJECT_ROOT / "firmware"
APP_DIR = WORKSPACE / APP_NAME
DEFAULT_OUTPUT = PROJECT_ROOT / "output" / "cpu0"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build the ZMPIO CPU0 bare-metal ELF with Vitis Unified 2023.2."
    )
    parser.add_argument(
        "--output",
        default=str(DEFAULT_OUTPUT),
        help="Directory that receives cpu0_application.elf and its SHA-256 file.",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="Recreate the app CMake build directory before building.",
    )
    parser.add_argument(
        "--skip-domain-regen",
        action="store_true",
        help="Skip domain.regenerate() -- use only when the domain library list has not changed since the last build.",
    )
    return parser.parse_args()


def fail(message):
    raise RuntimeError(message)


def status_is_failure(status):
    if status is False or status is None:
        return True
    if isinstance(status, int) and not isinstance(status, bool):
        return status != 0
    status_text = str(status).upper()
    return "FAIL" in status_text or "ERROR" in status_text


def require_success(step, status):
    if status_is_failure(status):
        fail("{0} failed (Vitis status: {1}).".format(step, status))
    print("[OK] {0}: {1}".format(step, status))


def reset_app_build_dir(reason):
    build_dir = APP_DIR / "build"
    if build_dir.exists():
        print("[INFO] Removing {0} ({1})".format(build_dir, reason))
        shutil.rmtree(str(build_dir))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_lwip_dhcp_enabled():
    """Force LWIP_DHCP/LWIP_RAW/LWIP_DNS on in every generated lwipopts.h copy.

    lwip213's default (lwip_dhcp=false) ships with DHCP compiled out, and
    this Vitis version does not expose a working Python API to override that
    MLD parameter through domain.set_lib(). lwipopts.h is a generated file
    (cmakedefine01 substituted from lwip213.mld defaults), so this patch
    must be reapplied after every domain.regenerate() -- it does not survive
    one on its own.

    There are two copies on disk and they are NOT interchangeable:
      bsp/include/lwipopts.h                       -- the "installed" copy
      bsp/libsrc/build_configs/gen_bsp/include/lwipopts.h -- the CMake
        build-tree copy, generated fresh at configure time from the MLD
        template

    The build-tree copy is the one actually used to compile raw.c/dhcp.c
    (its directory is passed first on the compiler's -I list, ahead of the
    installed copy, which additionally only reaches the command line via
    -isystem/a trailing -I and is therefore never consulted first). Worse,
    every platform.build() re-installs (overwrites) the "installed" copy
    FROM the build-tree copy, so patching only bsp/include/lwipopts.h gets
    silently reverted by the very next build. Both copies must be patched
    for the fix to stick and actually affect compilation.
    """

    lwipopts_paths = [
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "include" / "lwipopts.h",
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "libsrc" / "build_configs" / "gen_bsp" / "include" / "lwipopts.h",
    ]

    for lwipopts in lwipopts_paths:
        if not lwipopts.is_file():
            print("[INFO] lwipopts.h not found yet (no lwip213 in domain?): {0}".format(lwipopts))
            continue

        text = lwipopts.read_text(encoding="utf-8")
        patched = text
        patched = patched.replace("#define LWIP_DHCP  0", "#define LWIP_DHCP  1")
        patched = patched.replace("#define DHCP_DOES_ARP_CHECK  0", "#define DHCP_DOES_ARP_CHECK  1")
        if "#define LWIP_RAW" not in patched:
            # LWIP_RAW has no MLD parameter in this port at all (not just
            # defaulted off) -- lwip/opt.h itself defaults it to 0, so raw.c
            # compiles as an empty translation unit (raw_new/raw_recv/raw_bind/
            # raw_sendto all undefined at link time) unless something defines
            # it before lwip/opt.h is processed. net_ping.c needs the raw PCB
            # API for ICMP echo.
            patched = patched.rstrip("\n") + "\n\n#define LWIP_RAW 1\n"
        if "#define LWIP_DNS" not in patched:
            # Same story as LWIP_RAW: no MLD parameter, defaults to 0, so
            # dns.c compiles empty (dns_gethostbyname undefined at link
            # time) unless forced on here. net_ping.c resolves hostnames
            # (e.g. "google.com") via DNS before pinging; the DNS server
            # itself comes from DHCP once this is enabled (dhcp.c only
            # calls dns_setserver() when LWIP_DNS is set).
            patched = patched.rstrip("\n") + "\n\n#define LWIP_DNS 1\n"
        if patched != text:
            lwipopts.write_text(patched, encoding="utf-8")
            print("[INFO] Patched {0}: LWIP_DHCP=1, DHCP_DOES_ARP_CHECK=1, LWIP_RAW=1, LWIP_DNS=1".format(lwipopts))
        elif "#define LWIP_DHCP  1" in text and "#define LWIP_RAW" in text and "#define LWIP_DNS" in text:
            print("[INFO] LWIP_DHCP/LWIP_RAW/LWIP_DNS already enabled in {0}".format(lwipopts))
        else:
            print("[WARN] Could not find expected LWIP_DHCP line in {0} -- check manually".format(lwipopts))


_PHY_SCAN_ORIGINAL = (
    "#ifndef SGMII_FIXED_LINK\n"
    "\tdetect_phy(xemacpsp);\n"
    "\tfor (i = 31; i > 0; i--) {\n"
    "\t\tif (xemacpsp->Config.BaseAddress == XPAR_XEMACPS_0_BASEADDR) {\n"
    "\t\t\tif (phymapemac0[i] == TRUE) {\n"
    "\t\t\t\tlink_speed = phy_setup_emacps(xemacpsp, i);\n"
    "\t\t\t\tphyfoundforemac0 = TRUE;\n"
    "\t\t\t\tphyaddrforemac = i;\n"
    "\t\t\t}\n"
    "\t\t} else {\n"
    "\t\t\tif (phymapemac1[i] == TRUE) {\n"
    "\t\t\t\tlink_speed = phy_setup_emacps(xemacpsp, i);\n"
    "\t\t\t\tphyfoundforemac1 = TRUE;\n"
    "\t\t\t\tphyaddrforemac = i;\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t}\n"
    "\t/* If no PHY was detected, use broadcast PHY address of 0 */\n"
    "\tif (xemacpsp->Config.BaseAddress == XPAR_XEMACPS_0_BASEADDR) {\n"
    "\t\tif (phyfoundforemac0 == FALSE)\n"
    "\t\t\tlink_speed = phy_setup_emacps(xemacpsp, 0);\n"
    "\t} else {\n"
    "\t\tif (phyfoundforemac1 == FALSE)\n"
    "\t\t\tlink_speed = phy_setup_emacps(xemacpsp, 0);\n"
    "\t}\n"
    "#else\n"
)

_PHY_SCAN_PATCHED = (
    "#ifndef SGMII_FIXED_LINK\n"
    "\tif (xemacpsp->Config.BaseAddress == XPAR_XEMACPS_0_BASEADDR) {\n"
    "\t\t/* Z7 Lite board: the single RTL8201F PHY lives at MDIO address 0,\n"
    "\t\t * not address 1 as the schematic strap implies. The detect_phy()\n"
    "\t\t * scan below (addresses 31..1) produces a false-positive match\n"
    "\t\t * at address 1 on this board (ghost/floating MDIO read), which\n"
    "\t\t * makes phy_setup_emacps() negotiate against a nonexistent\n"
    "\t\t * device, fall back to a bogus default link speed, and skip\n"
    "\t\t * ever configuring the real PHY at address 0 -- the MAC and PHY\n"
    "\t\t * end up clocked at different speeds and the link never comes\n"
    "\t\t * up at the MAC/DMA level. Go straight to the known-good\n"
    "\t\t * address instead of trusting the scan. */\n"
    "\t\tlink_speed = phy_setup_emacps(xemacpsp, 0);\n"
    "\t\tphyfoundforemac0 = TRUE;\n"
    "\t\tphyaddrforemac = 0;\n"
    "\t} else {\n"
    "\t\tdetect_phy(xemacpsp);\n"
    "\t\tfor (i = 31; i > 0; i--) {\n"
    "\t\t\tif (phymapemac1[i] == TRUE) {\n"
    "\t\t\t\tlink_speed = phy_setup_emacps(xemacpsp, i);\n"
    "\t\t\t\tphyfoundforemac1 = TRUE;\n"
    "\t\t\t\tphyaddrforemac = i;\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t\t/* If no PHY was detected, use broadcast PHY address of 0 */\n"
    "\t\tif (phyfoundforemac1 == FALSE)\n"
    "\t\t\tlink_speed = phy_setup_emacps(xemacpsp, 0);\n"
    "\t}\n"
    "#else\n"
)


def ensure_gem0_phy_addr_fix():
    """Force GEM0's PHY setup to use MDIO address 0 directly, bypassing the
    Xilinx BSP's 31..1 auto-scan.

    xemacpsif_hw.c is a checked-in lwip213 library source (not a generated
    header), but domain.regenerate() still silently re-stages it from
    Vitis's pristine catalog on every call -- exactly like lwipopts.h -- so
    a hand-edit here does not survive a rebuild on its own. This patch must
    be reapplied after every domain.regenerate()/platform.build(), same
    placement and same reason as ensure_lwip_dhcp_enabled().

    Root cause: on this board, the BSP's detect_phy() scan (MDIO addresses
    31 down to 1, deliberately excluding 0) gets a false-positive match at
    address 1 -- a ghost/floating MDIO read, not the real chip at address 0.
    That skips the real PHY entirely: phy_setup_emacps() negotiates against
    the nonexistent device at 1, falls back to a bogus link speed, and the
    MAC ends up clocked at the wrong speed relative to the PHY's actual
    negotiated speed. Net effect: net_init() never completes -- boot hangs
    right after the BSP's own "link speed for phy address 1: ..." print,
    before DHCP or the CPU0 IPC menu ever appear on UART0.
    """

    hw_c = (
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "libsrc" / "lwip213" / "src" / "contrib" / "ports" /
        "xilinx" / "netif" / "xemacpsif_hw.c"
    )

    if not hw_c.is_file():
        print("[INFO] xemacpsif_hw.c not found yet (no lwip213 in domain?): {0}".format(hw_c))
        return

    text = hw_c.read_text(encoding="utf-8")
    if _PHY_SCAN_PATCHED in text:
        print("[INFO] GEM0 PHY-address-0 fix already applied in {0}".format(hw_c))
        return
    if _PHY_SCAN_ORIGINAL not in text:
        print("[WARN] Could not find expected PHY scan block in {0} -- check manually".format(hw_c))
        return

    patched = text.replace(_PHY_SCAN_ORIGINAL, _PHY_SCAN_PATCHED)
    hw_c.write_text(patched, encoding="utf-8")
    print("[INFO] Patched {0}: GEM0 PHY setup forced to MDIO address 0".format(hw_c))


_PHY_SPEED_ORIGINAL = (
    "\t\tif (temp_speed == IEEE_SPEED_1000)\n"
    "\t\t\treturn 1000;\n"
    "\t\telse if(temp_speed == IEEE_SPEED_100)\n"
    "\t\t\treturn 100;\n"
    "\t\telse\n"
    "\t\t\treturn 10;\n"
    "\t}\n"
    "\n"
    "\treturn XST_FAILURE;"
)

_PHY_SPEED_PATCHED = (
    "\t\tif (temp_speed == IEEE_SPEED_1000)\n"
    "\t\t\treturn 1000;\n"
    "\t\telse if(temp_speed == IEEE_SPEED_100)\n"
    "\t\t\treturn 100;\n"
    "\t\telse {\n"
    "\t\t\t/* Z7 Lite board (RTL8201F): this vendor status register's\n"
    "\t\t\t * bits[15:14] speed field, as decoded above, does not reflect\n"
    "\t\t\t * this chip's actual negotiated speed -- it can read back\n"
    "\t\t\t * bits[15:14]=00 (matching neither 1000 nor 100) even while\n"
    "\t\t\t * ANAR/ANLPAR both advertise and confirm 100BASE-TX full\n"
    "\t\t\t * duplex and BMSR shows link up with autoneg complete.\n"
    "\t\t\t * Falling through to a hardcoded 10 Mbps here mismatches the\n"
    "\t\t\t * MAC against the real 100 Mbps link and hangs net_init()\n"
    "\t\t\t * right after this PHY's speed print. Resolve the real speed\n"
    "\t\t\t * from the standard ANAR/ANLPAR priority arbitration (IEEE\n"
    "\t\t\t * 802.3 clause 28) instead of trusting this vendor field for\n"
    "\t\t\t * this chip. */\n"
    "\t\t\tu16_t anar;\n"
    "\t\t\tu16_t anlpar;\n"
    "\t\t\tu16_t common;\n"
    "\n"
    "\t\t\tXEmacPs_PhyRead(xemacpsp, phy_addr,\n"
    "\t\t\t\t\tIEEE_AUTONEGO_ADVERTISE_REG, &anar);\n"
    "\t\t\tXEmacPs_PhyRead(xemacpsp, phy_addr,\n"
    "\t\t\t\t\tIEEE_PARTNER_ABILITIES_1_REG_OFFSET, &anlpar);\n"
    "\t\t\tcommon = anar & anlpar & 0x03E0U;\n"
    "\n"
    "\t\t\tif (common & 0x0100U)      /* 100BASE-TX full duplex */\n"
    "\t\t\t\treturn 100;\n"
    "\t\t\telse if (common & 0x0200U) /* 100BASE-T4 */\n"
    "\t\t\t\treturn 100;\n"
    "\t\t\telse if (common & 0x0080U) /* 100BASE-TX half duplex */\n"
    "\t\t\t\treturn 100;\n"
    "\t\t\telse if (common & 0x0040U) /* 10BASE-T full duplex */\n"
    "\t\t\t\treturn 10;\n"
    "\t\t\telse if (common & 0x0020U) /* 10BASE-T half duplex */\n"
    "\t\t\t\treturn 10;\n"
    "\t\t\telse\n"
    "\t\t\t\treturn 10; /* no common mode found; keep prior fallback */\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\treturn XST_FAILURE;"
)


def ensure_gem0_phy_speed_fix():
    """Fix get_Realtek_phy_speed()'s wrong 10 Mbps fallback for RTL8201F.

    xemacpsif_physpeed.c is a checked-in lwip213 library source, re-staged
    from Vitis's pristine catalog by every domain.regenerate() call just
    like xemacpsif_hw.c (see ensure_gem0_phy_addr_fix()) -- this patch must
    be reapplied after every regenerate for the same reason.

    Root cause: get_Realtek_phy_speed() reads PHY register 17
    (IEEE_SPECIFIC_STATUS_REG) and expects a Gigabit-Realtek-style
    bits[15:14] speed field. On this board's RTL8201F (10/100 only), that
    register's bits[15:14] can read 00 -- matching neither IEEE_SPEED_1000
    nor IEEE_SPEED_100 -- so the original code silently falls back to a
    hardcoded 10 Mbps even while ANAR (reg4) and ANLPAR (reg5) show both
    sides advertising and supporting 100BASE-TX full duplex and BMSR shows
    link up with autonegotiation complete. By the standard IEEE 802.3
    clause 28 priority-arbitration rules, the real negotiated link in that
    case is unambiguously 100 Mbps full duplex, not 10. The wrong 10 Mbps
    result makes phy_setup_emacps() configure the MAC
    (XEmacPs_SetOperatingSpeed) at 10 Mbps against a real 100 Mbps PHY
    link -- the same MAC/PHY speed mismatch failure mode as the PHY-address
    scan issue above, just from a different function, and it produces the
    same net_init() hang.
    """

    hw_c = (
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "libsrc" / "lwip213" / "src" / "contrib" / "ports" /
        "xilinx" / "netif" / "xemacpsif_physpeed.c"
    )

    if not hw_c.is_file():
        print("[INFO] xemacpsif_physpeed.c not found yet (no lwip213 in domain?): {0}".format(hw_c))
        return

    text = hw_c.read_text(encoding="utf-8")
    if _PHY_SPEED_PATCHED in text:
        print("[INFO] GEM0 PHY-speed (RTL8201F ANAR/ANLPAR) fix already applied in {0}".format(hw_c))
        return
    if _PHY_SPEED_ORIGINAL not in text:
        print("[WARN] Could not find expected PHY speed-decode block in {0} -- check manually".format(hw_c))
        return

    patched = text.replace(_PHY_SPEED_ORIGINAL, _PHY_SPEED_PATCHED)
    hw_c.write_text(patched, encoding="utf-8")
    print("[INFO] Patched {0}: GEM0 PHY speed resolved from ANAR/ANLPAR instead of the wrong RTL8201F vendor-register fallback".format(hw_c))


def _gen_bsp_dir():
    return (
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "libsrc" / "build_configs" / "gen_bsp"
    )


def _liblwip213_install_paths(lwip213_src_dir):
    return [
        WORKSPACE / PLATFORM_NAME / "ps7_cortexa9_0" / DOMAIN_NAME /
        "bsp" / "lib" / "liblwip213.a",
        WORKSPACE / PLATFORM_NAME / "export" / PLATFORM_NAME / "sw" / DOMAIN_NAME /
        "lib" / "liblwip213.a",
        lwip213_src_dir / "liblwip213.a",
    ]


def force_recompile_lwip213():
    """Delete the object files/archives that were compiled with LWIP_RAW unset.

    Only the two affected object files are removed, not the whole lwip213
    build tree -- deleting the tree forces a full CMake reconfigure, which
    (like platform.build() itself, see rebuild_lwip213_with_raw()) regenerates
    the build-tree lwipopts.h from the MLD template and wipes the patch again.
    """

    lwip213_src_dir = _gen_bsp_dir() / "libsrc" / "lwip213" / "src"
    stale_objs = [
        lwip213_src_dir / "CMakeFiles" / "lwip213.dir" / "lwip-2.1.3" / "src" / "core" / "raw.c.obj",
        lwip213_src_dir / "CMakeFiles" / "lwip213.dir" / "lwip-2.1.3" / "src" / "core" / "ipv4" / "dhcp.c.obj",
        lwip213_src_dir / "CMakeFiles" / "lwip213.dir" / "lwip-2.1.3" / "src" / "core" / "dns.c.obj",
        # xemacpsif_hw.c patched to skip the buggy 31..1 PHY auto-scan for
        # GEM0 (false-positive detection at MDIO address 1 on this board --
        # see the comment in that file) and go straight to the MDIO-scan-
        # confirmed real PHY address 0.
        lwip213_src_dir / "CMakeFiles" / "lwip213.dir" / "contrib" / "ports" / "xilinx" / "netif" / "xemacpsif_hw.c.obj",
        # xemacpsif_physpeed.c patched to resolve GEM0's link speed from
        # ANAR/ANLPAR instead of trusting the RTL8201F's vendor status
        # register field the generic driver misreads (see
        # ensure_gem0_phy_speed_fix()).
        lwip213_src_dir / "CMakeFiles" / "lwip213.dir" / "contrib" / "ports" / "xilinx" / "netif" / "xemacpsif_physpeed.c.obj",
    ]
    for obj in stale_objs:
        if obj.is_file():
            obj.unlink()
            print("[INFO] Removed stale {0} (force recompile against patched lwipopts.h)".format(obj))

    for lib in _liblwip213_install_paths(lwip213_src_dir):
        if lib.is_file():
            lib.unlink()
            print("[INFO] Removed stale {0}".format(lib))


def rebuild_lwip213_with_raw():
    """Recompile liblwip213.a without going through platform.build().

    platform.build() regenerates every MLD-templated header (including
    lwipopts.h) from scratch on EVERY call, unconditionally -- not just when
    CMake's own dependency tracking says a reconfigure is needed. That is
    what silently reverts ensure_lwip_dhcp_enabled()'s patch each time it
    runs, no matter how carefully the object-file deletion above is timed.
    Calling `make` directly on the already-configured CMake project sidesteps
    that Vitis-specific regeneration step entirely: plain incremental make
    only recompiles the two object files removed by force_recompile_lwip213()
    against the header actually on disk, then re-archives liblwip213.a.
    """

    import subprocess

    gen_bsp_dir = _gen_bsp_dir()
    vitis_root = os.environ.get("XILINX_VITIS", "")
    make_exe = Path(vitis_root) / "gnuwin" / "bin" / "make.exe"
    if not make_exe.is_file():
        fail("Vitis make.exe not found: {0}".format(make_exe))
    if not gen_bsp_dir.is_dir():
        fail("lwip213 domain build directory not found: {0}".format(gen_bsp_dir))

    result = subprocess.run(
        [str(make_exe), "lwip213"],
        cwd=str(gen_bsp_dir),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        universal_newlines=True, check=False,
    )
    print(result.stdout)
    if result.returncode != 0:
        fail("make lwip213 failed with exit code {0}".format(result.returncode))

    lwip213_src_dir = gen_bsp_dir / "libsrc" / "lwip213" / "src"
    fresh_lib = lwip213_src_dir / "liblwip213.a"
    if not fresh_lib.is_file():
        fail("make lwip213 finished without producing {0}".format(fresh_lib))

    for dest in _liblwip213_install_paths(lwip213_src_dir):
        if dest == fresh_lib:
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(fresh_lib), str(dest))
        print("[INFO] Installed rebuilt liblwip213.a to {0}".format(dest))


def find_elf():
    expected = APP_DIR / "build" / (APP_NAME + ".elf")
    if expected.is_file():
        return expected
    candidates = sorted((APP_DIR / "build").rglob(APP_NAME + ".elf"))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        fail("Vitis finished without producing {0}.".format(expected))
    fail("More than one CPU0 ELF was found: {0}".format(
        ", ".join(str(c) for c in candidates)
    ))


def verify_elf(elf_path):
    vitis_root = os.environ.get("XILINX_VITIS", "")
    readelf = (
        Path(vitis_root) / "gnu" / "aarch32" / "nt" / "gcc-arm-none-eabi" /
        "bin" / "arm-none-eabi-readelf.exe"
    )
    if not readelf.is_file():
        fail("Vitis ARM readelf not found: {0}".format(readelf))

    import subprocess
    result = subprocess.run(
        [str(readelf), "-h", str(elf_path)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        universal_newlines=True, check=False,
    )
    if result.returncode != 0:
        fail("ELF verification failed:\n{0}".format(result.stdout))
    if "ELF" not in result.stdout or "ARM" not in result.stdout:
        fail("The output is not an ARM ELF:\n{0}".format(result.stdout))


def publish_elf(elf_path, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    output_elf = output_dir / (APP_NAME + ".elf")
    shutil.copy2(str(elf_path), str(output_elf))
    digest = sha256(output_elf)
    (output_dir / (APP_NAME + ".elf.sha256")).write_text(
        "{0}  {1}\n".format(digest, output_elf.name), encoding="ascii"
    )
    print("[SUCCESS] CPU0 ELF : {0}".format(output_elf))
    print("[SUCCESS] SHA-256  : {0}".format(digest))


def main():
    args = parse_args()
    output_dir = Path(args.output).expanduser().resolve()

    if not APP_DIR.is_dir():
        fail("CPU0 application component not found: {0}".format(APP_DIR))

    if args.clean:
        reset_app_build_dir("--clean requested")

    # Vitis stores absolute paths in component metadata; re-root them onto this
    # checkout before the workspace is opened.
    vitis_metadata.repair_component_metadata(
        WORKSPACE, (PLATFORM_NAME, APP_NAME)
    )

    try:
        import vitis
    except ImportError:
        fail(
            "The Vitis Python module is unavailable.\n\n"
            "  FIX: Run the batch wrapper instead of Python directly:\n"
            "    scripts\\build_cpu0.bat\n\n"
            "  Or if you have Vitis 2023.2 installed elsewhere, set VITIS_ROOT\n"
            "  in the batch file and run again."
        )

    client = None
    try:
        client = vitis.create_client()
        client.set_workspace(path=str(WORKSPACE))

        platform = client.get_platform_component(name=PLATFORM_NAME)
        if platform is None:
            fail("Platform component '{0}' was not found.".format(PLATFORM_NAME))

        domain = platform.get_domain(name=DOMAIN_NAME)
        if domain is None:
            fail("Domain '{0}' was not found in '{1}'.".format(DOMAIN_NAME, PLATFORM_NAME))

        # platform.build() rebuilds every domain in the platform, including
        # CPU1's free_rtos domain. That domain's openamp/CMakeLists.txt
        # aborts with "requires libmetal" unless this same script session
        # has touched the domain's library list first (mirrors
        # ensure_cpu1_domain_libraries() in build_cpu1.py) -- otherwise
        # Vitis's live BSP_LIBSRC_SUBDIRS state for that domain is stale.
        other_domain = platform.get_domain(name="free_rtos")
        if other_domain is not None:
            for lib in ("libmetal", "openamp"):
                configured = other_domain.get_libs()
                names = set(e.get("name", "").strip().lower() for e in (configured or []))
                if lib.lower() not in names:
                    other_domain.set_lib(lib)

        configured = domain.get_libs()
        configured_names = set(
            entry.get("name", "").strip().lower() for entry in (configured or [])
        )
        libs_changed = False
        for library in DOMAIN_LIBRARIES:
            if library.lower() in configured_names:
                print("[INFO] CPU0 domain library already enabled: {0}".format(library))
            else:
                require_success(
                    "enable CPU0 domain library '{0}'".format(library),
                    domain.set_lib(library),
                )
                libs_changed = True

        # lwip213 defaults to DHCP compiled OUT (lwip_dhcp=false in
        # lwip213.mld) -- Filelists.cmake excludes dhcp.c's object entirely
        # at CMake-configure time when this is off, so patching the
        # generated lwipopts.h macro after the fact is not enough (that only
        # changes a #define, not which .c files got compiled into
        # liblwip213.a). Must go through domain.set_config() before
        # regenerate so the CMake file list is generated correctly.
        for param, value in (("lwip213_dhcp", "true"), ("lwip213_dhcp_does_arp_check", "true")):
            require_success(
                "set lwip213 config {0}={1}".format(param, value),
                domain.set_config(option="lib", param=param, value=value, lib_name="lwip213"),
            )
            libs_changed = True

        if libs_changed or not args.skip_domain_regen:
            print("[INFO] Regenerating standalone BSP/domain '{0}'...".format(DOMAIN_NAME))
            domain.regenerate()
            print("[OK] standalone domain regenerated.")
            # domain.regenerate() alone stages lwip213's sources/config headers
            # but does not compile the archive into the domain's exported lib/
            # directory -- platform.build() runs the CMake super-build that
            # actually produces liblwip213.a (same requirement CPU1's
            # libmetal/openamp needed; see scripts/build_cpu1.py).
            require_success("platform build (compiles new domain libraries)", platform.build())
            reset_app_build_dir("domain library set changed")

        ensure_lwip_dhcp_enabled()
        ensure_gem0_phy_addr_fix()
        ensure_gem0_phy_speed_fix()
        # lwipopts.h is regenerated fresh (overwriting any hand edits) by
        # domain.regenerate() above, and the platform.build() that already
        # ran compiled liblwip213.a from the PRE-patch header (LWIP_RAW
        # undefined -> raw.c compiles empty). Calling platform.build() again
        # does NOT fix this -- it regenerates lwipopts.h from the MLD
        # template unconditionally on every call, wiping the patch just
        # applied before it ever reaches the compiler. Rebuild lwip213 with
        # `make` directly instead (see rebuild_lwip213_with_raw()).
        force_recompile_lwip213()
        rebuild_lwip213_with_raw()
        reset_app_build_dir("lwipopts.h patched (DHCP/RAW)")

        app = client.get_component(name=APP_NAME)
        if app is None:
            fail("Application component '{0}' was not found.".format(APP_NAME))
        require_success("CPU0 application build", app.build())

        elf_path = find_elf()
        verify_elf(elf_path)
        publish_elf(elf_path, output_dir)
    finally:
        try:
            vitis.dispose()
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("[ERROR] {0}".format(exc), file=sys.stderr)
        raise SystemExit(1)
