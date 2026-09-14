#!/usr/bin/env python3
"""Build the ZMPIO CPU1 FreeRTOS firmware with Vitis Unified 2023.2.

This script intentionally updates the existing Vitis workspace rather than
creating a second platform.  In this project the relevant components are:

* Platform: ``platform_dual``
* CPU1 domain: ``free_rtos`` on ``ps7_cortexa9_1``
* Application: ``app_freertos``

Run it through ``vitis -s`` (normally via build_cpu1.bat), not a system
Python interpreter.  See README_CPU1_BUILD.md for usage.
"""

from __future__ import print_function

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

import vitis_metadata


PLATFORM_NAME = "platform_dual"
DOMAIN_NAME = "free_rtos"
APP_NAME = "app_freertos"
CPU1_INSTANCE = "ps7_cortexa9_1"
CPU1_DOMAIN_LIBRARIES = ("libmetal", "openamp")

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
WORKSPACE = PROJECT_ROOT / "firmware"
PLATFORM_DIR = WORKSPACE / PLATFORM_NAME
APP_DIR = WORKSPACE / APP_NAME
PLATFORM_METADATA = PLATFORM_DIR / "vitis-comp.json"
APP_METADATA = APP_DIR / "vitis-comp.json"
APP_YAML = APP_DIR / "src" / "app.yaml"
PLATFORM_XPFM = PLATFORM_DIR / "export" / PLATFORM_NAME / (PLATFORM_NAME + ".xpfm")
DOMAIN_PATH = PLATFORM_DIR / "export" / PLATFORM_NAME / "sw" / DOMAIN_NAME
# platform.update_hw() names the imported copy after the source XSA's basename,
# so this must track whatever the Vivado top-level wrapper is currently called.
DEFAULT_XSA = PLATFORM_DIR / "hw" / "design_1_wrapper.xsa"
VIVADO_PROJECT_DIR = PROJECT_ROOT / "hardware" / "vivado" / "project_hub"
DEFAULT_VIVADO_XSA = VIVADO_PROJECT_DIR / "design_1_wrapper.xsa"
DEFAULT_VIVADO_BIT = (
    VIVADO_PROJECT_DIR / "project_hub.runs" / "impl_1" / "design_1_wrapper.bit"
)
CPU1_BITSTREAM = APP_DIR / "_ide" / "bitstream" / "cpu1_wrapper.bit"
DEFAULT_OUTPUT = PROJECT_ROOT / "output" / "cpu1"

# --- CPU1 BSP AMP build flag ------------------------------------------------
# The whole generated BSP tree is .gitignore'd, so this build script is the
# only version-controlled place where the setting can live.
CPU1_BSP_DIR = PLATFORM_DIR / CPU1_INSTANCE / DOMAIN_NAME / "bsp"
CPU1_BSP_TOOLCHAIN = CPU1_BSP_DIR / "cortexa9_toolchain.cmake"
CPU1_BSP_GEN_BUILD_DIR = CPU1_BSP_DIR / "libsrc" / "build_configs" / "gen_bsp"
CPU1_BSP_COMPILE_COMMANDS = (
    CPU1_BSP_DIR / "libsrc" / "compile_commands.json",
    CPU1_BSP_GEN_BUILD_DIR / "compile_commands.json",
)
CPU1_AMP_DEFINE = "-DUSE_AMP=1"
# Every BSP translation unit whose behaviour actually depends on the flag.
CPU1_AMP_GUARDED_SOURCES = ("boot.S", "xil_cache.c")
CPU1_BSP_TOP_CMAKELISTS = CPU1_BSP_DIR / "CMakeLists.txt"
CPU1_BSP_LIBMETAL_DIR = CPU1_BSP_DIR / "libsrc" / "libmetal"
# Deliberately OUTSIDE firmware/platform_dual entirely, not just outside
# libsrc/: domain.regenerate()'s "Successfully created Domain" step
# recreates libsrc/ as a whole, so a backup placed anywhere under libsrc/
# gets wiped along with it. output/ is this project's own build-artifact
# area (see DEFAULT_OUTPUT) and nothing Vitis manages ever touches it.
CPU1_BSP_LIBMETAL_BACKUP = PROJECT_ROOT / "output" / ".cpu1_libmetal_use_amp_backup"


def env_flag(name):
    """Return True for a conventional enabled environment-variable value."""

    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes", "on")


def path_from(value):
    return Path(value).expanduser().resolve()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Build the ZMPIO CPU1 FreeRTOS ELF with Vitis Unified 2023.2."
    )
    parser.add_argument(
        "--xsa",
        default=os.environ.get("CPU1_BUILD_XSA", str(DEFAULT_VIVADO_XSA)),
        help="Vivado XSA to import into platform_dual (default: %(default)s)",
    )
    parser.add_argument(
        "--bit",
        default=os.environ.get("CPU1_BUILD_BIT", str(DEFAULT_VIVADO_BIT)),
        help="Vivado bitstream to synchronize into the CPU1 workspace (default: %(default)s)",
    )
    parser.add_argument(
        "--output",
        default=os.environ.get("CPU1_BUILD_OUTPUT", str(DEFAULT_OUTPUT)),
        help="Directory that receives app_freertos.elf and its SHA-256 file.",
    )
    parser.add_argument(
        "--app-only",
        action="store_true",
        default=env_flag("CPU1_BUILD_APP_ONLY"),
        help="Build only app_freertos; do not update XSA/platform/domain.",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        default=env_flag("CPU1_BUILD_CLEAN"),
        help="Clean the platform first (full mode) and recreate the app CMake build directory.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        default=env_flag("CPU1_BUILD_DRY_RUN"),
        help="Validate paths and show the selected flow without changing or building anything.",
    )
    return parser.parse_args()


