#!/usr/bin/env python3
"""Repository layout and external toolchain locations, for Python callers.

Every in-tree path is derived from this file's own location, so the tree can
be cloned or moved anywhere. Only config/toolchain.env holds absolute paths,
and an already-set environment variable always overrides it.
"""

from __future__ import print_function

import os
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

CONFIG_DIR = REPO_ROOT / "config"
TOOLCHAIN_ENV = CONFIG_DIR / "toolchain.env"

COMMON_DIR = REPO_ROOT / "common"
DOCS_DIR = REPO_ROOT / "docs"
FIRMWARE_DIR = REPO_ROOT / "firmware"
CPU0_APP_DIR = FIRMWARE_DIR / "cpu0_application"
CPU1_APP_DIR = FIRMWARE_DIR / "app_freertos"
PLATFORM_DIR = FIRMWARE_DIR / "platform_dual"
HARDWARE_DIR = REPO_ROOT / "hardware"
RTL_DIR = HARDWARE_DIR / "rtl"
VIVADO_PROJECT_DIR = HARDWARE_DIR / "vivado" / "project_hub"
LINUX_SW_DIR = REPO_ROOT / "software" / "linux"
DSP_HOST_SIM_DIR = REPO_ROOT / "dsp_host_sim"
DEPLOY_DIR = REPO_ROOT / "deploy" / "petalinux_overlay"
SCRIPTS_DIR = REPO_ROOT / "scripts"
TOOLS_DIR = REPO_ROOT / "tools"
ARTIFACTS_DIR = REPO_ROOT / "artifacts"
OUTPUT_DIR = REPO_ROOT / "output"

VIVADO_XSA = VIVADO_PROJECT_DIR / "design_1_wrapper.xsa"
VIVADO_BIT = VIVADO_PROJECT_DIR / "project_hub.runs" / "impl_1" / "design_1_wrapper.bit"


def load_toolchain_env(path=TOOLCHAIN_ENV):
    """Parse config/toolchain.env into a dict. Missing file yields {}."""
    values = {}
    if not Path(path).is_file():
        return values
    with open(str(path), "r") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, value = line.partition("=")
            values[key.strip()] = value.strip()
    return values


def tool_path(name, default=None):
    """Resolve one toolchain setting: environment first, then the env file."""
    if os.environ.get(name):
        return os.environ[name]
    return load_toolchain_env().get(name, default)


if __name__ == "__main__":
    print("REPO_ROOT={0}".format(REPO_ROOT))
    for key, value in sorted(load_toolchain_env().items()):
        print("{0}={1}".format(key, tool_path(key, value)))
