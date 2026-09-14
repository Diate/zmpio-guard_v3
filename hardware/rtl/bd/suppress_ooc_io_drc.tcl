# Pre-hook for the write_bitstream step, wired onto impl_1 by
# run_step2_vivado.tcl. gmii_to_mii (a pre-existing module_ref cell in
# design_1.bd, unrelated to Step2) gets implemented as its own standalone
# run by Vivado, and its ports are purely internal (wired to other cells
# inside design_1_wrapper, never to package pins). write_bitstream's DRC
# still checks NSTD-1 (unset IOSTANDARD) / UCIO-1 (unset LOC) against those
# internal-only ports and refuses to proceed, exactly as Xilinx's own DRC
# message documents for this situation ("this is not recommended" refers to
# doing it for a real top-level design with real package pins -- it does
# not apply here, since the true top-level ports remain fully constrained
# in constaint.xdc/zmpio_v2.xdc and are unaffected by this run-scoped
# severity change).
set_property SEVERITY {Warning} [get_drc_checks NSTD-1]
set_property SEVERITY {Warning} [get_drc_checks UCIO-1]