def fail(message):
    raise RuntimeError(message)


def require_file(path, description):
    if not path.is_file():
        fail("{0} does not exist or is not a file: {1}".format(description, path))


def require_dir(path, description):
    if not path.is_dir():
        fail("{0} does not exist or is not a directory: {1}".format(description, path))


def replace_json_field(path, expected_name, replacements):
    """Apply expected metadata fixes and return whether the file changed."""

    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("name") != expected_name:
        fail("Unexpected component in {0}: expected '{1}', found '{2}'.".format(
            path, expected_name, data.get("name")
        ))

    changed = False
    for key, value in replacements.items():
        if data.get(key) != value:
            data[key] = value
            changed = True

    if changed:
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return changed


def repair_workspace_metadata(xsa_path):
    """Re-root the absolute paths Vitis stores in component metadata.

    Vitis records where the tree lived when it last wrote these files, so
    after a clone or a move every recorded path is wrong. Correcting them
    before the workspace is opened ensures the regenerated domain is the one
    app_freertos actually consumes.
    """

    vitis_metadata.repair_component_metadata(
        WORKSPACE, (PLATFORM_NAME, APP_NAME, "cpu0_application")
    )

    platform_data = json.loads(PLATFORM_METADATA.read_text(encoding="utf-8"))
    if platform_data.get("name") != PLATFORM_NAME:
        fail("Unexpected platform component in {0}.".format(PLATFORM_METADATA))

    configuration = platform_data.get("configuration")
    if not isinstance(configuration, dict):
        fail("Missing platform configuration in {0}.".format(PLATFORM_METADATA))

    changed_platform = False
    xsa_as_text = str(xsa_path)
    if configuration.get("xsa") != xsa_as_text:
        configuration["xsa"] = xsa_as_text
        changed_platform = True

    if changed_platform:
        PLATFORM_METADATA.write_text(
            json.dumps(platform_data, indent=2) + "\n", encoding="utf-8"
        )

    changed_app = replace_json_field(
        APP_METADATA,
        APP_NAME,
        {
            "platform": str(PLATFORM_XPFM),
            "domain": DOMAIN_NAME,
            "domainRealName": DOMAIN_NAME,
            "cpuInstance": CPU1_INSTANCE,
            "os": "freertos",
        },
    )

    yaml_lines = APP_YAML.read_text(encoding="utf-8").splitlines()
    new_domain_line = "domain_path: {0}".format(DOMAIN_PATH)
    changed_yaml = False
    found_domain_path = False
    rewritten_lines = []
    for line in yaml_lines:
        if line.startswith("domain_path:"):
            found_domain_path = True
            if line != new_domain_line:
                rewritten_lines.append(new_domain_line)
                changed_yaml = True
            else:
                rewritten_lines.append(line)
        else:
            rewritten_lines.append(line)

    if not found_domain_path:
        rewritten_lines.insert(0, new_domain_line)
        changed_yaml = True

    if changed_yaml:
        APP_YAML.write_text("\n".join(rewritten_lines) + "\n", encoding="utf-8")

    return changed_platform or changed_app or changed_yaml


def patch_cpu1_bsp_use_amp():
    """Force ``-DUSE_AMP=1`` into every CPU1 BSP compile, and say if it changed.

    Why this is mandatory, not cosmetic
    -----------------------------------
    Without USE_AMP the standalone BSP builds CPU1 as if it owned the whole
    APU.  Two blocks in ``arm/cortexa9/gcc/boot.S`` are then compiled in and
    run every time CPU0 releases CPU1 from WFE, while CPU0 is live:

      * ``str 0xffff, [0xf8f0000c]``  - SCU "invalidate all", which wipes the
        SCU duplicate tag RAM of BOTH cores and breaks CPU0's L1 coherency.
      * disable PL310 -> ``str 0xFFFF, [L2CCWay]`` -> re-enable.  That is an
        invalidate-by-way, which DISCARDS dirty L2 lines instead of writing
        them back, so any CPU0 data still dirty in the shared L2 (its stack,
        globals, lwIP buffers) silently reverts to the older DDR contents.

    The same flag also compiles the shared-L2 maintenance out of
    ``Xil_DCacheFlushRange()``/``Xil_DCacheInvalidateRange()``/
    ``Xil_DCacheEnable()``, so CPU1 stops driving the PL310 debug register
    underneath CPU0 at run time.  The L2 stays owned by CPU0, which is the
    arrangement Xilinx documents for Zynq AMP.

    Injection point
    ---------------
    ``cortexa9_toolchain.cmake`` is used because it is the only input proven
    to reach *both* languages: the BSP's ``ADD_DEFINITIONS(-c
    ${proc_extra_compiler_flags})`` expands to a bare ``-c`` on a cold
    configure (the cache variable is only populated later, from a
    subdirectory), so ``bsp.yaml``'s ``proc_extra_compiler_flags`` never
    reaches ``boot.S``.  Checked against the generated compile command for
    boot.S, and re-checked by verify_cpu1_bsp_use_amp() after every build.

    Call this AFTER domain.regenerate(), never only before it
    -----------------------------------------------------------
    domain.regenerate() re-runs its own "Successfully created Domain" step
    on this exact file, resetting it back to the pristine (no USE_AMP)
    template, THEN reconfigures and recompiles the whole BSP from that
    reset file as its own final phase -- silently undoing a patch applied
    before platform.build(), even though platform.build() itself compiled
    correctly with the flag in the meantime. So this call is only useful
    when followed by rebuild_cpu1_bsp_use_amp() (which reads this file
    fresh via a direct CMake invocation, bypassing domain.regenerate()
    entirely).
    """

    if not CPU1_BSP_TOOLCHAIN.is_file():
        # Full builds regenerate the BSP before this runs; app-only builds on
        # a workspace that has never been generated are caught by the verify.
        return False

    text = CPU1_BSP_TOOLCHAIN.read_text(encoding="utf-8")
    pattern = re.compile(
        r'(set\(\s*TOOLCHAIN_(?:C|CXX|ASM)_FLAGS\s+")([^"]*)(")'
    )

    def add_define(match):
        flags = match.group(2)
        if CPU1_AMP_DEFINE in flags:
            return match.group(0)
        return match.group(1) + flags + " " + CPU1_AMP_DEFINE + match.group(3)

    patched, substitutions = pattern.subn(add_define, text)
    if substitutions != 3:
        fail(
            "Expected TOOLCHAIN_C_FLAGS/TOOLCHAIN_CXX_FLAGS/TOOLCHAIN_ASM_FLAGS "
            "in {0}; found {1}. The Vitis BSP toolchain template changed - "
            "re-check where {2} has to be injected.".format(
                CPU1_BSP_TOOLCHAIN, substitutions, CPU1_AMP_DEFINE
            )
        )
    if patched == text:
        print("[INFO] CPU1 BSP already carries {0}.".format(CPU1_AMP_DEFINE))
        return False

    CPU1_BSP_TOOLCHAIN.write_text(patched, encoding="utf-8")
    print("[INFO] Added {0} to {1}.".format(CPU1_AMP_DEFINE, CPU1_BSP_TOOLCHAIN))
    return True


