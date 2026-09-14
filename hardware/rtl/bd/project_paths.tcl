# Repository layout and external toolchain locations, for Tcl callers
# (Vivado batch scripts, XSDB scripts).
#
# Source it from any depth:
#     source [file join [file dirname [info script]] project_paths.tcl]
#     source $zmpio(rtl_dir)/bd/project_paths.tcl
#
# Every in-tree path is derived from this file's own location, so the tree can
# be cloned or moved anywhere. Only config/toolchain.env holds absolute paths,
# and an environment variable that is already set wins over the file.

array unset zmpio

# Walk up from this file until config/toolchain.env identifies the root.
set _pp_dir [file normalize [file dirname [info script]]]
set _pp_root $_pp_dir
while {![file exists [file join $_pp_root config toolchain.env]]} {
    set _pp_parent [file dirname $_pp_root]
    if {$_pp_parent eq $_pp_root} {
        # No marker found: fall back to three levels up (hardware/rtl/bd).
        set _pp_root [file normalize [file join $_pp_dir .. .. ..]]
        break
    }
    set _pp_root $_pp_parent
}

set zmpio(root)            $_pp_root
set zmpio(config_dir)      [file join $_pp_root config]
set zmpio(toolchain_env)   [file join $zmpio(config_dir) toolchain.env]
set zmpio(common_dir)      [file join $_pp_root common]
set zmpio(firmware_dir)    [file join $_pp_root firmware]
set zmpio(platform_dir)    [file join $zmpio(firmware_dir) platform_dual]
set zmpio(hardware_dir)    [file join $_pp_root hardware]
set zmpio(rtl_dir)         [file join $zmpio(hardware_dir) rtl]
set zmpio(ip_repo_dir)     [file join $zmpio(rtl_dir) ip_repo]
set zmpio(bd_dir)          [file join $zmpio(rtl_dir) bd]
set zmpio(vivado_proj_dir) [file join $zmpio(hardware_dir) vivado project_hub]
set zmpio(project_xpr)     [file join $zmpio(vivado_proj_dir) project_hub.xpr]
set zmpio(out_xsa)         [file join $zmpio(vivado_proj_dir) design_1_wrapper.xsa]
set zmpio(out_bit)         [file join $zmpio(vivado_proj_dir) project_hub.runs impl_1 design_1_wrapper.bit]
set zmpio(dsp_host_sim)    [file join $_pp_root dsp_host_sim]
set zmpio(output_dir)      [file join $_pp_root output]
set zmpio(artifacts_dir)   [file join $_pp_root artifacts]

# Load toolchain.env without clobbering anything already in the environment.
if {[file exists $zmpio(toolchain_env)]} {
    set _pp_fh [open $zmpio(toolchain_env) r]
    foreach _pp_line [split [read $_pp_fh] "\n"] {
        set _pp_line [string trim $_pp_line]
        if {$_pp_line eq "" || [string index $_pp_line 0] eq "#"} { continue }
        set _pp_eq [string first "=" $_pp_line]
        if {$_pp_eq < 1} { continue }
        set _pp_key [string trim [string range $_pp_line 0 [expr {$_pp_eq - 1}]]]
        set _pp_val [string trim [string range $_pp_line [expr {$_pp_eq + 1}] end]]
        set zmpio(cfg.$_pp_key) $_pp_val
        if {![info exists ::env($_pp_key)]} {
            set ::env($_pp_key) $_pp_val
        }
    }
    close $_pp_fh
}

unset -nocomplain _pp_dir _pp_root _pp_parent _pp_fh _pp_line _pp_eq _pp_key _pp_val
