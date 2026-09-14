`timescale 1ns / 1ps
//
// zmpio_doorbell -- one-way CPU1->CPU0 wakeup doorbell (Step 5,
// docs/ROADMAP.md Step 5, docs/sdd_sad/SDD_DOORBELL.md). AXI4-Lite slave
// reachable from both cores over the shared PS7 GP AXI master path (same
// physical interconnect zmpio_dsp_ctrl already sits on) -- CPU1 rings it
// after publishing an ABI v3 response (ipc_v3.c's push_response()), CPU0
// drains/ACKs it from its own GIC-routed ISR (pl_doorbell.c). Command traffic
// (CPU0->CPU1) is unaffected: this module carries no data, only a wakeup.
//
// Register map (AXI4-Lite, word-aligned, C_S_AXI_ADDR_WIDTH=5):
//   0x00 DBELL_STATUS     bit0 PENDING (set by DBELL_SET, cleared by       (R)
//                         DBELL_ACK)
//   0x04 DBELL_SET        write bit0=1 -> PENDING<=1, DBELL_COUNT++         (W)
//                         (reads as 0). Writing while already PENDING still
//                         bumps DBELL_COUNT -- COUNT is the ground-truth
//                         event tally, independent of whether CPU0 has
//                         drained/ACKed yet, the same discipline used by
//                         zmpio_dsp_ctrl's FEATURE_COUNT.
//   0x08 DBELL_ACK         write bit0=1 -> PENDING<=0 (reads as 0)          (W)
//   0x0C DBELL_IRQ_ENABLE  bit0, mask on irq_out                            (R/W)
//   0x10 DBELL_COUNT       total DBELL_SET writes, saturating               (R)
//
// IRQ (irq_out, level) = IRQ_ENABLE && PENDING -- same discipline as
// zmpio_dsp_ctrl's irq_out: the consumer (CPU0's ISR) must mask
// DBELL_IRQ_ENABLE before draining and only re-enable it after DBELL_ACK +
// a STATUS recheck, or GIC will keep re-entering the ISR for as long as
// PENDING stays set (docs/sdd_sad/SDD_PL_SHELL.md, level-IRQ handling rules).
//
module zmpio_doorbell #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 5
) (
    input  wire                              s_axi_aclk,
    input  wire                              s_axi_aresetn,

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

    output wire                              irq_out
);

    localparam ADDR_STATUS      = 3'h0; // 0x00
    localparam ADDR_SET         = 3'h1; // 0x04
    localparam ADDR_ACK         = 3'h2; // 0x08
    localparam ADDR_IRQ_ENABLE  = 3'h3; // 0x0C
    localparam ADDR_COUNT       = 3'h4; // 0x10

    reg        pending;
    reg [31:0] dbell_count;
    reg        irq_enable_reg;

    assign irq_out = irq_enable_reg && pending;

    // ---- Write channel handshake (identical shape to zmpio_dsp_ctrl.v) ----
    wire [2:0] awaddr_word = s_axi_awaddr[4:2];
    reg  [2:0] write_word_addr;
    wire       write_word_hit = s_axi_wready && s_axi_wvalid;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_awready   <= 1'b0;
            s_axi_wready    <= 1'b0;
            write_word_addr <= 3'h0;
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
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= 2'b00;
        end else if (write_word_hit && !s_axi_bvalid) begin
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= 2'b00;
        end else if (s_axi_bvalid && s_axi_bready) begin
            s_axi_bvalid <= 1'b0;
        end
    end

    // ---- Register update ----
    // ADDR_ACK is applied before ADDR_SET below so that a same-cycle
    // SET+ACK (never expected from two separate AXI masters serialized by
    // the interconnect, but harmless to define) leaves PENDING set and
    // DBELL_COUNT incremented -- a ring must never lose to a same-cycle ack.
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            pending        <= 1'b0;
            dbell_count    <= 32'h0;
            irq_enable_reg <= 1'b0;
        end else begin
            if (write_word_hit) begin
                case (write_word_addr)
                    ADDR_ACK: begin
                        if (s_axi_wdata[0]) begin
                            pending <= 1'b0;
                        end
                    end
                    ADDR_IRQ_ENABLE: irq_enable_reg <= s_axi_wdata[0];
                    default: ; // ADDR_SET handled below, STATUS/COUNT read-only
                endcase
            end

            if (write_word_hit && (write_word_addr == ADDR_SET) && s_axi_wdata[0]) begin
                pending <= 1'b1;
                if (dbell_count != 32'hFFFFFFFF) begin
                    dbell_count <= dbell_count + 32'h1;
                end
            end
        end
    end

    // ---- Read channel (identical shape to zmpio_dsp_ctrl.v) ----
    reg [2:0] read_word_addr;

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
        end else if (!s_axi_arready && s_axi_arvalid) begin
            s_axi_arready  <= 1'b1;
            read_word_addr <= s_axi_araddr[4:2];
        end else begin
            s_axi_arready <= 1'b0;
        end
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_rvalid <= 1'b0;
            s_axi_rresp  <= 2'b00;
        end else begin
            if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rresp  <= 2'b00;
                case (read_word_addr)
                    ADDR_STATUS:     s_axi_rdata <= {31'h0, pending};
                    ADDR_IRQ_ENABLE: s_axi_rdata <= {31'h0, irq_enable_reg};
                    ADDR_COUNT:      s_axi_rdata <= dbell_count;
                    default:         s_axi_rdata <= 32'h0; // ADDR_SET/ADDR_ACK read as 0
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