def cpu1_bsp_cmake_command():
    """Return the exact cmake.exe Vitis itself configured this BSP with.

    Read from the existing CMakeCache.txt (key CMAKE_COMMAND) instead of
    searching Vitis's install tree by version number, since this Vitis
    install carries more than one bundled CMake (cmake-3.24.2 AND
    cmake-3.3.2 side by side under tps/win64/) and only the cache says which
    one actually configured this specific build directory.
    """

    cache_file = CPU1_BSP_GEN_BUILD_DIR / "CMakeCache.txt"
    require_file(cache_file, "CPU1 BSP CMakeCache.txt")
    for line in cache_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_COMMAND:"):
            return line.split("=", 1)[1].strip()
    fail("CMAKE_COMMAND not found in {0}.".format(cache_file))


def run_cmake(cmake_exe, args, description):
    result = subprocess.run(
        [cmake_exe] + args,
        cwd=str(CPU1_BSP_GEN_BUILD_DIR),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        check=False,
    )
    print(result.stdout)
    if result.returncode != 0:
        fail("{0} failed (exit {1}).".format(description, result.returncode))


def backup_cpu1_bsp_libmetal():
    """Snapshot libsrc/libmetal right after platform.build() builds it.

    domain.regenerate() deletes and recreates this BSP's libsrc/ tree as
    part of its own "Successfully created Domain" step, and the recreated
    tree does not include libmetal, even though platform.build() (which
    runs just before domain.regenerate() in this same flow) already
    compiled it successfully. Call this right after platform.build()
    succeeds, while libsrc/libmetal still exists, so
    rebuild_cpu1_bsp_use_amp() can restore it after domain.regenerate() has
    torn it down.
    """

    if not CPU1_BSP_LIBMETAL_DIR.is_dir():
        fail(
            "{0} does not exist right after platform.build(); expected "
            "platform.build() to have populated it (ensure_cpu1_domain_libraries() "
            "enabled the 'libmetal' library beforehand).".format(CPU1_BSP_LIBMETAL_DIR)
        )
    if CPU1_BSP_LIBMETAL_BACKUP.exists():
        shutil.rmtree(str(CPU1_BSP_LIBMETAL_BACKUP))
    CPU1_BSP_LIBMETAL_BACKUP.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(str(CPU1_BSP_LIBMETAL_DIR), str(CPU1_BSP_LIBMETAL_BACKUP))
    print("[INFO] Backed up {0} (domain.regenerate() is about to delete it).".format(
        CPU1_BSP_LIBMETAL_DIR))


def restore_cpu1_bsp_libmetal():
    """Put libsrc/libmetal back if domain.regenerate() deleted it.

    See backup_cpu1_bsp_libmetal()'s docstring for why this is needed at
    all: domain.regenerate() does this every time openamp is enabled, so
    the restore is not optional defensive code.
    """

    if CPU1_BSP_LIBMETAL_DIR.is_dir():
        return
    if not CPU1_BSP_LIBMETAL_BACKUP.is_dir():
        fail(
            "{0} is missing and no backup exists at {1} to restore it from "
            "-- backup_cpu1_bsp_libmetal() must run after platform.build() "
            "and before domain.regenerate().".format(
                CPU1_BSP_LIBMETAL_DIR, CPU1_BSP_LIBMETAL_BACKUP
            )
        )
    shutil.copytree(str(CPU1_BSP_LIBMETAL_BACKUP), str(CPU1_BSP_LIBMETAL_DIR))
    print("[INFO] Restored {0} from backup (domain.regenerate() deleted it).".format(
        CPU1_BSP_LIBMETAL_DIR))


