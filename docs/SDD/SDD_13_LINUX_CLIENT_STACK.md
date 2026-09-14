# SDD_13 — Linux client stack: libzmpio / zmpiod / zmpioctl (CMP-LNX-002)

**Document ID:** ZMPIO-SDD-13
**Component:** CMP-LNX-002
**Level:** L2
**Source:** `software/linux/libzmpio/`, `software/linux/zmpiod/`,
`software/linux/zmpioctl/`, `software/linux/zmpio_socket_proto.h`,
`deploy/petalinux_overlay/zmpiod.init`, `deploy/petalinux_overlay/zmpiod-supervise`
**Requirement:** REQ-STR-002, REQ-STR-003, REQ-STR-005, REQ-OPS-001
**Decision:** ADR-018
**Supersedes:** `SDD_13_LINUX_IPC_CLIENT.md` (CMP-LNX-001, `ipc_linux` — the lab client over
`/dev/mem`)

---

## 1. Purpose

CMP-LNX-002 is the **production** Linux path: a single process that owns both UIO endpoints,
speaks ABI v2 + ABI v3 with CPU1, receives the feature stream, persists it to file, and exposes a
permission-gated socket for the command-line tool.

It replaces `ipc_linux` (SDD_13) rather than running alongside it: SDD_13 maps the shared DDR via
`/dev/mem`, runs as `root`, polls with `nanosleep`, and — critically since Step 7 — acts as a
**second reader of the TX ring**. Two readers on an SPSC ring split messages between them
deterministically (REQ-STR-002), so `ipc_linux` must be absent from the production image, not merely
"discouraged from running alongside" `zmpiod`.

## 2. Architecture

```text
        CPU1 (FreeRTOS)                    Linux (CPU0)
   feature_stream.c ──ring TX v2──►  zmpiod ──► /var/log/zmpio/STREAMnnnnn.ZBIN
   ipc_v3.c        ──rsp ring v3──►    │   └──► watcher socket (zmpioctl watch)
   pl_doorbell.c   ──IRQ SPI 63──►  poll() ▲
                                        │    │
                                   libzmpio │  AF_UNIX 0660 group zmpio
                                        │    │
                                   /dev/uioN └── zmpioctl (status, stream-status,
                                                  set-dsp-config, start/stop-log, watch)
```

Three layers, cleanly bounded:

| Layer | Responsible for | NOT responsible for |
|---|---|---|
| `libzmpio` | transport: UIO mmap, v2/v3 rings, CRC, doorbell ACK/re-enable, demux | files, sockets, policy |
| `zmpiod` | policy: where capture files go, how they rotate, which counters, who is allowed to command | ring layout, CRC |
| `zmpioctl` | presentation: one command, one line, an exit code | everything in the two layers above |

`zmpioctl` **never** touches shared memory. That is not layering for its own sake: it is the only
way to preserve "exactly one reader" while a command-line tool can be invoked at any time.

## 3. Ownership (REQ-STR-002)

A `zmpio_handle_t` is held by **exactly one process** for that process's entire lifetime. Before
Step 7 this was a recommendation; from Step 7 onward it is a load-bearing constraint, since the TX
ring now also carries the stream. It is enforced in three places:

1. `zmpiod-supervise` refuses to `exec` if a `zmpiod` instance is already running (§6).
2. `install_zmpiod()` in `script1_fast_package.sh` does not install `ipc_linux` unless
   `INSTALL_IPC_LINUX=1` (T7.5-4).
3. The `grep -r /dev/mem` gate on the production rootfs must return nothing (TEST-BOOT-003).

## 4. ALG-LNX-001 — one drain pass, then demux

**The problem.** The Step 6 version of `zmpio_v2_request()` read the TX ring until it found a
message whose `header.timestamp` matched its own `request_id`, **discarding** everything else along
the way. When the ring only carried responses, nothing was lost. Once the stream started sharing the
ring, a `zmpioctl status` running between two frames would swallow the frames in between — silently,
data-dependently, and non-reproducibly.

**The algorithm.**

