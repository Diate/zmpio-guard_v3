#!/bin/sh
# Repository layout and external toolchain locations, for POSIX shell callers.
#
# Source it, do not execute it:
#     . "$(dirname "$0")/project_paths.sh"
#
# Every in-tree path is derived from this file's own location. Only
# config/toolchain.env holds absolute paths, and a variable that is already
# exported always wins over the value in that file.

_pp_self_dir=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
if [ -z "$_pp_self_dir" ]; then
    _pp_self_dir=$(pwd)
fi

# When sourced from scripts/, the root is one level up. When sourced from a
# deeper directory (deploy/, tools/), walk up until config/toolchain.env shows.
REPO_ROOT=$_pp_self_dir
while [ "$REPO_ROOT" != "/" ] && [ ! -f "$REPO_ROOT/config/toolchain.env" ]; do
    REPO_ROOT=$(dirname "$REPO_ROOT")
done

CONFIG_DIR="$REPO_ROOT/config"
TOOLCHAIN_ENV="$CONFIG_DIR/toolchain.env"

COMMON_DIR="$REPO_ROOT/common"
DOCS_DIR="$REPO_ROOT/docs"
FIRMWARE_DIR="$REPO_ROOT/firmware"
HARDWARE_DIR="$REPO_ROOT/hardware"
RTL_DIR="$HARDWARE_DIR/rtl"
VIVADO_PROJECT_DIR="$HARDWARE_DIR/vivado/project_hub"
LINUX_SW_DIR="$REPO_ROOT/software/linux"
DSP_HOST_SIM_DIR="$REPO_ROOT/dsp_host_sim"
DEPLOY_DIR="$REPO_ROOT/deploy/petalinux_overlay"
SCRIPTS_DIR="$REPO_ROOT/scripts"
TOOLS_DIR="$REPO_ROOT/tools"
ARTIFACTS_DIR="$REPO_ROOT/artifacts"
OUTPUT_DIR="$REPO_ROOT/output"

export REPO_ROOT CONFIG_DIR TOOLCHAIN_ENV COMMON_DIR DOCS_DIR FIRMWARE_DIR
export HARDWARE_DIR RTL_DIR VIVADO_PROJECT_DIR LINUX_SW_DIR DSP_HOST_SIM_DIR
export DEPLOY_DIR SCRIPTS_DIR TOOLS_DIR ARTIFACTS_DIR OUTPUT_DIR

# Load toolchain.env without clobbering anything already in the environment.
if [ -f "$TOOLCHAIN_ENV" ]; then
    while IFS='=' read -r _pp_key _pp_val; do
        case "$_pp_key" in
            ''|\#*) continue ;;
        esac
        _pp_key=$(printf '%s' "$_pp_key" | tr -d ' \t\r')
        _pp_val=$(printf '%s' "$_pp_val" | sed 's/[[:space:]]*$//; s/\r$//')
        [ -z "$_pp_key" ] && continue
        eval "_pp_cur=\${$_pp_key:-}"
        if [ -z "$_pp_cur" ]; then
            eval "$_pp_key=\$_pp_val"
            export "$_pp_key"
        fi
    done < "$TOOLCHAIN_ENV"
fi
unset _pp_self_dir _pp_key _pp_val _pp_cur
