// Exercise the start-once production IP through its real AXI interfaces.
// Register layout is the Vitis HLS 2021.2 Vivado-flow layout of the C structs.
`timescale 1ns/1ps
module tb_mmio;
`ifdef RE_HLS_RTL_QUICK
  localparam [15:0] TEST_CPGP_NM = 16'd2;
`else
  localparam [15:0] TEST_CPGP_NM = 16'd20;
`endif
  reg clk = 0;
  always #4.5 clk = ~clk;
  reg reset_n = 0;
  reg [7:0] awaddr = 0, araddr = 0;
  reg awvalid = 0, wvalid = 0, arvalid = 0;
  reg [31:0] wdata = 0;
  wire awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  wire [1:0] bresp, rresp;
  reg [511:0] tdata = 0;
  reg [63:0] tkeep = 0;
  reg [64:0] tuser = 0;
  reg tlast = 0, tvalid = 0;
  wire tready;
  wire [511:0] perf_tdata;
  wire perf_tvalid;
  reEngineKernel dut (
    .ap_clk(clk), .ap_rst_n(reset_n),
    .s_axi_control_AWADDR(awaddr), .s_axi_control_AWVALID(awvalid), .s_axi_control_AWREADY(awready),
    .s_axi_control_WDATA(wdata), .s_axi_control_WSTRB(4'hf),
    .s_axi_control_WVALID(wvalid), .s_axi_control_WREADY(wready),
    .s_axi_control_BRESP(bresp), .s_axi_control_BVALID(bvalid), .s_axi_control_BREADY(1'b1),
    .s_axi_control_ARADDR(araddr), .s_axi_control_ARVALID(arvalid), .s_axi_control_ARREADY(arready),
    .s_axi_control_RDATA(rdata), .s_axi_control_RRESP(rresp),
    .s_axi_control_RVALID(rvalid), .s_axi_control_RREADY(1'b1),
    .dma_in_TDATA(tdata), .dma_in_TKEEP(tkeep), .dma_in_TSTRB(tkeep),
    .dma_in_TUSER(tuser), .dma_in_TLAST(tlast), .dma_in_TVALID(tvalid), .dma_in_TREADY(tready),
    .perf_out_TDATA(perf_tdata), .perf_out_TVALID(perf_tvalid), .perf_out_TREADY(1'b1)
  );
  initial begin
    repeat (2000000) @(posedge clk);
    $fatal(1, "MMIO watchdog: design or bus transaction stalled");
  end
  task wr(input [7:0] addr, input [31:0] value);
    begin
      @(negedge clk); awaddr = addr; awvalid = 1;
      @(posedge clk); while (!awready) @(posedge clk);
      @(negedge clk); awvalid = 0; wdata = value; wvalid = 1;
      @(posedge clk); while (!wready) @(posedge clk);
      @(negedge clk); wvalid = 0;
      @(posedge clk); while (!bvalid) @(posedge clk);
      if (bresp !== 0) $fatal(1, "AXI write response error");
    end
  endtask
  // A complete, out-of-order configured rule: ^[ab]*c$.
  task configure_class_rule;
    begin
      command(3, 1, 0, 13);
      command(3, 1, 8, {16'd1, 16'd1, 32'd7});
      command(3, 1, 64, 64'h0100000000000000); // END
      command(3, 1, 56, 64'h2400000000000000); // END_BUF
      command(3, 1, 48, 64'h0701630000000000); // STR_N c
      command(3, 1, 40, 64'h3a00000100000000); // JUMP 1
      command(3, 1, 32, 64'h0e01000000000000); // CCLASS 0
      command(3, 1, 24, 64'h3b10000400000000); // PUSH 4
      command(3, 1, 16, 64'h2300000000000000); // BEGIN_BUF
      command(3, 1, 96, 0);
      command(3, 1, 88, 0);
      command(3, 1, 80, 64'h0000000600000000); // bits a,b
      command(3, 1, 72, 0); // final missing word triggers compilation
      wr('h20, 0);
    end
  endtask
  task check_class_result(input [31:0] expected_match, input [31:0] expected_range,
                          input bit maximum_packet = 0);
    reg [31:0] result_word;
    begin
      wr('h20, 0); // return from configuration to runtime before DMA
      if (maximum_packet) send_max_packet(); else send_packet();
      poll('hc8, 'h00010101, 'h00000100);
      rd('h80, result_word);
      if (result_word !== expected_match) $fatal(1, "Configured class rule match mismatch expected=%h got=%h", expected_match, result_word);
      if (expected_match == 1) begin
        rd('h84, result_word);
        if (result_word !== expected_range) $fatal(1, "Configured class rule range mismatch expected=%h got=%h", expected_range, result_word);
      end
      $display("MMIO class rule verified: match=%h range=%h time=%t", expected_match, expected_range, $time);
      wr('h18, 1); poll('hc8, 'h00010101, 1);
      $display("MMIO class result acknowledgement raised at time=%t", $time);
      wr('h18, 0); poll('hc8, 'h00010101, 0);
      $display("MMIO class result retired at time=%t", $time);
    end
  endtask
  task rd(input [7:0] addr, output [31:0] value);
    begin
      @(negedge clk); araddr = addr; arvalid = 1;
      @(posedge clk); while (!arready) @(posedge clk);
      @(negedge clk); arvalid = 0;
      @(posedge clk); while (!rvalid) @(posedge clk);
      if (rresp !== 0) $fatal(1, "AXI read response error");
      value = rdata;
    end
  endtask
  task poll(input [7:0] addr, input [31:0] mask, input [31:0] expected);
    reg [31:0] value;
    integer n;
    begin : polling
      for (n = 0; n < 50000; n = n + 1) begin
        rd(addr, value);
        if ((value & mask) === expected) disable polling;
      end
      $fatal(1, "Poll timeout addr=%h mask=%h expected=%h got=%h", addr, mask, expected, value);
    end
  endtask
  task command(input [3:0] cmd, input [15:0] job, input [63:0] a, input [63:0] b);
    reg [31:0] value;
    begin
      wr('h20, {job, 8'h00, 4'h0, cmd});
      // VALID remains asserted until ACK, so configuration completion uses
      // the handshake rather than a long fixed delay for every rule word.
      repeat (8) @(posedge clk);
      wr('h24, 0); wr('h28, a[31:0]); wr('h2c, a[63:32]);
      wr('h30, b[31:0]); wr('h34, b[63:32]);
      wr('h20, {job, 8'h01, 4'h0, cmd});
      poll('h10, 1, 1);
      repeat (8) @(posedge clk);
      rd('h10, value);
      if (!value[0]) $fatal(1, "Configuration ack was not held for software");
      wr('h20, {job, 8'h00, 4'h0, cmd});
      poll('h10, 'h00010001, 'h00010000);
    end
  endtask
  task configure;
    begin
      command(5, 0, 0, 0);
      command(1, 1, 'h1000, 'h2000);
      // OP_END, OP_END. The normal test uses 20 capture groups and two
      // output chunks; reduced HLS builds use two groups and one chunk.
      command(3, 1, 0, 4);
      command(3, 1, 8, {TEST_CPGP_NM, 32'd0, 16'd2});
      command(3, 1, 16, 64'h0100000000000000);
      command(3, 1, 24, 64'h0100000000000000);
      wr('h20, 0);
      repeat (128) @(posedge clk);
    end
  endtask
  task send_packet;
    begin
      @(negedge clk); tdata = 'h636261; tkeep = 7; tlast = 1;
      tuser = (65'h1001 << 1) | 1; tvalid = 1;
      @(posedge clk); while (!tready) @(posedge clk);
      @(negedge clk); tvalid = 0;
      $display("MMIO short packet accepted at time=%t", $time);
    end
  endtask
  task send_max_packet;
    integer beat;
    begin
      for (beat = 0; beat < 64; beat = beat + 1) begin
        @(negedge clk);
        tdata = {64{8'h61}}; tkeep = 64'hffffffffffffffff;
        // Keep the wide address assignment separate from the one-bit SOP.
        // XSim 2021.2 can leave upper bits unknown for a 65-bit OR with a
        // runtime one-bit expression.
        tuser = 65'h2002; tuser[0] = (beat == 0);
        tlast = (beat == 63); tvalid = 1;
        if (beat == 63) begin
          tdata[447:440] = 8'h63; // 4087 a bytes followed by c
          tdata[511:448] = 0; tkeep = 64'h00ffffffffffffff;
        end
        @(posedge clk); while (!tready) @(posedge clk);
      end
      @(negedge clk); tvalid = 0;
      $display("MMIO maximum packet accepted at time=%t", $time);
    end
  endtask
  task check_max_captures;
    integer chunk, lane, word_index;
    reg [31:0] result_word, expected_word;
    begin
      command(3, 1, 8, {16'd512, 16'd1, 32'd7});
      wr('h20, 0); send_packet();
      for (chunk = 0; chunk < 33; chunk = chunk + 1) begin
        poll('hc8, 'h00010101, 'h00000100);
        for (lane = 0; lane < 16; lane = lane + 1) begin
          word_index = chunk * 16 + lane;
          expected_word = word_index == 0 ? 1 : word_index == 1 ? 'h00030000 :
                          word_index <= 512 ? 32'hffffffff : 0;
          rd('h80 + lane * 4, result_word);
          if (result_word !== expected_word)
            $fatal(1, "Maximum capture mismatch word=%0d expected=%h got=%h", word_index, expected_word, result_word);
        end
        wr('h18, 1); poll('hc8, 'h00010101, 1);
        wr('h18, 0);
      end
      poll('hc8, 'h00010101, 0);
      command(3, 1, 8, {16'd1, 16'd1, 32'd7});
      check_class_result(1, 'h00030000);
    end
  endtask
  reg [31:0] value;
  integer i;
  initial begin
    repeat (8) @(negedge clk); reset_n = 1;
    // The AXI-Lite RAM windows are not reset.  Seed them before ap_start so
    // the persistent ingress cannot consume unknown configuration bits.
    wr('h18, 0); wr('h1c, 0);
    wr('h20, {16'd0, 8'h00, 4'h0, 4'd5});
    wr('h24, 0); wr('h28, 0); wr('h2c, 0); wr('h30, 0); wr('h34, 0);
    wr('h00, 1); // ap_start: the dataflow invocation then runs continuously.
    configure(); send_packet();
    // Payload lives at 0x80; address at 0xc0; ack/valid/halt bytes at 0xc8.
    poll('hc8, 'h00010101, 'h00000100);
    rd('hc0, value); if (value !== 'h1001) $fatal(1, "Wrong result address");
    for (i = 0; i < 16; i = i + 1) begin
      rd('h80 + i*4, value);
`ifdef RE_HLS_RTL_QUICK
      if (value !== (i == 0 ? 1 : i == 1 ? 0 : i == 2 ? 32'hffffffff : 0)) $fatal(1, "First chunk mismatch word %d", i);
`else
      if (value !== (i == 0 ? 1 : i == 1 ? 0 : 32'hffffffff)) $fatal(1, "First chunk mismatch word %d", i);
`endif
    end
    // The published result must remain stable across repeated kernel steps.
    for (i = 0; i < 100; i = i + 1) begin
      rd('h80, value); if (value !== 1) $fatal(1, "Pending payload was transiently overwritten");
      rd('hc8, value); if ((value & 'h00010101) !== 'h00000100) $fatal(1, "Pending metadata changed");
    end
    wr('h18, 1); poll('hc8, 'h00010101, 'h00000001);
    wr('h18, 0);
`ifndef RE_HLS_RTL_QUICK
    poll('hc8, 'h00010101, 'h00000100);
    for (i = 0; i < 16; i = i + 1) begin
      rd('h80 + i*4, value);
      if (value !== (i < 5 ? 32'hffffffff : 0)) $fatal(1, "Second chunk mismatch word %d", i);
    end
    repeat (500) @(posedge clk);
    rd('hc8, value); if (!value[8]) $fatal(1, "Second chunk was not held until acknowledgement");
    wr('h18, 1); poll('hc8, 'h00010101, 1);
    wr('h18, 0); poll('hc8, 'h00010101, 0);
`else
    // The reduced rule fits in one chunk, so dropping its acknowledgement
    // completes the record instead of revealing another chunk.
    poll('hc8, 'h00010101, 0);
`endif
    // Reset with an outstanding result, clear, and run again.
    send_packet(); poll('hc8, 'h100, 'h100);
    @(negedge clk); reset_n = 0;
    repeat (8) @(negedge clk); reset_n = 1;
    wr('h00, 1);
    configure(); send_packet(); poll('hc8, 'h00010101, 'h00000100);
    rd('h80, value); if (value !== 1) $fatal(1, "Warm reset recovery failed");
`ifndef RE_HLS_RTL_QUICK
    // Retire both chunks before reconfiguring this rule.
    wr('h18, 1); poll('hc8, 'h00010101, 1);
    wr('h18, 0); poll('hc8, 'h00010101, 'h00000100);
    wr('h18, 1); poll('hc8, 'h00010101, 1);
    wr('h18, 0); poll('hc8, 'h00010101, 0);
    configure_class_rule();
    $display("MMIO class rule configured at time=%t", $time);
    check_class_result(1, 'h00030000);
    check_class_result(1, 'h0ff80000, 1);
    command(3, 1, 80, 0); // complete-rule class edit
    check_class_result(0, 0);
    command(3, 1, 80, 64'h0000000600000000);
    command(3, 1, 48, 64'h0701640000000000); // literal d
    check_class_result(0, 0);
    command(3, 1, 48, 64'h0701630000000000); // literal c
    check_class_result(1, 'h00030000);
    command(3, 1, 24, 64'h3b10000600000000); // different loop exit: scalar fallback
    check_class_result(1, 'h00020000);
    command(3, 1, 24, 64'h3b10000400000000);
    check_class_result(1, 'h00030000);
    check_max_captures();
`endif
    $display("PASS production RTL: AXI configuration, DMA sidebands, stable MMIO, held acknowledgements, warm reset");
    $finish;
  end
endmodule
