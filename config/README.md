# Build configuration

`toolchain.env` is the **only** file in this tree that may contain an absolute
path. Everything inside the repository is addressed relatively, derived from the
calling script's own location, so the tree can be cloned or moved anywhere.

## Settings

| Key | Used by | Meaning |
|---|---|---|
| `VITIS_ROOT` | `scripts/build_cpu0.*`, `scripts/build_cpu1.*` | Vitis Unified install directory. The build scripts run under `vitis -s`, which is launched from here. |
| `VIVADO_ROOT` | `hardware/rtl/bd/*.tcl` | Vivado install directory, for batch synthesis and bitstream generation. |
| `ARM_LINUX_CC` | `software/linux/BUILD_STEP.ps1`, `scripts/stage_fast_package_inputs.bat` | Cross compiler for the PetaLinux user space (`libzmpio`, `zmpiod`, `zmpioctl`). |
| `WINDOWS_SHARE_DIR` | `scripts/stage_fast_package_inputs.bat` | Staging directory shared between the Windows host and the PetaLinux build VM. |
| `PETALINUX_PROJECT_DIR` | `scripts/sync_share_to_petalinux.sh`, `deploy/petalinux_overlay/script1_fast_package.sh` | PetaLinux project root on the build VM. |
| `TARGET_HOST` | `tools/cold_boot_check.sh`, `tools/soak_report.py` | SSH destination of the board, `user@host`. |

## Precedence

An environment variable that is already set always wins over the value in
`toolchain.env`. Nothing needs to be edited here for CI or a one-off override:

```powershell
$env:VITIS_ROOT = 'D:\Xilinx\Vitis\2024.1'
.\scripts\build_cpu1.bat
```

## Loaders

Each language reads the same file and resolves the repository root from its own
script location:

| Language | Loader | Usage |
|---|---|---|
| Python | `scripts/project_paths.py` | `from project_paths import REPO_ROOT, tool_path` |
| POSIX shell | `scripts/project_paths.sh` | `. "$(dirname "$0")/project_paths.sh"` |
| Batch | `scripts/project_paths.bat` | `call "%~dp0project_paths.bat"` |
| PowerShell | `scripts/ProjectPaths.psm1` | `Import-Module .../ProjectPaths.psm1; Get-ZmpioPaths` |
| Tcl | `hardware/rtl/bd/project_paths.tcl` | `source .../project_paths.tcl` then `$zmpio(rtl_dir)` |

Run any loader directly to print what it resolved:

```powershell
python scripts\project_paths.py
```