def ensure_libmetal_in_bsp_subdirs():
    """Add "libmetal" to this BSP's top CMakeLists.txt BSP_LIBSRC_SUBDIRS list.

    Root cause: the top CMakeLists.txt Vitis generates for this BSP
    hardcodes ``set (BSP_LIBSRC_SUBDIRS libsrc standalone xiltimer
    freertos10_xilinx openamp)`` -- "openamp" is listed (so its
    CMakeLists.txt gets add_subdirectory()'d and its guard runs), but
    "libmetal" never is, even though ensure_cpu1_domain_libraries() enables
    both libraries identically via domain.set_lib(). Without "libmetal" in
    this list, libsrc/openamp/src/CMakeLists.txt's ``if (NOT ("libmetal"
    IN_LIST BSP_LIBSRC_SUBDIRS))`` guard raises a plain CMake FATAL_ERROR
    ("This library requires libmetal library in the Board Support
    Package"). BSP_LIBSRC_SUBDIRS is a plain (non-CACHE) variable, so this
    is not a stale-cache problem -- it is simply missing from the template
    on every configure. Vitis's own domain.regenerate() produces a working
    build anyway through some other internal path that does not go through
    this file, unlike a direct ``cmake <build_dir>`` reconfigure (used by
    rebuild_cpu1_bsp_use_amp()), so this direct edit is required only for
    that direct reconfigure path.
    """

    text = CPU1_BSP_TOP_CMAKELISTS.read_text(encoding="utf-8")
    pattern = re.compile(r"(set\s*\(\s*BSP_LIBSRC_SUBDIRS\s+)([^)]*)(\))")
    match = pattern.search(text)
    if not match:
        fail("BSP_LIBSRC_SUBDIRS not found in {0}.".format(CPU1_BSP_TOP_CMAKELISTS))

    entries = match.group(2).split()
    if "libmetal" in entries:
        print("[INFO] {0} already lists libmetal in BSP_LIBSRC_SUBDIRS.".format(
            CPU1_BSP_TOP_CMAKELISTS))
        return False

    # Position before "openamp" (if present) so add_subdirectory() visits
    # libmetal first -- openamp's own CMakeLists.txt links against it.
    if "openamp" in entries:
        entries.insert(entries.index("openamp"), "libmetal")
    else:
        entries.append("libmetal")

    patched = pattern.sub(match.group(1) + " ".join(entries) + match.group(3), text, count=1)
    CPU1_BSP_TOP_CMAKELISTS.write_text(patched, encoding="utf-8")
    print("[INFO] Added libmetal to BSP_LIBSRC_SUBDIRS in {0}.".format(
        CPU1_BSP_TOP_CMAKELISTS))
    return True


