// C-reference/RTL regression for the persistent start-once reEngineKernel.
// test_cosim.cpp creates cosim_input.txt and cosim_expected.txt from the same
// log, regex compiler, and Oniguruma reference used by test.cpp.
`timescale 1ns/1ps

module test_cosim;
  reg clk = 0;
  always #4.5 clk = ~clk;

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
    .dma_in_TREADY(tready)
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
                           input last);
    begin
      @(negedge clk);
      tdata = data;
      tkeep = keep;
      tuser = {address, first};
      tlast = last;
      tvalid = 1;
      @(posedge clk);
      while (!tready) @(posedge clk);
      @(negedge clk);
      tvalid = 0;
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
  integer message;
  integer word;
  integer beat;
  integer chunk;
  integer chunk_words;
  integer message_length;
  integer beat_count;
  integer remaining_bytes;
  integer beat_bytes;
  reg [63:0] configuration_word;
  reg [63:0] message_address;
  reg [511:0] beat_data;
  reg [63:0] beat_keep;
  reg [31:0] actual_word;
  reg [31:0] expected_word;
  reg [31:0] result_address_low;
  reg [31:0] result_address_high;
  reg expected_match;

  initial begin
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

    for (message = 0; message < message_count; message = message + 1) begin
      scan_result = $fscanf(input_file, "%d %h %d\n",
                            message_length, message_address, beat_count);
      if (scan_result != 3)
        $fatal(1, "Missing message header %0d", message);

      remaining_bytes = message_length;
      for (beat = 0; beat < beat_count; beat = beat + 1) begin
        scan_result = $fscanf(input_file, "%h\n", beat_data);
        if (scan_result != 1)
          $fatal(1, "Missing message %0d beat %0d", message, beat);
        beat_bytes = remaining_bytes > 64 ? 64 : remaining_bytes;
        beat_keep = beat_bytes == 64 ? 64'hffffffffffffffff :
                    ((64'h1 << beat_bytes) - 1);
        send_beat(beat_data, beat_keep, message_address,
                  beat == 0, beat + 1 == beat_count);
        remaining_bytes = remaining_bytes - beat_bytes;
      end

      expected_match = 0;
      for (chunk = 0; chunk < (words_per_message + 15) / 16;
           chunk = chunk + 1) begin
        poll(8'hc8, 32'h00010101, 32'h00000100);
        axi_read(8'hc0, result_address_low);
        axi_read(8'hc4, result_address_high);
        if ({result_address_high, result_address_low} !== message_address)
          $fatal(1, "Message %0d result address mismatch", message);

        chunk_words = words_per_message - chunk * 16;
        if (chunk_words > 16) chunk_words = 16;
        for (word = 0; word < chunk_words; word = word + 1) begin
          scan_result = $fscanf(expected_file, "%h\n", expected_word);
          if (scan_result != 1)
            $fatal(1, "Missing expected word for message %0d", message);
          axi_read(8'h80 + word * 4, actual_word);
          if (chunk == 0 && word == 0) expected_match = expected_word[0];
          if ((chunk == 0 && word == 0) || expected_match) begin
            if (actual_word !== expected_word)
              $fatal(1,
                     "Result mismatch message=%0d word=%0d expected=%h actual=%h",
                     message, chunk * 16 + word, expected_word, actual_word);
          end
        end

        axi_write(8'h18, 1);
        poll(8'hc8, 32'h00010101, 32'h00000001);
        axi_write(8'h18, 0);
        if (chunk + 1 == (words_per_message + 15) / 16)
          poll(8'hc8, 32'h00010101, 0);
      end
    end

    $fclose(input_file);
    $fclose(expected_file);
    $display("PASS C/RTL log regression: %0d messages matched the test.cpp reference",
             message_count);
    $finish;
  end
endmodule
