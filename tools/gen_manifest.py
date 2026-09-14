#!/usr/bin/env python3
"""Step 7.5 T7.5-5: build artifacts/manifest.sha256 for a release candidate.

"Every artifact has a hash and a version" is the last line of the V3 pass
gate, and it is worth being pedantic about: a board that boots from an image
nobody can identify afterwards has not been verified, it has merely worked
once.

The manifest records, for each artifact that exists:
  - SHA-256, size and mtime;
  - the git commit the tree was at, and whether the tree was dirty;
  - the ABI constants that must agree between CPU1 and Linux
    (IPC_PROTOCOL_VERSION, IPC_STREAM_ABI_VERSION, ZMPIO_ABI_V3_LAYOUT_HASH),
    read out of the headers rather than typed in;
  - the CPU1 ELF's PT_LOAD extents when readelf is available, so the
    "everything inside [0x18000000, 0x19000000)" check of T7.5-1 is part of
    the same record rather than a separate note someone has to remember.

    python3 tools/gen_manifest.py                 # writes artifacts/manifest.sha256
    python3 tools/gen_manifest.py --check         # verify an existing manifest
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Ordered by boot sequence, which is also the order they matter in when a
# cold boot fails: what the ROM loads first is listed first.
ARTIFACTS = [
    ("boot/BOOT.BIN", "artifacts/BOOT.BIN"),
    ("boot/image.ub", "artifacts/image.ub"),
    ("boot/boot.scr", "artifacts/boot.scr"),
    ("boot/system.dtb", "artifacts/system.dtb"),
    ("boot/rootfs.tar.gz", "artifacts/rootfs.tar.gz"),
    ("cpu1/app_freertos.elf", "output/cpu1/app_freertos.elf"),
    ("cpu1/app_freertos.bin", "output/cpu1/app_freertos.bin"),
    ("pl/design_1_wrapper.bit",
     "hardware/vivado/project_hub/project_hub.runs/impl_1/design_1_wrapper.bit"),
    ("pl/design_1_wrapper.xsa", "firmware/platform_dual/hw/design_1_wrapper.xsa"),
    ("linux/zmpiod", "output/linux/zmpiod"),
    ("linux/zmpioctl", "output/linux/zmpioctl"),
]

MANIFEST_REL = "artifacts/manifest.sha256"

# name -> (file, regex with one capture group)
ABI_CONSTANTS = {
    "IPC_PROTOCOL_VERSION": ("common/zmpio_protocol.h",
                             r"#define\s+IPC_PROTOCOL_VERSION\s+(\S+)"),
    "IPC_STREAM_ABI_VERSION": ("common/zmpio_protocol.h",
                               r"#define\s+IPC_STREAM_ABI_VERSION\s+(\S+)"),
    "ZMPIO_ABI_V3_LAYOUT_HASH": ("common/zmpio_abi_v3_layout_hash.h",
                                 r"#define\s+ZMPIO_ABI_V3_LAYOUT_HASH\s+(\S+)"),
    "APP_RULE_ANOMALY_VERSION": ("firmware/app_freertos/src/app_config.h",
                                 r"#define\s+APP_RULE_ANOMALY_VERSION\s+(\S+)"),
}

CPU1_LOAD_LOW = 0x18000000
CPU1_LOAD_HIGH = 0x19000000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"


def read_constants() -> dict[str, str]:
    values: dict[str, str] = {}
    for name, (relative, pattern) in ABI_CONSTANTS.items():
        path = REPO / relative
        if not path.exists():
            values[name] = "missing"
            continue
        match = re.search(pattern, path.read_text(encoding="utf-8",
                                                 errors="replace"))
        values[name] = match.group(1) if match else "not-found"
    return values


def cpu1_load_segments(elf: Path) -> tuple[list[str], bool]:
    """PT_LOAD extents plus whether they all sit in CPU1's reserved window.

    Returns ([], True) when readelf is unavailable -- an absent tool must not
    turn into a silent "checked and fine".  The caller prints which it was.
    """
    readelf = shutil.which("readelf") or shutil.which("arm-none-eabi-readelf")
    if readelf is None or not elf.exists():
        return [], True

    result = subprocess.run([readelf, "-lW", str(elf)], capture_output=True,
                            text=True)
    lines = []
    inside = True
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) < 6 or parts[0] != "LOAD":
            continue
        try:
            phys = int(parts[3], 16)
            memsz = int(parts[5], 16)
        except ValueError:
            continue
        end = phys + memsz
        fits = (phys >= CPU1_LOAD_LOW) and (end <= CPU1_LOAD_HIGH)
        inside = inside and fits
        lines.append(f"PT_LOAD paddr=0x{phys:08x} memsz=0x{memsz:x} "
                     f"end=0x{end:08x} {'in-range' if fits else 'OUT-OF-RANGE'}")
    return lines, inside


def build_manifest() -> tuple[str, list[str]]:
    problems: list[str] = []
    constants = read_constants()
    # The manifest can never be clean while describing its own writing, so its
    # own path does not count towards "was the tree dirty".  Everything else
    # does: a manifest generated from a tree with uncommitted source in it
    # cannot identify a release candidate, and must say so.
    # --untracked-files=all so an untracked manifest shows up under its own
    # path rather than as a bare "artifacts/" directory entry.
    dirty = [line for line in
             git("status", "--porcelain", "--untracked-files=all").splitlines()
             if MANIFEST_REL not in line.replace("\\", "/")]

    out = [
        "# ZMPIO Guard V3 release manifest (tools/gen_manifest.py)",
        "#",
        "# Every line below identifies one artifact of ONE release candidate.",
        "# A cold-boot evidence run is only meaningful together with this file:",
        "# it is what says which bytes were on the card.",
        "#",
        f"generated_utc: {datetime.datetime.now(datetime.timezone.utc).isoformat()}",
        f"git_commit: {git('rev-parse', 'HEAD')}",
        f"git_branch: {git('rev-parse', '--abbrev-ref', 'HEAD')}",
        f"git_dirty: {'yes' if dirty else 'no'}",
        "",
        "# ABI constants that CPU1 and Linux must agree on.  Read from the",
        "# headers at generation time, never transcribed by hand.",
    ]
    for name, value in constants.items():
        out.append(f"{name}: {value}")
        if value in ("missing", "not-found"):
            problems.append(f"{name} could not be read")

    out.append("")
    out.append("# sha256  size_bytes  role  path")

    for role, relative in ARTIFACTS:
        path = REPO / relative
        if not path.exists():
            out.append(f"# MISSING  -  {role}  {relative}")
            problems.append(f"{role}: {relative} not found")
            continue
        out.append(f"{sha256(path)}  {path.stat().st_size}  {role}  {relative}")

    elf = REPO / "output/cpu1/app_freertos.elf"
    segments, inside = cpu1_load_segments(elf)
    out.append("")
    out.append("# CPU1 ELF load map (T7.5-1): every PT_LOAD must lie inside")
    out.append(f"# [0x{CPU1_LOAD_LOW:08x}, 0x{CPU1_LOAD_HIGH:08x}).")
    if not segments:
        out.append("# readelf unavailable or ELF missing -- NOT CHECKED")
        problems.append("CPU1 PT_LOAD range not checked (no readelf / no ELF)")
    else:
        out.extend(f"# {line}" for line in segments)
        if not inside:
            problems.append("CPU1 ELF has a PT_LOAD outside its reserved window")

    return "\n".join(out) + "\n", problems


def check_manifest(path: Path) -> int:
    failures = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or (":" in line.split("  ")[0]) or not line.strip():
            continue
        parts = line.split("  ")
        if len(parts) != 4:
            continue
        digest, _size, role, relative = parts
        target = REPO / relative
        if not target.exists():
            print(f"[FAIL] {role}: {relative} is gone")
            failures += 1
            continue
        actual = sha256(target)
        if actual != digest:
            print(f"[FAIL] {role}: {relative} hash changed\n"
                  f"       manifest {digest}\n       actual   {actual}")
            failures += 1
        else:
            print(f"[ OK ] {role}: {relative}")
    print(f"{'PASS' if failures == 0 else f'FAIL ({failures})'}")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path,
                        default=REPO / "artifacts" / "manifest.sha256")
    parser.add_argument("--check", action="store_true",
                        help="verify the artifacts against an existing manifest")
    args = parser.parse_args()

    if args.check:
        if not args.out.exists():
            print(f"gen_manifest: {args.out} does not exist", file=sys.stderr)
            return 2
        return check_manifest(args.out)

    text, problems = build_manifest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    print(f"gen_manifest: wrote {args.out}")

    if problems:
        print("\nINCOMPLETE -- this manifest does not yet describe a full "
              "release candidate:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("manifest complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
