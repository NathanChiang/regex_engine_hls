`timescale 1ns/1ps

module test_new_accel_apache_rtl;
  localparam integer MAX_CONFIG_WORDS = 512;
  localparam integer MAX_RESULTS = 64;
  localparam integer VM_TIMEOUT_CYCLES = 20000;

  reg clk = 0;
  always #3 clk = ~clk;

  reg reset = 1;
  reg start = 0;
  integer rtl_cycle = 0;
  always @(posedge clk) rtl_cycle = rtl_cycle + 1;

  reg [255:0] bitset_mem [0:127];
  reg [511:0] instruction_mem [0:255];
  reg [511:0] message_mem [0:63];
  reg [15:0] offset_mem [0:511];
  reg [63:0] config_mem [0:MAX_CONFIG_WORDS-1];
  reg [31:0] expected_word [0:MAX_RESULTS-1];

  wire [6:0] bitset_address0;
  wire bitset_ce0;
  reg [255:0] bitset_q0;
  wire [6:0] bitset_address1;
  wire bitset_ce1;
  reg [255:0] bitset_q1;
`ifdef RE_VM_PREDECODED
  wire [7:0] instruction_address0, instruction_address1;
`else
  wire [4:0] instruction_address0, instruction_address1;
`endif
  wire instruction_ce0, instruction_ce1;
  reg [511:0] instruction_q0, instruction_q1;
  wire [5:0] message_address0, message_address1;
  wire message_ce0, message_ce1;
  reg [511:0] message_q0, message_q1;
  wire [8:0] offset_address0, offset_address1;
  wire offset_ce0, offset_ce1, offset_we0, offset_we1;
  wire [15:0] offset_d0, offset_d1;
  reg [15:0] offset_q0, offset_q1;
  wire done, idle, ready;
  wire [31:0] match_result;
  reg [31:0] message_length = 0;

  always @(posedge clk) begin
    if (bitset_ce0) begin
      bitset_q0 <= bitset_mem[bitset_address0];
    end
    if (bitset_ce1) bitset_q1 <= bitset_mem[bitset_address1];
    if (instruction_ce0)
      instruction_q0 <= instruction_mem[instruction_address0];
    if (instruction_ce1)
      instruction_q1 <= instruction_mem[instruction_address1];
    if (message_ce0) message_q0 <= message_mem[message_address0];
    if (message_ce1) message_q1 <= message_mem[message_address1];
    if (offset_ce0) begin
      offset_q0 <= offset_mem[offset_address0];
      if (offset_we0) offset_mem[offset_address0] <= offset_d0;
    end
    if (offset_ce1) begin
      offset_q1 <= offset_mem[offset_address1];
      if (offset_we1) offset_mem[offset_address1] <= offset_d1;
    end
  end

  regex_vm_new_accel_perf dut (
    .ap_clk(clk), .ap_rst(reset), .ap_start(start),
    .ap_done(done), .ap_idle(idle), .ap_ready(ready),
    .ap_local_block(), .ap_local_deadlock(),
    .bitset_address0(bitset_address0), .bitset_ce0(bitset_ce0),
    .bitset_q0(bitset_q0),
`ifdef RE_VM_TWO_CLASS_PORTS
    .bitset_address1(bitset_address1), .bitset_ce1(bitset_ce1), .bitset_q1(bitset_q1),
`endif
    .instructions_address0(instruction_address0),
    .instructions_ce0(instruction_ce0), .instructions_q0(instruction_q0),
`ifndef RE_VM_PREDECODED
    .instructions_address1(instruction_address1),
    .instructions_ce1(instruction_ce1), .instructions_q1(instruction_q1),
`endif
    .message_address0(message_address0), .message_ce0(message_ce0),
    .message_q0(message_q0), .message_address1(message_address1),
    .message_ce1(message_ce1), .message_q1(message_q1),
    .length_r(message_length), .ap_return(match_result),
    .offsets_address0(offset_address0), .offsets_ce0(offset_ce0),
    .offsets_q0(offset_q0), .offsets_we0(offset_we0), .offsets_d0(offset_d0),
    .offsets_address1(offset_address1), .offsets_ce1(offset_ce1),
    .offsets_we1(offset_we1), .offsets_d1(offset_d1), .offsets_q1(offset_q1)
  );

  task automatic run_vm(output integer execution_cycles);
    integer launch_cycle;
    integer elapsed;
    begin
      while (!idle) @(posedge clk);
      @(negedge clk);
      start = 1;
      @(posedge clk);
      #1 launch_cycle = rtl_cycle;
      @(negedge clk);
      start = 0;
      elapsed = 0;
      while (!done && elapsed < VM_TIMEOUT_CYCLES) begin
        @(posedge clk);
        #1 elapsed = elapsed + 1;
      end
      if (!done)
        $fatal(1, "regexVM_new RTL timeout after %0d cycles", elapsed);
      // ap_done may coincide with the final capture-memory write.  Sample the
      // external memory after its nonblocking write has committed.
      @(posedge clk);
      #1;
      execution_cycles = rtl_cycle - launch_cycle;
    end
  endtask

  integer input_fd;
  integer expected_fd;
  integer cycle_fd;
  integer scan_result;
  integer config_words;
  integer message_count;
  integer expected_message_count;
  integer result_words;
  integer expected_result_words;
  integer instruction_count;
  integer class_count;
  integer capture_count;
  integer message_id;
  integer length_value;
  integer beat_count;
  integer beat;
  integer lane;
  integer i;
  integer execution_cycles;
  reg [63:0] input_address;
  reg [63:0] expected_address;
  reg [511:0] payload;

  initial begin
    input_fd = $fopen("cosim_input.txt", "r");
    expected_fd = $fopen("cosim_expected.txt", "r");
    cycle_fd = $fopen("regex_vm_new_accel_apache_rtl_cycles.csv", "w");
    if (!input_fd || !expected_fd || !cycle_fd)
      $fatal(1, "Cannot open regexVM_new Apache vector/output files");

    scan_result = $fscanf(input_fd, "%d %d %d", config_words,
                          message_count, result_words);
    if (scan_result != 3 || config_words > MAX_CONFIG_WORDS)
      $fatal(1, "Invalid Apache input header");
    for (i = 0; i < config_words; i = i + 1) begin
      scan_result = $fscanf(input_fd, "%h", config_mem[i]);
      if (scan_result != 1) $fatal(1, "Invalid configuration word %0d", i);
    end

    instruction_count = config_mem[1][31:0];
    class_count = config_mem[1][47:32];
    capture_count = config_mem[1][63:48];
    if (instruction_count > 256 || class_count * 8 > 1024)
      $fatal(1, "Apache rule exceeds standalone memories");

