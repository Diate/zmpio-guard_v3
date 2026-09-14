# program_and_run.tcl
# Standard JTAG bring-up: program the bitstream, PS7 init, then bring up
# CPU1 and CPU0. Leaves both cores running so UART0 (CPU0 ethernet_test) /
# UART1 (CPU1 app_freertos) and the ABI v3 control block can be read back to
# confirm board status.
#
# CPU1 self-heal retry (see docs/architecture/07_FINDINGS_AND_DISCREPANCIES.md
# #29): app_freertos.elf can crash (Prefetch Abort or Data Abort, parked
# forever in the BSP's default exception handler) if it starts running too
# soon after a fresh PL bitstream reprogram. This is a non-deterministic
# race that a fixed settle delay alone does not reliably eliminate. The
# crash can also land a few seconds INTO CPU1's own init (observed inside
# ipc_v3_init()/Xil_DCacheInvalidateRange), not only in the first ~2s after
# boot, so a quick "PC moved once" check is not sufficient proof of health.
# The acceptance criterion this project cares about is the ABI v3 handshake
# reaching cpu1_link_state==ONLINE(2), so that is what this script polls
# for, with an outer retry that resets CPU1 alone (never reprogramming the
# PL) whenever the handshake does not complete.
#
# DAP-sticky recovery, ANY point in the run (see
# docs/architecture/07_FINDINGS_AND_DISCREPANCIES.md #20/#24/#28/#30): this
# JTAG cable (Digilent JTAG-HS2) has no SRST wiring, so a wedged DAP ("APB AP
# transaction error, DAP status 0x3...0021" / "0xF...0021") cannot be
# recovered by a normal reset. Selecting the DAP target itself (NOT the
# xc7z020/PL target) and issuing "rst -system" on it clears the error and
# re-enumerates both Cortex-A9 cores, in software, with no power-cycle
# needed.
#
# A DAP wedge can happen before, between, or during ANY JTAG step -- including
# mid-`dow`, where it can silently corrupt a partial download of
# app_freertos.elf (early boot prints, written first, can still come out
# correct while whatever is written later, such as the ABI v3 command-ring
# consumer, never runs). This script therefore treats DAP-sticky recovery as
# something that can trigger at any point, and reruns the ENTIRE session
# (bitstream + PS7 init + both cores) from scratch after recovering, up to a
# bounded number of times -- never trusting a `dow` that happened while the
# DAP was, or had just been, wedged.
#
# Run with: xsct.bat program_and_run.tcl > program_and_run.log 2>&1

# Repository paths -- resolved from this script's own location.
source [file join [file dirname [info script]] .. .. rtl bd project_paths.tcl]

connect

# Reads one 32-bit word via mrd and returns it as a plain hex string (no
# leading "0x", no address prefix). mrd -force ADDR 1 prints one line like
# "1904000c:   00000001" -- capture the value after the colon without
# end-anchoring the regexp (trailing whitespace/newline breaks a $-anchored
# match).
proc read_word {addr} {
    set out [mrd -force $addr 1]
    regexp {:\s*([0-9a-fA-F]+)} $out -> val
    return $val
}

proc cpu1_pc {} {
    targets -set -filter {name =~ "ARM Cortex-A9 MPCore #1*"}
    set pc_str [rrd pc]
    regexp {pc:\s*([0-9a-fA-F]+)} $pc_str -> pc_hex
    return $pc_hex
}

proc is_dap_sticky_text {text} {
    return [expr {[string match "*APB AP transaction error*" $text] ||
                  [string match "*DAP status*" $text] ||
                  [string match "*AP transaction timeout*" $text]}]
}

# Selects the DAP target itself (never xc7z020/APU -- those vanish from
# enumeration while wedged) and issues rst -system on it, up to 3 tries.
# Returns 1 if the error clears, 0 otherwise.
proc dap_recover {} {
    set targets_out [targets]
    if {![is_dap_sticky_text $targets_out]} {
        return 1
    }
    puts "DAP looks wedged, attempting software recovery (rst -system on DAP target)..."
    for {set dtry 1} {$dtry <= 3} {incr dtry} {
        set dap_index -1
        foreach line [split $targets_out "\n"] {
            if {[string match "*DAP*" $line] && [regexp {^\s*(\d+)} $line -> idx]} {
                set dap_index $idx
            }
        }
        if {$dap_index >= 0} {
            catch {targets $dap_index}
            catch {rst -system} rst_err
        } else {
            set rst_err "DAP target not found in target list"
        }
        after 800
        set targets_out [targets]
        if {![is_dap_sticky_text $targets_out]} {
            puts "DAP recovered after $dtry attempt(s)"
            return 1
        }
        puts "DAP recovery attempt $dtry failed: $rst_err"
    }
    return 0
}

