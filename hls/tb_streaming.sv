// Verify completion-driven collection, decoupled ingress/routing, and bounded
// backpressure against the production post-synthesis AXI interfaces.
`timescale 1ns/1ps
module tb_streaming;
  localparam integer MAX_GROUPS = 512;
  localparam integer BURST_COUNT = 80;
  reg clk = 0;
  always #2 clk = ~clk;
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
    .dma_in_TUSER(tuser), .dma_in_TLAST(tlast), .dma_in_TVALID(tvalid), .dma_in_TREADY(tready)
  );
  initial begin
    repeat (4000000) @(posedge clk);
    $fatal(1, "Streaming watchdog: design or bus transaction stalled");
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
      repeat (128) @(posedge clk);
      wr('h24, 0); wr('h28, a[31:0]); wr('h2c, a[63:32]);
      wr('h30, b[31:0]); wr('h34, b[63:32]);
      wr('h20, {job, 8'h01, 4'h0, cmd});
      poll('h10, 1, 1);
      repeat (200) @(posedge clk);
      rd('h10, value);
      if (!value[0]) $fatal(1, "Configuration ack was not held for software");
      wr('h20, {job, 8'h00, 4'h0, cmd});
      poll('h10, 'h00010001, 'h00010000);
    end
  endtask

  integer cycle = 0;
  integer dma_stall_cycles = 0;
  integer body_beats = 0;
  integer longest_body_run = 0;
  integer body_run = 0;
  integer next_id = 0;
  integer reset_epoch = 0;
  integer groups_by_id [0:255];
  integer length_by_id [0:255];
  reg [63:0] address_by_id [0:255];
  bit valid_by_id [0:255];
  bit seen_by_id [0:255];
  integer result_log;

  always @(posedge clk) begin
    cycle = cycle + 1;
    if (tvalid && !tready) dma_stall_cycles = dma_stall_cycles + 1;
    if (tvalid && tready && !tuser[0]) begin
      body_beats = body_beats + 1;
      body_run = body_run + 1;
      if (body_run > longest_body_run) longest_body_run = body_run;
    end else body_run = 0;
  end

  task load_end_rule(input integer job, input integer groups);
    begin
      command(3, job, 0, 4);
      command(3, job, 8, (64'(groups) << 48) | 64'd2);
      command(3, job, 16, 64'h0100000000000000);
      command(3, job, 24, 64'h0100000000000000);
    end
  endtask

  task configure;
    begin
      command(5, 0, 0, 0);
      command(1, 1, 'h1000, 'h2000);
      command(1, 2, 'h2000, 'h3000);
      command(1, 3, 'h3000, 'h4000);
      // Slow rule: BEGIN_BUF, ANYCHAR_STAR, END, END read guard.
      command(3, 1, 0, 6);
      command(3, 1, 8, (64'd1 << 48) | 4);
      command(3, 1, 16, 64'h2300000000000000);
      command(3, 1, 24, 64'h1600000000000000);
      command(3, 1, 32, 64'h0100000000000000);
      command(3, 1, 40, 64'h0100000000000000);
      load_end_rule(2, 20);
      load_end_rule(3, MAX_GROUPS);
      wr('h20, 0);
      poll('h10, 'h01010000, 'h01010000);
    end
  endtask

  task send_packet(input integer length, input [63:0] address,
                   input integer groups, input bit valid_packet,
                   input bit sparse_keep);
    integer b;
    integer bytes;
    integer id;
    begin
      id = next_id;
      next_id = next_id + 1;
      groups_by_id[id] = groups;
      length_by_id[id] = length;
      address_by_id[id] = address;
      valid_by_id[id] = valid_packet;
      for (b = 0; b < (length + 63)/64; b = b + 1) begin
        bytes = length - b*64;
        if (bytes > 64) bytes = 64;
        @(negedge clk);
        tdata = {64{8'h61}};
        tkeep = bytes == 64 ? 64'hffffffffffffffff :
                             ((64'h1 << bytes) - 1);
        if (sparse_keep && b == 0) tkeep = 5;
        tlast = b + 1 == (length + 63)/64;
        tuser = {address, b == 0};
        tvalid = 1;
        @(posedge clk);
        while (!tready) @(posedge clk);
      end
      @(negedge clk);
      tvalid = 0;
    end
  endtask

  task consume_record(input integer required_id);
    reg [31:0] id;
    reg [31:0] address_lo, address_hi, actual, expected, check_id;
    reg [31:0] held_word;
    integer chunk, word, global_word, total;
    integer first_cycle;
    begin
      poll('hc8, 'h00010101, 'h00000100);
      first_cycle = cycle;
      rd('hcc, id);
      if (id >= next_id || !valid_by_id[id] || seen_by_id[id])
        $fatal(1, "Unexpected, dropped, or duplicate message ID %0d", id);
      if (required_id >= 0 && id != required_id)
        $fatal(1, "Completion-order failure: required ID %0d, got %0d",
               required_id, id);
      total = groups_by_id[id] + 1;
      for (chunk = 0; chunk < (total+15)/16; chunk = chunk + 1) begin
        poll('hc8, 'h00010101, 'h00000100);
        rd('hc0, address_lo); rd('hc4, address_hi); rd('hcc, check_id);
        if ({address_hi,address_lo} !== address_by_id[id] || check_id !== id)
          $fatal(1, "Interleaved record metadata id=%0d chunk=%0d", id, chunk);
        for (word = 0; word < 16; word = word + 1) begin
          global_word = 16*chunk + word;
          expected = global_word >= total ? 0 :
                     global_word == 0 ? 1 :
                     global_word == 1 ?
                       (address_by_id[id] < 'h2000 ? length_by_id[id] << 16 : 0) :
                       32'hffffffff;
          rd('h80 + 4*word, actual);
          if (actual !== expected)
            $fatal(1, "Payload mismatch id=%0d word=%0d expected=%h actual=%h",
                   id, global_word, expected, actual);
        end
        if (chunk == 0) begin
          rd('h80, held_word);
          repeat (97) @(posedge clk);
          rd('h80, actual);
          rd('hcc, check_id);
          if (actual !== held_word || check_id !== id)
            $fatal(1, "Pending result changed under delayed acknowledgement");
        end
        wr('h18, 1);
        poll('hc8, 'h00010101, 1);
        // Holding ACK high must not discard a second chunk.
        repeat (41) @(posedge clk);
        rd('hc8, actual);
        if ((actual & 'h00010101) !== 1)
          $fatal(1, "Held ACK skipped a result chunk");
        wr('h18, 0);
      end
      seen_by_id[id] = 1;
      $fdisplay(result_log, "%0d,%0d,%0d,%h,%0d,%0d", reset_epoch, id,
                first_cycle, address_by_id[id], length_by_id[id], total);
      $fflush(result_log);
    end
  endtask

  integer n;
  reg [31:0] value;
  initial begin
    result_log = $fopen("streaming_results.csv", "w");
    $fdisplay(result_log, "reset_epoch,message_id,first_observed_cycle,address,message_bytes,result_words");
    repeat (8) @(negedge clk);
    reset_n = 1;
    wr('h18, 0); wr('h1c, 0);
    wr('h20, {16'd0, 8'h00, 4'h0, 4'd5});
    wr('h24, 0); wr('h28, 0); wr('h2c, 0); wr('h30, 0); wr('h34, 0);
    wr('h00, 1);
    configure();

    send_packet(4088, 'h1001, 1, 1, 0); // ID 0, deliberately slow.
    send_packet(3, 'h2001, 20, 1, 0);   // ID 1, finishes first.
    consume_record(1);
    consume_record(0);
    $display("PASS short job overtook slow job; no serial-number HOL blocking");

    // These SOP packets consume IDs but must not produce results.
    send_packet(65, 'h3001, MAX_GROUPS, 0, 1); // Invalid KEEP, ID 2.
    send_packet(3, 'h4001, MAX_GROUPS, 0, 0);  // IOVA miss, ID 3.
    send_packet(4089, 'h3001, MAX_GROUPS, 0, 0); // Length rejection, ID 4.

    // Multiple PU reuse rounds, repeated addresses, 33-chunk records, and a
    // consumer slower than ingress. Backpressure must neither lose nor mix data.
    fork
      begin : producer
        integer p;
        for (p = 0; p < BURST_COUNT; p = p + 1)
          send_packet(p % 3 == 0 ? 4088 : p % 3 == 1 ? 65 : 3,
                      p < 16 ? 'h3001 : 'h2001,
                      p < 16 ? MAX_GROUPS : 20, 1, 0);
      end
      begin : consumer
        integer c;
        repeat (2000) @(posedge clk);
        for (c = 0; c < BURST_COUNT; c = c + 1) consume_record(-1);
      end
    join
    repeat (500) @(posedge clk);
    rd('hc8, value);
    if (value[8]) $fatal(1, "Unexpected residual result");
    for (n = 0; n < next_id; n = n + 1)
      if (seen_by_id[n] != valid_by_id[n])
        $fatal(1, "Missing/extra result for ID %0d", n);
    if (longest_body_run < 32)
      $fatal(1, "DMA capture failed to stream one beat per cycle: longest run=%0d",
             longest_body_run);
    $display("DMA: stalls=%0d accepted_body_beats=%0d longest_body_run=%0d",
             dma_stall_cycles, body_beats, longest_body_run);

    // Reset while a result is pending, then check packet-boundary quiescence,
    // rule replica configuration, queue reset and sequence-counter reset.
    send_packet(3, 'h2001, 20, 1, 0);
    poll('hc8, 'h100, 'h100);
    @(negedge clk); reset_n = 0;
    repeat (8) @(negedge clk); reset_n = 1;
    wr('h00, 1);
    next_id = 0;
    reset_epoch = reset_epoch + 1;
    for (n = 0; n < 256; n = n + 1) begin
      seen_by_id[n] = 0;
      valid_by_id[n] = 0;
    end
    configure();
    send_packet(3, 'h2001, 20, 1, 0);
    consume_record(0);
    $fclose(result_log);
    $display("PASS production RTL streaming: completion order, %0d burst records (16 max captures), drops, backpressure, reset",
             BURST_COUNT);
    $finish;
  end
endmodule
