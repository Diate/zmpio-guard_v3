#!/usr/bin/env python3
"""Quick analysis of a parsed ZLOG CSV file."""
import csv
import sys
from collections import Counter
from pathlib import Path

def analyze(csv_path: Path) -> dict:
    sources = Counter()
    t_min, t_max = float("inf"), 0
    mpu_count = 0
    linux_count = 0
    accel_samples = []
    first_ts = None
    last_ts = None
    zero_count = 0

    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            src = int(row["source"])
            sources[src] += 1
            ts = int(row["timestamp_us"])
            if first_ts is None:
                first_ts = ts
            last_ts = ts
            if ts < t_min:
                t_min = ts
            if ts > t_max:
                t_max = ts

            if src == 1:  # MPU6050
                mpu_count += 1
                ax = int(row["accel_x"])
                ay = int(row["accel_y"])
                az = int(row["accel_z"])
                if ax == 0 and ay == 0 and az == 0:
                    zero_count += 1
                if mpu_count <= 10:
                    accel_samples.append((ax, ay, az))
            elif src == 2:
                linux_count += 1

    duration_s = (t_max - t_min) / 1e6 if t_max > t_min else 0
    return {
        "total": sum(sources.values()),
        "mpu6050": sources.get(1, 0),
        "linux": sources.get(2, 0),
        "other": {k: v for k, v in sources.items() if k not in (1, 2)},
        "first_ts_us": first_ts,
        "last_ts_us": last_ts,
        "duration_s": duration_s,
        "duration_min": duration_s / 60,
        "sample_rate_hz": mpu_count / duration_s if duration_s > 0 else 0,
        "zero_samples": zero_count,
        "zero_pct": (zero_count / mpu_count * 100) if mpu_count else 0,
        "first_samples": accel_samples,
    }


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("zmpio-guard/dsp_host_sim/logs/LOG00006.csv")
    info = analyze(path)
    print(f"=== {path.name} Analysis ===")
    print(f"Total records:    {info['total']:>10,}")
    print(f"  MPU6050(1):     {info['mpu6050']:>10,}")
    print(f"  Linux(2):       {info['linux']:>10,}")
    for k, v in info["other"].items():
        print(f"  Source({k}):     {v:>10,}")
    print(f"Timestamp:        {info['first_ts_us']} -> {info['last_ts_us']} us")
    print(f"Duration:         {info['duration_s']:.1f}s  ({info['duration_min']:.1f} min)")
    print(f"Sample rate:      {info['sample_rate_hz']:.1f} Hz")
    print(f"Zero-accel samples: {info['zero_samples']:,} ({info['zero_pct']:.1f}%)")
    print()
    print("First 10 MPU6050 accel samples (raw int16):")
    for i, (ax, ay, az) in enumerate(info["first_samples"]):
        print(f"  [{i:2d}] X={ax:+6d}  Y={ay:+6d}  Z={az:+6d}")


if __name__ == "__main__":
    main()