```
drain_one():
    pop 1 message or return EMPTY/NOT_READY
    bad magic | length > payload      -> malformed++, DISCARDED
    type is {FEATURE_V2, DSP_HEALTH}:
        length != sizeof(struct)      -> malformed++, DISCARDED   (ABI drift)
        no sink registered            -> no_sink_dropped++, DISCARDED
        otherwise                     -> account + call the sink, STREAM
    anything else                     -> return to the caller, RESPONSE

zmpio_v2_drain(n):   loop drain_one; an unclaimed RESPONSE -> orphan_push()
wait_for_reply(id):  orphan_take(id) first; then loop drain_one until matched or timed out
```

Invariant: **no function advances `tail` and then discards a message it simply doesn't care
about.** Only two things are discarded — structural corruption, and stream data with no sink
registered — and both are counted.

The orphan queue (8 slots, `ZMPIO_V2_ORPHAN_MAX`) is not a throughput buffer: `zmpiod` issues at
most one request at a time. It exists so that a response arriving **right after** the caller has
already timed out is still available for the retry, instead of being lost along with its ring slot.
A full queue means `orphan_overflow++` — a number worth investigating, not one meant to be absorbed.

**Regression.** `software/linux/tests/zmpio_v2_demux_test.c` runs the real `zmpio_v2.c` against a
`malloc()`ed buffer laid out identically to the shared-DDR window — no UIO, no board, no root. 7
cases, of which case 1 is exactly the bug above. This test **fails** if `drain_one` is reverted to
its pre-Step-7 behavior, meaning it genuinely catches the regression rather than merely exercising
the code path.

## 5. ALG-LNX-002 — the daemon's poll loop (REQ-STR-003)

```
poll({doorbell_fd, listen_fd, watcher_fd[...]}, timeout = 1000 ms)
  doorbell readable -> zmpio_wait_doorbell()    (drain UIO, ACK, re-enable)
                    -> zmpio_v2_drain()          -> messages_from_doorbell +=
                    -> wake_hist += (t_after_drain - t_when_poll_returned)
  listen readable   -> accept + process 1 command; WATCH keeps the fd open
  watcher readable  -> means the client closed -> close the fd
  1000 ms elapsed   -> ensure_online() + a safety-net drain + flush capture
                    -> messages_from_tick +=
```

`messages_from_tick` is deliberately counted **separately**. The "no timer-based polling on the
normal path" gate cannot be proven by saying "we use `poll()`" — it is proven by that counter
staying at 0 while `messages_from_doorbell` accounts for the entire message total. If doorbells stop
arriving, data still gets through (nothing is lost), and this counter is what plainly says the gate
has broken.

## 6. Startup and supervision (BUG-036)