def rebuild_cpu1_bsp_use_amp():
    """Make USE_AMP=1 actually reach the linked libxilstandalone.a.

    Must run AFTER domain.regenerate(), which is the step that resets
    cortexa9_toolchain.cmake and does its own final rebuild from that reset
    file (see patch_cpu1_bsp_use_amp()'s docstring). Going through
    platform.build() or domain.regenerate() again here would only repeat
    that same reset, so this drives CMake directly instead:

      1. Re-patch the toolchain file (domain.regenerate() just reset it),
         restore libsrc/libmetal (domain.regenerate() deletes it -- see
         backup_cpu1_bsp_libmetal()), and add "libmetal" to this BSP's top
         CMakeLists.txt BSP_LIBSRC_SUBDIRS list (see
         ensure_libmetal_in_bsp_subdirs()). This is needed even though this
         function never builds openamp/libmetal (see step 3): the top
         CMakeLists.txt's foreach(entry ${SUBDIR_LIST}) always calls
         add_subdirectory() on openamp when SUBDIR_LIST=ALL (step 2), and
         openamp/src/CMakeLists.txt has a hard `if (NOT ("libmetal" IN_LIST
         BSP_LIBSRC_SUBDIRS)) message(FATAL_ERROR ...)` guard evaluated at
         CONFIGURE time -- so configure itself aborts before we ever get to
         choose a build target unless libmetal is listed and its source
         directory exists.
      2. cmake -U on the EXISTING gen_bsp build directory, uncaching THREE
         separate things Vitis's own CMakeCache.txt is left holding from an
         earlier, narrower run:
           - SUBDIR_LIST (re-set to ALL): domain.set_lib()/domain.regenerate()
             leave this CACHE variable pinned to just "openamp" (whatever
             library was most recently toggled through the Vitis API), so a
             plain reconfigure of this same build dir silently never
             add_subdirectory()s "standalone" at all -- no error, just no
             xilstandalone build rule in the generated Makefiles.
           - TOOLCHAIN_C_FLAGS / TOOLCHAIN_CXX_FLAGS / TOOLCHAIN_ASM_FLAGS:
             CACHE STRING, so set(VAR value CACHE STRING ...) in the
             now-repatched toolchain file is a no-op without -U first.
           - CMAKE_C_FLAGS / CMAKE_CXX_FLAGS / CMAKE_ASM_FLAGS: the SAME
             toolchain file derives these from the TOOLCHAIN_*_FLAGS above
             via another `set(CMAKE_C_FLAGS "${TOOLCHAIN_C_FLAGS} ..." CACHE
             STRING ...)` -- itself a second, independent CACHE STRING that
             is just as stale and needs its own -U, or the freshly-uncached
             TOOLCHAIN_C_FLAGS never actually reaches a single compile
             command: uncaching only the TOOLCHAIN_* variables leaves
             -DUSE_AMP=1 sitting in CMakeCache.txt while every actual
             arm-none-eabi-gcc invocation in flags.make still lacks it.
      3. Build ONLY the "xilstandalone" target (the CMake target that owns
         boot.S/xil_cache.c -- confirmed via its build.make path at
         libsrc/standalone/src/CMakeFiles/xilstandalone.dir), never the
         default "all" target. "all" also compiles openamp, which is
         legacy/unused in this project's data path (see CLAUDE.md section 6)
         and, independently of USE_AMP, fails to compile from a bare direct
         reconfigure: openamp's virtio.c needs metal/compiler.h generated
         by libmetal's own nested CMake project, which only happens if
         libmetal is actually BUILT (not just configured) first -- a
         Vitis-internal multi-pass sequencing this direct reconfigure does
         not replicate. Restricting the build to "xilstandalone" sidesteps
         that whole class of problem instead of trying to reproduce it.
      4. Copy the resulting libxilstandalone.a to this BSP's own lib/ and to
         platform_dual/export/.../free_rtos/lib/ by hand: that copy is
         otherwise made by domain.regenerate()'s own export packaging,
         which this function deliberately does not re-invoke. app_freertos
         links against the export copy, not against this BSP's lib/
         directly (confirmed via app_freertos/build/CMakeCache.txt's -L
         path).
    """

    patch_cpu1_bsp_use_amp()
    restore_cpu1_bsp_libmetal()
    ensure_libmetal_in_bsp_subdirs()
    cmake_exe = cpu1_bsp_cmake_command()

    run_cmake(
        cmake_exe,
        ["-U", "SUBDIR_LIST", "-D", "SUBDIR_LIST=ALL",
         "-U", "TOOLCHAIN_C_FLAGS", "-U", "TOOLCHAIN_CXX_FLAGS",
         "-U", "TOOLCHAIN_ASM_FLAGS",
         "-U", "CMAKE_C_FLAGS", "-U", "CMAKE_CXX_FLAGS", "-U", "CMAKE_ASM_FLAGS",
         str(CPU1_BSP_GEN_BUILD_DIR)],
        "Uncaching CPU1 BSP subdir list and toolchain flags",
    )
    run_cmake(
        cmake_exe,
        ["--build", str(CPU1_BSP_GEN_BUILD_DIR), "--target", "xilstandalone"],
        "Rebuilding CPU1 xilstandalone for USE_AMP=1",
    )

    built_archive = (
        CPU1_BSP_GEN_BUILD_DIR / "libsrc" / "standalone" / "src" / "libxilstandalone.a"
    )
    require_file(built_archive, "rebuilt libxilstandalone.a")
    bsp_lib_dir = CPU1_BSP_DIR / "lib"
    export_lib_dir = DOMAIN_PATH / "lib"
    require_dir(bsp_lib_dir, "CPU1 BSP lib directory")
    require_dir(export_lib_dir, "CPU1 domain export lib directory")
    shutil.copy2(str(built_archive), str(bsp_lib_dir / "libxilstandalone.a"))
    shutil.copy2(str(built_archive), str(export_lib_dir / "libxilstandalone.a"))
    print("[INFO] Re-exported rebuilt libxilstandalone.a to {0} and {1}".format(
        bsp_lib_dir, export_lib_dir))

    if CPU1_BSP_LIBMETAL_BACKUP.exists():
        shutil.rmtree(str(CPU1_BSP_LIBMETAL_BACKUP))


def probe_cpu1_bsp_use_amp():
    """Report, without failing, whether the built BSP objects carry the flag.

    Returns (ok, reason). Used to decide whether the BSP must be recompiled:
    the toolchain file being correct is NOT sufficient evidence, because it
    may already have been patched by an earlier run whose build did not
    complete, leaving stale objects behind.

    Checks every candidate in CPU1_BSP_COMPILE_COMMANDS and succeeds as soon
    as ONE confirms the flag, rather than stopping at the first candidate
    that merely exists: libsrc/compile_commands.json can be a stale copy
    left over from an earlier Vitis-driven pass (platform.build()/
    domain.regenerate(), which both predate rebuild_cpu1_bsp_use_amp() in
    the calling flow and do not carry USE_AMP) while
    libsrc/build_configs/gen_bsp/compile_commands.json -- the actual CMake
    build directory rebuild_cpu1_bsp_use_amp() just compiled from --
    correctly has it. Stopping at the first existing file would make a
    genuinely correct rebuild report as failed.
    """

    failures = []
    checked_any = False
    for candidate in CPU1_BSP_COMPILE_COMMANDS:
        if not candidate.is_file():
            continue
        checked_any = True
        try:
            entries = json.loads(candidate.read_text(encoding="utf-8", errors="replace"))
        except ValueError as exc:
            fail("Could not parse {0}: {1}".format(candidate, exc))

        missing = []
        for source in CPU1_AMP_GUARDED_SOURCES:
            matches = [
                entry for entry in entries
                if str(entry.get("file", "")).replace("\\", "/").endswith("/" + source)
            ]
            if not matches:
                missing.append("{0} (not compiled in this BSP)".format(source))
                continue
            if not all(CPU1_AMP_DEFINE in entry.get("command", "") for entry in matches):
                missing.append("{0} (compiled without {1})".format(source, CPU1_AMP_DEFINE))

        if missing:
            failures.append("{0}; evidence file: {1}".format(
                ", ".join(missing), candidate))
            continue
        return True, str(candidate)

    if not checked_any:
        return False, "no BSP compile_commands.json found under {0}".format(CPU1_BSP_DIR)
    return False, " | ".join(failures)


