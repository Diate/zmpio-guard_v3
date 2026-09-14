#!/usr/bin/env python3
"""Prove that a comment-only edit changed no code.

Strips comments and normalises whitespace on both sides of an edit, then
compares what is left byte for byte. Any difference in the remaining code is a
failure: the comment pass must never alter behaviour.

Usage:
    python tools/verify_code_unchanged.py <reference-tree> [paths...]

``<reference-tree>`` is the tree to compare against -- normally a pristine copy
of the sources taken before the comment pass. ``paths`` are repository-relative
files or directories to check; with none given, every C, C++ and header file in
the repository is checked.

Exit status is 0 when every file's code is identical, 1 otherwise.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
C_SUFFIXES = {".c", ".h", ".cpp", ".hpp", ".cc"}


def strip_c_comments(text: str) -> str:
    """Remove /* */ and // comments without touching string or char literals."""

    out = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]

        # String literal
        if ch == '"':
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                elif text[i] == '"':
                    i += 1
                    break
                i += 1
            continue

        # Character literal
        if ch == "'":
            out.append(ch)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\":
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                elif text[i] == "'":
                    i += 1
                    break
                i += 1
            continue

        # Block comment
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = n if end == -1 else end + 2
            out.append(" ")
            continue

        # Line comment
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            end = text.find("\n", i)
            i = n if end == -1 else end
            out.append(" ")
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def normalised_code(path: Path) -> str:
    """Comment-free, whitespace-normalised code of one file."""

    text = path.read_text(encoding="utf-8", errors="replace")
    stripped = strip_c_comments(text)
    lines = (" ".join(line.split()) for line in stripped.splitlines())
    return "\n".join(line for line in lines if line)


def iter_targets(paths: list[str]) -> list[Path]:
    if not paths:
        roots = [REPO_ROOT]
    else:
        roots = [REPO_ROOT / p for p in paths]

    found: list[Path] = []
    for root in roots:
        if root.is_file():
            if root.suffix in C_SUFFIXES:
                found.append(root)
        elif root.is_dir():
            for suffix in sorted(C_SUFFIXES):
                found.extend(
                    p for p in root.rglob("*" + suffix) if ".git" not in p.parts
                )
    return sorted(set(found))


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2

    reference = Path(argv[1]).resolve()
    if not reference.is_dir():
        print("reference tree not found: {0}".format(reference))
        return 2

    targets = iter_targets(argv[2:])
    if not targets:
        print("no C/C++ sources matched")
        return 2

    checked = 0
    missing = 0
    failures = []
    for path in targets:
        relative = path.relative_to(REPO_ROOT)
        original = reference / relative
        if not original.is_file():
            missing += 1
            continue
        checked += 1
        if normalised_code(path) != normalised_code(original):
            failures.append(relative)

    for relative in failures:
        print("CODE CHANGED: {0}".format(relative.as_posix()))

    print(
        "checked {0} file(s), {1} not in reference, {2} with changed code".format(
            checked, missing, len(failures)
        )
    )
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
