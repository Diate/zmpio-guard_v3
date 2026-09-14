# Building the CPU1 FreeRTOS ELF on Windows

Run `build_cpu1.bat` from this directory. It drives the Vitis Unified 2023.2
workspace under `firmware/`; it never creates a second platform or a second
CPU1 application.

Project configuration:

| Item | Value |
|---|---|
| Platform component | `platform_dual` |
| CPU1 domain | `free_rtos` on `ps7_cortexa9_1` |
| CPU1 application | `app_freertos` |
| Hardware input | `hardware/vivado/project_hub/design_1_wrapper.xsa` |

`platform.update_hw()` names the imported copy after the **basename of the
source XSA**, so the name tracks whatever the Vivado top-level wrapper is
currently called.

All paths are resolved from the script's own location through
`scripts/project_paths.bat` and `config/toolchain.env`; nothing needs editing
for a fresh clone.

## Default (full) flow

```text
new XSA
  -> platform_dual.update_hw()
  -> ensure libmetal + openamp in free_rtos
  -> platform_dual.build()
  -> free_rtos.regenerate()
  -> app_freertos.build()
  -> output/cpu1/app_freertos.elf
```

Close any Vitis UI holding this workspace before a full build, so two Vitis
processes cannot update the same component metadata and CMake build directory
at once.

## Commands

```bat
rem Full rebuild with the XSA currently stored in the platform
build_cpu1.bat

rem Full rebuild with a newly exported XSA
build_cpu1.bat --xsa "<dir>\new_design.xsa"

rem Fast mode: app_freertos/src only, no XSA / platform / BSP update
build_cpu1.bat --app-only

rem Recreate the generated CPU1 CMake build directory first
build_cpu1.bat --clean

rem Validate paths and print the selected flow without building
build_cpu1.bat --dry-run

rem Write the ELF somewhere else
build_cpu1.bat --output "<dir>"
```

The script verifies that the artefact really is an ARM ELF, copies it to the
output directory, and writes `app_freertos.elf.sha256` beside it.

## What the script repairs automatically

**Stale metadata paths.** Vitis records absolute paths in `vitis-comp.json` and
`src/app.yaml` — the platform XSA, the exported `.xpfm`, the domain path, and a
QEMU resource directory per domain. They point at wherever the tree lived when
Vitis last wrote them, which is never right after a clone. Before opening the
workspace, the script re-roots every one of them onto the current checkout
(`scripts/vitis_metadata.py`) and invalidates the CPU1 CMake cache when that
repair, or a full XSA rebuild, requires it.

**FreeRTOS domain libraries.** It keeps `libmetal` and `openamp` in the domain's
library list, which the CPU1 sources require.

**OpenAMP library naming.** Vitis 2023.2 emits `libopen_amp.a` while its
application builder still links `-lopenamp`. The script creates a compatible
`libopenamp.a` alias in the generated domain output before calling
`app.build()`, so CPU1 cannot silently link against an incomplete BSP after an
XSA change.

**CPU1 timer setup.** The application corrects the Vitis 2023.2 SDT `xiltimer`
SCU reload calculation: it programs the CPU1 private timer from
`XPAR_CPU_CORE_CLOCK_FREQ_HZ / 2` after the scheduler starts, so the fix
survives both app-only and full platform rebuilds. Timestamps come from the
shared Cortex-A9 global timer rather than the wrapping TTC counter; the
application reads and applies the existing global-timer prescaler and never
clears the control value it shares with Linux.
