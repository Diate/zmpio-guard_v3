# Packages hardware/rtl/src/mmio_axis_bridge.v and zmpio_dsp_ctrl.v as real
# IP-XACT catalog IP (vendor zmpio, library user) instead of leaving them as
# Vivado's create_bd_cell -type module -reference stand-in.
#
# Why this exists: Vivado's "module reference" wraps a raw .v file into a BD
# cell by guessing ports from naming convention, but never writes a real
# component.xml and cannot declare a port as an Interrupt signal. That is a
# direct, confirmed cause of pl.dtsi missing `interrupts` for
# zmpio_dsp_ctrl_0, and the leading hypothesis for Vitis 2023.2 Unified IDE
# Python API's platform.update_hw()/domain.get_libs() both crashing with an
# empty 'StatusCode.UNKNOWN' against this platform -- suspected to be an
# unhandled exception server-side while it tries to build a driver/BSP
# database entry for a peripheral with no IP-XACT metadata.
#
# This script only changes packaging/metadata -- it does not touch the RTL
# logic in mmio_axis_bridge.v / zmpio_dsp_ctrl.v at all (both are added via
# add_files -norecurse, read-only from this script's point of view).
#
# Run standalone, no project needs to be open first (creates and discards
# its own throwaway packaging projects under a .pkg_tmp scratch dir next to
# this script):
#   vivado -mode batch -source hardware/rtl/bd/package_zmpio_user_ip.tcl
#
# Output (checked into git, persistent -- unlike the HLS_IP_REPO_DIR scratch
# path at the top of add_zmpio_dsp_shell.tcl, which points at a session
# temp dir and goes stale on purpose; this one does not):
#   hardware/rtl/ip_repo/mmio_axis_bridge/component.xml
#   hardware/rtl/ip_repo/zmpio_dsp_ctrl/component.xml
#   hardware/rtl/ip_repo/zmpio_doorbell/component.xml (Step 5)
# add_zmpio_dsp_shell.tcl/add_zmpio_doorbell.tcl both add hardware/rtl/ip_repo
# to ip_repo_paths and reference these VLNVs:
#   zmpio:user:mmio_axis_bridge:1.0
#   zmpio:user:zmpio_dsp_ctrl:1.0
#   zmpio:user:zmpio_doorbell:1.0
#
# Idempotent: re-running deletes and rebuilds both packaged IP directories
# from whatever the current .v sources say, so a future RTL port-list change
# only requires re-running this before the next BD build.

set rtl_root [file normalize [file join [file dirname [info script]] ..]]
set verilog_src_dir [file normalize [file join $rtl_root src]]
set ip_repo_dir [file normalize [file join $rtl_root ip_repo]]
# Same part project_hub.xpr is built against (hardware/vivado/project_hub/project_hub.xpr).
set part "xc7z020clg400-2"

set tmp_root [file normalize [file join [file dirname [info script]] .pkg_tmp]]
if {[file exists $tmp_root]} {
    file delete -force $tmp_root
}
file mkdir $tmp_root

# name       : module/IP name, matches the .v file's module name
# verilog_file: absolute path to the single-file RTL source
# irq_port   : port name to declare as an Interrupt signal, or "" for none
proc package_zmpio_module {name verilog_file ip_repo_dir tmp_root part irq_port} {
    set proj_dir [file join $tmp_root "proj_$name"]
    set out_dir [file join $ip_repo_dir $name]
    if {[file exists $out_dir]} {
        file delete -force $out_dir
    }

    create_project -force "pkg_$name" $proj_dir -part $part
    add_files -norecurse $verilog_file
    update_compile_order -fileset sources_1

    ipx::package_project -root_dir $out_dir -vendor zmpio -library user \
        -taxonomy /UserIP -import_files -set_current true

    set core [ipx::current_core]
    set_property display_name $name $core
    set_property description \
        "ZMPIO Guard Step2 PL shell -- $name (hardware/rtl/src/$name.v)" $core
    set_property vendor_display_name {ZMPIO Guard} $core
    set_property supported_families {zynq Production} $core

    if {$irq_port ne ""} {
        # AXI4-Lite (s_axi) and AXI4-Stream (m_axis/s_axis) interfaces are
        # already auto-inferred by ipx::package_project from the standard
        # s_axi_*/m_axis_*/s_axis_* port naming used in the RTL -- confirmed
        # by the fact the old module-reference cells already exposed working
        # m_axis/s_axi bus interfaces to connect_bd_intf_net (see
        # add_zmpio_dsp_shell.tcl section 4-5). The one piece that naming
        # convention alone cannot infer is that irq_out is an interrupt.
        ipx::infer_bus_interface $irq_port xilinx.com:signal:interrupt_rtl:1.0 $core

        set busif [ipx::get_bus_interfaces -of_objects $core $irq_port]
        if {[llength $busif] != 1} {
            error "package_zmpio_user_ip.tcl: expected exactly 1 bus interface named '$irq_port' on $name after ipx::infer_bus_interface, got [llength $busif]."
        }
        if {[llength [ipx::get_bus_parameters -quiet SENSITIVITY -of_objects $busif]] == 0} {
            ipx::add_bus_parameter SENSITIVITY $busif
        }
        # irq_out is a combinational OR of level-sensitive status bits (see
        # zmpio_dsp_ctrl.v header: "IRQ (irq_out, level) = IRQ_ENABLE &&
        # (FEATURE_READY || RESULT_OVERFLOW)") -- not an edge pulse.
        set_property value LEVEL_HIGH [ipx::get_bus_parameters SENSITIVITY -of_objects $busif]
    }

    ipx::create_xgui_files $core
    ipx::update_checksums $core
    ipx::save_core $core
    close_project
}

package_zmpio_module mmio_axis_bridge \
    "$verilog_src_dir/mmio_axis_bridge.v" $ip_repo_dir $tmp_root $part ""

package_zmpio_module zmpio_dsp_ctrl \
    "$verilog_src_dir/zmpio_dsp_ctrl.v" $ip_repo_dir $tmp_root $part "irq_out"

# Step 5 -- zmpio_doorbell (docs/ROADMAP.md STEP 5, docs/sdd_sad/SDD_DOORBELL.md).
# Same reason as zmpio_dsp_ctrl above: irq_out must be declared as a real
# IP-XACT interrupt bus interface or pl.dtsi will be missing `interrupts`
# for it.
package_zmpio_module zmpio_doorbell \
    "$verilog_src_dir/zmpio_doorbell.v" $ip_repo_dir $tmp_root $part "irq_out"

file delete -force $tmp_root

puts "package_zmpio_user_ip.tcl: DONE. Packaged IP under $ip_repo_dir/mmio_axis_bridge, $ip_repo_dir/zmpio_dsp_ctrl and $ip_repo_dir/zmpio_doorbell (vendor=zmpio library=user version 1.0, zmpio_dsp_ctrl's and zmpio_doorbell's irq_out declared as xilinx.com:signal:interrupt_rtl:1.0/LEVEL_HIGH). Next: run hardware/rtl/bd/run_step2_vivado.tcl for the Step 2 shell, or hardware/rtl/bd/add_zmpio_doorbell.tcl for Step 5's doorbell."
