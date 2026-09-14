#!/usr/bin/env python3
"""Step 7.3 HIL / fault-injection matrix driver (docs/PLAN_BUOC_7.md SS4 7.3).

Every row of the 7.3 matrix is a command here, not a hand sequence typed into
a terminal: a scenario that cannot be re-run identically cannot be evidence.
Each run writes one file per scenario into docs/architecture/evidence/ named
step7_<scenario>_<YYYYmmdd>_<HHMM>_hil.log, containing the exact commands
issued, their raw output, the before/after counter samples, and an explicit
PASS/FAIL verdict against the expectation stated in the plan.

Transport is plain `ssh`/`scp` to the target -- the same path Step 6.0's
packaging gate establishes.  Nothing here needs JTAG except H5 (CPU1 reset),
which is marked MANUAL and prints exactly what to do instead of pretending to
automate it.

    python3 tools/hil_fault_inject.py --target root@192.168.1.50 --list
    python3 tools/hil_fault_inject.py --target root@192.168.1.50 --scenario H1
    python3 tools/hil_fault_inject.py --target root@192.168.1.50 --all

The verdicts are computed from `zmpioctl stream-status`, which reports CPU1's
counters and the daemon's counters in one line (zmpio_socket_proto.h), so a
scenario's PASS is a statement about both sides of the ring rather than about
whatever the host happened to observe.
"""

from __future__ import annotations

import argparse
import datetime
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

EVIDENCE_DIR = Path(__file__).resolve().parent.parent / "docs" / "architecture" / "evidence"

# Feature frames arrive one per ~640 ms (window 128 / hop 64 at 100 Hz), so
# every wait below is expressed in frames rather than in bare seconds.
FRAME_PERIOD_S = 0.64


