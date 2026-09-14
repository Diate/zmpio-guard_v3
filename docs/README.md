# ZMPIO Guard V3 — Design Documentation

This set covers the system in three parts:

| Document | Answers |
|---|---|
| [`SAD.md`](SAD.md) | What is the system, what are its major parts, how do they interact, why was this architecture chosen (includes the full Architecture Decision Record log), what are the risks |
| [`ARCHITECTURE_DETAIL.md`](ARCHITECTURE_DETAIL.md) | The interface catalog, the DDR/MMIO memory map, FreeRTOS and PL resource budgets, and the bit-exact wire format of every message, register, and file format |
| [`SDD/`](SDD/) | One detailed design document per component (`SDD_01`..`SDD_14`): responsibilities, module/file structure, function contracts, state machines, algorithms, error handling, concurrency, timing, and known limitations |

## Reading order

- New to the project: `SAD.md` end to end, then the `SDD/` file for the component you are changing.
- Changing a wire format or a register: `ARCHITECTURE_DETAIL.md` first, then the owning `SDD/` file.
- Understanding a design trade-off: `SAD.md` §18 (Architecture Decisions).

## Component index

| SDD | Component | Domain |
|---|---|---|
| [`SDD_01_CPU1_RUNTIME.md`](SDD/SDD_01_CPU1_RUNTIME.md) | CPU1 runtime (scheduler, tasks, timing) | CPU1 |
| [`SDD_02_SENSOR_MPU6050.md`](SDD/SDD_02_SENSOR_MPU6050.md) | MPU6050 sensor over I2C | CPU1 |
| [`SDD_03_STORAGE_SDSPI_ZLOG.md`](SDD/SDD_03_STORAGE_SDSPI_ZLOG.md) | Storage: SD-SPI, FatFs, ZLOG | CPU1 |
| [`SDD_04_IPC_ABI_V2.md`](SDD/SDD_04_IPC_ABI_V2.md) | IPC ABI v2 data ring | CPU0+CPU1+Linux |
| [`SDD_05_IPC_ABI_V3.md`](SDD/SDD_05_IPC_ABI_V3.md) | IPC ABI v3 command/response | CPU0+CPU1 |
| [`SDD_06_PL_DSP_SHELL.md`](SDD/SDD_06_PL_DSP_SHELL.md) | PL DSP shell (bridge, FIFO, control, reset) | PL |
| [`SDD_07_DSP_CORE_ALGORITHMS.md`](SDD/SDD_07_DSP_CORE_ALGORITHMS.md) | DSP core and algorithms | PL (HLS) |
| [`SDD_08_FPGA_DSP_HAL.md`](SDD/SDD_08_FPGA_DSP_HAL.md) | FPGA DSP HAL and GIC discipline | CPU1 |
| [`SDD_09_ANOMALY_RULE_HEALTH.md`](SDD/SDD_09_ANOMALY_RULE_HEALTH.md) | Anomaly rule and DSP health | CPU1 |
| [`SDD_10_DOORBELL.md`](SDD/SDD_10_DOORBELL.md) | One-way PL doorbell | PL+CPU0+CPU1 |
| [`SDD_11_CPU0_CONTROLLER.md`](SDD/SDD_11_CPU0_CONTROLLER.md) | CPU0 test controller | CPU0 |
| [`SDD_12_ETHERNET_LWIP.md`](SDD/SDD_12_ETHERNET_LWIP.md) | Ethernet GEM0, PHY, lwIP | CPU0 |
| [`SDD_13_LINUX_CLIENT_STACK.md`](SDD/SDD_13_LINUX_CLIENT_STACK.md) | Linux client stack (`libzmpio`/`zmpiod`/`zmpioctl`) | Linux |
| [`SDD_14_FEATURE_STREAM.md`](SDD/SDD_14_FEATURE_STREAM.md) | Feature stream, CPU1 -> Linux | CPU1+Linux |

## Maintenance rule

When code and documentation disagree, treat the discrepancy as a design issue to resolve explicitly
— either the code is wrong, or the design changed and the document must be updated. Do not silently
edit one to match the other without deciding which is correct.
