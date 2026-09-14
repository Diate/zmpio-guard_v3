#!/usr/bin/env python3
"""Re-root the absolute paths Vitis stores in component metadata.

Vitis records absolute paths in ``vitis-comp.json`` and ``src/app.yaml``: the
platform XSA, the exported ``.xpfm``, one QEMU resource directory per domain,
the domain path an application builds against, and the template directory the
application was created from. Those paths point at wherever the tree lived when
Vitis last wrote them, so after a clone or a move every one of them is wrong.

Two roots are recognised:

* ``<repo>/firmware`` -- anything containing a ``firmware`` path segment is
  re-rooted onto this checkout's workspace directory.
* the Vitis installation -- anything containing a ``Vitis`` segment followed by
  a version is re-rooted onto the active Vitis root.

The published tree ships these files with ``<ZMPIO_ROOT>`` and
``<VITIS_ROOT_PARENT>`` placeholders, which the same rules resolve on the first
build. Both build scripts call :func:`repair_component_metadata` before opening
the workspace, so Vitis never sees a stale path.
"""

from __future__ import print_function

import json
import os
from pathlib import Path

WORKSPACE_DIR_NAME = "firmware"
VITIS_DIR_NAME = "vitis"


def _split(value):
    return [part for part in value.replace("\\", "/").split("/") if part]


def _reroot_at(value, segment, new_root, keep_segment=False):
    """Re-root ``value`` at the last ``segment`` onto ``new_root``.

    Returns ``(value, changed)``. ``keep_segment`` keeps the matched segment and
    everything after it, which is what the Vitis root needs (the root is the
    parent of ``Vitis``).
    """

    if not isinstance(value, str) or not value.strip() or new_root is None:
        return value, False

    parts = _split(value)
    lowered = [part.lower() for part in parts]
    if segment not in lowered:
        return value, False

    index = len(lowered) - 1 - lowered[::-1].index(segment)
    tail = parts[index:] if keep_segment else parts[index + 1:]
    rerooted = str(Path(new_root).joinpath(*tail)) if tail else str(Path(new_root))
    return (rerooted, True) if rerooted != value else (value, False)


def reroot_path(value, workspace, vitis_root_parent=None):
    """Apply both re-rooting rules to one string."""

    value, changed = _reroot_at(value, WORKSPACE_DIR_NAME, workspace)
    if changed:
        return value, True
    return _reroot_at(value, VITIS_DIR_NAME, vitis_root_parent, keep_segment=True)


def reroot_tree(value, workspace, vitis_root_parent=None):
    """Apply :func:`reroot_path` across a nested JSON structure."""

    if isinstance(value, dict):
        changed = False
        for key, child in value.items():
            rerooted, child_changed = reroot_tree(child, workspace, vitis_root_parent)
            if child_changed:
                value[key] = rerooted
                changed = True
        return value, changed

    if isinstance(value, list):
        changed = False
        result = []
        for child in value:
            rerooted, child_changed = reroot_tree(child, workspace, vitis_root_parent)
            result.append(rerooted)
            changed = changed or child_changed
        return result, changed

    return reroot_path(value, workspace, vitis_root_parent)


def resolve_vitis_root_parent():
    """Directory holding the ``Vitis`` install tree, or None if unknown.

    ``VITIS_ROOT`` points at the versioned install (``.../Xilinx/Vitis/2023.2``)
    while the metadata stores ``<parent>/Vitis/<version>/...``, so walk up to the
    directory named ``Vitis`` and return its parent.
    """

    for name in ("XILINX_VITIS", "VITIS_ROOT"):
        value = os.environ.get(name, "").strip()
        if not value:
            continue
        current = Path(value)
        while current.name and current.name.lower() != VITIS_DIR_NAME:
            parent = current.parent
            if parent == current:
                return None
            current = parent
        return str(current.parent) if current.name else None
    return None


def repair_json_metadata(path, workspace, vitis_root_parent=None):
    """Re-root every path in one ``vitis-comp.json``. Returns True if changed."""

    path = Path(path)
    if not path.is_file():
        return False
    data = json.loads(path.read_text(encoding="utf-8"))
    data, changed = reroot_tree(data, workspace, vitis_root_parent)
    if changed:
        path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return changed


def repair_app_yaml(path, workspace, vitis_root_parent=None):
    """Re-root ``domain_path`` and ``app_src_dir``. Returns True if changed."""

    path = Path(path)
    if not path.is_file():
        return False
    changed = False
    lines = []
    for line in path.read_text(encoding="utf-8").splitlines():
        key, sep, value = line.partition(":")
        if sep and key.strip() in ("domain_path", "app_src_dir"):
            rerooted, line_changed = reroot_path(
                value.strip(), workspace, vitis_root_parent
            )
            if line_changed:
                line = "{0}: {1}".format(key.strip(), rerooted)
                changed = True
        lines.append(line)
    if changed:
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return changed


def repair_component_metadata(workspace, components):
    """Repair the platform and every application component under ``workspace``.

    ``components`` is an iterable of component directory names. Returns True if
    anything was rewritten.
    """

    workspace = Path(workspace)
    vitis_root_parent = resolve_vitis_root_parent()
    changed = False
    for name in components:
        component = workspace / name
        changed |= repair_json_metadata(
            component / "vitis-comp.json", workspace, vitis_root_parent
        )
        changed |= repair_app_yaml(
            component / "src" / "app.yaml", workspace, vitis_root_parent
        )
    return changed


if __name__ == "__main__":
    repo_root = Path(__file__).resolve().parents[1]
    ws = repo_root / "firmware"
    touched = repair_component_metadata(
        ws, ("platform_dual", "app_freertos", "cpu0_application")
    )
    print("metadata {0}".format("repaired" if touched else "already correct"))