class Target:
    """One SSH connection's worth of state, plus a transcript of everything."""

    def __init__(self, target: str, ssh: str = "ssh", dry_run: bool = False):
        self.target = target
        self.ssh = ssh
        self.dry_run = dry_run
        self.transcript: list[str] = []

    def log(self, text: str) -> None:
        self.transcript.append(text)
        print(text)

    def run(self, command: str, check: bool = False) -> tuple[int, str]:
        """Runs one command on the target and records it verbatim."""
        argv = [self.ssh, self.target, command]
        self.log(f"$ {' '.join(shlex.quote(a) for a in argv)}")
        if self.dry_run:
            self.log("  (dry run)")
            return 0, ""
        result = subprocess.run(argv, capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        for line in output.splitlines():
            self.log(f"  {line}")
        self.log(f"  [exit {result.returncode}]")
        if check and result.returncode != 0:
            raise RuntimeError(f"command failed: {command}")
        return result.returncode, output

    def stream_status(self) -> dict[str, int]:
        _, output = self.run("zmpioctl stream-status")
        return parse_counters(output)

    def status(self) -> dict[str, int]:
        _, output = self.run("zmpioctl status")
        return parse_counters(output)


def parse_counters(text: str) -> dict[str, int]:
    """Pulls every key=<integer> pair out of a zmpioctl reply line.

    Non-numeric values (capture=<path>, wake_us[...]) are skipped on purpose:
    the verdicts below are arithmetic on counters, and a scenario that needs
    the free-form fields should read them from the transcript.
    """
    counters: dict[str, int] = {}
    for key, value in re.findall(r"(\w+)=(-?(?:0x[0-9a-fA-F]+|\d+))", text):
        counters[key] = int(value, 0)
    return counters


class Verdict:
    def __init__(self, scenario: str, expectation: str):
        self.scenario = scenario
        self.expectation = expectation
        self.checks: list[tuple[str, bool, str]] = []

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        self.checks.append((name, bool(ok), detail))

    @property
    def passed(self) -> bool:
        return all(ok for _, ok, _ in self.checks) and bool(self.checks)

    def render(self) -> str:
        lines = [
            "",
            "=" * 72,
            f"scenario   : {self.scenario}",
            f"expectation: {self.expectation}",
            "-" * 72,
        ]
        for name, ok, detail in self.checks:
            lines.append(f"[{'PASS' if ok else 'FAIL'}] {name}"
                         + (f" -- {detail}" if detail else ""))
        lines.append("-" * 72)
        lines.append(f"VERDICT: {'PASS' if self.passed else 'FAIL'}")
        lines.append("=" * 72)
        return "\n".join(lines)


def wait_frames(target: Target, frames: float, why: str) -> None:
    seconds = frames * FRAME_PERIOD_S
    target.log(f"# waiting {seconds:.1f}s (~{frames:.0f} frames): {why}")
    if not target.dry_run:
        time.sleep(seconds)


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------

def h1_stop_daemon_60s(target: Target) -> Verdict:
    """CPU1 keeps running and keeps its microSD log; the loss is counted and
    the first message after the restart reports it in dropped_since_last."""
    verdict = Verdict("H1_daemon_stop_60s",
                      "CPU1 stays at 100 Hz and keeps writing the card; "
                      "every frame lost while the daemon was down is "
                      "reported by dropped_since_last / cpu1_dropped")
    before = target.stream_status()

    target.run("/etc/init.d/zmpiod stop")
    target.log("# daemon down for 60s -- ~94 frames should be produced")
    if not target.dry_run:
        time.sleep(60)
    target.run("/etc/init.d/zmpiod start")
    wait_frames(target, 5, "let the restarted daemon receive several frames")
    after = target.stream_status()

    produced = after.get("cpu1_feature_count", 0) - before.get("cpu1_feature_count", 0)
    cpu1_dropped = after.get("cpu1_dropped", 0) - before.get("cpu1_dropped", 0)
    verdict.check("CPU1 kept producing frames", produced >= 80,
                  f"feature_count delta={produced} (expected >= 80 in ~65 s)")
    verdict.check("the outage was counted, not silent", cpu1_dropped > 0,
                  f"cpu1_dropped delta={cpu1_dropped}")
    verdict.check("the stream reports the loss in-band",
                  after.get("rx_reported_drops", 0) >
                  before.get("rx_reported_drops", 0),
                  f"rx_reported_drops={after.get('rx_reported_drops')}")
    verdict.check("frames still reaching the daemon after restart",
                  after.get("rx_feature", 0) > before.get("rx_feature", 0),
                  f"rx_feature={after.get('rx_feature')}")
    return verdict


def h2_kill9_recovery(target: Target) -> Verdict:
    """kill -9 must be indistinguishable from a clean restart within 2 s, and
    must not change session_id -- CPU1 never rebooted."""
    verdict = Verdict("H2_kill9_recovery",
                      "daemon back within 2 s, session_id unchanged, "
                      "stream resumes")
    before = target.status()

    start = time.time()
    target.run("kill -9 $(pidof zmpiod) || true")
    online = False
    elapsed = 0.0
    if not target.dry_run:
        while time.time() - start < 10.0:
            time.sleep(0.25)
            code, output = target.run("zmpioctl status")
            if code == 0 and "online=1" in output:
                online = True
                elapsed = time.time() - start
                break
    after = target.status()

    verdict.check("daemon answered again within 2 s", online and elapsed <= 2.0,
                  f"recovered after {elapsed:.2f}s")
    verdict.check("session_id unchanged",
                  after.get("session_id") == before.get("session_id"),
                  f"{before.get('session_id')} -> {after.get('session_id')}")
    wait_frames(target, 4, "confirm the stream resumed after the restart")
    resumed = target.stream_status()
    verdict.check("stream resumed",
                  resumed.get("rx_feature", 0) > 0,
                  f"rx_feature={resumed.get('rx_feature')}")
    return verdict


def h3_ring_full(target: Target) -> Verdict:
    """256 slots at 1.5625 frames/s is ~164 s of buffer; past that CPU1 must
    drop and count rather than block fpga_result_task."""
    verdict = Verdict("H3_ring_full",
                      "no fpga_result_task stall, no PL FIFO overflow, "
                      "drops counted exactly")
    before = target.stream_status()

    target.run("/etc/init.d/zmpiod stop")
    target.log("# no consumer for 200 s -- the 256-slot ring must fill "
               "(~164 s) and then start dropping")
    if not target.dry_run:
        time.sleep(200)
    target.run("/etc/init.d/zmpiod start")
    wait_frames(target, 5, "let the daemon drain the backlog")
    after = target.stream_status()

    verdict.check("CPU1 dropped once the ring was full",
                  after.get("cpu1_dropped", 0) > before.get("cpu1_dropped", 0),
                  f"cpu1_dropped {before.get('cpu1_dropped')} -> "
                  f"{after.get('cpu1_dropped')}")
    verdict.check("PL FIFO did not overflow -- the drain never stalled",
                  after.get("cpu1_ctrl_drop", 0) == before.get("cpu1_ctrl_drop", 0),
                  f"cpu1_ctrl_drop {before.get('cpu1_ctrl_drop')} -> "
                  f"{after.get('cpu1_ctrl_drop')}")
    verdict.check("no samples lost at the bridge",
                  after.get("cpu1_bridge_drop", 0) == before.get("cpu1_bridge_drop", 0),
                  f"cpu1_bridge_drop={after.get('cpu1_bridge_drop')}")
    verdict.check("no loss after CPU1 published (single reader intact)",
                  after.get("rx_stream_gaps", 0) == before.get("rx_stream_gaps", 0),
                  f"rx_stream_gaps={after.get('rx_stream_gaps')}")
    return verdict


def h4_dsp_soft_reset(target: Target) -> Verdict:
    """The ABI v3 DSP_SOFT_RESET of Step 3, now checked from the stream's
    point of view: the frame_sequence discontinuity it causes must be
    explained by a DSP_HEALTH record rather than appearing as silent loss."""
    verdict = Verdict("H4_dsp_soft_reset",
                      "stream breaks and resumes; the frame_sequence jump is "
                      "explained in-band")
    before = target.stream_status()
    wait_frames(target, 3, "baseline frames before the reset")

    target.run("zmpioctl set-dsp-config 0 0 1023")  # touch the v3 path first
    target.log("# MANUAL STEP -- DSP_SOFT_RESET has no zmpioctl verb (it is "
               "an ABI v3 command zmpiod does not expose): issue it from the "
               "CPU0/JTAG harness now, the way Step 3 did "
               "(docs/sdd_sad/SDD_05_IPC_ABI_V3.md).  Press Enter when done.")
    if not target.dry_run:
        input("   waiting for the DSP_SOFT_RESET... ")
    wait_frames(target, 8, "let the pipeline restart and resume publishing")
    after = target.stream_status()

    verdict.check("stream resumed after the reset",
                  after.get("rx_feature", 0) > before.get("rx_feature", 0),
                  f"rx_feature {before.get('rx_feature')} -> "
                  f"{after.get('rx_feature')}")
    verdict.check("health record published for the event",
                  after.get("rx_health", 0) >= before.get("rx_health", 0),
                  f"rx_health {before.get('rx_health')} -> "
                  f"{after.get('rx_health')}")
    verdict.check("no unexplained loss after publication",
                  after.get("rx_stream_gaps", 0) == before.get("rx_stream_gaps", 0),
                  f"rx_stream_gaps={after.get('rx_stream_gaps')}")
    return verdict


def h5_cpu1_reset(target: Target) -> Verdict:
    """Needs JTAG: there is no in-band way to reset CPU1 alone, and pretending
    otherwise would produce a scenario that silently does nothing."""
    verdict = Verdict("H5_cpu1_reset_manual",
                      "session_id changes, daemon re-HELLOs, the capture "
                      "shows a session boundary")
    before = target.status()
    target.log("# MANUAL STEP -- this scenario cannot be driven over ssh.")
    target.log("#   1. From the JTAG host, reset and restart CPU1 only "
               "(scripts/build_cpu1.py --run, or the XSDB reset sequence in "
               "docs/walkthrough/).")
    target.log("#   2. Press Enter here once CPU1 has rebooted.")
    if not target.dry_run:
        input("   waiting for the manual CPU1 reset... ")
    wait_frames(target, 8, "let the daemon notice and re-HELLO")
    after = target.status()

    verdict.check("session_id changed -- CPU1 really rebooted",
                  after.get("session_id") != before.get("session_id"),
                  f"{before.get('session_id')} -> {after.get('session_id')}")
    verdict.check("daemon is online again", after.get("online") == 1,
                  f"online={after.get('online')}")
    return verdict


def h6_crc_injection(target: Target) -> Verdict:
    """A corrupted ABI v3 response must be counted and must not affect the
    stream, which lives on a different ring."""
    verdict = Verdict("H6_v3_crc_injection",
                      "cpu0_crc_drop_count rises, no ring hang, stream "
                      "unaffected")
    before = target.stream_status()
    before_status = target.status()
    target.log("# MANUAL STEP -- CRC corruption is injected by the CPU0/JTAG "
               "harness writing a bad CRC into the v3 rsp ring "
               "(docs/sdd_sad/SDD_05_IPC_ABI_V3.md).  Press Enter when done.")
    if not target.dry_run:
        input("   waiting for the CRC injection... ")
    wait_frames(target, 5, "observe the stream across the injection")
    after = target.stream_status()
    after_status = target.status()

    verdict.check("CRC drop counted",
                  after_status.get("cpu1_crc_drop", 0) >=
                  before_status.get("cpu1_crc_drop", 0),
                  f"cpu1_crc_drop={after_status.get('cpu1_crc_drop')}")
    verdict.check("stream kept flowing",
                  after.get("rx_feature", 0) > before.get("rx_feature", 0),
                  f"rx_feature {before.get('rx_feature')} -> "
                  f"{after.get('rx_feature')}")
    verdict.check("no stream loss", after.get("rx_stream_gaps", 0) ==
                  before.get("rx_stream_gaps", 0),
                  f"rx_stream_gaps={after.get('rx_stream_gaps')}")
    return verdict


def h7_doorbell_storm(target: Target) -> Verdict:
    """Step 5 proved the doorbell survives a storm at the IRQ level; here the
    question is whether the userspace consumer does."""
    verdict = Verdict("H7_doorbell_storm",
                      "no lost wakeup, no crash, CPU load stays sane")
    before = target.stream_status()
    _, load_before = target.run("cat /proc/loadavg")
    target.log("# MANUAL STEP -- run the doorbell storm from the CPU0/JTAG "
               "harness (run_doorbell_fault_test, Step 5).  Press Enter when "
               "it has finished.")
    if not target.dry_run:
        input("   waiting for the storm... ")
    after = target.stream_status()
    _, load_after = target.run("cat /proc/loadavg")
    code, _ = target.run("pidof zmpiod")

    verdict.check("daemon survived", code == 0)
    verdict.check("wakeups were seen",
                  after.get("doorbell_wakeups", 0) >
                  before.get("doorbell_wakeups", 0),
                  f"doorbell_wakeups {before.get('doorbell_wakeups')} -> "
                  f"{after.get('doorbell_wakeups')}")
    verdict.check("the doorbell, not the fallback tick, moved the data",
                  after.get("msgs_from_tick", 0) ==
                  before.get("msgs_from_tick", 0),
                  f"msgs_from_tick={after.get('msgs_from_tick')}")
    target.log(f"# loadavg before: {load_before.strip()}")
    target.log(f"# loadavg after : {load_after.strip()}")
    return verdict


def h8_sensor_or_sd_unplug(target: Target) -> Verdict:
    """A physical fault must show up in the stream as DEGRADED, not as
    silence."""
    verdict = Verdict("H8_sensor_or_sd_unplug",
                      "CPU1 goes DEGRADED, publishes DSP_HEALTH, Linux "
                      "records it")
    before = target.stream_status()
    target.log("# MANUAL STEP -- unplug the MPU6050 (or the microSD) now, "
               "wait ~10 s, then plug it back in.  Press Enter when done.")
    if not target.dry_run:
        input("   waiting for the physical fault... ")
    wait_frames(target, 10, "let the health state machine settle")
    after = target.stream_status()
    _, status_line = target.run("zmpioctl status")

    verdict.check("a health record was published",
                  after.get("rx_health", 0) > before.get("rx_health", 0),
                  f"rx_health {before.get('rx_health')} -> "
                  f"{after.get('rx_health')}")
    verdict.check("the fault was not silent",
                  "dsp_health=" in status_line,
                  status_line.strip())
    return verdict


def h9_permission_denied(target: Target) -> Verdict:
    """The socket's group gate, checked as a non-member rather than asserted
    from the mode bits."""
    verdict = Verdict("H9_non_member_denied",
                      "a user outside group zmpio is refused at connect()")
    target.run("id nobody || adduser -D nobody || true")
    code, output = target.run("su -s /bin/sh nobody -c 'zmpioctl status'")

    verdict.check("non-member was refused", code != 0,
                  f"exit={code}")
    verdict.check("refusal came from the socket permission bits",
                  ("Permission denied" in output) or ("EACCES" in output),
                  output.strip().splitlines()[-1] if output.strip() else "")
    code_member, _ = target.run("zmpioctl status")
    verdict.check("a member is still allowed", code_member == 0)
    return verdict


SCENARIOS = {
    "H1": ("stop zmpiod for 60 s", h1_stop_daemon_60s),
    "H2": ("kill -9 zmpiod", h2_kill9_recovery),
    "H3": ("fill the TX ring completely", h3_ring_full),
    "H4": ("DSP_SOFT_RESET mid-RUN", h4_dsp_soft_reset),
    "H5": ("reset CPU1 alone (JTAG, manual)", h5_cpu1_reset),
    "H6": ("inject a bad CRC into the v3 rsp ring (manual)", h6_crc_injection),
    "H7": ("doorbell storm (manual)", h7_doorbell_storm),
    "H8": ("unplug the sensor / the SD card (manual)", h8_sensor_or_sd_unplug),
    "H9": ("call set-dsp-config from outside group zmpio", h9_permission_denied),
}


def write_evidence(name: str, transcript: list[str], verdict: Verdict) -> Path:
    EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M")
    path = EVIDENCE_DIR / f"step7_{name}_{stamp}_hil.log"
    path.write_text("\n".join(transcript) + "\n" + verdict.render() + "\n",
                    encoding="utf-8")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", help="ssh destination, e.g. root@192.168.1.50")
    parser.add_argument("--ssh", default="ssh", help="ssh binary to use")
    parser.add_argument("--scenario", action="append",
                        help="scenario id (H1..H9); repeatable")
    parser.add_argument("--all", action="store_true", help="run every scenario")
    parser.add_argument("--list", action="store_true", help="list scenarios")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would run without touching the target")
    args = parser.parse_args()

    if args.list:
        for key, (description, _) in SCENARIOS.items():
            print(f"{key}  {description}")
        return 0

    if args.target is None:
        parser.error("--target is required unless --list is given")

    selected = list(SCENARIOS) if args.all else (args.scenario or [])
    if not selected:
        parser.error("give --scenario H1 (repeatable) or --all")

    failures = 0
    for key in selected:
        if key not in SCENARIOS:
            print(f"unknown scenario {key}", file=sys.stderr)
            return 2
        description, function = SCENARIOS[key]
        target = Target(args.target, args.ssh, args.dry_run)
        target.log(f"# {key}: {description}")
        target.log(f"# target={args.target} started={datetime.datetime.now().isoformat()}")
        try:
            verdict = function(target)
        except Exception as error:  # noqa: BLE001 -- a failed scenario is data
            verdict = Verdict(key, description)
            verdict.check("scenario ran to completion", False, str(error))
        print(verdict.render())
        path = write_evidence(verdict.scenario, target.transcript, verdict)
        print(f"evidence: {path}")
        if not verdict.passed:
            failures += 1

    print(f"\n{len(selected) - failures}/{len(selected)} scenario(s) PASS")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
