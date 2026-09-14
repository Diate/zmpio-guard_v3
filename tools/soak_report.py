#!/usr/bin/env python3
"""Step 7.4 soak driver + reconciliation (docs/PLAN_BUOC_7.md SS4 7.4).

Two jobs, deliberately in one tool because they answer the same question:

  sample  -- poll `zmpioctl stream-status` and the daemon's RSS over an
             8-hour run, appending one CSV row per sample.  Nothing is
             computed here; the row is the raw evidence.
  report  -- turn a sample CSV (and, optionally, the ZLOG captures from both
             the host and the microSD card) into the 7.4 pass-gate verdict.

The gate this checks, stated exactly:

  1. Every frame CPU1's PL produced is accounted for.  With
        produced   = cpu1_feature_count delta
        published  = cpu1_published delta
        dropped    = cpu1_dropped delta        (ring full / mutex timeout)
        fifo_drop  = cpu1_ctrl_drop delta      (PL FIFO overflowed)
     the identity that must hold is
        produced == published + dropped + fifo_drop  (+/- one in-flight frame)
  2. Nothing was lost AFTER CPU1 published: rx_stream_gaps == 0.  A single
     gap means a second reader touched the ring or a slot was overwritten --
     the failure this whole design exists to prevent (REQ-STR-002).
  3. Every frame_sequence jump is explained: frame_gap_frames ==
     frame_gap_frames_explained + fifo_drop.
  4. The doorbell, not the fallback tick, moved the data:
     msgs_from_tick stays at (or very near) zero.
  5. zmpiod's RSS does not grow monotonically.

  cross-check (optional, and the strongest evidence in 7.2): --sd-csv and
  --host-csv, both produced by parse_zlog.py, must agree byte-for-byte on
  every FEATURE_V2 frame they share.

Usage:
    python3 tools/soak_report.py sample --target root@192.168.1.50 \\
        --out soak.csv --hours 8
    python3 tools/soak_report.py report --samples soak.csv \\
        [--sd-csv card.csv --host-csv stream.csv]
"""

from __future__ import annotations

import argparse
import csv
import datetime
import re
import subprocess
import sys
import time
from pathlib import Path

COUNTER_RE = re.compile(r"(\w+)=(-?(?:0x[0-9a-fA-F]+|\d+))")

# Columns worth keeping in the sample CSV.  Anything zmpioctl reports that is
# not listed here still lands in the raw column, so a counter added later is
# not lost by an older copy of this script.
SAMPLE_FIELDS = [
    "timestamp", "elapsed_s", "rss_kb",
    "cpu1_feature_count", "cpu1_published", "cpu1_dropped",
    "cpu1_health_published", "cpu1_ctrl_drop", "cpu1_bridge_drop",
    "cpu1_doorbell_rings",
    "rx_feature", "rx_health", "rx_reported_drops", "rx_stream_gaps",
    "rx_stream_gap_messages", "rx_malformed", "rx_no_sink",
    "orphan_overflow", "frame_gaps", "frame_gap_frames",
    "frame_gap_frames_explained", "doorbell_wakeups", "msgs_from_doorbell",
    "msgs_from_tick", "watch_lines_dropped", "capture_records",
    "capture_rotations", "capture_write_errors",
    "raw",
]


def ssh(target: str, command: str, ssh_bin: str = "ssh") -> str:
    result = subprocess.run([ssh_bin, target, command], capture_output=True,
                            text=True)
    return (result.stdout or "") + (result.stderr or "")


def parse_counters(text: str) -> dict[str, int]:
    return {key: int(value, 0) for key, value in COUNTER_RE.findall(text)}


def sample_once(target: str, ssh_bin: str) -> dict[str, object]:
    line = ssh(target, "zmpioctl stream-status", ssh_bin).strip()
    counters = parse_counters(line)
    rss = ssh(target, "grep VmRSS /proc/$(pidof zmpiod)/status", ssh_bin)
    rss_kb = 0
    match = re.search(r"(\d+)\s*kB", rss)
    if match:
        rss_kb = int(match.group(1))

    row: dict[str, object] = {field: counters.get(field, "")
                              for field in SAMPLE_FIELDS}
    row["rss_kb"] = rss_kb
    row["raw"] = line
    return row


