`timescale 1ns/1ps

module test_new_perf_rtl;
  localparam integer SAMPLE_COUNT = 200;
  localparam integer MIN_INSTRUCTIONS = 2;
  localparam integer MAX_RULE_BYTES = 2048;
  localparam integer RULE_HEADER_WORDS = 2;
  localparam integer MAX_INSTRUCTIONS =
    (MAX_RULE_BYTES / 8) - RULE_HEADER_WORDS;
  localparam integer MESSAGE_BYTES = 4096;

  localparam [63:0] INST_END       = 64'h0100000000000000;
  localparam [63:0] INST_ANYCHAR   = 64'h1400000000000000;
  localparam [63:0] INST_BEGIN_BUF = 64'h2300000000000000;

  reg clk = 0;
  always #3 clk = ~clk;

  reg reset = 1;
  reg start = 0;
  integer rtl_cycle = 0;
  always @(posedge clk) rtl_cycle = rtl_cycle + 1;

  reg [31:0] bitset_mem [0:1023];
  reg [63:0] instruction_mem [0:4095];
  reg [31:0] message_mem [0:1023];
  reg [15:0] offset_mem [0:511];

  wire [9:0] bitset_address0, bitset_address1;
  wire bitset_ce0, bitset_ce1, bitset_we0, bitset_we1;
  wire [31:0] bitset_d0, bitset_d1;
  reg [31:0] bitset_q0, bitset_q1;
  wire [11:0] instruction_address0, instruction_address1;
  wire instruction_ce0, instruction_ce1;
  reg [63:0] instruction_q0, instruction_q1;
  wire [9:0] message_address0, message_address1;
  wire message_ce0, message_ce1;
  reg [31:0] message_q0, message_q1;
  wire [8:0] offset_address1;
  wire offset_ce1, offset_we1;
  wire [15:0] offset_d1;
  wire done, idle, ready;
  wire [31:0] ignored_match;

  always @(posedge clk) begin
    if (bitset_ce0) begin
      bitset_q0 <= bitset_mem[bitset_address0];
      if (bitset_we0) bitset_mem[bitset_address0] <= bitset_d0;
    end
    if (bitset_ce1) begin
      bitset_q1 <= bitset_mem[bitset_address1];
      if (bitset_we1) bitset_mem[bitset_address1] <= bitset_d1;
    end
    if (instruction_ce0)
      instruction_q0 <= instruction_mem[instruction_address0];
    if (instruction_ce1)
      instruction_q1 <= instruction_mem[instruction_address1];
    if (message_ce0) message_q0 <= message_mem[message_address0];
    if (message_ce1) message_q1 <= message_mem[message_address1];
    if (offset_ce1 && offset_we1) offset_mem[offset_address1] <= offset_d1;
  end

  regex_vm_new_perf dut (
    .ap_clk(clk), .ap_rst(reset), .ap_start(start),
    .ap_done(done), .ap_idle(idle), .ap_ready(ready),
    .ap_local_block(), .ap_local_deadlock(),
    .bitset_address0(bitset_address0), .bitset_ce0(bitset_ce0),
    .bitset_we0(bitset_we0), .bitset_d0(bitset_d0), .bitset_q0(bitset_q0),
    .bitset_address1(bitset_address1), .bitset_ce1(bitset_ce1),
    .bitset_we1(bitset_we1), .bitset_d1(bitset_d1), .bitset_q1(bitset_q1),
    .instructions_address0(instruction_address0),
    .instructions_ce0(instruction_ce0), .instructions_q0(instruction_q0),
    .instructions_address1(instruction_address1),
    .instructions_ce1(instruction_ce1), .instructions_q1(instruction_q1),
    .message_address0(message_address0), .message_ce0(message_ce0),
    .message_q0(message_q0), .message_address1(message_address1),
    .message_ce1(message_ce1), .message_q1(message_q1),
    .length_r(MESSAGE_BYTES), .ap_return(ignored_match),
    .offsets_address1(offset_address1), .offsets_ce1(offset_ce1),
    .offsets_we1(offset_we1), .offsets_d1(offset_d1)
  );

  task automatic build_rule(input integer instruction_count);
    integer i;
    integer any_count;
    integer anchor_count;
    begin
      for (i = 0; i < 4096; i = i + 1)
        instruction_mem[i] = INST_END;
      any_count = instruction_count - 2;
      if (any_count > MESSAGE_BYTES) any_count = MESSAGE_BYTES;
      anchor_count = instruction_count - any_count - 1;
      for (i = 0; i < anchor_count; i = i + 1)
        instruction_mem[i] = INST_BEGIN_BUF;
      for (i = 0; i < any_count; i = i + 1)
        instruction_mem[anchor_count + i] = INST_ANYCHAR;
      instruction_mem[instruction_count - 1] = INST_END;
    end
  endtask

  task automatic run_vm(output integer execution_cycles);
    integer launch_cycle;
    integer done_cycle;
    begin
      while (!idle) @(posedge clk);
      @(negedge clk);
      start = 1;
      @(posedge clk);
      #1 launch_cycle = rtl_cycle;
      @(negedge clk);
      start = 0;
      @(posedge done);
      #1 done_cycle = rtl_cycle;
      execution_cycles = done_cycle - launch_cycle;
    end
  endtask

  integer csv;
  integer sample;
  integer i;
  integer instruction_count;
  integer execution_cycles;

  initial begin
    csv = $fopen("regex_vm_new_cycles.csv", "w");
    if (!csv) $fatal(1, "Cannot create regex_vm_new_cycles.csv");
    $fdisplay(csv,
      "sample,rule_instructions,rule_bytes,message_bytes,regexVM_new_cycles");

    for (i = 0; i < 1024; i = i + 1) begin
      bitset_mem[i] = 0;
      message_mem[i] = 32'h61616161;
    end
    for (i = 0; i < 512; i = i + 1) offset_mem[i] = 0;

    repeat (8) @(posedge clk);
    @(negedge clk);
    reset = 0;

    for (sample = 0; sample < SAMPLE_COUNT; sample = sample + 1) begin
      if (sample != 0) begin
        @(negedge clk);
        reset = 1;
        repeat (4) @(posedge clk);
        @(negedge clk);
        reset = 0;
      end
      instruction_count = MIN_INSTRUCTIONS +
        (sample * (MAX_INSTRUCTIONS - MIN_INSTRUCTIONS)) /
        (SAMPLE_COUNT - 1);
      build_rule(instruction_count);
      run_vm(execution_cycles);
      $fdisplay(csv, "%0d,%0d,%0d,%0d,%0d", sample,
                instruction_count,
                (instruction_count + RULE_HEADER_WORDS) * 8,
                MESSAGE_BYTES, execution_cycles);
      $fflush(csv);
    end

    $fclose(csv);
    $display("PASS regexVM_new RTL sweep: samples=%0d message_bytes=%0d",
             SAMPLE_COUNT, MESSAGE_BYTES);
    $finish;
  end

  initial begin
    repeat (5000000) @(posedge clk);
    $fatal(1, "regexVM_new RTL sweep watchdog expired");
  end
endmodule
