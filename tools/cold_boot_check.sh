#!/bin/sh
#
# tools/cold_boot_check.sh -- Step 7.5 T7.5-6.
#
# One power-cycle, one verdict.  Runs ON THE TARGET (busybox ash is enough)
# right after a cold boot with no JTAG attached, and answers the only question
# the 20/20 cold-boot gate asks: did this board come up, on its own, actually
# streaming?
#
#   scp tools/cold_boot_check.sh root@target:/usr/bin/
#   ssh root@target /usr/bin/cold_boot_check.sh
#
# Exit code 0 = PASS.  Every check prints its own line so a failing boot in a
# 20-run series says which part failed, not merely that it failed.  Run it
# with --json for a machine-readable summary line to append to an evidence
# file.
#
# What each check exists for:
#   nproc=1        -- Linux must not have taken CPU1 (maxcpus=1); if it did,
#                     CPU1's firmware is being overwritten and everything else
#                     below is meaningless.
#   UIO nodes      -- both endpoints present AND matched by name, never by
#                     /dev/uio0 ordering (ROADMAP STEP 6 rejects that).
#   zmpiod online  -- the daemon is up and its ABI v3 HELLO succeeded.
#   >= 90 records  -- the data path really moved: at 1.5625 frames/s, 60 s is
#                     ~93 feature frames, each producing 2 capture records, so
#                     90 records is a deliberately conservative floor.
#   UART1 alive    -- CPU1 is still executing, checked without JTAG.
#

WINDOW_S=60
JSON=0
FAILURES=0

for arg in "$@"; do
    case "$arg" in
        --json) JSON=1 ;;
        --window=*) WINDOW_S="${arg#--window=}" ;;
        *) echo "usage: $0 [--json] [--window=SECONDS]" >&2; exit 2 ;;
    esac
done

report() {
    # $1 = name, $2 = 0/1 pass, $3 = detail
    if [ "$2" = "1" ]; then
        [ "$JSON" = "0" ] && echo "[PASS] $1 -- $3"
    else
        [ "$JSON" = "0" ] && echo "[FAIL] $1 -- $3"
        FAILURES=$((FAILURES + 1))
    fi
}

# 1. maxcpus=1 -----------------------------------------------------------
# busybox does not always ship nproc; /proc/cpuinfo is always there.
CPUS=$(nproc 2>/dev/null)
if [ -z "$CPUS" ]; then
    CPUS=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null)
fi
[ -z "$CPUS" ] && CPUS="?" 
[ "$CPUS" = "1" ] && ok=1 || ok=0
report "linux owns exactly one core" "$ok" "nproc=$CPUS (bootargs must keep maxcpus=1)"

# 2. UIO nodes by name ---------------------------------------------------
UIO_SHM=""
UIO_DBELL=""
for dir in /sys/class/uio/uio*; do
    [ -e "$dir/name" ] || continue
    name=$(cat "$dir/name")
    case "$name" in
        zmpio-shm*|zmpio_shm*) UIO_SHM="$(basename "$dir")" ;;
        zmpio-doorbell*|zmpio_doorbell*) UIO_DBELL="$(basename "$dir")" ;;
    esac
done
[ -n "$UIO_SHM" ] && ok=1 || ok=0
report "zmpio-shm UIO node present" "$ok" "${UIO_SHM:-not found}"
[ -n "$UIO_DBELL" ] && ok=1 || ok=0
report "zmpio-doorbell UIO node present" "$ok" "${UIO_DBELL:-not found}"

# 3. daemon up and online ------------------------------------------------
if pidof zmpiod >/dev/null 2>&1; then
    report "zmpiod is running" 1 "pid $(pidof zmpiod)"
else
    report "zmpiod is running" 0 "no process -- check the inittab respawn entry"
fi

STATUS=$(zmpioctl status 2>&1)
case "$STATUS" in
    *online=1*) ok=1 ;;
    *) ok=0 ;;
esac
report "ABI v3 link online" "$ok" "$STATUS"

# 4. the stream actually moved -------------------------------------------
before=$(zmpioctl stream-status 2>/dev/null)
before_rx=$(echo "$before" | sed -n 's/.*rx_feature=\([0-9]*\).*/\1/p')
before_records=$(echo "$before" | sed -n 's/.*capture_records=\([0-9]*\).*/\1/p')
[ -z "$before_rx" ] && before_rx=0
[ -z "$before_records" ] && before_records=0

[ "$JSON" = "0" ] && echo "... sampling the stream for ${WINDOW_S}s"
sleep "$WINDOW_S"

after=$(zmpioctl stream-status 2>/dev/null)
after_rx=$(echo "$after" | sed -n 's/.*rx_feature=\([0-9]*\).*/\1/p')
after_records=$(echo "$after" | sed -n 's/.*capture_records=\([0-9]*\).*/\1/p')
after_gaps=$(echo "$after" | sed -n 's/.*rx_stream_gaps=\([0-9]*\).*/\1/p')
after_tick=$(echo "$after" | sed -n 's/.*msgs_from_tick=\([0-9]*\).*/\1/p')
[ -z "$after_rx" ] && after_rx=0
[ -z "$after_records" ] && after_records=0
[ -z "$after_gaps" ] && after_gaps=0
[ -z "$after_tick" ] && after_tick=0

RECORDS=$((after_records - before_records))
FRAMES=$((after_rx - before_rx))
[ "$RECORDS" -ge 90 ] && ok=1 || ok=0
report "at least 90 capture records in ${WINDOW_S}s" "$ok" \
       "records=$RECORDS frames=$FRAMES"

[ "$after_gaps" = "0" ] && ok=1 || ok=0
report "no loss after CPU1 published" "$ok" "rx_stream_gaps=$after_gaps"

[ "$after_tick" = "0" ] && ok=1 || ok=0
report "doorbell drove the data, not the fallback tick" "$ok" \
       "msgs_from_tick=$after_tick"

# 5. CPU1 still alive ----------------------------------------------------
# UART1 is CPU1's own console and is not readable from here, so liveness is
# inferred from the thing only a running CPU1 can do: move its own counters.
cpu1_published=$(echo "$after" | sed -n 's/.*cpu1_published=\([0-9]*\).*/\1/p')
[ -z "$cpu1_published" ] && cpu1_published=0
[ "$cpu1_published" -gt 0 ] && ok=1 || ok=0
report "CPU1 firmware alive and publishing" "$ok" \
       "cpu1_published=$cpu1_published (UART1 banner must be captured separately)"

if [ "$JSON" = "1" ]; then
    echo "{\"pass\":$([ $FAILURES -eq 0 ] && echo true || echo false),\"failures\":$FAILURES,\"records\":$RECORDS,\"frames\":$FRAMES,\"gaps\":$after_gaps,\"msgs_from_tick\":$after_tick,\"nproc\":\"$CPUS\",\"uio_shm\":\"$UIO_SHM\",\"uio_doorbell\":\"$UIO_DBELL\"}"
else
    echo "----------------------------------------------------------------"
    if [ "$FAILURES" -eq 0 ]; then
        echo "COLD BOOT: PASS"
    else
        echo "COLD BOOT: FAIL ($FAILURES check(s))"
    fi
fi

[ "$FAILURES" -eq 0 ] || exit 1
exit 0
