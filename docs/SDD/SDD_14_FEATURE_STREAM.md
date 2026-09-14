# SDD_14 — Feature stream, CPU1 -> Linux (CMP-STR-001)

**Document ID:** ZMPIO-SDD-14
**Component:** CMP-STR-001
**Level:** L2
**Source:** `common/zmpio_protocol.h` (wire contract), `firmware/app_freertos/src/feature_stream.{c,h}`,
`firmware/app_freertos/src/zmpio_ipc_logic.c` (`ipc_send_timeout()`),
`software/linux/libzmpio/src/zmpio_v2.c` (drain + demux),
`software/linux/zmpiod/zmpio_capture.{c,h}` (ZLOG capture)
**Requirement:** REQ-STR-001..005, REQ-OPS-002
**Decision:** ADR-018 (transport), ADR-017 (no inference on the receiving end)

---

## 1. Purpose

This component is **the data path V3 exists to prove**: vibration -> PL DSP -> CPU1 -> Linux.

Before Step 7, `feature_frame_v2` only went out to the microSD card, which CPU1 owns exclusively.
Linux had no way to read it — `msg_type_t` had no FEATURE variant, and ABI v3 was only a 16-slot x
64 B command/response channel. The line in the old roadmap ("`zmpio_ml` consumes the
`feature_frame_v2` that `zmpiod` drains via UIO") described something that had never been
implemented.

CMP-STR-001 is that thing, and only that thing: a one-way telemetry stream from CPU1 to Linux, with
evidence of no frame loss. There is no inference at all on the receiving end (ADR-017).

## 2. Responsibility

### MUST

- Publish each feature frame together with its own rule verdict **after** the microSD record has
  already been queued — the on-card copy is the archival record and must not depend on whether the
  stream succeeds.
- Never block `fpga_result_task`: every publish has a deadline (`ipc_send_timeout()`); on expiry the
  frame is dropped and counted.
- Carry loss information within the stream itself (`dropped_since_last`) — REQ-STR-001.
- Ring the doorbell after every successful publish, so the Linux side wakes on an IRQ rather than on
  a timer — REQ-STR-003.
- Emit `DSP_HEALTH` at exactly the moments CPU1 writes a DSP_HEALTH record to the card, so every
  jump in `frame_sequence` caused by the PL has an explanation in the stream.
- On the Linux side: a single drain loop, demultiplexed by type, where no function advances `tail`
  and then discards a message it does not care about — REQ-STR-002.
- Reject (and count) any message with a bad `magic` or a `length` that does not exactly match the
  corresponding struct's size.

### MUST NOT

- MUST NOT create a new ring inside the ABI v3 region (this would change `layout_hash` and
  invalidate the Step 4/5 evidence — ADR-018).
- MUST NOT use `portMAX_DELAY` anywhere on the stream path.
- MUST NOT place the 256 B message buffer on `fpga_result_task`'s stack (see §5).
- MUST NOT decode a message whose size is "approximately" right — that is ABI drift and must fail
  loudly.

## 3. Wire contract (`common/zmpio_protocol.h`)

| Message | Value | Direction | Payload | Size |
|---|---|---|---|---|
| `MSG_TYPE_FEATURE_V2` | `0x30` | CPU1 -> Linux, unsolicited | `ipc_feature_stream_v1_t` | 80 B |
| `MSG_TYPE_DSP_HEALTH` | `0x31` | CPU1 -> Linux, on state change + heartbeat only | `ipc_dsp_health_stream_v1_t` | 36 B |
| `MSG_TYPE_STREAM_STATUS` | `0x32` | request/response | `ipc_stream_status_t` | 48 B |

```c
typedef struct __attribute__((packed)) {
    uint32_t stream_sequence;      /* monotonic across BOTH message types */
    uint32_t dropped_since_last;   /* frames CPU1 had to drop before this message */
    uint64_t timestamp_us;         /* CPU1's platform_time_us() at publish time */
    ipc_feature_frame_v2_t frame;  /* 48 B, identical to the on-card FEATURE_V2 record */
    ipc_anomaly_rule_v1_t  rule;   /* 16 B, shares frame_sequence -> pairs 1:1 */
} ipc_feature_stream_v1_t;
```

**Adds `timestamp_us` on top of the minimal 72-byte field set.**
Reason: it lets `zmpiod` timestamp its capture record with the **same clock** the on-card record
uses, so the cross-check in §7 compares both the timestamp and the payload rather than the payload
alone — and it avoids fabricating a host-side timestamp for an event that happened on CPU1.

`stream_sequence` is **one** counter shared across both message types: a gap in it means "a message
was lost after CPU1 published it," regardless of type. It does not reset when the Linux process
restarts; only a CPU1 reboot resets it, and that event is already marked by ABI v3's `session_id`.

### Compile-time constraint (REQ-OPS-002)

`common/` cannot include firmware headers, so the two structs `ipc_feature_frame_v2_t` and
`ipc_anomaly_rule_v1_t` are **wire-format copies** of `fpga_feature_frame_t` and
`zlog_anomaly_rule_v1_t`. `firmware/app_freertos/src/zlog_feature_v2.h` — the one file that sees both
definitions — holds a series of `_Static_assert` checks on `offsetof()`/`sizeof()` that tie them
together field by field. Adding, removing, or reordering a field on either side **breaks the CPU1
build**, rather than shipping a Linux decoder that silently reads the wrong offset.

## 4. CPU1-side flow (`feature_stream.c`)

```
fpga_result_task (main.c)
  |- fpga_dsp_hal_pop_feature()
     |- logger_submit(FEATURE_V2)        -> microSD (archival copy, priority 1)
     |- rule_anomaly_evaluate()
     |- logger_submit(ANOMALY_RULE_V1)   -> microSD
     `- feature_stream_publish_feature() -> ABI v2 TX ring (telemetry copy)
          |- stream_reserve()            : claims a sequence number + reads the drop count
          |- ipc_send_timeout(5 ms)
          |- stream_commit()             : commits the sequence number, OR increments the drop count
          `- pl_doorbell_ring()          : only on a successful send
```

The "card first, stream second" ordering is deliberate: the on-card copy is what §7 cross-checks
against, so it must never depend on the ring's condition.

## 5. Why the buffer lives at file scope, not on the stack

`fpga_result_task`'s stack has overflowed **twice** in this project's history
(`app_config.h`, `APP_FPGA_RESULT_TASK_STACK_WORDS`: 512 -> 1536, with a real crash signature
captured over JTAG). An `ipc_message_t` is 256 B = 64 words; placing it on that same task's frame at
`-O0`, alongside `frame`/`counters`/`health`/`rule_result` and three `logger_submit()` calls each
building a 268 B `log_queue_item_t`, is exactly the combination that caused both previous overflows.

The buffer is therefore `static` at file scope. That is safe **only because** of the threading
contract in `feature_stream.h`: `publish_*()` and `note_counters()` may only be called from
`fpga_result_task`. `feature_stream_get_status()` is the only function called from another task
(`ipc_rx_task`), and it only reads the counter struct — protected by a critical section, the same
pattern `main.c` already uses for `latest_sensor_sample`.

## 6. Loss policy (REQ-STR-001, REQ-STR-005)

Two classes of loss, distinguishable by design:

| Class | Cause | Visible in |
|---|---|---|
| CPU1 fails to publish | ring full (Linux stopped consuming) or mutex timeout | `dropped_since_last` on the next message; `cpu1_dropped` in STREAM_STATUS |
| PL drops a frame before CPU1 sees it | `zmpio_dsp_ctrl`'s 64-deep FIFO is full | `ctrl_drop_count` in DSP_HEALTH and in STREAM_STATUS |
| Lost after CPU1 has published | **must never happen** (single reader) | `rx_stream_gaps` != 0 |

`stream_sequence` is only **committed on a successful send** (`stream_commit()`), so a dropped
frame never consumes a sequence number. That is what keeps the three rows above distinguishable: if
a drop also consumed a sequence number, "CPU1 dropped it" and "lost in transit" would look identical
on the receiving end.

Ring budget: 256 slots at 1.5625 frames/s gives ~164 seconds of buffering. Wide enough to cover a
`kill -9` + respawn cycle (the ≤ 2 s gate), **not** enough for a daemon that stays dead for a full
minute unnoticed — so a full ring is a designed, counted state, not an accident.

## 7. Cross-check against the microSD card (REQ-STR-004)

`zmpiod` writes each feature message as **two** ZLOG records — FEATURE_V2 (48 B) and
ANOMALY_RULE_V1 (16 B) — using the same `timestamp_us` and the same `flags` CPU1 used when writing
to the card. The result: the host's `STREAMnnnnn.ZBIN` file and the card's `LOGnnnnn.BIN` file
differ by exactly **one** field: `log_record_header_t::sequence` (CPU1 numbers it across all
sources; the capture numbers it within its own file).

Verification procedure:

```sh
python3 software/linux/parse_zlog.py LOG00042.BIN   -o card.csv
python3 software/linux/parse_zlog.py STREAM00000.ZBIN -o host.csv
python3 tools/soak_report.py report --samples soak.csv --sd-csv card.csv --host-csv host.csv
```

Using a single parser to read both files is the key point. Two separate decoders would only prove
that they agree with each other.

## 8. Linux side: one drain loop + demux (`zmpio_v2.c`)

See `SDD_14_LINUX_CLIENT_STACK.md` §4 for details. Contract summary:

- `drain_one()` is the **only** gate through which a message leaves the TX ring.
- A stream message goes to `zmpiod`'s sink; a response goes to whoever is waiting, or into the
  8-slot orphan queue if nobody is waiting yet.
- Only two things are discarded, and both are counted: structurally invalid messages (which cannot
  be routed anywhere), and stream messages arriving before a sink is registered.
- `zmpio_v2_request()` is safe to call at any time, even while the stream is actively flowing — that
  is exactly the regression `tests/zmpio_v2_demux_test.c` locks in place.

## 9. Verification status

| Item | Status | Evidence |
|---|---|---|
| Wire contract + `_Static_assert` on both sides | PASS (static) | clean CPU1 build + ARM cross-build |
| Demux does not swallow stream data (R1) | PASS (host test) | `zmpio_v2_demux_test`, 7 cases; this test FAILS against a mutant that reproduces the pre-Step-7 behavior |
| ZLOG capture parses via `parse_zlog.py` | PASS (host test) | `tests/capture_roundtrip.py`, including rotation |
| Runs on real board, no JTAG, ~25 minutes continuous | **PASS (board, 2026-09-11)** | `evidence/step7_feature_stream_uio_fix_20260911_2145_uart1.log` — session `session_id=0x00426e7a` from 21:19:49, `DSP feature #0` -> `#2383+` with no interruption and no reboot in between; 0 Data Abort, 0 SD mount failures, 0 feature-stream drops across the entire window |
| `zmpiod` online, UIO bound with the correct names, demux alive on real board | **PASS (board, 2026-09-11)** | `evidence/step7_feature_stream_uio_fix_20260911_2145_uart0.log` SOURCE §2: `cat /sys/class/uio/uio*/name` -> `zmpio-doorbell`/`zmpio-shm`; `zmpioctl status` -> `online=1 v2_ready=1`, `session_id` matches CPU1, `stream_dropped=0 stream_gaps=0 frame_gaps=0` (measured at 304 stream records) |
| Cross-check card <-> host (byte-for-byte via `parse_zlog.py`) | **Deferred by scope decision (2026-09-11)** — CPU1's SD card has not yet been pulled to cross-check `LOGnnnnn.BIN` against `STREAM00000.ZBIN` | project-owner scope decision, not a gate failure; the procedure in §7 is ready to run |
| `poll()` wakeups == delta `DBELL_COUNT` | **Deferred by scope decision (2026-09-11)** — `messages_from_tick` has not yet been measured separately against `messages_from_doorbell` on the board | same scope decision above |

Per ADR-016, none of the rows above may be marked PASS without a real board-captured artifact. The
two "deferred by scope decision" rows are not OPEN due to a capability gap or a failed gate — the
project owner chose to stop at the current level of evidence (the root cause of the two bugs that had
blocked the entire data path — `install_zmpiod()` crashing the build because of a dangling
`/var/log` symlink, and a missing `uio_pdrv_genirq.of_id=generic-uio` bootarg — was found and fixed
using real board evidence, not guesswork; see `deploy/petalinux_overlay/script1_fast_package.sh`),
and closing out the remaining large-scale acceptance items in full is not required for the current
goal.

## 10. Known Limitations

- **LIM-STR-001:** `dropped_since_last` is a `uint32_t` and is only cleared when a message sends
  successfully. If the ring stays full for more than 2^32 frames (~87 years at 1.5625 frames/s) it
  will wrap. Not handled.
- **LIM-STR-002:** the doorbell is rung on every message. While draining a backlog (for example
  after `FIFO_FULL_INJECT`) this can mean 65 doorbell rings in a row; since `irq_out` is
  level-triggered they naturally coalesce, but `DBELL_COUNT` will increment more times than
  `poll()` returns. The "wakeups == delta count" gate must therefore be measured in steady state, not
  immediately after an injection.
- **LIM-STR-003:** there is no backpressure from Linux back to CPU1 — by design. CPU1 must never
  slow down because of a slow consumer; it drops and counts instead.
- **LIM-STR-004:** only feature frames, rule verdicts, and DSP health cross the stream. Raw 100 Hz
  MPU6050 data still lives only on the card (the bandwidth, 100 x 14 B/s, is negligible, but nothing
  currently requires it on the Linux side).
