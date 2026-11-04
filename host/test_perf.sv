// Performance-only RTL testbench for the persistent reEngineKernel.
//
// It configures three synthetic linear rules and streams a 3 x 3 message/rule
// length matrix four times. Match values are logged but intentionally never
// compared: this bench measures the synthesized architecture, not correctness.
`timescale 1ns/1ps

module test_perf;
  localparam integer MESSAGE_COUNT = 36;
  localparam integer WORDS_PER_RESULT = 2;

  reg clk = 0;
  always #3.0 clk = ~clk;

  reg reset_n = 0;
  reg [7:0] awaddr = 0;
  reg [7:0] araddr = 0;
  reg awvalid = 0;
  reg wvalid = 0;
  reg arvalid = 0;
  reg [31:0] wdata = 0;
  wire awready, wready, bvalid, arready, rvalid;
  wire [31:0] rdata;
  wire [1:0] bresp, rresp;

  reg [511:0] tdata = 0;
  reg [63:0] tkeep = 0;
  reg [64:0] tuser = 0;
  reg tlast = 0;
  reg tvalid = 0;
  wire tready;

  wire [511:0] perf_tdata;
  wire perf_tvalid;
  reg perf_tready = 1;

  reEngineKernel dut (
    .ap_clk(clk),
    .ap_rst_n(reset_n),
    .s_axi_control_AWADDR(awaddr),
    .s_axi_control_AWVALID(awvalid),
    .s_axi_control_AWREADY(awready),
    .s_axi_control_WDATA(wdata),
    .s_axi_control_WSTRB(4'hf),
    .s_axi_control_WVALID(wvalid),
    .s_axi_control_WREADY(wready),
    .s_axi_control_BRESP(bresp),
    .s_axi_control_BVALID(bvalid),
    .s_axi_control_BREADY(1'b1),
    .s_axi_control_ARADDR(araddr),
    .s_axi_control_ARVALID(arvalid),
    .s_axi_control_ARREADY(arready),
    .s_axi_control_RDATA(rdata),
    .s_axi_control_RRESP(rresp),
    .s_axi_control_RVALID(rvalid),
    .s_axi_control_RREADY(1'b1),
    .dma_in_TDATA(tdata),
    .dma_in_TKEEP(tkeep),
    .dma_in_TSTRB(tkeep),
    .dma_in_TUSER(tuser),
    .dma_in_TLAST(tlast),
    .dma_in_TVALID(tvalid),
    .dma_in_TREADY(tready),
    .perf_out_TDATA(perf_tdata),
    .perf_out_TVALID(perf_tvalid),
    .perf_out_TREADY(perf_tready)
  );

  integer message_log;
  integer engine_log;
  integer workload_log;
  integer sim_cycle = 0;
  integer dma_stall_cycles = 0;
  integer dma_stall_events = 0;
  integer dma_stall_intervals_logged = 0;
  integer stall_start = 0;
  reg in_dma_stall = 0;
  integer perf_message_records = 0;
  integer perf_snapshot_records = 0;
  integer last_all_busy = 0;
  integer last_ingress_handoff = 0;
  integer last_scheduler_wait = 0;

  initial begin
    message_log = $fopen("perf_message_results.log", "w");
    engine_log = $fopen("perf_engine.log", "w");
    workload_log = $fopen("perf_workload.log", "w");
    if (!message_log || !engine_log || !workload_log)
      $fatal(1, "Cannot create performance logs");
    $fdisplay(message_log,
      "message_id address result_word0 result_word1 note=values_are_not_checked");
    $fdisplay(engine_log,
      "Performance telemetry. All values are synthesized-kernel cycles.");
    $fdisplay(engine_log,
      "MSG id job pu msgB ruleB beats sop iova rule ready dispatch input compute_start compute_done result iova_c rule_c join_c sched_c input_c compute_c result_c total_c correlator_drops");
    $fdisplay(workload_log,
      "message_id job_id message_bytes rule_instruction_words rule_image_bytes address");
  end

  always @(posedge clk) begin
    sim_cycle <= sim_cycle + 1;
    if (tvalid && !tready) begin
      dma_stall_cycles <= dma_stall_cycles + 1;
      if (!in_dma_stall) begin
        in_dma_stall <= 1;
        stall_start <= sim_cycle;
        dma_stall_events <= dma_stall_events + 1;
      end
    end else if (in_dma_stall) begin
      in_dma_stall <= 0;
      if (dma_stall_intervals_logged < 16) begin
        $fdisplay(engine_log,
          "DMA_STALL start=%0d end=%0d cycles=%0d cause=kernel_input_backpressure",
          stall_start, sim_cycle, sim_cycle - stall_start);
        dma_stall_intervals_logged <= dma_stall_intervals_logged + 1;
      end else if (dma_stall_intervals_logged == 16) begin
        $fdisplay(engine_log,
          "DMA_STALL further_intervals_suppressed; see DMA_SUMMARY totals");
        dma_stall_intervals_logged <= dma_stall_intervals_logged + 1;
      end
    end

    if (perf_tvalid && perf_tready) begin
      if (perf_tdata[3:0] == 1) begin
        perf_message_records <= perf_message_records + 1;
        $fdisplay(engine_log,
          "MSG %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d %0d",
          perf_tdata[35:4], perf_tdata[43:36], perf_tdata[51:44],
          perf_tdata[67:52], perf_tdata[83:68], perf_tdata[459:444],
          perf_tdata[123:84], perf_tdata[163:124],
          perf_tdata[203:164], perf_tdata[243:204],
          perf_tdata[283:244], perf_tdata[323:284],
          perf_tdata[363:324], perf_tdata[403:364],
          perf_tdata[443:404],
          perf_tdata[163:124] - perf_tdata[123:84],
          perf_tdata[203:164] - perf_tdata[163:124],
          perf_tdata[243:204] - perf_tdata[203:164],
          perf_tdata[283:244] - perf_tdata[243:204],
          perf_tdata[363:324] - perf_tdata[323:284],
          perf_tdata[403:364] - perf_tdata[363:324],
          perf_tdata[443:404] - perf_tdata[403:364],
          perf_tdata[443:404] - perf_tdata[123:84],
          perf_tdata[475:460]);
      end else if (perf_tdata[3:0] == 2) begin
        perf_snapshot_records <= perf_snapshot_records + 1;
        last_all_busy <= perf_tdata[427:404];
        last_ingress_handoff <= perf_tdata[451:428];
        last_scheduler_wait <= perf_tdata[475:452];
        $fdisplay(engine_log,
          "SNAP id=%0d cycle=%0d idle=[%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d] all_busy=%0d ingress_handoff=%0d scheduler_wait=%0d correlator_drops=%0d completed=%0d",
          perf_tdata[35:4], perf_tdata[83:36],
          perf_tdata[103:84], perf_tdata[123:104],
          perf_tdata[143:124], perf_tdata[163:144],
          perf_tdata[183:164], perf_tdata[203:184],
          perf_tdata[223:204], perf_tdata[243:224],
          perf_tdata[263:244], perf_tdata[283:264],
          perf_tdata[303:284], perf_tdata[323:304],
          perf_tdata[343:324], perf_tdata[363:344],
          perf_tdata[383:364], perf_tdata[403:384],
          perf_tdata[427:404], perf_tdata[451:428],
          perf_tdata[475:452], perf_tdata[491:476],
          perf_tdata[507:492]);
      end
    end
  end

  initial begin
    repeat (250000000) @(posedge clk);
    $fatal(1, "Performance RTL watchdog expired");
  end

  task automatic axi_write(input [7:0] address, input [31:0] value);
    begin
      @(negedge clk);
      awaddr = address;
      awvalid = 1;
      @(posedge clk);
      while (!awready) @(posedge clk);
      @(negedge clk);
      awvalid = 0;
      wdata = value;
      wvalid = 1;
      @(posedge clk);
      while (!wready) @(posedge clk);
      @(negedge clk);
      wvalid = 0;
      @(posedge clk);
      while (!bvalid) @(posedge clk);
      if (bresp !== 0) $fatal(1, "AXI-Lite write response error");
    end
  endtask

  task automatic axi_read(input [7:0] address, output [31:0] value);
    begin
      @(negedge clk);
      araddr = address;
      arvalid = 1;
      @(posedge clk);
      while (!arready) @(posedge clk);
      @(negedge clk);
      arvalid = 0;
      @(posedge clk);
      while (!rvalid) @(posedge clk);
      if (rresp !== 0) $fatal(1, "AXI-Lite read response error");
      value = rdata;
    end
  endtask

  task automatic poll(input [7:0] address,
                      input [31:0] mask,
                      input [31:0] expected);
    reg [31:0] value;
    integer attempt;
    begin : poll_loop
      value = 0;
      for (attempt = 0; attempt < 3000000; attempt = attempt + 1) begin
        axi_read(address, value);
        if ((value & mask) === expected) disable poll_loop;
      end
      $fatal(1, "Poll timeout addr=%h expected=%h got=%h",
             address, expected, value);
    end
  endtask

  task automatic configure_word(input [3:0] command,
                                input [15:0] job,
                                input [63:0] argument0,
                                input [63:0] argument1);
    reg [31:0] status;
    integer attempt;
    begin
      axi_write(8'h20, {job, 8'h00, 4'h0, command});
      repeat (8) @(posedge clk);
      axi_write(8'h24, 0);
      axi_write(8'h28, argument0[31:0]);
      axi_write(8'h2c, argument0[63:32]);
      axi_write(8'h30, argument1[31:0]);
      axi_write(8'h34, argument1[63:32]);
      axi_write(8'h20, {job, 8'h01, 4'h0, command});
      begin : wait_for_ack
        status = 0;
        for (attempt = 0; attempt < 3000000; attempt = attempt + 1) begin
          axi_read(8'h10, status);
          if (status[8])
            $fatal(1, "Configuration rejected cmd=%0d job=%0d", command, job);
          if (status[0]) disable wait_for_ack;
        end
        $fatal(1, "Configuration timeout cmd=%0d job=%0d", command, job);
      end
      axi_write(8'h20, {job, 8'h00, 4'h0, command});
      poll(8'h10, 32'h00010001, 32'h00010000);
    end
  endtask

  task automatic configure_rule(input [15:0] job,
                                input integer instruction_words);
    integer word;
    reg [63:0] instruction;
    begin
      configure_word(4'd3, job, 0, instruction_words + 2);
      configure_word(4'd3, job, 8,
                     (64'd1 << 48) | instruction_words);
      for (word = 0; word < instruction_words; word = word + 1) begin
        if (word == 0)
          instruction = 64'h2300000000000000; // OP_BEGIN_BUF
        else if (word >= instruction_words - 2)
          instruction = 64'h0100000000000000; // OP_END + read guard
        else
          instruction = 64'h1400000000000000; // OP_ANYCHAR
        configure_word(4'd3, job, (word + 2) * 8, instruction);
      end
    end
  endtask

  task automatic send_message(input integer message_id,
                              input integer job,
                              input integer message_bytes,
                              input [63:0] address,
                              input integer rule_words);
    integer beat;
    integer byte_index;
    integer beat_count;
    integer bytes_this_beat;
    reg [511:0] beat_data;
    reg [63:0] beat_keep;
    begin
      beat_count = (message_bytes + 63) / 64;
      $fdisplay(workload_log, "%0d %0d %0d %0d %0d %016h",
                message_id, job, message_bytes, rule_words,
                (rule_words + 2) * 8, address);
      for (beat = 0; beat < beat_count; beat = beat + 1) begin
        beat_data = 0;
        for (byte_index = 0; byte_index < 64; byte_index = byte_index + 1)
          beat_data[byte_index * 8 +: 8] = 8'h60 + job;
        bytes_this_beat = message_bytes - beat * 64;
        if (bytes_this_beat > 64) bytes_this_beat = 64;
        beat_keep = bytes_this_beat == 64 ? 64'hffffffffffffffff :
                    ((64'h1 << bytes_this_beat) - 1);
        @(negedge clk);
        tdata = beat_data;
        tkeep = beat_keep;
        tuser = {address, beat == 0};
        tlast = beat + 1 == beat_count;
        tvalid = 1;
        @(posedge clk);
        while (!tready) @(posedge clk);
      end
      @(negedge clk);
      tvalid = 0;
      tlast = 0;
      tuser = 0;
    end
  endtask

  integer repeat_index;
  integer job_index;
  integer length_index;
  integer message_id;
  integer message_length;
  integer rule_words;
  integer message;
  integer wait_attempt;
  reg [63:0] message_address;
  reg [31:0] result_address_low;
  reg [31:0] result_address_high;
  reg [31:0] result_word0;
  reg [31:0] result_word1;

  initial begin
    repeat (8) @(negedge clk);
    reset_n = 1;

    // Initialize the AXI-Lite RAM windows before starting the persistent top.
    axi_write(8'h18, 0);
    axi_write(8'h1c, 0);
    axi_write(8'h20, {16'd0, 8'h00, 4'h0, 4'd5});
    axi_write(8'h24, 0);
    axi_write(8'h28, 0);
    axi_write(8'h2c, 0);
    axi_write(8'h30, 0);
    axi_write(8'h34, 0);
    axi_write(8'h00, 1);

    configure_word(4'd5, 0, 0, 0);
    configure_word(4'd1, 1, 64'h1000, 64'h2000);
    configure_word(4'd1, 2, 64'h2000, 64'h3000);
    configure_word(4'd1, 3, 64'h3000, 64'h4000);
    configure_rule(1, 8);    // 80-byte short rule image
    configure_rule(2, 64);   // 528-byte medium rule image
    configure_rule(3, 192);  // 1552-byte long rule image

    axi_write(8'h20, 0);
    poll(8'h10, 32'h01010000, 32'h01010000);

    // Four repetitions expose both the 3x3 length matrix and 16-PU overlap.
    message_id = 0;
    for (repeat_index = 0; repeat_index < 4; repeat_index = repeat_index + 1) begin
      for (job_index = 1; job_index <= 3; job_index = job_index + 1) begin
        for (length_index = 0; length_index < 3; length_index = length_index + 1) begin
          case (length_index)
            0: message_length = 256;
            1: message_length = 1024;
            default: message_length = 4088;
          endcase
          case (job_index)
            1: rule_words = 8;
            2: rule_words = 64;
            default: rule_words = 192;
          endcase
          message_address = job_index * 64'h1000 + message_id + 1;
          send_message(message_id, job_index, message_length,
                       message_address, rule_words);
          message_id = message_id + 1;
        end
      end
    end

    // Results stay ordered. Log every returned word; do not compare it.
    for (message = 0; message < MESSAGE_COUNT; message = message + 1) begin
      poll(8'hc8, 32'h00010101, 32'h00000100);
      axi_read(8'hc0, result_address_low);
      axi_read(8'hc4, result_address_high);
      axi_read(8'h80, result_word0);
      axi_read(8'h84, result_word1);
      $fdisplay(message_log, "%0d %08h%08h %08h %08h",
                message, result_address_high, result_address_low,
                result_word0, result_word1);
      axi_write(8'h18, 1);
      poll(8'hc8, 32'h00010101, 32'h00000001);
      axi_write(8'h18, 0);
      poll(8'hc8, 32'h00010101, 0);
    end

    begin : wait_for_telemetry
      for (wait_attempt = 0; wait_attempt < 2000000;
           wait_attempt = wait_attempt + 1) begin
        @(posedge clk);
        if (perf_message_records == MESSAGE_COUNT &&
            perf_snapshot_records == MESSAGE_COUNT)
          disable wait_for_telemetry;
      end
      $fatal(1, "Telemetry timeout records=%0d snapshots=%0d",
             perf_message_records, perf_snapshot_records);
    end

    if (in_dma_stall)
      $fdisplay(engine_log,
        "DMA_STALL start=%0d end=%0d cycles=%0d cause=kernel_input_backpressure",
        stall_start, sim_cycle, sim_cycle - stall_start);
    $fdisplay(engine_log,
      "DMA_SUMMARY stall_events=%0d stall_cycles=%0d average_interval=%0d interface_condition=TVALID&&!TREADY",
      dma_stall_events, dma_stall_cycles,
      dma_stall_events == 0 ? 0 : dma_stall_cycles / dma_stall_events);
    $fdisplay(engine_log,
      "ENGINE_SUMMARY all_busy=%0d ingress_handoff=%0d scheduler_wait=%0d",
      last_all_busy, last_ingress_handoff, last_scheduler_wait);
    $fdisplay(engine_log,
      "INTERPRETATION repeated per-beat stalls expose ingress service interval; scheduler_wait with all_busy=0 exposes serialized dispatcher payload forwarding; nonzero all_busy exposes PU saturation; per-PU idle counters show load balance.");
    $fclose(message_log);
    $fclose(engine_log);
    $fclose(workload_log);
    $display("PASS performance run (correctness intentionally unchecked): messages=%0d DMA_stall_cycles=%0d",
             MESSAGE_COUNT, dma_stall_cycles);
    $finish;
  end
endmodule
