#!/usr/bin/env python3
"""Validate a ZLOG binary file and export its records as CSV."""

from __future__ import annotations

import argparse
import binascii
import csv
import struct
import sys
from pathlib import Path

FILE_MAGIC = 0x474F4C5A
RECORD_MAGIC = 0x52474F4C
FORMAT_VERSION = 1
SOURCE_MPU6050 = 1
SOURCE_LINUX = 2
SOURCE_EVENT = 3
# Source values defined in firmware/app_freertos/src/zlog_feature_v2.h.
# ANOMALY_ML_V1 (6) has no CPU1 producer and is left undecoded here on
# purpose -- it falls through to the generic hex dump below like any other
# unrecognized source.
SOURCE_FEATURE_V2 = 4
SOURCE_ANOMALY_RULE_V1 = 5
SOURCE_DSP_HEALTH = 7

FILE_HEADER = struct.Struct("<IHHQ4I")
RECORD_HEADER = struct.Struct("<IHHIQHHI")
MPU6050_PAYLOAD = struct.Struct("<7h")
FEATURE_V2_PAYLOAD = struct.Struct("<12I")
ANOMALY_RULE_V1_PAYLOAD = struct.Struct("<4B3I")
DSP_HEALTH_PAYLOAD = struct.Struct("<4B4I")


def parse_records(path: Path):
    with path.open("rb") as stream:
        raw_header = stream.read(FILE_HEADER.size)
        if len(raw_header) != FILE_HEADER.size:
            raise ValueError("file header is truncated")

        magic, version, header_size, start_time_us, *_ = FILE_HEADER.unpack(
            raw_header
        )
        if magic != FILE_MAGIC:
            raise ValueError(f"invalid file magic 0x{magic:08x}")
        if version != FORMAT_VERSION:
            raise ValueError(f"unsupported file version {version}")
        if header_size < FILE_HEADER.size:
            raise ValueError(f"invalid file header size {header_size}")
        if header_size > FILE_HEADER.size:
            stream.seek(header_size - FILE_HEADER.size, 1)

        record_index = 0
        while True:
            offset = stream.tell()
            raw_record_header = stream.read(RECORD_HEADER.size)
            if not raw_record_header:
                break
            if len(raw_record_header) != RECORD_HEADER.size:
                raise ValueError(f"truncated record header at offset {offset}")

            (
                record_magic,
                record_version,
                source,
                sequence,
                timestamp_us,
                payload_len,
                flags,
                expected_crc,
            ) = RECORD_HEADER.unpack(raw_record_header)
            if record_magic != RECORD_MAGIC:
                raise ValueError(
                    f"invalid record magic at offset {offset}: "
                    f"0x{record_magic:08x}"
                )
            if record_version != FORMAT_VERSION:
                raise ValueError(
                    f"unsupported record version {record_version} at {offset}"
                )
            if payload_len > 240:
                raise ValueError(f"invalid payload length {payload_len} at {offset}")

            payload = stream.read(payload_len)
            if len(payload) != payload_len:
                raise ValueError(f"truncated payload at offset {offset}")
            actual_crc = binascii.crc32(payload) & 0xFFFFFFFF
            if actual_crc != expected_crc:
                raise ValueError(
                    f"CRC mismatch at offset {offset}: "
                    f"expected 0x{expected_crc:08x}, got 0x{actual_crc:08x}"
                )

            yield {
                "file_start_us": start_time_us,
                "record_index": record_index,
                "source": source,
                "sequence": sequence,
                "timestamp_us": timestamp_us,
                "flags": flags,
                "payload": payload,
            }
            record_index += 1


