`timescale 1ns / 1ps
//
// Behavioral self-check for zmpio_doorbell.v, run with Icarus Verilog (same
// host-side sanity-check convention as tb_zmpio_dsp_shell.v -- not part of
// the Vivado build). Drives the AXI4-Lite slave directly as both "CPU1"
// (DBELL_SET) and "CPU0" (DBELL_ACK) would over the shared PS7 GP AXI path.
//
// Run: iverilog -g2012 -o tb.vvp tb_zmpio_doorbell.v zmpio_doorbell.v && vvp tb.vvp
//
module tb_zmpio_doorbell;

    reg clk = 0;
    reg aresetn = 0;
    always #5 clk = ~clk; // 100 MHz sim clock (rate is irrelevant here)

    integer errors = 0;
    task check(input cond, input [8*48-1:0] msg);
        begin
            if (!cond) begin
                errors = errors + 1;
                $display("FAIL: %0s", msg);
            end
        end
    endtask

    reg  [4:0]  awaddr;
    reg         awvalid;
    wire        awready;
    reg  [31:0] wdata;
    reg  [3:0]  wstrb;
    reg         wvalid;
    wire        wready;
    wire [1:0]  bresp;
    wire        bvalid;
    reg         bready;
    reg  [4:0]  araddr;
    reg         arvalid;
    wire        arready;
    wire [31:0] rdata;
    wire [1:0]  rresp;
    wire        rvalid;
    reg         rready;
    wire        irq_out;

    zmpio_doorbell dut (
        .s_axi_aclk(clk), .s_axi_aresetn(aresetn),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .irq_out(irq_out)
    );

    // Register word offsets (bytes) -- see zmpio_doorbell.v header.
    localparam ADDR_STATUS     = 5'h00;
    localparam ADDR_SET        = 5'h04;
    localparam ADDR_ACK        = 5'h08;
    localparam ADDR_IRQ_ENABLE = 5'h0C;
    localparam ADDR_COUNT      = 5'h10;

    task axi_write(input [4:0] addr, input [31:0] data);
        begin
            awaddr = addr; awvalid = 1; wdata = data; wstrb = 4'hF; wvalid = 1; bready = 1;
            @(posedge clk);
            while (!bvalid) @(posedge clk);
            awvalid = 0; wvalid = 0; bready = 0;
            @(posedge clk);
        end
    endtask

    task axi_read(input [4:0] addr, output [31:0] data);
        begin
            araddr = addr; arvalid = 1; rready = 1;
            @(posedge clk);
            while (!arready) @(posedge clk);
            @(posedge clk);
            arvalid = 0;
            while (!rvalid) @(posedge clk);
            data = rdata;
            @(posedge clk);
        end
    endtask

    reg [31:0] rd;
    integer i;

    initial begin
        awaddr=0; awvalid=0; wdata=0; wstrb=0; wvalid=0; bready=0;
        araddr=0; arvalid=0; rready=0;

        repeat (4) @(posedge clk);
        aresetn = 1;
        repeat (2) @(posedge clk);

        // ---- Test 1: reset state ----
        axi_read(ADDR_STATUS, rd);
        check(rd == 32'h0, "reset: PENDING clear");
        axi_read(ADDR_COUNT, rd);
        check(rd == 32'h0, "reset: DBELL_COUNT==0");
        check(irq_out == 1'b0, "reset: irq_out low");

        // ---- Test 2: SET without IRQ_ENABLE raises PENDING/COUNT but not irq_out ----
        axi_write(ADDR_SET, 32'h1);
        axi_read(ADDR_STATUS, rd);
        check(rd[0] == 1'b1, "set: PENDING set");
        axi_read(ADDR_COUNT, rd);
        check(rd == 32'd1, "set: DBELL_COUNT==1");
        check(irq_out == 1'b0, "set: irq_out stays low while IRQ_ENABLE==0");

        // ---- Test 3: enabling IRQ_ENABLE with PENDING already set raises irq_out ----
        axi_write(ADDR_IRQ_ENABLE, 32'h1);
        @(posedge clk);
        check(irq_out == 1'b1, "enable: irq_out high once IRQ_ENABLE set and PENDING");

        // ---- Test 4: ACK+recheck clears PENDING and deasserts irq_out ----
        axi_write(ADDR_ACK, 32'h1);
        axi_read(ADDR_STATUS, rd);
        check(rd[0] == 1'b0, "ack: PENDING cleared");
        check(irq_out == 1'b0, "ack: irq_out low after ACK+recheck");
        axi_read(ADDR_COUNT, rd);
        check(rd == 32'd1, "ack: DBELL_COUNT unaffected by ACK (ground truth)");

        // ---- Test 5: ACK when not PENDING is a no-op ----
        axi_write(ADDR_ACK, 32'h1);
        axi_read(ADDR_STATUS, rd);
        check(rd[0] == 1'b0, "ack-noop: PENDING stays clear");

        // ---- Test 6: doorbell storm -- N back-to-back SETs (faster than any
        // ACK) must not lose a single one from DBELL_COUNT's point of view,
        // and irq_out must stay asserted throughout (level IRQ, same
        // discipline as the GIC-loss bug documented for zmpio_dsp_ctrl in
        // docs/architecture/08_VAN_DE_DANG_MO.md muc 12 -- this is the same
        // failure class, tested here from the CPU0-target side). ----
        for (i = 0; i < 50; i = i + 1) begin
            axi_write(ADDR_SET, 32'h1);
        end
        axi_read(ADDR_COUNT, rd);
        check(rd == 32'd51, "storm: DBELL_COUNT counts every SET (1 earlier + 50 here)");
        check(irq_out == 1'b1, "storm: irq_out held asserted (IRQ_ENABLE still on) through the storm");
        axi_write(ADDR_ACK, 32'h1);
        axi_read(ADDR_STATUS, rd);
        check(rd[0] == 1'b0, "storm: single ACK after the storm still clears PENDING");
        check(irq_out == 1'b0, "storm: irq_out low after post-storm ACK");

        // ---- Test 7: disabling IRQ_ENABLE masks irq_out even while PENDING ----
        axi_write(ADDR_SET, 32'h1);
        check(irq_out == 1'b1, "disable-prep: irq_out high before disabling");
        axi_write(ADDR_IRQ_ENABLE, 32'h0);
        @(posedge clk);
        check(irq_out == 1'b0, "disable: irq_out low once IRQ_ENABLE cleared, PENDING still set");
        axi_read(ADDR_STATUS, rd);
        check(rd[0] == 1'b1, "disable: PENDING itself untouched by IRQ_ENABLE");

        if (errors == 0) begin
            $display("tb_zmpio_doorbell PASS");
        end else begin
            $display("tb_zmpio_doorbell FAIL: %0d error(s)", errors);
        end
        $finish;
    end

endmodule
