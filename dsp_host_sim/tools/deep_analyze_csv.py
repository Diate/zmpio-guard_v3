#!/usr/bin/env python3
"""Deep quality analysis of parsed ZLOG CSV files — sequence gaps, interval stats."""
import csv
import sys
from pathlib import Path

def deep_analyze(csv_path: Path) -> dict:
    sequences = []
    intervals_us = []
    prev_ts = None
    seq_gaps = 0
    mpu_count = 0

    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            src = int(row["source"])
            if src != 1:
                continue
            mpu_count += 1
            seq = int(row["sequence"])
            ts = int(row["timestamp_us"])
            sequences.append(seq)
            if prev_ts is not None:
                intervals_us.append(ts - prev_ts)
            prev_ts = ts

    # Sequence gap detection
    sorted_seqs = sorted(sequences)
    gaps = []
    for i in range(1, len(sorted_seqs)):
        diff = sorted_seqs[i] - sorted_seqs[i - 1]
        if diff != 1:
            gaps.append((sorted_seqs[i - 1], sorted_seqs[i], diff - 1))
            seq_gaps += diff - 1

    # Interval stats
    if intervals_us:
        intervals_sorted = sorted(intervals_us)
        n = len(intervals_sorted)
        avg = sum(intervals_sorted) / n
        p50 = intervals_sorted[n // 2]
        p95 = intervals_sorted[int(n * 0.95)]
        p99 = intervals_sorted[int(n * 0.99)]
        min_i = intervals_sorted[0]
        max_i = intervals_sorted[-1]
        neg_count = sum(1 for x in intervals_us if x < 0)
    else:
        avg = p50 = p95 = p99 = min_i = max_i = neg_count = 0

    return {
        "mpu_count": mpu_count,
        "seq_min": sorted_seqs[0] if sorted_seqs else 0,
        "seq_max": sorted_seqs[-1] if sorted_seqs else 0,
        "seq_gaps": seq_gaps,
        "gap_count": len(gaps),
        "first_gaps": gaps[:5],
        "interval_avg_us": avg,
        "interval_p50_us": p50,
        "interval_p95_us": p95,
        "interval_p99_us": p99,
        "interval_min_us": min_i,
        "interval_max_us": max_i,
        "neg_intervals": neg_count,
    }


def main():
    files = sys.argv[1:] if len(sys.argv) > 1 else [
        "zmpio-guard/dsp_host_sim/logs/LOG00004.csv",
        "zmpio-guard/dsp_host_sim/logs/LOG00006.csv",
    ]
    for f in files:
        p = Path(f)
        d = deep_analyze(p)
        print(f"=== {p.name} Deep Analysis ===")
        print(f"MPU records:    {d['mpu_count']:>10,}")
        print(f"Sequence range: {d['seq_min']:>10,} -> {d['seq_max']:>10,}")
        print(f"Sequence gaps:  {d['seq_gaps']:>10,}  (in {d['gap_count']} segments)")
        if d["first_gaps"]:
            print("First gaps (seq_before -> seq_after, missing):")
            for a, b, m in d["first_gaps"]:
                print(f"  {a} -> {b}  (missing {m})")
        print(f"Interval stats (us):")
        print(f"  avg={d['interval_avg_us']:.0f}  p50={d['interval_p50_us']}  p95={d['interval_p95_us']}  p99={d['interval_p99_us']}")
        print(f"  min={d['interval_min_us']}  max={d['interval_max_us']}")
        print(f"  negative intervals: {d['neg_intervals']}")
        print(f"  effective rate: {1e6/d['interval_avg_us']:.1f} Hz" if d['interval_avg_us'] > 0 else "")
        print()


if __name__ == "__main__":
    main()
