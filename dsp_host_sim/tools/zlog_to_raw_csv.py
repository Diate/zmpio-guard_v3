#!/usr/bin/env python3
"""Validate baseline ZLOG v1 and export MPU6050 records for Step1."""

from __future__ import annotations

import argparse
import binascii
import csv
import struct
from pathlib import Path

FILE_MAGIC = 0x474F4C5A
RECORD_MAGIC = 0x52474F4C
FORMAT_VERSION = 1
SOURCE_MPU6050 = 1
FILE_HEADER = struct.Struct("<IHHQ4I")
RECORD_HEADER = struct.Struct("<IHHIQHHI")
MPU_PAYLOAD = struct.Struct("<7h")


def mpu_records(path: Path):
    with path.open("rb") as stream:
        raw = stream.read(FILE_HEADER.size)
        if len(raw) != FILE_HEADER.size:
            raise ValueError("truncated ZLOG file header")
        magic, version, header_size, _start_time, *_reserved = FILE_HEADER.unpack(raw)
        if magic != FILE_MAGIC or version != FORMAT_VERSION:
            raise ValueError("not a supported ZLOG v1 file")
        if header_size < FILE_HEADER.size:
            raise ValueError("invalid ZLOG file header size")
        stream.seek(header_size, 0)

        while True:
            offset = stream.tell()
            raw = stream.read(RECORD_HEADER.size)
            if not raw:
                return
            if len(raw) != RECORD_HEADER.size:
                raise ValueError(f"truncated record header at {offset}")
            (
                record_magic,
                record_version,
                source,
                sequence,
                timestamp_us,
                payload_len,
                flags,
                expected_crc,
            ) = RECORD_HEADER.unpack(raw)
            if record_magic != RECORD_MAGIC or record_version != FORMAT_VERSION:
                raise ValueError(f"invalid record header at {offset}")
            if payload_len > 240:
                raise ValueError(f"oversized payload at {offset}")
            payload = stream.read(payload_len)
            if len(payload) != payload_len:
                raise ValueError(f"truncated payload at {offset}")
            if (binascii.crc32(payload) & 0xFFFFFFFF) != expected_crc:
                raise ValueError(f"payload CRC mismatch at {offset}")
            if source != SOURCE_MPU6050:
                continue
            if len(payload) != MPU_PAYLOAD.size:
                raise ValueError(f"invalid MPU payload size at {offset}")
            yield (sequence, timestamp_us, flags, *MPU_PAYLOAD.unpack(payload))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--session-id", required=True)
    parser.add_argument("--label", default="UNLABELED")
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "session_id",
                "label",
                "sequence",
                "timestamp_us",
                "flags",
                "accel_x",
                "accel_y",
                "accel_z",
                "temperature_raw",
                "gyro_x",
                "gyro_y",
                "gyro_z",
            ]
        )
        for record in mpu_records(args.input):
            writer.writerow([args.session_id, args.label, *record])
            count += 1
    print(f"Validated and exported {count} MPU6050 records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