def verify_cpu1_bsp_use_amp():
    """Hard gate: a silently-missing USE_AMP is exactly the failure this whole
    change exists to prevent, so an unverifiable build is a failed build."""

    ok, detail = probe_cpu1_bsp_use_amp()
    if not ok:
        fail(
            "CPU1 BSP was not built with {0} ({1}). Re-run a full build "
            "(without --app-only) so the BSP is regenerated and recompiled; "
            "leaving this unset lets CPU1's boot.S invalidate the shared L2 "
            "and the SCU under CPU0 every time CPU1 starts.".format(
                CPU1_AMP_DEFINE, detail
            )
        )
    print("[OK] CPU1 BSP built with {0} (verified via {1}).".format(
        CPU1_AMP_DEFINE, detail))


def reset_app_build_dir(reason):
    """Remove only the generated CMake build directory for app_freertos."""

    build_dir = APP_DIR / "build"
    if build_dir.exists():
        print("[INFO] Removing {0} ({1})".format(build_dir, reason))
        shutil.rmtree(str(build_dir))


def same_path(first, second):
    """Compare Windows paths without requiring that both targets are present."""

    return os.path.normcase(os.path.normpath(str(first))) == os.path.normcase(
        os.path.normpath(str(second))
    )


def ensure_cpu1_domain_libraries(domain):
    """Keep the CPU1 OpenAMP dependencies in the regenerated BSP.

    Vitis only regenerates libraries recorded in the domain configuration.  A
    newly updated XSA otherwise produces a FreeRTOS BSP with xiltimer only and
    Vitis then removes ``metal;openamp`` from app_freertos/CMakeLists.txt.
    """

    configured = domain.get_libs()
    configured_names = set(
        entry.get("name", "").strip().lower() for entry in (configured or [])
    )
    for library in CPU1_DOMAIN_LIBRARIES:
        if library.lower() in configured_names:
            print("[INFO] CPU1 domain library already enabled: {0}".format(library))
        else:
            require_success(
                "enable CPU1 domain library '{0}'".format(library),
                domain.set_lib(library),
            )


