#!/usr/bin/env python3
"""Step 7.2 T7.2-5: prove zmpiod's capture files parse with parse_zlog.py.

Runs the zmpio_capture_gen helper (which drives the daemon's real capture
writer) and then validates every STREAMnnnnn.ZBIN it produced with the SAME
parser used on the microSD captures in docs/architecture/evidence/ -- not a
second decoder written for the test, which could only prove that two new
pieces of code agree with each other.

    python3 capture_roundtrip.py ./zmpio_capture_gen [--pairs N] [--max-bytes N]

Exit code 0 means: every file has a valid ZLOG header, every record's CRC32
matches, the record types decode to the expected FEATURE_V2 / ANOMALY_RULE_V1
/ DSP_HEALTH layouts, and the frame_sequence values are contiguous across the
rotation boundaries.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from parse_zlog import (  # noqa: E402  (path set up above)
    SOURCE_ANOMALY_RULE_V1,
    SOURCE_DSP_HEALTH,
    SOURCE_FEATURE_V2,
    parse_records,
    record_to_row,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generator", type=Path,
                        help="path to the built zmpio_capture_gen binary")
    parser.add_argument("--pairs", type=int, default=100,
                        help="feature/rule record pairs to generate")
    parser.add_argument("--max-bytes", type=int, default=4096,
                        help="capture rotation threshold (small forces "
                             "several files)")
    parser.add_argument("--keep", type=Path, default=None,
                        help="write the capture here instead of a temp dir")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        out_dir = args.keep if args.keep is not None else Path(tmp)
        out_dir.mkdir(parents=True, exist_ok=True)

        result = subprocess.run(
            [str(args.generator), str(out_dir), str(args.pairs),
             str(args.max_bytes)],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            print("capture_roundtrip: generator failed", file=sys.stderr)
            return 1

        files = sorted(out_dir.glob("STREAM*.ZBIN"))
        if not files:
            print("capture_roundtrip: generator produced no files",
                  file=sys.stderr)
            return 1

        features = []
        rules = []
        healths = 0
        total = 0
        for path in files:
            try:
                for record in parse_records(path):
                    row = record_to_row(record)
                    total += 1
                    if record["source"] == SOURCE_FEATURE_V2:
                        features.append(row["frame_sequence"])
                    elif record["source"] == SOURCE_ANOMALY_RULE_V1:
                        rules.append(row["frame_sequence"])
                    elif record["source"] == SOURCE_DSP_HEALTH:
                        healths += 1
                    else:
                        print(f"capture_roundtrip: unexpected source "
                              f"{record['source']} in {path}", file=sys.stderr)
                        return 1
            except ValueError as error:
                print(f"capture_roundtrip: {path}: {error}", file=sys.stderr)
                return 1

        if len(features) != args.pairs or len(rules) != args.pairs:
            print(f"capture_roundtrip: got {len(features)} features and "
                  f"{len(rules)} rules, expected {args.pairs} of each",
                  file=sys.stderr)
            return 1
        if healths != 1:
            print(f"capture_roundtrip: got {healths} health records, "
                  f"expected 1", file=sys.stderr)
            return 1
        if features != list(range(args.pairs)):
            print("capture_roundtrip: frame_sequence is not contiguous across "
                  "the rotation boundaries", file=sys.stderr)
            return 1
        if features != rules:
            print("capture_roundtrip: FEATURE_V2 and ANOMALY_RULE_V1 do not "
                  "pair 1:1 by frame_sequence", file=sys.stderr)
            return 1

        print(f"capture_roundtrip: {len(files)} file(s), {total} records, "
              f"0 parse errors, {args.pairs} feature/rule pairs contiguous")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
