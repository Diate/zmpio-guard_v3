`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 08/14/2026 09:44:52 PM
// Design Name: 
// Module Name: gmii_to_mii
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module gmii_to_mii (
    input  wire [7:0] gmii_txd,
    input  wire       gmii_tx_en,
    input  wire       gmii_tx_er,

    output wire [7:0] gmii_rxd,
    output wire       gmii_rx_dv,
    output wire       gmii_rx_er,
    output wire       gmii_crs,
    output wire       gmii_col,

    output wire [3:0] mii_txd,
    output wire       mii_tx_en,

    input  wire [3:0] mii_rxd,
    input  wire       mii_rx_dv
);

    assign mii_txd   = gmii_txd[3:0];
    assign mii_tx_en = gmii_tx_en;

    assign gmii_rxd   = {4'b0000, mii_rxd};
    assign gmii_rx_dv = mii_rx_dv;
    assign gmii_rx_er = 1'b0;

    assign gmii_crs = 1'b0;
    assign gmii_col = 1'b0;

endmodule