# One full bring-up session (program bitstream, PS7 init, outer CPU1/CPU0
# retry loop polling for ABI v3 ONLINE) per pass of this loop. Deliberately
# NOT wrapped in a proc: Xilinx's generated ps7_init.tcl (sourced below)
# reads/writes plain global variables (e.g. PCW_SILICON_VER_1_0) that a proc
# body cannot see without explicit `global` declarations. Every risky JTAG
# step is wrapped in its own `catch` instead, and a DAP-sticky error text
# anywhere aborts the whole session (a dow that happened while the DAP was
# wedging cannot be trusted, even if it did not throw) so the outer loop can
# recover the DAP and restart clean.
set max_sessions 3
set session_ok 0

for {set session 1} {$session <= $max_sessions} {incr session} {
    puts "######## session $session/$max_sessions ########"

    if {![dap_recover]} {
        puts "FATAL: DAP still wedged after software recovery attempts. Physical power-cycle needed."
        exit 1
    }

    set handshake_ok 0
    set session_err [catch {
        targets -set -filter {name =~ "xc7z020"}
        fpga -file $zmpio(out_bit)
        puts "FPGA programmed"

        targets -set -filter {name =~ "APU"}
        source [file join $zmpio(platform_dir) hw sdt ps7_init.tcl]
        ps7_init
        ps7_post_config
        puts "PS7 init done"

        after 1000

        set max_outer_attempts 5
        set link_state_addr 0x1904000C

        for {set outer 1} {$outer <= $max_outer_attempts} {incr outer} {
            puts "=== outer attempt $outer/$max_outer_attempts ==="

            targets -set -filter {name =~ "ARM Cortex-A9 MPCore #1*"}
            rst -processor
            dow [file join $zmpio(output_dir) cpu1 app_freertos.elf]
            con

            after 1500
            set pcA [cpu1_pc]
            after 400
            set pcB [cpu1_pc]
            if {$pcA eq $pcB} {
                puts "outer $outer: CPU1 CRASHED early -- PC frozen at 0x$pcA. Retrying CPU1 only..."
                continue
            }
            puts "outer $outer: CPU1 PC moving (0x$pcA -> 0x$pcB), starting CPU0 / polling handshake..."

            # CPU0's own ABI v3 HELLO now retries periodically in its main
            # loop (see cpu0_application/src/main.c), but that retry can only
            # succeed against the CPU1 incarnation CPU0 is currently talking
            # to. Every outer attempt resets CPU1 with a brand new session_id,
            # so CPU0 must also be reset/redownloaded here on every attempt --
            # not just the first -- or a CPU0 left running from a previous
            # attempt (already past its own boot-time HELLO window) would
            # otherwise depend solely on that 2s periodic retry to notice the
            # new session before this script's own poll loop times out.
            targets -set -filter {name =~ "ARM Cortex-A9 MPCore #0*"}
            rst -processor
            dow [file join $zmpio(output_dir) cpu0 cpu0_application.elf]
            con
            puts "CPU0 running"

            set poll_ok 0
            for {set p 0} {$p < 12} {incr p} {
                after 1000
                set state [read_word $link_state_addr]
                if {[string equal -nocase $state "00000002"]} {
                    puts "outer $outer: cpu1_link_state == ONLINE after ~[expr {($p+1)*1000}]ms"
                    set poll_ok 1
                    break
                }
            }

            if {$poll_ok} {
                set handshake_ok 1
                break
            }

            set pcC [cpu1_pc]
            puts "outer $outer: handshake NOT online after poll timeout (cpu1_link_state=0x[read_word $link_state_addr], CPU1 PC=0x$pcC). Retrying CPU1 only..."
        }
    } session_result]

    if {$handshake_ok} {
        set session_ok 1
        break
    }

    if {$session_err} {
        if {[is_dap_sticky_text $session_result]} {
            puts "Session $session hit a DAP-sticky error mid-run: $session_result"
            puts "Recovering DAP and restarting the WHOLE session (a dow during a DAP wedge cannot be trusted)..."
            continue
        }
        # Not a DAP error -- a real bug, do not hide it by looping.
        error $session_result
    }

    puts "Session $session: handshake never reached ONLINE after its outer attempts. Not a DAP error this time."
}

if {!$session_ok} {
    puts "FATAL: ABI v3 handshake did not reach ONLINE after $max_sessions full session(s)."
    exit 1
}

puts "ABI v3 handshake ONLINE confirmed"

# Final status readback
targets -set -filter {name =~ "ARM Cortex-A9 MPCore #0*"}
puts "CPU0 PC: [rrd pc]"
targets -set -filter {name =~ "ARM Cortex-A9 MPCore #1*"}
puts "CPU1 PC: [rrd pc]"
puts "cpu1_link_state: [read_word 0x1904000C]"
puts "cmd_head/tail: [read_word 0x19040014] / [read_word 0x19040018]"
puts "rsp_head/tail: [read_word 0x1904001C] / [read_word 0x19040020]"
puts "cpu1_crc_drop/cpu0_crc_drop: [read_word 0x1904002C] / [read_word 0x19040030]"

puts "DONE program_and_run.tcl"
