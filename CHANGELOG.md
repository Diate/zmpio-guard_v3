# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-09-12

First public release of the ZMPIO Guard V3 source tree.

### Added

- **CPU1 FreeRTOS application** — MPU6050 acquisition over I2C, microSD logging
  over SPI with the ZLOG container format, threshold anomaly rules, DSP result
  handling, UART1 debug channel, and I2C bus recovery.
- **CPU0 bare-metal controller** — CPU1 startup, Ethernet GEM0 / RTL8201F
  bring-up with lwIP, DHCP, ICMP and DNS, and an IPC console.
- **PL DSP shell** — FIR3, detrend, Hann window, 128-point real FFT and feature
  extraction in programmable logic, packaged as IP-XACT, with an MMIO control
  register block and a doorbell interrupt to both cores.
- **Shared-DDR IPC** — ABI v2 data ring at `0x1900_0000` and ABI v3
  command/response control block at `0x1904_0000`, each with its own versioned,
  hash-pinned layout.
- **Linux user space** — `libzmpio` (UIO mapping, ring consumption),
  `zmpiod` (feature capture to ZLOG, control socket), and `zmpioctl`.
- **Feature stream** — CPU1 to Linux over the ABI v2 ring, with bounded-wait
  sends and explicit drop accounting.
- **Host DSP simulation** — bit-accurate model of the PL pipeline with golden
  vectors and a regression harness that runs without a board.
- **PetaLinux overlay** — device tree, UIO udev rules, sysvinit supervision for
  `zmpiod`, and image packaging.
- **Tooling** — release manifest generation, ABI layout hash checking, fault
  injection, soak reporting, and cold-boot verification.
- **Documentation** — requirements, system architecture, architecture decision
  records, interfaces, memory and resource map, protocols, fifteen component
  design documents, and a debug playbook.

### Notes

- Anomaly detection is rule based. No machine learning runs anywhere in V3, by
  architectural decision.
- `LOG_SOURCE_ANOMALY_ML_V1 = 6` is retained as a **reserved** log source value
  with no producer, so that previously recorded captures stay self-describing.

[1.0.0]: https://example.invalid/zmpio-guard/releases/tag/v1.0.0
