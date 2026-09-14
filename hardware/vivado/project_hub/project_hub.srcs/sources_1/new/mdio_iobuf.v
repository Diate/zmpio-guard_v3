`timescale 1ns / 1ps
// mdio_iobuf.v
// Thin wrapper around the Xilinx IOBUF primitive so the MDIO tri-state pin
// (single bidirectional J15 board pin) can be instantiated inside IP
// Integrator. PS7 GEM0's EMIO MDIO interface exposes MDIO as three separate
// signals (O/T/I); this module merges them into one inout port.
module mdio_iobuf (
    input  wire I,   // data to drive onto the pad (from GEM MDIO_O)
    input  wire T,   // tri-state control, 1 = high-Z/input, 0 = drive (from GEM MDIO_T)
    output wire O,   // data sampled from the pad (to GEM MDIO_I)
    inout  wire IO   // the physical bidirectional pin (board net ETH_MDIO / J15)
);

    IOBUF mdio_iobuf_inst (
        .I  (I),
        .T  (T),
        .O  (O),
        .IO (IO)
    );

endmodule
