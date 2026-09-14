# ZMPIO Guard V3

**v1.0.0** — Vibration monitoring system on a Xilinx Zynq-7020 SoC, built as an
asymmetric multiprocessing (AMP) design across two Cortex-A9 cores and the FPGA
fabric.

MPU6050 accelerometer samples are acquired by a FreeRTOS core, filtered and
transformed by a DSP pipeline running in programmable logic, reduced to a
10-element feature frame, checked against versioned threshold rules, logged to a
microSD card, and streamed to a Linux core over shared DDR.

---

## 1. Architecture at a glance

```text
                    Zynq-7020
  ┌──────────────────────────────┬──────────────────────────────┐
  │ CPU0  Cortex-A9 #0           │ CPU1  Cortex-A9 #1           │
  │ Linux (PetaLinux)            │ FreeRTOS                     │
  │  • zmpiod    feature capture │  • MPU6050 over I2C          │
  │  • zmpioctl  control socket  │  • microSD over SPI + ZLOG   │
  │  • libzmpio  UIO + ring RX   │  • threshold anomaly rules   │
  │                              │  • DSP control over MMIO/IRQ │
  └──────────────┬───────────────┴───────────────┬──────────────┘
                 │      shared DDR (two rings)   │
                 │  ABI v2 data    0x1900_0000   │
                 │  ABI v3 control 0x1904_0000   │
                 └───────────────┬───────────────┘
                                 │ AXI-Lite + SPI 62/63
  ┌──────────────────────────────┴──────────────────────────────┐
  │ PL fabric — DSP shell                                        │
  │  FIR3 → detrend → Hann window → 128-point real FFT →         │
  │  feature extraction → 64-deep feature FIFO → doorbell IRQ    │
  └──────────────────────────────────────────────────────────────┘
```

Peripheral ownership is exclusive by design: **only CPU1 touches the MPU6050 (I2C)
and the microSD card (SPI)**, and **only one Linux process reads the ABI v2 data
ring**. See `docs/SAD.md`.

## 2. Repository layout

| Path | Contents |
|---|---|
| `common/` | ABI headers shared by CPU0, CPU1 and Linux — wire structs, ring layout, ZLOG container, CRC32 |
| `firmware/app_freertos/` | CPU1 FreeRTOS application: sensor, storage, IPC, DSP result tasks |
| `firmware/cpu0_application/` | CPU0 bare-metal test controller: Ethernet/lwIP bring-up, CPU1 start, IPC console |
| `firmware/platform_dual/` | Vitis platform shared by both cores (hardware definition, PS init) |
| `hardware/rtl/` | PL sources: Verilog, XDC constraints, HLS wrapper, packaged IP, Vivado Tcl |
| `hardware/vivado/project_hub/` | The Vivado project itself |
| `software/linux/` | `libzmpio`, `zmpiod`, `zmpioctl`, host tests |
| `dsp_host_sim/` | Bit-accurate host model of the PL DSP pipeline, golden vectors, regression harness |
| `deploy/petalinux_overlay/` | Device tree overlay, udev rules, sysvinit scripts, image packaging |
| `scripts/` | Build entry points and the path loaders |
| `tools/` | Manifest generation, ABI layout hash, fault injection, soak and cold-boot checks |
| `docs/` | Design documentation — see `docs/README.md` |
| `config/` | `toolchain.env`, the only place absolute paths live — see `config/README.md` |

## 3. Prerequisites

| Tool | Version used | Needed for |
|---|---|---|
| Xilinx Vivado | 2023.2 | PL bitstream, block design, IP packaging |
| Xilinx Vitis Unified | 2023.2 | CPU0 and CPU1 firmware |
| Vitis HLS | 2023.2 | Regenerating the DSP core IP from C++ |
| PetaLinux | 2023.2 | Linux image for CPU0 |
| CMake | ≥ 3.16 | Linux user space and the host DSP simulation |
| Python | ≥ 3.8 | Build drivers and tooling |

Point `config/toolchain.env` at your installs before the first build. Nothing else
in the tree needs editing.

## 4. Building

```powershell
# Host-only: DSP model regression, no board or Xilinx tools required
dsp_host_sim\BUILD_STEP.cmd
dsp_host_sim\VERIFY_STEP.cmd

# CPU1 FreeRTOS firmware  ->  output/cpu1/app_freertos.elf
scripts\build_cpu1.bat

# CPU0 bare-metal controller  ->  output/cpu0/cpu0_application.elf
scripts\build_cpu0.bat

# Linux user space (cross build for the target)
cmake -B build -S software/linux -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc
cmake --build build
```

The PL bitstream is rebuilt from the Vivado project:

```powershell
vivado -mode batch -source hardware\rtl\bd\run_step2_vivado.tcl
```

## 5. Status

This section states what has actually been demonstrated, and on what.

**Verified on real hardware**

| Subsystem | Evidence |
|---|---|
| CPU0 bring-up over JTAG, CPU1 FreeRTOS start | Board |
| MPU6050 acquisition, microSD logging, ZLOG container | Board |
| Shared-DDR IPC, ABI v2 data ring and ABI v3 control block | Board, including a 1-hour soak |
| PL DSP shell: live feature production, zero drops | Board |
| PL → CPU1 doorbell interrupt | Board, 10 000 / 10 000 events, no lost wakeups |
| Threshold anomaly rules, health records, fault injection | Board, 4 / 4 fault-injection gates |
| Ethernet GEM0 + RTL8201F, lwIP, DHCP, ICMP and DNS to the Internet | Board |
| Feature stream CPU1 → Linux, captured by `zmpiod` | Board, ~25 min continuous, no drops or gaps |
| Host DSP model vs. golden vectors | CI-style regression, reproducible offline |

**Not demonstrated — do not assume these**

- **No long-duration soak.** The longest continuous run is roughly one hour.
  Nothing here establishes behaviour over days.
- **Anomaly thresholds are narrowly grounded.** They were derived from a single
  labelled acquisition session using manual excitation, not from a catalogue of
  real mechanical fault modes. Treat detection capability claims accordingly.
- **MPU6050 power cycling is compiled out.** The GPIO power-cut path exists in
  `mpu6050_power.c` but is disabled: the board has no P-MOSFET fitted and the
  block design exposes no `MPU_PWR_N` port.
- **Large-scale acceptance was deliberately deferred**, including cold-boot
  repetition, byte-for-byte SD ↔ host reconciliation, and full hardware-in-the-loop
  fault sweeps. Tooling for all of these is in `tools/`; what is missing is board
  time, not code.

Per the project's own architecture decision on evidence, **V3 is not declared
complete**. It is released at the level that has been proven.

## 6. Documentation

Start at [`docs/README.md`](docs/README.md). The set is three documents: the System
Architecture Document (`docs/SAD.md`), the register/memory-map/wire-protocol
reference (`docs/ARCHITECTURE_DETAIL.md`), and one detailed design document per
component (`docs/SDD/`).

## 7. License

MIT — see [`LICENSE`](LICENSE).