`ifdef RE_VM_PREDECODED
    $readmemh("instruction_rows.txt", instruction_mem);
`else
    for (i = 0; i < 32; i = i + 1)
      instruction_mem[i] = {8{64'h0100000000000000}};
    for (i = 0; i < instruction_count; i = i + 1)
      instruction_mem[i >> 3][64*(i & 7) +: 64] = config_mem[2 + i];
`endif
    for (i = 0; i < 128; i = i + 1) bitset_mem[i] = 0;
    for (i = 0; i < class_count * 4; i = i + 1) begin
      bitset_mem[i >> 2][64*(i & 3) +: 64] = config_mem[2 + instruction_count + i];
    end
    $display("RTL rule: instructions=%0d classes=%0d captures=%0d first=%h second=%h",
             instruction_count, class_count, capture_count,
             instruction_mem[0], instruction_mem[1]);

    scan_result = $fscanf(expected_fd, "%d %d", expected_message_count,
                          expected_result_words);
    if (scan_result != 2 || expected_message_count != message_count ||
        expected_result_words != result_words || result_words > MAX_RESULTS)
      $fatal(1, "Input/reference headers disagree");

    $fdisplay(cycle_fd, "message_id,message_bytes,regexVM_new_accel_cycles");
    repeat (8) @(posedge clk);
    @(negedge clk);
    reset = 0;

    for (message_id = 0; message_id < message_count;
         message_id = message_id + 1) begin
      for (i = 0; i < 64; i = i + 1) message_mem[i] = 0;
      for (i = 0; i < 512; i = i + 1) offset_mem[i] = 16'hffff;

      scan_result = $fscanf(input_fd, "%d %h %d", length_value,
                            input_address, beat_count);
      if (scan_result != 3) $fatal(1, "Invalid message header %0d", message_id);
      for (beat = 0; beat < beat_count; beat = beat + 1) begin
        scan_result = $fscanf(input_fd, "%h", payload);
        if (scan_result != 1) $fatal(1, "Invalid payload %0d", message_id);
        message_mem[beat] = payload;
      end

      scan_result = $fscanf(expected_fd, "%h", expected_address);
      if (scan_result != 1 || expected_address != input_address)
        $fatal(1, "Reference address mismatch message %0d", message_id);
      for (i = 0; i < result_words; i = i + 1) begin
        scan_result = $fscanf(expected_fd, "%h", expected_word[i]);
        if (scan_result != 1) $fatal(1, "Invalid reference message %0d", message_id);
      end

      message_length = length_value;
      run_vm(execution_cycles);
      if (match_result[1:0] !== expected_word[0][1:0])
        $fatal(1, "Match mismatch message=%0d expected=%0d actual=%0d",
               message_id, expected_word[0][1:0], match_result[1:0]);
      for (i = 0; i < capture_count; i = i + 1) begin
        if (offset_mem[2*i] !== expected_word[i+1][15:0] ||
            offset_mem[2*i+1] !== expected_word[i+1][31:16])
          $fatal(1, "Capture mismatch message=%0d capture=%0d expected=%h actual=%h%h",
                 message_id, i, expected_word[i+1], offset_mem[2*i+1],
                 offset_mem[2*i]);
      end
      $fdisplay(cycle_fd, "%0d,%0d,%0d", message_id, length_value,
                execution_cycles);
      $fflush(cycle_fd);
    end

    $fclose(cycle_fd);
    $fclose(expected_fd);
    $fclose(input_fd);
    $display("PASS regexVM_new Apache RTL differential: messages=%0d captures=%0d",
             message_count, capture_count);
    $finish;
  end
endmodule