def ensure_cpu1_app_link_libraries():
    """Bridge Vitis 2023.2's OpenAMP archive/link-name mismatch."""

    lib_dir = DOMAIN_PATH / "lib"
    require_file(lib_dir / "libmetal.a", "generated libmetal archive")

    generated_openamp = lib_dir / "libopen_amp.a"
    expected_openamp = lib_dir / "libopenamp.a"
    if generated_openamp.is_file():
        # Vitis' generated build_app.py links '-lopenamp', but this Vitis
        # 2023.2 BSP emits libopen_amp.a.  Make the name expected by the
        # Vitis application builder available without changing the archive.
        if (
            not expected_openamp.is_file()
            or expected_openamp.stat().st_size != generated_openamp.stat().st_size
            or sha256(expected_openamp) != sha256(generated_openamp)
        ):
            shutil.copy2(str(generated_openamp), str(expected_openamp))
            print("[INFO] Created OpenAMP compatibility archive: {0}".format(expected_openamp))
    elif expected_openamp.is_file():
        # Platform exports generated by earlier Vitis component revisions
        # already use the library name expected by build_app.py.
        pass
    else:
        fail("Neither libopen_amp.a nor libopenamp.a was generated in {0}.".format(lib_dir))

    desired_line = "collect(PROJECT_LIB_DEPS xiltimer;metal;openamp)"
    cmake_text = (APP_DIR / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    cmake_lines = cmake_text.splitlines(keepends=True)
    replacements = 0
    for index, line in enumerate(cmake_lines):
        if line.strip().startswith("collect(PROJECT_LIB_DEPS xiltimer"):
            line_ending = "\r\n" if line.endswith("\r\n") else "\n"
            cmake_lines[index] = desired_line + line_ending
            replacements += 1

    if replacements != 1:
        fail("The CPU1 CMake dependency line was not found in app_freertos/src/CMakeLists.txt.")
    updated_text = "".join(cmake_lines)
    if updated_text != cmake_text:
        (APP_DIR / "src" / "CMakeLists.txt").write_text(updated_text, encoding="utf-8")
        print("[INFO] Restored CPU1 CMake OpenAMP dependency.")


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


def find_elf():
    expected = APP_DIR / "build" / (APP_NAME + ".elf")
    if expected.is_file():
        return expected

    candidates = sorted((APP_DIR / "build").rglob(APP_NAME + ".elf"))
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        fail("Vitis finished without producing {0}.".format(expected))
    fail("More than one CPU1 ELF was found: {0}".format(
        ", ".join(str(candidate) for candidate in candidates)
    ))


def verify_elf(elf_path):
    """Confirm that the produced file is an ARM ELF, not a stale text/binary file."""

    vitis_root = os.environ.get("XILINX_VITIS", "")
    readelf = (
        Path(vitis_root)
        / "gnu"
        / "aarch32"
        / "nt"
        / "gcc-arm-none-eabi"
        / "bin"
        / "arm-none-eabi-readelf.exe"
    )
    require_file(readelf, "Vitis ARM readelf")

    result = subprocess.run(
        [str(readelf), "-h", str(elf_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        universal_newlines=True,
        check=False,
    )
    if result.returncode != 0:
        fail("ELF verification failed:\n{0}".format(result.stdout))
    if "ELF" not in result.stdout or "ARM" not in result.stdout:
        fail("The output is not an ARM ELF:\n{0}".format(result.stdout))


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as input_file:
        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_xsa_bitstream_pair(xsa_path, bit_path):
    """Verify that the standalone bitstream is the one embedded in the XSA."""

    bit_digest = sha256(bit_path)
    try:
        with zipfile.ZipFile(str(xsa_path), "r") as archive:
            candidates = [
                name for name in archive.namelist()
                if name.lower().endswith(".bit") and not name.endswith("/")
            ]
            if not candidates:
                fail("The XSA does not contain an embedded bitstream: {0}".format(xsa_path))

            for name in candidates:
                digest = hashlib.sha256()
                with archive.open(name, "r") as embedded_bit:
                    for chunk in iter(lambda: embedded_bit.read(1024 * 1024), b""):
                        digest.update(chunk)
                if digest.hexdigest() == bit_digest:
                    print("[OK] XSA/bitstream pair matched: {0}".format(name))
                    return
    except zipfile.BadZipFile:
        fail("The selected XSA is not a valid archive: {0}".format(xsa_path))

    fail("The standalone bitstream does not match any bitstream embedded in {0}.".format(xsa_path))


def synchronize_bitstream(bit_path):
    """Copy the Vivado bitstream into the CPU1 workspace and verify the copy."""

    if same_path(bit_path, CPU1_BITSTREAM):
        print("[INFO] Bitstream is already at the CPU1 workspace destination.")
    elif CPU1_BITSTREAM.is_file() and sha256(CPU1_BITSTREAM) == sha256(bit_path):
        print("[INFO] CPU1 bitstream is already synchronized: {0}".format(CPU1_BITSTREAM))
    else:
        CPU1_BITSTREAM.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(bit_path), str(CPU1_BITSTREAM))
        print("[INFO] Synchronized CPU1 bitstream: {0}".format(CPU1_BITSTREAM))

    require_file(CPU1_BITSTREAM, "synchronized CPU1 bitstream")
    if sha256(CPU1_BITSTREAM) != sha256(bit_path):
        fail("Bitstream verification failed after copying to {0}.".format(CPU1_BITSTREAM))


def verify_imported_hardware(source_xsa, source_bit):
    """Confirm that Vitis retained exactly the selected Vivado artifacts."""

    require_file(DEFAULT_XSA, "imported platform XSA")
    require_file(CPU1_BITSTREAM, "imported CPU1 bitstream")
    if sha256(DEFAULT_XSA) != sha256(source_xsa):
        fail("Imported platform XSA does not match the selected Vivado XSA.")
    if sha256(CPU1_BITSTREAM) != sha256(source_bit):
        fail("Imported CPU1 bitstream does not match the selected Vivado bitstream.")
    print("[OK] Imported XSA and CPU1 bitstream hashes match the Vivado sources.")


def publish_elf(elf_path, output_dir):
    output_dir.mkdir(parents=True, exist_ok=True)
    output_elf = output_dir / (APP_NAME + ".elf")
    shutil.copy2(str(elf_path), str(output_elf))

    digest = sha256(output_elf)
    checksum_file = output_dir / (APP_NAME + ".elf.sha256")
    checksum_file.write_text(
        "{0}  {1}\n".format(digest, output_elf.name), encoding="ascii"
    )
    print("[SUCCESS] CPU1 ELF : {0}".format(output_elf))
    print("[SUCCESS] SHA-256  : {0}".format(digest))


def preflight(xsa_path, bit_path, output_dir, app_only):
    require_dir(PROJECT_ROOT, "Project root")
    require_dir(WORKSPACE, "Vitis workspace")
    require_dir(PLATFORM_DIR, "Platform component")
    require_dir(APP_DIR, "CPU1 application component")
    require_file(PLATFORM_METADATA, "Platform metadata")
    require_file(APP_METADATA, "CPU1 application metadata")
    require_file(APP_YAML, "CPU1 application YAML")
    if app_only:
        require_file(DEFAULT_XSA, "local platform XSA")
    else:
        require_file(xsa_path, "Vivado XSA")
        require_file(bit_path, "Vivado bitstream")
    if output_dir.exists() and not output_dir.is_dir():
        fail("Output path exists but is not a directory: {0}".format(output_dir))


def main():
    args = parse_args()
    xsa_path = path_from(args.xsa)
    bit_path = path_from(args.bit)
    output_dir = path_from(args.output)

    preflight(xsa_path, bit_path, output_dir, args.app_only)
    print("[INFO] Workspace : {0}".format(WORKSPACE))
    print("[INFO] XSA source: {0}".format(xsa_path))
    print("[INFO] BIT source: {0}".format(bit_path))
    print("[INFO] Output    : {0}".format(output_dir))
    print("[INFO] Mode      : {0}".format("app only" if args.app_only else "full rebuild"))

    if args.dry_run:
        if not args.app_only:
            verify_xsa_bitstream_pair(xsa_path, bit_path)
        print("[DRY-RUN] Full mode: sync bit -> update XSA -> build platform -> regenerate free_rtos -> apply USE_AMP=1 -> build app.")
        print("[DRY-RUN] No files were changed.")
        return 0

    if not args.app_only:
        verify_xsa_bitstream_pair(xsa_path, bit_path)
        synchronize_bitstream(bit_path)

    metadata_changed = repair_workspace_metadata(DEFAULT_XSA if args.app_only else xsa_path)
    if metadata_changed:
        print("[INFO] Repaired Vitis metadata paths for this workspace.")

    # A new XSA changes generated headers, BSP libraries and toolchain paths, so
    # the app CMake cache must never be reused across that boundary -- nor after
    # the metadata paths were re-rooted onto a different checkout.
    if not args.app_only:
        reset_app_build_dir("full XSA/platform rebuild")
    elif args.clean or metadata_changed:
        reset_app_build_dir("requested clean or repaired component metadata")

    try:
        import vitis
    except ImportError:
        fail("The Vitis Python module is unavailable. Run via vitis -s build_cpu1.py.")

    client = None
    try:
        client = vitis.create_client()
        client.set_workspace(path=str(WORKSPACE))

        platform = client.get_platform_component(name=PLATFORM_NAME)
        if platform is None:
            fail("Platform component '{0}' was not found.".format(PLATFORM_NAME))

        if not args.app_only:
            # The default XSA is already stored at platform_dual/hw.  Calling
            # update_hw() with that exact file makes Vitis 2023.2 try to copy
            # a file onto itself and print a misleading copy error.  The
            # platform metadata was updated above, so a platform build is the
            # correct full-rebuild operation in this case.
            if same_path(xsa_path, DEFAULT_XSA):
                print("[INFO] XSA is already in platform_dual/hw; skipping self-copy update_hw().")
            else:
                require_success("hardware update", platform.update_hw(hw=str(xsa_path)))
                # update_hw imports the supplied XSA into platform_dual/hw.
                # Store that local copy in metadata so no later UI/build action
                # can silently return to the external/legacy source path.
                repair_workspace_metadata(DEFAULT_XSA)
                print("[INFO] Platform metadata now uses the local imported XSA: {0}".format(DEFAULT_XSA))

            domain = platform.get_domain(name=DOMAIN_NAME)
            if domain is None:
                fail("Domain '{0}' was not found in '{1}'.".format(DOMAIN_NAME, PLATFORM_NAME))
            ensure_cpu1_domain_libraries(domain)
            require_success("platform build", platform.build())
            backup_cpu1_bsp_libmetal()

            # Vitis 2023.2 exposes regenerate() without a return value; a
            # failure is normally reported as an exception by the API --
            # EXCEPT for one specific failure mode: openamp/src/
            # CMakeLists.txt's "libmetal not in BSP_LIBSRC_SUBDIRS" CMake
            # FATAL_ERROR trips a bug in Vitis's OWN error handler --
            # library_utils.py's except-block calls os.path.join(None, ...)
            # and raises TypeError, which swallows the real CMake error
            # instead of propagating it, so this call can return normally
            # even after wiping this BSP's libsrc/libmetal directory
            # mid-run. When that happens, ensure_cpu1_app_link_libraries()
            # right below fails clearly on a missing libmetal.a -- the fix
            # at that point is to re-run this script (this failure mode can
            # be intermittent, sometimes needing more than one full rebuild
            # attempt), not to remove this call: it is also what populates
            # platform_dual/export/, which app.build() depends on and
            # platform.build() alone does not produce. See
            # docs/TECHNICAL_INTERVIEW_PROBLEM_ANALYSIS.md for the detailed
            # root-cause writeup.
            print("[INFO] Regenerating FreeRTOS BSP/domain '{0}'...".format(DOMAIN_NAME))
            domain.regenerate()
            print("[OK] FreeRTOS domain regenerated.")
            ensure_cpu1_app_link_libraries()

            # domain.regenerate() unconditionally resets and recompiles
            # cortexa9_toolchain.cmake from its pristine template as part of
            # its own "Successfully created Domain" step -- a flag patched
            # and verified present right after platform.build() is gone
            # from that file, and from the resulting libxilstandalone.a,
            # the moment domain.regenerate() finishes -- so USE_AMP can
            # only be asserted AFTER this call, never only before it. See
            # rebuild_cpu1_bsp_use_amp()'s docstring for the mechanism.
            rebuild_cpu1_bsp_use_amp()

        if args.app_only:
            # Cannot rebuild the BSP here; patch anyway so the next full build
            # starts compliant, and let verify_cpu1_bsp_use_amp() below decide
            # whether this ELF may be published.
            patch_cpu1_bsp_use_amp()

        # Run this after the full-domain path above as well as in app-only
        # mode, because app.build() itself writes its standard dependency line.
        ensure_cpu1_app_link_libraries()
        app = client.get_component(name=APP_NAME)
        if app is None:
            fail("Application component '{0}' was not found.".format(APP_NAME))
        require_success("CPU1 application build", app.build())

        elf_path = find_elf()
        verify_elf(elf_path)
        verify_cpu1_bsp_use_amp()
        publish_elf(elf_path, output_dir)
    finally:
        # Official Vitis API: closes the command-line Vitis client/server.
        try:
            vitis.dispose()
        except Exception:
            pass

    # Vitis persists component JSON while its server shuts down and can restore
    # the external path passed to update_hw().  Run this only after dispose so
    # the workspace always records its imported local XSA for later UI builds.
    if not args.app_only and repair_workspace_metadata(DEFAULT_XSA):
        print("[INFO] Finalized metadata paths to the local CPU1 XSA/workspace.")

    if not args.app_only:
        verify_imported_hardware(xsa_path, bit_path)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("[ERROR] {0}".format(exc), file=sys.stderr)
        raise SystemExit(1)