def record_to_row(record: dict) -> dict:
    row = {
        "record_index": record["record_index"],
        "source": record["source"],
        "sequence": record["sequence"],
        "timestamp_us": record["timestamp_us"],
        "flags": record["flags"],
        "accel_x": "",
        "accel_y": "",
        "accel_z": "",
        "temperature_raw": "",
        "gyro_x": "",
        "gyro_y": "",
        "gyro_z": "",
        "linux_payload": "",
        "frame_sequence": "",
        "rms_q24_8": "",
        "peak_q24_8": "",
        "variance_q32_0": "",
        "kurtosis_q16_16": "",
        "dominant_frequency_q16_16": "",
        "dominant_power_q32_0": "",
        "band0_q32_0": "",
        "band1_q32_0": "",
        "band2_q32_0": "",
        "band3_q32_0": "",
        "rule_version": "",
        "rule_metric_id": "",
        "rule_verdict": "",
        "rule_value_q24_8": "",
        "rule_threshold_q24_8": "",
        "health_version": "",
        "health_state": "",
        "health_fault": "",
        "health_consecutive_fault_count": "",
        "health_feature_count": "",
        "health_ctrl_drop_count": "",
        "health_bridge_drop_count": "",
    }

    payload = record["payload"]
    if record["source"] == SOURCE_MPU6050:
        if len(payload) != MPU6050_PAYLOAD.size:
            raise ValueError(
                f"MPU6050 record {record['record_index']} has "
                f"{len(payload)} bytes, expected {MPU6050_PAYLOAD.size}"
            )
        (
            row["accel_x"],
            row["accel_y"],
            row["accel_z"],
            row["temperature_raw"],
            row["gyro_x"],
            row["gyro_y"],
            row["gyro_z"],
        ) = MPU6050_PAYLOAD.unpack(payload)
    elif record["source"] == SOURCE_LINUX:
        row["linux_payload"] = payload.decode("utf-8", errors="backslashreplace")
    elif record["source"] == SOURCE_FEATURE_V2 and len(payload) == FEATURE_V2_PAYLOAD.size:
        (row["frame_sequence"], _window_end, row["rms_q24_8"], row["peak_q24_8"],
         row["variance_q32_0"], row["kurtosis_q16_16"], row["dominant_frequency_q16_16"],
         row["dominant_power_q32_0"], row["band0_q32_0"], row["band1_q32_0"],
         row["band2_q32_0"], row["band3_q32_0"]) = FEATURE_V2_PAYLOAD.unpack(payload)
    elif record["source"] == SOURCE_ANOMALY_RULE_V1 and len(payload) == ANOMALY_RULE_V1_PAYLOAD.size:
        (row["rule_version"], row["rule_metric_id"], row["rule_verdict"], _reserved0,
         row["frame_sequence"], row["rule_value_q24_8"],
         row["rule_threshold_q24_8"]) = ANOMALY_RULE_V1_PAYLOAD.unpack(payload)
    elif record["source"] == SOURCE_DSP_HEALTH and len(payload) == DSP_HEALTH_PAYLOAD.size:
        (row["health_version"], row["health_state"], row["health_fault"], _reserved0,
         row["health_consecutive_fault_count"], row["health_feature_count"],
         row["health_ctrl_drop_count"],
         row["health_bridge_drop_count"]) = DSP_HEALTH_PAYLOAD.unpack(payload)
    else:
        row["linux_payload"] = payload.hex()
    return row


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="LOGxxxxx.BIN file")
    parser.add_argument(
        "-o", "--output", type=Path, help="CSV destination (default: stdout)"
    )
    args = parser.parse_args()

    output = args.output.open("w", newline="", encoding="utf-8") if args.output else sys.stdout
    close_output = output is not sys.stdout
    fieldnames = list(record_to_row({
        "record_index": 0,
        "source": 0,
        "sequence": 0,
        "timestamp_us": 0,
        "flags": 0,
        "payload": b"",
    }).keys())

    try:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        count = 0
        for record in parse_records(args.input):
            writer.writerow(record_to_row(record))
            count += 1
    except (OSError, ValueError) as error:
        print(f"parse_zlog: {error}", file=sys.stderr)
        return 1
    finally:
        if close_output:
            output.close()

    print(f"Validated {count} records", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
