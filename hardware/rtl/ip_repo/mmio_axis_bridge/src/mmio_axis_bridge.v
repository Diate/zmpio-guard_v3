`timescale 1ns / 1ps
//
// mmio_axis_bridge -- converts CPU1's AXI4-Lite register pokes into a single
// AXI4-Stream "sample_frame" beat (Step1 SampleFrameV1, 160 bits / 20 bytes).
// CPU1 is bare-metal FreeRTOS with no DMA/AXI-Stream-master driver, so it can
// only write plain registers; this bridge is the hand-written glue the
// roadmap calls for (docs/ROADMAP_DETAIL.md Step 2, docs/sdd_sad/
// SDD_10_Step2_PL_Shell.md).
//
// Register map (AXI4-Lite, word-aligned, C_S_AXI_ADDR_WIDTH=5):
//   0x00 SAMPLE_W0  accel_x[15:0]     | accel_y[31:16]        (write)
//   0x04 SAMPLE_W1  accel_z[15:0]     | temperature[31:16]    (write)
//   0x08 SAMPLE_W2  gyro_x[15:0]      | gyro_y[31:16]         (write)
//   0x0C SAMPLE_W3  gyro_z[15:0]      | flags[31:16]          (write)
//   0x10 SAMPLE_W4  sample_sequence                            (write; commit strobe)
//   0x14 STATUS     bit0 BUSY                                  (read)
//   0x18 DROP_COUNT samples dropped while BUSY                 (read)
//
// Writing SAMPLE_W4 is the commit strobe: if the previous AXIS beat has
// already been accepted (tready seen, not BUSY), SAMPLE_W0..W4 are latched
// into m_axis_tdata and a single beat (tlast=1) is pushed. If the previous
// beat has NOT been accepted yet (BUSY), the new sample is dropped and
// DROP_COUNT increments -- the in-flight beat is never disturbed, so a
// downstream consumer never sees a torn/mixed sample.
//
module mmio_axis_bridge #(
    parameter integer C_S_AXI_DATA_WIDTH = 32,
    parameter integer C_S_AXI_ADDR_WIDTH = 5
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

    // AXI4-Stream master -- one 160-bit sample_frame beat per commit.
    // m_axis_aclk is electrically the same clock as s_axi_aclk (both tied
    // to FCLK_CLK0 in the block design) and is not used anywhere in this
    // module's logic -- it exists only so Vivado's IP-Integrator naming
    // convention ("<busif-prefix>_aclk") auto-associates the m_axis bus
    // interface with a clock. Without it, create_bd_cell's auto-inferred
    // component only associates s_axi_aclk with the s_axi (AXI4-Lite) bus
    // and leaves m_axis unclocked, which fails validate_bd_design; the
    // ASSOCIATED_BUSIF override that would normally fix this is read-only
    // on an auto-inferred module reference (see hardware/rtl/bd/add_zmpio_dsp_shell.tcl).
    output reg  [159:0]                      m_axis_tdata,
    output reg                               m_axis_tvalid,
    input  wire                              m_axis_tready,
    output reg                               m_axis_tlast,
    output reg  [0:0]                        m_axis_tuser,
    input  wire                              m_axis_aclk
);

    localparam ADDR_W0     = 3'h0; // 0x00
    localparam ADDR_W1     = 3'h1; // 0x04
    localparam ADDR_W2     = 3'h2; // 0x08
    localparam ADDR_W3     = 3'h3; // 0x0C
    localparam ADDR_W4     = 3'h4; // 0x10
    localparam ADDR_STATUS = 3'h5; // 0x14
    localparam ADDR_DROP   = 3'h6; // 0x18

    // Staging registers -- freely overwritable by CPU1 at any time; only
    // atomically committed into the AXIS beat on a successful W4 write.
    reg [31:0] sample_w0, sample_w1, sample_w2, sample_w3;
    reg        busy;
    reg [31:0] drop_count;

    // ---- Write channel ----
    wire [2:0] awaddr_word = s_axi_awaddr[4:2];
    reg  [2:0] write_word_addr;
    wire       write_commit_pulse = s_axi_wready && s_axi_wvalid &&
                                     (write_word_addr == ADDR_W4);

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
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
            sample_w0 <= 32'h0;
            sample_w1 <= 32'h0;
            sample_w2 <= 32'h0;
            sample_w3 <= 32'h0;
        end else if (s_axi_wready && s_axi_wvalid) begin
            case (write_word_addr)
                ADDR_W0: sample_w0 <= s_axi_wdata;
                ADDR_W1: sample_w1 <= s_axi_wdata;
                ADDR_W2: sample_w2 <= s_axi_wdata;
                ADDR_W3: sample_w3 <= s_axi_wdata;
                default: ; // ADDR_W4 (sample_sequence) is not staged --
                           // written straight into the commit below.
            endcase
        end
    end

    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            s_axi_bvalid <= 1'b0;
            s_axi_bresp  <= 2'b00;
        end else if (s_axi_wready && s_axi_wvalid && !s_axi_bvalid) begin
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= 2'b00; // OKAY, always -- no illegal-address trap
        end else if (s_axi_bvalid && s_axi_bready) begin
            s_axi_bvalid <= 1'b0;
        end
    end

    // ---- Commit / AXIS beat state machine ----
    always @(posedge s_axi_aclk) begin
        if (!s_axi_aresetn) begin
            busy          <= 1'b0;
            drop_count    <= 32'h0;
            m_axis_tvalid <= 1'b0;
            m_axis_tdata  <= 160'h0;
            m_axis_tlast  <= 1'b0;
            m_axis_tuser  <= 1'b1;
        end else begin
            if (write_commit_pulse) begin
                if (!busy) begin
                    m_axis_tdata  <= {s_axi_wdata,   // sample_sequence (W4)
                                       sample_w3, sample_w2, sample_w1, sample_w0};
                    m_axis_tvalid <= 1'b1;
                    m_axis_tlast  <= 1'b1;
                    busy          <= 1'b1;
                end else begin
                    drop_count <= drop_count + 32'h1;
                end
            end

            if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
                busy          <= 1'b0;
            end
        end
    end

    // ---- Read channel ----
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
        end else if (s_axi_arready && s_axi_arvalid && !s_axi_rvalid) begin
            s_axi_rvalid <= 1'b1;
            s_axi_rresp  <= 2'b00;
            case (read_word_addr)
                ADDR_STATUS: s_axi_rdata <= {31'h0, busy};
                ADDR_DROP:   s_axi_rdata <= drop_count;
                default:     s_axi_rdata <= 32'h0;
            endcase
        end else if (s_axi_rvalid && s_axi_rready) begin
            s_axi_rvalid <= 1'b0;
        end
    end

endmodule
