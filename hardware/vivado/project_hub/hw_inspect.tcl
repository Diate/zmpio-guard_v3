# hw_inspect.tcl - READ ONLY: connect to the existing hw_server, list targets/devices,
# and check for any existing hw_ila cores. Does not program or reset anything.
open_hw_manager
connect_hw_server -allow_non_jtag
puts "===== hw_targets ====="
puts [get_hw_targets -quiet]
foreach t [get_hw_targets -quiet] {
    puts "-- opening $t --"
    open_hw_target $t
    puts "hw_devices: [get_hw_devices -quiet]"
    foreach dev [get_hw_devices -quiet] {
        puts "  device $dev, PROGRAM.FILE=[get_property -quiet PROGRAM.FILE $dev]"
    }
    close_hw_target $t
}
puts "DONE hw_inspect.tcl"
