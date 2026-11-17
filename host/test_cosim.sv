// C-reference/RTL regression for the persistent start-once reEngineKernel.
// test_cosim.cpp creates cosim_input.txt and cosim_expected.txt from the same
// log, regex compiler, and Oniguruma reference used by test.cpp.
`timescale 1ns/1ps

module test_cosim;
  // The synthesized reorder/correlation path has 64 slots. Use the complete
  // window so the source can keep all 16 PUs busy without allowing a message
  // ID to reuse a telemetry slot before its previous record is emitted.
  localparam integer PERF_INFLIGHT_LIMIT = 64;
  localparam real CLOCK_PERIOD_NS = 6.0;

  reg clk = 0;
  always #(CLOCK_PERIOD_NS / 2.0) clk = ~clk;

  reg reset_n = 0;
  reg [7:0] awaddr = 0;
  reg [7:0] araddr = 0;
  reg awvalid = 0;
  reg wvalid = 0;
  reg arvalid = 0;
  reg [31:0] wdata = 0;
  wire awready;
  wire wready;
  wire bvalid;
  wire arready;
  wire rvalid;
  wire [31:0] rdata;
  wire [1:0] bresp;
  wire [1:0] rresp;

  reg [511:0] tdata = 0;
  reg [63:0] tkeep = 0;
  reg [64:0] tuser = 0;
  reg tlast = 0;
  reg tvalid = 0;
  wire tready;
  wire [511:0] perf_tdata;
  wire perf_tvalid;
  wire perf_tready = 1'b1;

  integer sim_cycle = 0;
  always @(posedge clk) sim_cycle <= sim_cycle + 1;

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

  initial begin
    repeat (200000000) @(posedge clk);
    $fatal(1, "C/RTL watchdog: persistent RTL or bus transaction stalled");
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
      for (attempt = 0; attempt < 2000000; attempt = attempt + 1) begin
        axi_read(address, value);
        if ((value & mask) === expected) disable poll_loop;
      end
      $fatal(1, "Poll timeout addr=%h mask=%h expected=%h got=%h",
             address, mask, expected, value);
    end
  endtask

  task automatic configure_word(input [3:0] command,
                                input [15:0] job,
                                input [63:0] argument0,
                                input [63:0] argument1);
    reg [31:0] status;
    integer attempt;
    begin
      // Select the command with valid low, then publish stable arguments.
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
        for (attempt = 0; attempt < 2000000; attempt = attempt + 1) begin
          axi_read(8'h10, status);
          if (status[8])
            $fatal(1, "Configuration rejected cmd=%0d job=%0d", command, job);
          if (status[0]) disable wait_for_ack;
        end
        $fatal(1, "Configuration timeout cmd=%0d job=%0d", command, job);
      end

      // Completion must remain visible until valid is lowered.
      axi_read(8'h10, status);
      if (!status[0]) $fatal(1, "Configuration acknowledgement was not held");
      axi_write(8'h20, {job, 8'h00, 4'h0, command});
      poll(8'h10, 32'h00010001, 32'h00010000);
    end
  endtask

  task automatic send_beat(input [511:0] data,
                           input [63:0] keep,
                           input [63:0] address,
                           input first,
                           input last,
                           output integer stall_cycles);
    begin
      stall_cycles = 0;
      @(negedge clk);
      tdata = data;
      tkeep = keep;
      tuser = {address, first};
      tlast = last;
      tvalid = 1;
      @(posedge clk);
      while (tready !== 1'b1) begin
        stall_cycles = stall_cycles + 1;
        @(posedge clk);
      end
      // Keep TVALID asserted. The producer installs the next beat on the
      // immediately following falling edge, allowing consecutive beats and
      // consecutive messages to be accepted on consecutive rising edges.
    end
  endtask

  integer input_file;
  integer expected_file;
  integer scan_result;
  integer configuration_words;
  integer message_count;
  integer words_per_message;
  integer expected_message_count;
  integer expected_words_per_message;
  integer word;
  integer cycle_file;
  integer stall_file;
  integer perf_message_records = 0;
  integer perf_snapshot_records = 0;
  integer setup_start_cycle = 0;
  integer processing_start_cycle = 0;
  integer processing_end_cycle = 0;
  realtime setup_start_time = 0.0;
  realtime processing_start_time = 0.0;
  realtime processing_end_time = 0.0;
  integer total_dma_stall_cycles = 0;
  integer total_dma_stall_events = 0;
  integer total_source_wait_cycles = 0;
  integer messages_with_dma_stalls = 0;
  integer total_message_bytes = 0;
  integer total_dma_beats = 0;
  integer first_dma_accept_cycle = -1;
  integer last_dma_accept_cycle = -1;
  reg [63:0] configuration_word;
  reg [39:0] current_regexvm_cycles;
  reg [39:0] current_kernel_cycles;
  reg [39:0] min_regexvm_cycles = 40'hffffffffff;
  reg [39:0] max_regexvm_cycles = 0;
  reg [39:0] min_kernel_cycles = 40'hffffffffff;
  reg [39:0] max_kernel_cycles = 0;
  reg [63:0] total_regexvm_cycles = 0;
  reg [63:0] total_kernel_cycles = 0;
  reg [15:0] telemetry_correlator_drops = 0;

  initial begin
    cycle_file = $fopen("re_engine_kernel_cycles.csv", "w");
    stall_file = $fopen("re_engine_kernel_stalls.csv", "w");
    if (!cycle_file || !stall_file)
      $fatal(1, "Cannot create C/RTL timing reports");
    $fdisplay(cycle_file,
      "message_id,job_id,processing_unit,message_bytes,rule_bytes,dma_beats,regexVM_cycles,kernel_cycles,sop_cycle,iova_cycle,rule_cycle,ready_cycle,dispatch_cycle,pu_input_cycle,compute_start_cycle,compute_done_cycle,result_cycle,iova_cycles,rule_lookup_cycles,join_cycles,scheduler_cycles,pu_load_cycles,result_buffer_cycles,correlator_drops");
    $fdisplay(stall_file,
      "message_id,message_bytes,dma_beats,source_wait_cycles,dma_stall_events,dma_stall_cycles,boundary_stall_cycles,payload_stall_cycles,source_reason,dma_reason");
  end

  // The synthesized full kernel exposes compute-start/compute-done timestamps
  // in each type-1 performance record. The current HLS schedule places the
  // start marker before capture-offset initialization, so regexVM_cycles is a
  // PU setup-plus-VM interval. DMA SOP through result-buffered timestamps is
  // still the complete per-message kernel latency.
  always @(posedge clk) begin
    if (perf_tvalid && perf_tready) begin
      if (perf_tdata[3:0] == 1) begin
        current_regexvm_cycles =
          perf_tdata[403:364] - perf_tdata[363:324];
        current_kernel_cycles =
          perf_tdata[443:404] - perf_tdata[123:84];
        perf_message_records <= perf_message_records + 1;
        total_regexvm_cycles <= total_regexvm_cycles +
                                current_regexvm_cycles;
        total_kernel_cycles <= total_kernel_cycles + current_kernel_cycles;
        if (current_regexvm_cycles < min_regexvm_cycles)
          min_regexvm_cycles <= current_regexvm_cycles;
        if (current_regexvm_cycles > max_regexvm_cycles)
          max_regexvm_cycles <= current_regexvm_cycles;
        if (current_kernel_cycles < min_kernel_cycles)
          min_kernel_cycles <= current_kernel_cycles;
        if (current_kernel_cycles > max_kernel_cycles)
          max_kernel_cycles <= current_kernel_cycles;
        if (perf_tdata[475:460] > telemetry_correlator_drops)
          telemetry_correlator_drops <= perf_tdata[475:460];

        $fdisplay(cycle_file,
          "%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d",
          perf_tdata[35:4], perf_tdata[43:36], perf_tdata[51:44],
          perf_tdata[67:52], perf_tdata[83:68], perf_tdata[459:444],
          current_regexvm_cycles, current_kernel_cycles,
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
          perf_tdata[443:404] - perf_tdata[403:364],
          perf_tdata[475:460]);
        if ((perf_message_records + 1) % 100 == 0) $fflush(cycle_file);
      end else if (perf_tdata[3:0] == 2) begin
        perf_snapshot_records <= perf_snapshot_records + 1;
      end
    end
  end

  // Keep DMA production independent from result acknowledgement.  The old
  // single loop waited for each result before sending the next message, which
  // serialized a 16-PU kernel and made a 5k RTL regression take hours.  These
  // tasks use separate vector files and interfaces, while the kernel's reorder
  // path guarantees that the checked results remain in input order.
  task automatic drive_messages;
    integer producer_scan_result;
    integer producer_message;
    integer producer_beat;
    integer producer_message_length;
    integer producer_beat_count;
    integer producer_remaining_bytes;
    integer producer_beat_bytes;
    integer producer_beat_stall_cycles;
    integer producer_source_wait_cycles;
    integer producer_message_stall_cycles;
    integer producer_message_stall_events;
    integer producer_boundary_stall_cycles;
    integer producer_payload_stall_cycles;
    reg [63:0] producer_message_address;
    reg [511:0] producer_beat_data;
    reg [63:0] producer_beat_keep;
    reg [8*64-1:0] producer_source_reason;
    reg [8*64-1:0] producer_dma_reason;
    begin
      for (producer_message = 0; producer_message < message_count;
           producer_message = producer_message + 1) begin
        producer_source_wait_cycles = 0;
        producer_message_stall_cycles = 0;
        producer_message_stall_events = 0;
        producer_boundary_stall_cycles = 0;
        producer_payload_stall_cycles = 0;

        // Functional backpressure is expressed through TREADY. The only
        // source-side limit is the full 64-entry telemetry/reorder window.
        // Drop TVALID before waiting so the previously accepted final beat
        // cannot be sampled a second time.
        if (producer_message - perf_message_records >=
            PERF_INFLIGHT_LIMIT) begin
          @(negedge clk);
          tvalid = 0;
          tlast = 0;
          tuser = 0;
        end
        while (producer_message - perf_message_records >=
               PERF_INFLIGHT_LIMIT) begin
          @(posedge clk);
          producer_source_wait_cycles = producer_source_wait_cycles + 1;
        end
        total_source_wait_cycles = total_source_wait_cycles +
                                   producer_source_wait_cycles;

        producer_scan_result = $fscanf(input_file, "%d %h %d\n",
                                       producer_message_length,
                                       producer_message_address,
                                       producer_beat_count);
        if (producer_scan_result != 3)
          $fatal(1, "Missing message header %0d", producer_message);

        producer_remaining_bytes = producer_message_length;
        for (producer_beat = 0; producer_beat < producer_beat_count;
             producer_beat = producer_beat + 1) begin
          producer_scan_result = $fscanf(input_file, "%h\n",
                                         producer_beat_data);
          if (producer_scan_result != 1)
            $fatal(1, "Missing message %0d beat %0d",
                   producer_message, producer_beat);
          producer_beat_bytes = producer_remaining_bytes > 64 ?
                                64 : producer_remaining_bytes;
          producer_beat_keep = producer_beat_bytes == 64 ?
                               64'hffffffffffffffff :
                               ((64'h1 << producer_beat_bytes) - 1);
          send_beat(producer_beat_data, producer_beat_keep,
                    producer_message_address, producer_beat == 0,
                    producer_beat + 1 == producer_beat_count,
                    producer_beat_stall_cycles);

          if (first_dma_accept_cycle < 0)
            first_dma_accept_cycle = sim_cycle;
          last_dma_accept_cycle = sim_cycle;
          total_dma_beats = total_dma_beats + 1;
          if (producer_beat_stall_cycles != 0) begin
            producer_message_stall_events =
              producer_message_stall_events + 1;
            producer_message_stall_cycles =
              producer_message_stall_cycles + producer_beat_stall_cycles;
            total_dma_stall_events = total_dma_stall_events + 1;
            total_dma_stall_cycles = total_dma_stall_cycles +
                                     producer_beat_stall_cycles;
            if (producer_beat == 0)
              producer_boundary_stall_cycles =
                producer_boundary_stall_cycles +
                producer_beat_stall_cycles;
            else
              producer_payload_stall_cycles =
                producer_payload_stall_cycles +
                producer_beat_stall_cycles;
          end
          producer_remaining_bytes = producer_remaining_bytes -
                                     producer_beat_bytes;
        end

        total_message_bytes = total_message_bytes + producer_message_length;
        if (producer_message_stall_cycles != 0)
          messages_with_dma_stalls = messages_with_dma_stalls + 1;

        producer_source_reason = producer_source_wait_cycles == 0 ?
                                 "none" :
                                 "telemetry_correlation_window";
        if (producer_boundary_stall_cycles == 0 &&
            producer_payload_stall_cycles == 0)
          producer_dma_reason = "none";
        else if (producer_payload_stall_cycles == 0)
          producer_dma_reason =
            "packet_boundary_or_descriptor_queue_backpressure";
        else if (producer_boundary_stall_cycles == 0)
          producer_dma_reason = "payload_queue_backpressure";
        else
          producer_dma_reason = "mixed_boundary_and_payload_backpressure";

        $fdisplay(stall_file,
          "%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0s,%0s",
          producer_message, producer_message_length, producer_beat_count,
          producer_source_wait_cycles, producer_message_stall_events,
          producer_message_stall_cycles, producer_boundary_stall_cycles,
          producer_payload_stall_cycles, producer_source_reason,
          producer_dma_reason);
        $display(
          "C/RTL ingress: message=%0d bytes=%0d beats=%0d source_wait_cycles=%0d source_reason=%0s dma_stall_events=%0d dma_stall_cycles=%0d dma_reason=%0s",
          producer_message, producer_message_length, producer_beat_count,
          producer_source_wait_cycles, producer_source_reason,
          producer_message_stall_events, producer_message_stall_cycles,
          producer_dma_reason);
      end

      @(negedge clk);
      tvalid = 0;
      tlast = 0;
      tuser = 0;
      $fflush(stall_file);
    end
  endtask

  task automatic check_results;
    integer consumer_scan_result;
    integer consumer_message;
    integer consumer_chunk;
    integer consumer_word;
    integer consumer_chunk_words;
    reg [63:0] consumer_expected_address;
    reg [31:0] consumer_actual_word;
    reg [31:0] consumer_expected_word;
    reg [31:0] consumer_result_address_low;
    reg [31:0] consumer_result_address_high;
    reg consumer_expected_match;
    begin
      for (consumer_message = 0; consumer_message < message_count;
           consumer_message = consumer_message + 1) begin
        consumer_scan_result = $fscanf(expected_file, "%h\n",
                                       consumer_expected_address);
        if (consumer_scan_result != 1)
          $fatal(1, "Missing expected address for message %0d",
                 consumer_message);

        consumer_expected_match = 0;
        for (consumer_chunk = 0;
             consumer_chunk < (words_per_message + 15) / 16;
             consumer_chunk = consumer_chunk + 1) begin
          poll(8'hc8, 32'h00010101, 32'h00000100);
          axi_read(8'hc0, consumer_result_address_low);
          axi_read(8'hc4, consumer_result_address_high);
          if ({consumer_result_address_high, consumer_result_address_low} !==
              consumer_expected_address)
            $fatal(1, "Message %0d result address mismatch", consumer_message);

          consumer_chunk_words = words_per_message - consumer_chunk * 16;
          if (consumer_chunk_words > 16) consumer_chunk_words = 16;
          for (consumer_word = 0; consumer_word < consumer_chunk_words;
               consumer_word = consumer_word + 1) begin
            consumer_scan_result = $fscanf(expected_file, "%h\n",
                                           consumer_expected_word);
            if (consumer_scan_result != 1)
              $fatal(1, "Missing expected word for message %0d",
                     consumer_message);
            axi_read(8'h80 + consumer_word * 4, consumer_actual_word);
            if (consumer_chunk == 0 && consumer_word == 0)
              consumer_expected_match = consumer_expected_word[0];
            if ((consumer_chunk == 0 && consumer_word == 0) ||
                consumer_expected_match) begin
              if (consumer_actual_word !== consumer_expected_word)
                $fatal(1,
                       "Result mismatch message=%0d word=%0d expected=%h actual=%h",
                       consumer_message,
                       consumer_chunk * 16 + consumer_word,
                       consumer_expected_word, consumer_actual_word);
            end
          end

          axi_write(8'h18, 1);
          poll(8'hc8, 32'h00010101, 32'h00000001);
          axi_write(8'h18, 0);
          if (consumer_chunk + 1 == (words_per_message + 15) / 16)
            poll(8'hc8, 32'h00010101, 0);
        end

        if ((consumer_message + 1) % 100 == 0 ||
            consumer_message + 1 == message_count)
          $display("C/RTL progress: checked %0d/%0d messages",
                   consumer_message + 1, message_count);
      end
    end
  endtask

  task automatic wait_for_performance_records;
    integer attempt;
    begin : wait_for_perf
      for (attempt = 0; attempt < 2000000; attempt = attempt + 1) begin
        @(posedge clk);
        #1;
        if (perf_message_records == message_count) disable wait_for_perf;
      end
      $fatal(1, "Performance telemetry timeout: records=%0d expected=%0d",
             perf_message_records, message_count);
    end
  endtask

  initial begin
    setup_start_cycle = sim_cycle;
    setup_start_time = $realtime;
    input_file = $fopen("cosim_input.txt", "r");
    expected_file = $fopen("cosim_expected.txt", "r");
    if (!input_file || !expected_file)
      $fatal(1, "Cannot open C-generated CoSim vectors");

    scan_result = $fscanf(input_file, "%d %d %d\n",
                          configuration_words, message_count,
                          words_per_message);
    if (scan_result != 3) $fatal(1, "Malformed cosim_input.txt header");
    scan_result = $fscanf(expected_file, "%d %d\n",
                          expected_message_count,
                          expected_words_per_message);
    if (scan_result != 2 || expected_message_count != message_count ||
        expected_words_per_message != words_per_message)
      $fatal(1, "Input/reference vector headers disagree");

    repeat (8) @(negedge clk);
    reset_n = 1;

    // cfg_in and result_in synthesize as AXI-Lite RAM windows and are not
    // reset by HLS.  Initialize every word before starting the persistent
    // dataflow process so its first MMIO sample cannot contain X values.
    axi_write(8'h18, 0);
    axi_write(8'h1c, 0);
    axi_write(8'h20, {16'd0, 8'h00, 4'h0, 4'd5});
    axi_write(8'h24, 0);
    axi_write(8'h28, 0);
    axi_write(8'h2c, 0);
    axi_write(8'h30, 0);
    axi_write(8'h34, 0);
    axi_write(8'h00, 1); // Start the persistent dataflow exactly once.

    configure_word(4'd5, 0, 0, 0); // CLEAR
    for (word = 0; word < 8; word = word + 1)
      configure_word(4'd1, 1,
                     64'h10000000 + word * 64'h1000,
                     64'h10001000 + word * 64'h1000);

    for (word = 0; word < configuration_words; word = word + 1) begin
      scan_result = $fscanf(input_file, "%h\n", configuration_word);
      if (scan_result != 1)
        $fatal(1, "Missing rule word %0d", word);
      configure_word(4'd3, 1, word * 8, configuration_word);
    end

    // Return to runtime mode and require both tables to be ready.
    axi_write(8'h20, 0);
    poll(8'h10, 32'h01010000, 32'h01010000);

    processing_start_cycle = sim_cycle;
    processing_start_time = $realtime;
    $display(
      "C/RTL setup complete: setup_cycles=%0d setup_time_ns=%0.3f",
      processing_start_cycle - setup_start_cycle,
      processing_start_time - setup_start_time);

    fork
      drive_messages();
      check_results();
    join
    wait_for_performance_records();
    if (telemetry_correlator_drops != 0)
      $fatal(1, "Performance telemetry dropped %0d correlation events",
             telemetry_correlator_drops);

    processing_end_cycle = sim_cycle;
    processing_end_time = $realtime;

    $fclose(input_file);
    $fclose(expected_file);
    $fflush(cycle_file);
    $fclose(cycle_file);
    $fflush(stall_file);
    $fclose(stall_file);
    $display("C/RTL cycle summary: records=%0d regexVM_cycles[min=%0d avg=%0d max=%0d] kernel_cycles[min=%0d avg=%0d max=%0d]",
             perf_message_records,
             min_regexvm_cycles,
             total_regexvm_cycles / perf_message_records,
             max_regexvm_cycles,
             min_kernel_cycles,
             total_kernel_cycles / perf_message_records,
             max_kernel_cycles);
    $display(
      "C/RTL execution time: setup_cycles=%0d processing_cycles=%0d total_cycles=%0d setup_time_ns=%0.3f processing_time_ns=%0.3f total_time_ns=%0.3f nominal_clock_period_ns=%0.3f",
      processing_start_cycle - setup_start_cycle,
      processing_end_cycle - processing_start_cycle,
      processing_end_cycle - setup_start_cycle,
      processing_start_time - setup_start_time,
      processing_end_time - processing_start_time,
      processing_end_time - setup_start_time,
      CLOCK_PERIOD_NS);
    $display(
      "C/RTL DMA feed summary: messages=%0d bytes=%0d beats=%0d feed_cycles=%0d source_wait_cycles=%0d dma_stall_events=%0d dma_stall_cycles=%0d messages_with_dma_stalls=%0d",
      message_count, total_message_bytes, total_dma_beats,
      last_dma_accept_cycle - first_dma_accept_cycle + 1,
      total_source_wait_cycles, total_dma_stall_events,
      total_dma_stall_cycles, messages_with_dma_stalls);
    $display("PASS C/RTL log regression: %0d messages matched the test.cpp reference",
             message_count);
    $finish;
  end
endmodule
