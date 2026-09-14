`timescale 1ns / 1ps
//
// zmpio_dsp_ctrl -- AXI4-Lite control/status peripheral + 64-entry feature
// FIFO for Step 2's PL shell (docs/ROADMAP_DETAIL.md Step 2, docs/sdd_sad/
// SDD_10_Step2_PL_Shell.md). Sits downstream of dsp_core_axis_top's
// feature_axis master port; CPU1 drains frames through FEATURE_POP and gets
// woken by IRQ instead of polling STATUS in the normal path.
//
// Register map (AXI4-Lite, word-aligned, C_S_AXI_ADDR_WIDTH=7):
//   0x00 CONTROL      bit0 RUN, bit1 SOFT_RESET, bit2 IRQ_ENABLE   (R/W)
//                      SOFT_RESET (level-sensitive, software pulses it via
//                      two writes -- see fpga_dsp_hal.c) clears this
//                      module's own FIFO/counters AND drives dsp_soft_rst_n
//                      below, which resets dsp_core_axis_top + mmio_axis_bridge.
//                      It deliberately does NOT touch this module's own AXI4-
//                      Lite slave interface (s_axi_aresetn stays wired to the
//                      shared rst_ps7_0_49M network) or axi_iic_0, which sits
//                      on that same shared network in the block design, so
//                      resetting the DSP pipeline mid-RUN cannot disturb
//                      I2C or SD.
//   0x04 CONFIG_SEQ   plain scratch counter CPU1 bumps each            (R/W)
//                      STOP->config->START cycle
//   0x08 STATUS       bit0 FEATURE_READY (FIFO non-empty)              (R)
//                      bit1 RESULT_OVERFLOW (sticky, write-1-to-clear)  (R/W1C)
//   0x0C FEATURE_COUNT total feature beats accepted into the FIFO       (R, saturating)
//   0x10 DROP_COUNT   AXIS slave port stalled because the FIFO was full (R)
//                      -- an AXI4-Stream tready=0 stall event, never a
//                      silent data loss: the upstream dsp_core_axis_top
//                      (and mmio_axis_bridge behind it) simply holds until
//                      the FIFO drains. Only mmio_axis_bridge's own
//                      DROP_COUNT represents an actual discarded sample.
//   0x18-0x44 FEATURE_POP[0..11]  mirrors the FIFO head entry (48B/12
//                      words: frame_sequence .. band_energy_q32_0[3]).
//                      Reading word 11 (offset 0x44) auto-advances the FIFO
//                      to the next entry.
//
// IRQ (irq_out, level) = IRQ_ENABLE && (FEATURE_READY || RESULT_OVERFLOW):
// deasserts on its own once CPU1 drains the FIFO to empty and W1C-clears
// RESULT_OVERFLOW -- no separate pending-ack register needed.
//
module zmpio_dsp_ctrl #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 7
) (
    input  wire                              s_axi_aclk,
    input  wire                              s_axi_aresetn,

    // AXI4-Lite slave
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_awaddr,
    input  wire                              s_axi_awvalid,
    output reg                               s_axi_awready,
    input  wire [C_S_AXI_DATA_WIDTH-1:0]     s_axi_wdata,
    input  wire [(C_S_AXI_DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  wire                              s_axi_wvalid,
    output reg                               s_axi_wready,
    output reg  [1:0]                        s_axi_bresp,
    output reg                               s_axi_bvalid,
    input  wire                              s_axi_bready,
    input  wire [C_S_AXI_ADDR_WIDTH-1:0]     s_axi_araddr,
    input  wire                              s_axi_arvalid,
    output reg                               s_axi_arready,
    output reg  [C_S_AXI_DATA_WIDTH-1:0]     s_axi_rdata,
    output reg  [1:0]                        s_axi_rresp,
    output reg                               s_axi_rvalid,
    input  wire                              s_axi_rready,

    // AXI4-Stream slave -- one 384-bit feature_frame beat per push.
    // s_axis_aclk is electrically the same clock as s_axi_aclk (see the
    // identical note in mmio_axis_bridge.v) and exists only for Vivado's
    // IP-Integrator naming-convention clock/bus-interface association.
    input  wire [383:0]                      s_axis_tdata,
    input  wire                              s_axis_tvalid,
    output wire                              s_axis_tready,
    input  wire                              s_axis_tlast,
    input  wire                              s_axis_aclk,

    output wire                              irq_out,

    // Registered, active-low reset for dsp_core_axis_top/mmio_axis_bridge --
    // asserted by either s_axi_aresetn (shared network) or CONTROL.SOFT_RESET
    // (this module's own bit1), independent of axi_iic_0's reset. See the
    // CONTROL register map comment above.
    output wire                              dsp_soft_rst_n
);

    localparam ADDR_CONTROL  = 5'h0; // 0x00
    localparam ADDR_CONFIG   = 5'h1; // 0x04
    localparam ADDR_STATUS   = 5'h2; // 0x08
    localparam ADDR_FCOUNT   = 5'h3; // 0x0C
    localparam ADDR_DROP     = 5'h4; // 0x10
    // 0x14 reserved
    localparam ADDR_POP_BASE = 5'h6; // 0x18 -> word index 0 of FEATURE_POP
    localparam POP_WORDS     = 12;
    localparam FIFO_DEPTH    = 64;

    // ---- Feature FIFO ----
    reg [383:0] fifo_mem [0:FIFO_DEPTH-1];
    reg [5:0]   wr_ptr, rd_ptr;
    reg [6:0]   fifo_count; // 0..64
    wire        fifo_full  = (fifo_count == FIFO_DEPTH[6:0]);
    wire        fifo_empty = (fifo_count == 7'd0);
    wire [383:0] fifo_head = fifo_mem[rd_ptr];

    assign s_axis_tready = !fifo_full;
    wire push_event = s_axis_tvalid && s_axis_tready;

    // ---- Control/status registers ----
    reg [31:0] control_reg;
    reg [31:0] config_seq_reg;
    reg        result_overflow;
    reg [31:0] feature_count;
    reg [31:0] drop_count;
    reg        fifo_full_prev;

    wire irq_enable    = control_reg[2];
    wire feature_ready = !fifo_empty;
    assign irq_out = irq_enable && (feature_ready || result_overflow);

    // ---- SOFT_RESET (CONTROL bit1) ----
    // Level-sensitive: FIFO/counters below and dsp_soft_rst_n_r stay held in
    // reset for as long as software holds this bit set. control_reg itself
    // (and the AXI4-Lite write/read channel FSMs) are deliberately NOT part
    // of this reset domain -- see the register map comment at the top of
    // this file for why (self-deadlock avoidance + I2C isolation).
    wire soft_reset_active = control_reg[1];

    reg dsp_soft_rst_n_r;
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            dsp_soft_rst_n_r <= 1'b0;
        end else begin
            dsp_soft_rst_n_r <= !soft_reset_active;
        end
    end
    assign dsp_soft_rst_n = dsp_soft_rst_n_r;

    // pop_event is issued when the read-data phase serves the last
    // FEATURE_POP word (see read channel below).
    reg pop_event;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn || soft_reset_active) begin
            wr_ptr         <= 6'h0;
            rd_ptr         <= 6'h0;
            fifo_count     <= 7'h0;
            feature_count  <= 32'h0;
            drop_count     <= 32'h0;
            result_overflow<= 1'b0;
            fifo_full_prev <= 1'b0;
        end else begin
            if (push_event) begin
                fifo_mem[wr_ptr] <= s_axis_tdata;
                wr_ptr <= wr_ptr + 6'h1;
                if (feature_count != 32'hFFFFFFFF) begin
                    feature_count <= feature_count + 32'h1;
                end
            end

            if (pop_event && !fifo_empty) begin
                rd_ptr <= rd_ptr + 6'h1;
            end

            case ({push_event, (pop_event && !fifo_empty)})
                2'b10:   fifo_count <= fifo_count + 7'h1; // push only
                2'b01:   fifo_count <= fifo_count - 7'h1; // pop only
                default: fifo_count <= fifo_count;         // both or neither
            endcase

            // Diagnostic: the AXIS slave port was offered a beat while the
            // FIFO was already full (a backpressure/stall event, not a
            // silent data loss -- see module header). One increment per
            // stall episode, not per stalled clock cycle.
            if (s_axis_tvalid && fifo_full && !fifo_full_prev) begin
                drop_count      <= drop_count + 32'h1;
                result_overflow <= 1'b1;
            end
            fifo_full_prev <= s_axis_tvalid && fifo_full;

            if (write_word_hit && (write_word_addr == ADDR_STATUS)) begin
                // write-1-to-clear on bit1
                if (s_axi_wdata[1]) begin
                    result_overflow <= 1'b0;
                end
            end
        end
    end

    // ---- Write channel ----
    wire [4:0] awaddr_word = s_axi_awaddr[6:2];
    reg  [4:0] write_word_addr;
    wire       write_word_hit = s_axi_wready && s_axi_wvalid;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_awready   <= 1'b0;
            s_axi_wready    <= 1'b0;
            write_word_addr <= 5'h0;
        end else begin
            if (!s_axi_awready && s_axi_awvalid && !s_axi_wready) begin
                s_axi_awready   <= 1'b1;
                write_word_addr <= awaddr_word;
            end else begin
                s_axi_awready <= 1'b0;
            end

            if (!s_axi_wready && s_axi_wvalid && s_axi_awready) begin
                s_axi_wready <= 1'b1;
            end else begin
                s_axi_wready <= 1'b0;
            end
        end
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            control_reg    <= 32'h0;
            config_seq_reg <= 32'h0;
        end else if (write_word_hit) begin
            case (write_word_addr)
                ADDR_CONTROL: control_reg    <= s_axi_wdata;
                ADDR_CONFIG:  config_seq_reg <= s_axi_wdata;
                default: ; // STATUS W1C handled above; others read-only
            endcase
        end
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= 2'b00;
        end else if (write_word_hit && !s_axi_bvalid) begin
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= 2'b00;
        end else if (s_axi_bvalid && s_axi_bready) begin
            s_axi_bvalid <= 1'b0;
        end
    end

    // ---- Read channel ----
    reg [4:0] read_word_addr;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
        end else if (!s_axi_arready && s_axi_arvalid) begin
            s_axi_arready  <= 1'b1;
            read_word_addr <= s_axi_araddr[6:2];
        end else begin
            s_axi_arready <= 1'b0;
        end
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rresp  <= 2'b00;
            pop_event    <= 1'b0;
        end else begin
            pop_event <= 1'b0; // default: single-cycle pulse
            if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                if ((read_word_addr >= ADDR_POP_BASE) &&
                    (read_word_addr < ADDR_POP_BASE + POP_WORDS[4:0])) begin
                    s_axi_rdata <= fifo_head[(read_word_addr - ADDR_POP_BASE)*32 +: 32];
                    if (read_word_addr == ADDR_POP_BASE + POP_WORDS[4:0] - 5'h1) begin
                        pop_event <= 1'b1; // last word read -> advance FIFO
                    end
                end else begin
                    case (read_word_addr)
                        ADDR_CONTROL: s_axi_rdata <= control_reg;
                        ADDR_CONFIG:  s_axi_rdata <= config_seq_reg;
                        ADDR_STATUS:  s_axi_rdata <= {30'h0, result_overflow, feature_ready};
                        ADDR_FCOUNT:  s_axi_rdata <= feature_count;
                        ADDR_DROP:    s_axi_rdata <= drop_count;
                        default:      s_axi_rdata <= 32'h0;
                    endcase
                end
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
