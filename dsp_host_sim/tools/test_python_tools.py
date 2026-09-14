#!/usr/bin/env python3
"""Smoke-test all Step1 Python tools using temporary synthetic fixtures."""

from __future__ import annotations

import ast
import binascii
import csv
import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*arguments: object) -> None:
    subprocess.run([str(item) for item in arguments], check=True)


def test_python_syntax(step_root: Path) -> None:
    files = list(step_root.rglob("*.py"))
    for path in files:
        ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    print(f"Python AST PASS ({len(files)} files)")


def test_golden(step_root: Path, temporary: Path) -> None:
    generated = temporary / "golden.csv"
    run(
        sys.executable,
        step_root / "tools" / "gen_golden_vectors.py",
        "--generator",
        step_root / "build" / "step1_golden.exe",
        "--output",
        generated,
    )
    expected = step_root / "golden" / "golden_tone_features.csv"
    if generated.read_bytes() != expected.read_bytes():
        raise AssertionError("Python golden wrapper changed deterministic output")


def test_zlog_converter(step_root: Path, temporary: Path) -> None:
    file_header = struct.Struct("<IHHQ4I")
    record_header = struct.Struct("<IHHIQHHI")
    payload_struct = struct.Struct("<7h")
    payload = payload_struct.pack(1, -2, 3, 4, -5, 6, -7)
    fixture = temporary / "fixture.zlog"
    fixture.write_bytes(
        file_header.pack(0x474F4C5A, 1, file_header.size, 123456, 0, 0, 0, 0)
        + record_header.pack(
            0x52474F4C,
            1,
            1,
            42,
            654321,
            len(payload),
            9,
            binascii.crc32(payload) & 0xFFFFFFFF,
        )
        + payload
    )
    output = temporary / "raw.csv"
    run(
        sys.executable,
        step_root / "tools" / "zlog_to_raw_csv.py",
        fixture,
        output,
        "--session-id",
        "smoke_session",
        "--label",
        "normal",
    )
    with output.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise AssertionError("ZLOG converter did not emit exactly one MPU row")
    expected = {
        "session_id": "smoke_session",
        "label": "normal",
        "sequence": "42",
        "timestamp_us": "654321",
        "flags": "9",
        "accel_x": "1",
        "accel_y": "-2",
        "accel_z": "3",
        "temperature_raw": "4",
        "gyro_x": "-5",
        "gyro_y": "6",
        "gyro_z": "-7",
    }
    if rows[0] != expected:
        raise AssertionError(f"ZLOG converter mismatch: {rows[0]}")


def main() -> int:
    step_root = Path(__file__).resolve().parents[1]
    test_python_syntax(step_root)
    with tempfile.TemporaryDirectory(prefix="zmpio_step1_") as directory:
        temporary = Path(directory)
        test_golden(step_root, temporary)
        test_zlog_converter(step_root, temporary)
    print("Step1 Python tool smoke tests PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
