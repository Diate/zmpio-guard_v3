# Staging build artefacts for PetaLinux

Two scripts move the Windows-side build output to the PetaLinux build VM.

## 1. On Windows — stage into the share

```bat
scripts\stage_fast_package_inputs.bat
```

It cross-compiles `zmpiod` and `zmpioctl` with the Vitis ARM Linux compiler
(`ARM_LINUX_CC` from `config/toolchain.env`), then writes these files into
`WINDOWS_SHARE_DIR`, replacing any file of the same name:

| File | Source |
|---|---|
| `app_freertos.elf` | `output/cpu1/` |
| `cpu1_bitstream.bit` | `firmware/app_freertos/_ide/bitstream/cpu1_wrapper.bit` |
| `cpu1_wrapper.xsa` | `firmware/platform_dual/hw/design_1_wrapper.xsa` |
| `system-user-openamp-template.dtsi` | `deploy/petalinux_overlay/` |
| `zmpio-enable-cpu1-clocks` | `deploy/petalinux_overlay/` |
| `zmpio-clock-init.service` | `deploy/petalinux_overlay/` |
| `zmpiod`, `zmpioctl` | built here |
| `zmpiod.init`, `zmpiod-supervise` | `deploy/petalinux_overlay/` |
| `zmpio-uio.rules` | `deploy/petalinux_overlay/` |
| `script1_fast_package.sh` | `deploy/petalinux_overlay/` |
| `sync_share_to_petalinux.sh` | `scripts/` |

The destination name `cpu1_wrapper.xsa` is fixed because the PetaLinux side
consumes that exact filename; the *content* is the current
`design_1_wrapper.xsa`, so renaming either side alone would break the pair.

Options:

- `--dry-run` — validate paths and print what would be staged.
- `--stage-only` — skip the cross-compile and stage the binaries already in
  `output/linux/`.

## 2. On the Linux build VM — pull from the share

```bash
./sync_share_to_petalinux.sh
```

It reads the staged artefacts from `/media/sf_share` and refreshes the same
filenames under the PetaLinux share directory. Override the source when the
share is mounted elsewhere:

```bash
SOURCE_SHARE_DIR=/path/to/share ./sync_share_to_petalinux.sh
```

This script also has `--dry-run`. It strips CR from every text artefact, so a
Windows checkout with `core.autocrlf=true` cannot break shell parsing on the
target.

## 3. CPU1 clock enablement

`system-user-openamp-template.dtsi` keeps the CPU1 peripheral clocks alive via
the `clk_ignore_unused` boot argument. The two `zmpio-*` files are the rootfs
fallback: at boot they set `SPI_CLK_CTRL.CLKACT0` and
`APER_CLK_CTRL.SPI0_CPU_1XCLKACT`, which covers both a full image build and a
fast package that reuses an existing `image.ub`.

After booting a new image, verify on the target:

```sh
cat /proc/cmdline
/usr/sbin/zmpio-enable-cpu1-clocks
devmem 0xF8000158 32     # SPI_CLK_CTRL   -> expect CLKACT0 set
devmem 0xF800012C 32     # APER_CLK_CTRL  -> expect SPI0_CPU_1XCLKACT set
```

On the current design these typically read `0x00001201` and `0x00104400`.
