#!/usr/bin/env python3
"""Run the bit-accurate C++ generator from a built Step1 snapshot."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    step_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=step_root / "golden" / "golden_tone_features.csv",
    )
    parser.add_argument(
        "--generator",
        type=Path,
        default=step_root / "build" / "step1_golden.exe",
    )
    args = parser.parse_args()

    if not args.generator.is_file():
        raise SystemExit(
            f"Missing {args.generator}; run dsp_host_sim/BUILD_STEP.ps1 first"
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        [str(args.generator), str(args.output)], check=False
    )
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())