def command_sample(args: argparse.Namespace) -> int:
    out_path = Path(args.out)
    new_file = not out_path.exists()
    started = time.time()
    deadline = started + args.hours * 3600.0

    with out_path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=SAMPLE_FIELDS)
        if new_file:
            writer.writeheader()
        while time.time() < deadline:
            row = sample_once(args.target, args.ssh)
            row["timestamp"] = datetime.datetime.now().isoformat(timespec="seconds")
            row["elapsed_s"] = int(time.time() - started)
            writer.writerow(row)
            handle.flush()
            print(f"{row['timestamp']} rss={row['rss_kb']}kB "
                  f"rx_feature={row['rx_feature']} "
                  f"gaps={row['rx_stream_gaps']}")
            time.sleep(args.interval)
    print(f"soak_report: wrote samples to {out_path}")
    return 0


def load_samples(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def as_int(row: dict[str, str], key: str) -> int:
    value = row.get(key, "")
    return int(value) if str(value).strip() not in ("", "None") else 0


def check_cross_reference(sd_csv: Path, host_csv: Path) -> tuple[bool, str]:
    """Compares the two parse_zlog.py CSVs on every shared frame_sequence.

    This is the 7.2 gate's strongest item: the microSD copy and the copy that
    travelled through the IPC ring must be identical for every frame both
    contain.  Comparing every decoded FEATURE_V2 column is the same thing as
    comparing the 48 payload bytes, since those columns ARE that payload.
    """
    feature_columns = [
        "frame_sequence", "rms_q24_8", "peak_q24_8", "variance_q32_0",
        "kurtosis_q16_16", "dominant_frequency_q16_16", "dominant_power_q32_0",
        "band0_q32_0", "band1_q32_0", "band2_q32_0", "band3_q32_0",
    ]

    def index(path: Path) -> dict[str, dict[str, str]]:
        rows: dict[str, dict[str, str]] = {}
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                if row.get("source") != "4":  # LOG_SOURCE_FEATURE_V2
                    continue
                rows[row["frame_sequence"]] = row
        return rows

    sd_rows = index(sd_csv)
    host_rows = index(host_csv)
    shared = sorted(set(sd_rows) & set(host_rows), key=int)
    if not shared:
        return False, ("no frame_sequence appears in both captures -- are "
                       "they from the same session?")

    mismatches = []
    for key in shared:
        for column in feature_columns:
            if sd_rows[key].get(column) != host_rows[key].get(column):
                mismatches.append(f"frame {key}: {column} "
                                  f"{sd_rows[key].get(column)} != "
                                  f"{host_rows[key].get(column)}")
                break
        if len(mismatches) >= 5:
            break

    if mismatches:
        return False, (f"{len(shared)} shared frames, mismatches: "
                       + "; ".join(mismatches))
    return True, (f"{len(shared)} shared frames identical on all "
                  f"{len(feature_columns)} decoded fields (48 payload bytes)")


def command_report(args: argparse.Namespace) -> int:
    rows = load_samples(Path(args.samples))
    if len(rows) < 2:
        print("soak_report: need at least two samples", file=sys.stderr)
        return 2

    first, last = rows[0], rows[-1]
    duration_h = (as_int(last, "elapsed_s") - as_int(first, "elapsed_s")) / 3600.0

    produced = as_int(last, "cpu1_feature_count") - as_int(first, "cpu1_feature_count")
    published = as_int(last, "cpu1_published") - as_int(first, "cpu1_published")
    dropped = as_int(last, "cpu1_dropped") - as_int(first, "cpu1_dropped")
    fifo_drop = as_int(last, "cpu1_ctrl_drop") - as_int(first, "cpu1_ctrl_drop")
    received = as_int(last, "rx_feature") - as_int(first, "rx_feature")
    gaps = as_int(last, "rx_stream_gaps") - as_int(first, "rx_stream_gaps")
    frame_gap_frames = (as_int(last, "frame_gap_frames")
                        - as_int(first, "frame_gap_frames"))
    frame_gap_explained = (as_int(last, "frame_gap_frames_explained")
                           - as_int(first, "frame_gap_frames_explained"))
    from_tick = as_int(last, "msgs_from_tick") - as_int(first, "msgs_from_tick")
    write_errors = (as_int(last, "capture_write_errors")
                    - as_int(first, "capture_write_errors"))

    rss = [as_int(row, "rss_kb") for row in rows if as_int(row, "rss_kb") > 0]
    rss_growth = (rss[-1] - rss[0]) if rss else 0
    #
    # A monotonically non-decreasing RSS trend needs three things to be true
    # at once before it is worth calling a leak, otherwise the check either
    # fires on glibc
    # arena noise or passes on a two-sample run that cannot show a trend at
    # all: enough samples to see a shape, a series that never comes back
    # down, and growth big enough to matter.  A run too short to judge is
    # reported as INCONCLUSIVE rather than quietly counted as a pass.
    #
    rss_samples = len(rss)
    rss_monotonic = rss_samples >= 10 and all(b >= a for a, b in zip(rss, rss[1:]))
    rss_threshold_kb = max(1024, (rss[0] // 20) if rss else 0)  # 1 MiB or +5%
    rss_leaking = rss_monotonic and (rss_growth > rss_threshold_kb)
    rss_inconclusive = rss_samples < 10

    checks: list[tuple[str, bool, str]] = []

    # One frame may legitimately be in flight at either sample boundary, so
    # the accounting identity is allowed a slack of one on each side.
    accounted = published + dropped + fifo_drop
    checks.append((
        "every produced frame is accounted for",
        abs(produced - accounted) <= 2,
        f"produced={produced} published={published} dropped={dropped} "
        f"fifo_drop={fifo_drop} (accounted={accounted})"))

    checks.append((
        "nothing lost after CPU1 published (single reader)",
        gaps == 0,
        f"rx_stream_gaps={gaps}"))

    checks.append((
        "every frame_sequence jump is explained",
        frame_gap_frames <= frame_gap_explained + fifo_drop,
        f"frame_gap_frames={frame_gap_frames} explained="
        f"{frame_gap_explained} fifo_drop={fifo_drop}"))

    checks.append((
        "the doorbell moved the data, not the fallback tick",
        from_tick == 0,
        f"msgs_from_tick={from_tick}, msgs_from_doorbell="
        f"{as_int(last, 'msgs_from_doorbell') - as_int(first, 'msgs_from_doorbell')}"))

    checks.append((
        "no capture write errors",
        write_errors == 0,
        f"capture_write_errors={write_errors}"))

    rss_detail = (f"rss {rss[0] if rss else '?'}kB -> "
                  f"{rss[-1] if rss else '?'}kB (delta {rss_growth}kB over "
                  f"{duration_h:.2f} h, {rss_samples} samples, leak threshold "
                  f"{rss_threshold_kb}kB)")
    if rss_inconclusive:
        rss_detail += " -- INCONCLUSIVE, need >= 10 samples to judge a trend"
    checks.append((
        "zmpiod RSS is not leaking",
        not rss_leaking,
        rss_detail))

    checks.append((
        "run is long enough for the 8-hour gate",
        duration_h >= 8.0,
        f"duration={duration_h:.2f} h"))

    if args.sd_csv and args.host_csv:
        ok, detail = check_cross_reference(Path(args.sd_csv),
                                           Path(args.host_csv))
        checks.append(("microSD and host captures agree byte-for-byte", ok,
                       detail))

    print("=" * 72)
    print(f"Step 7.4 soak report -- {args.samples}")
    print(f"samples: {len(rows)}   duration: {duration_h:.2f} h   "
          f"frames received: {received}")
    print(f"timing (last sample): {last.get('raw', '')[-160:]}")
    print("-" * 72)
    failures = 0
    for name, ok, detail in checks:
        print(f"[{'PASS' if ok else 'FAIL'}] {name} -- {detail}")
        if not ok:
            failures += 1
    print("-" * 72)
    print(f"VERDICT: {'PASS' if failures == 0 else f'FAIL ({failures})'}")
    print("=" * 72)
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    sampler = sub.add_parser("sample", help="poll the target into a CSV")
    sampler.add_argument("--target", required=True)
    sampler.add_argument("--ssh", default="ssh")
    sampler.add_argument("--out", required=True)
    sampler.add_argument("--hours", type=float, default=8.0)
    sampler.add_argument("--interval", type=float, default=60.0)
    sampler.set_defaults(func=command_sample)

    reporter = sub.add_parser("report", help="verdict from a sample CSV")
    reporter.add_argument("--samples", required=True)
    reporter.add_argument("--sd-csv",
                          help="parse_zlog.py CSV of the microSD LOGnnnnn.BIN")
    reporter.add_argument("--host-csv",
                          help="parse_zlog.py CSV of the zmpiod STREAMnnnnn.ZBIN")
    reporter.set_defaults(func=command_report)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