This project's PetaLinux rootfs runs **sysvinit** — the actual boot log shows `INIT: version 3.04
booting`. A systemd unit on that rootfs is never read, produces no error, and leaves no log. The
real mechanism:

| File | Role |
|---|---|
| `/usr/sbin/zmpiod-supervise` | the target of the `zmpd:2345:respawn:` line in `/etc/inittab`. sysvinit restarts it immediately on exit -> recovers from a `kill -9` in under 2 s |
| `/etc/init.d/zmpiod` | the administrative half: start/stop/restart/status |
| `/run/zmpiod.disabled` | a flag shared between the two files above |

Why both are needed: `respawn` alone cannot stop the daemon (it comes right back), yet scenarios H1/
H3 require a consumer that is **genuinely absent** for 60-200 seconds. `init.d` plus the `rcS.d`
symlink alone has no automatic recovery — it starts once and stops there. The flag bridges the two
mechanisms: `stop` sets the flag **before** sending the signal (the reverse order would race with
`respawn` and the daemon would come straight back).

`/run` is tmpfs, so the "stopped" state does not survive a reboot. This is deliberate: a board that
is power-cycled must come back up streaming, regardless of what state anyone left it in — the 20/20
cold-boot gate depends exactly on that.

## 7. ZLOG capture (`zmpio_capture.c`)

- Path: `$ZMPIO_CAPTURE_DIR` (default `/var/log/zmpio`), files named `STREAMnnnnn.ZBIN`.
- Container: exactly `common/zmpio_zlog.h` — the same format CPU1 writes to the memory card, so
  `parse_zlog.py` reads both (REQ-STR-004). There is no second parser in this project.
- Rotates by size (`$ZMPIO_CAPTURE_MAX_BYTES`, default 16 MiB); the index always seeks a
  non-existent filename, so restarting the daemon never overwrites the previous run's capture.
- A write error (e.g. ENOSPC) does **not** disable capture: it is counted in
  `capture_write_errors` and retried, because a transient error must not be allowed to kill the live
  stream.
- Failing to open a capture path (`mkdir` failure) also does not kill the daemon — streaming and
  counters keep running, and STATUS reports `capture=disabled`.

## 8. Socket protocol

See `software/linux/zmpio_socket_proto.h` for the full wire specification. Points worth noting:

- `STATUS` answers entirely from local state (no round trip to CPU1).
- `STREAM_STATUS` costs one ABI v2 request, and returns **both sides** in a single line: CPU1's
  counter and the daemon's counter, plus two latency histograms. One line, so a test script gets a
  consistent sample instead of stitching together multiple calls made at different times.
- `WATCH` is the only command that holds the connection open. A slow watcher has lines dropped
  **one at a time** (`MSG_DONTWAIT`, `watch_lines_dropped++`), and never creates backpressure on the
  data path. A lost watch line is not lost data — the capture file and counters remain intact.
- Access policy is enforced through the socket inode's permission bits (0660, group `zmpio`): a
  process outside the group is rejected at `connect()` with EACCES, before the daemon ever sees
  anything. Neither side needs to read `SO_PEERCRED`.

## 9. Verification status

| Item | Status | Evidence |
|---|---|---|
| Clean ARM cross-build (lib + daemon + ctl + test) | PASS | build with 0 warnings via `arm-linux-gnueabihf-gcc` |
| Demux/orphan/gap/malformed | PASS (host test) | `zmpio_v2_demux_test` 7/7 |
| Capture <-> `parse_zlog.py` round trip | PASS (host test) | `capture_roundtrip.py` |
| `zmpiod` self-starts via sysvinit respawn, no manual step | **PASS (board, 2026-09-11)** | `evidence/step7_feature_stream_uio_fix_20260911_2145_uart0.log` — `zmpiod` was already in UIO retry state immediately after boot, with no manual `/etc/init.d/zmpiod start` invocation |
| `zmpio` group policy on the board | **PASS (board, 2026-09-11)** | same file, SOURCE §2: `zmpioctl status` without sudo -> `Permission denied` (EACCES, as designed); with sudo -> succeeds |
| `kill -9` recovery within 2 s on the board | **Deferred by scope decision (2026-09-11)** — the project owner decided it is not needed for the current goal | — |
| 10,000 doorbells reaching userspace | **Deferred by scope decision (2026-09-11)** — only a spot check (`doorbell_count=306`) exists, not a large-scale stress test | same scope decision |
| No `/dev/mem` in the production image | **Guaranteed by build configuration, not yet scanned directly on the image** | `INSTALL_IPC_LINUX` was unset (default 0) for the 2026-09-11 build -> `ipc_linux` is not added to the rootfs by `install_zmpiod()`/`prepare_rootfs_bbappend()`; the `grep -r /dev/mem` gate (TEST-BOOT-003) on the actual `.img` has not been run |

## 10. Known Limitations

- **LIM-LNX-013:** `zmpiod` runs as `root`. Dropping to an unprivileged user requires a udev rule
  granting that user access to `/dev/uioN` plus write access to `/var/log/zmpio`; not yet done.
- **LIM-LNX-014:** single-threaded, one command at a time. A slow client blocks the loop for up to
  `ZMPIO_CLIENT_IO_TIMEOUT_MS` (2 s) — acceptable since the socket is already gated by group, but it
  means a misbehaving watcher can still delay a drain pass. This is a latency effect at worst, not a
  message loss, since doorbells remain level-triggered.
- **LIM-LNX-015:** a hard cap of 4 watchers (`ZMPIO_MAX_WATCHERS`), not configurable.
- **LIM-LNX-016:** the "fast package" rootfs overlay path cannot **append a line** to
  `/etc/inittab` (the tar overlay can only replace whole files). On that path, the respawn line must
  be added by hand after the first boot; the full `petalinux-build` path has `install_zmpiod()` do
  this correctly at rootfs build time.
- **LIM-LNX-017:** the latency histogram uses power-of-2 buckets, so p50/p99 are the **upper edge**
  of a bucket (clamped to the real max). Good enough to compare against a budget, not precise enough
  to compare two builds that differ by only a few percent.
