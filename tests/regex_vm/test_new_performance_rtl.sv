`timescale 1ns/1ps

module test_new_performance_rtl;
  localparam [63:0] INST_END       = 64'h0100000000000000;

  reg [31:0] input_length = 0;
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

  wire [9:0] bitset_address0;
  wire bitset_ce0;
  reg [31:0] bitset_q0;
  wire [11:0] instruction_address0, instruction_address1;
  wire instruction_ce0, instruction_ce1;
  reg [63:0] instruction_q0, instruction_q1;
  wire [9:0] message_address0, message_address1;
  wire message_ce0, message_ce1;
  reg [31:0] message_q0, message_q1;
  wire [8:0] offset_address0, offset_address1;
  wire offset_ce0, offset_ce1, offset_we0, offset_we1;
  wire [15:0] offset_d0, offset_d1;
  reg [15:0] offset_q0, offset_q1;
  wire done, idle, ready;
  wire [31:0] actual_match;

  always @(posedge clk) begin
    if (bitset_ce0) bitset_q0 <= bitset_mem[bitset_address0];
    if (instruction_ce0)
      instruction_q0 <= instruction_mem[instruction_address0];
    if (instruction_ce1)
      instruction_q1 <= instruction_mem[instruction_address1];
    if (message_ce0) message_q0 <= message_mem[message_address0];
    if (message_ce1) message_q1 <= message_mem[message_address1];
    if (offset_ce0) offset_q0 <= offset_mem[offset_address0];
    if (offset_ce0 && offset_we0) offset_mem[offset_address0] <= offset_d0;
    if (offset_ce1) offset_q1 <= offset_mem[offset_address1];
    if (offset_ce1 && offset_we1) offset_mem[offset_address1] <= offset_d1;
  end

  regex_vm_new_perf dut (
    .ap_clk(clk), .ap_rst(reset), .ap_start(start),
    .ap_done(done), .ap_idle(idle), .ap_ready(ready),
    .ap_local_block(), .ap_local_deadlock(),
    .bitset_address0(bitset_address0), .bitset_ce0(bitset_ce0),
    .bitset_q0(bitset_q0),
    .instructions_address0(instruction_address0),
    .instructions_ce0(instruction_ce0), .instructions_q0(instruction_q0),
    .instructions_address1(instruction_address1),
    .instructions_ce1(instruction_ce1), .instructions_q1(instruction_q1),
    .message_address0(message_address0), .message_ce0(message_ce0),
    .message_q0(message_q0), .message_address1(message_address1),
    .message_ce1(message_ce1), .message_q1(message_q1),
    .length_r(input_length), .ap_return(actual_match),
    .offsets_address0(offset_address0), .offsets_ce0(offset_ce0),
    .offsets_q0(offset_q0), .offsets_we0(offset_we0), .offsets_d0(offset_d0),
    .offsets_address1(offset_address1),
    .offsets_ce1(offset_ce1), .offsets_we1(offset_we1),
    .offsets_d1(offset_d1), .offsets_q1(offset_q1)
  );

  integer vectors, csv, scanned, rule_count, cases_per_rule;
  integer rule_index, case_index, instruction_count, class_words;
  integer i, words, expected_match, cycles, measurements = 0, mismatches = 0;

  // Count accepted ap_start to ap_done; vector/memory loading is not timed.
  task automatic run_vm(output integer execution_cycles);
    integer launch_cycle, waited;
    begin
      waited = 0;
      while (!idle && waited < 1000) begin
        @(posedge clk); #1; waited = waited + 1;
      end
      if (!idle) $fatal(1, "VM did not become idle");
      @(negedge clk); start = 1;
      @(posedge clk); #1; launch_cycle = rtl_cycle;
      @(negedge clk); start = 0;
      waited = 0;
      while (!done && waited < 1000000) begin
        @(posedge clk); #1; waited = waited + 1;
      end
      if (!done) $fatal(1, "VM timeout: rule=%0d case=%0d", rule_index, case_index);
      execution_cycles = rtl_cycle - launch_cycle;
    end
  endtask

  initial begin
    vectors = $fopen("performance_vectors.txt", "r");
    csv = $fopen("cycles.csv", "w");
    if (!vectors || !csv) $fatal(1, "Cannot open benchmark vectors/output");
    scanned = $fscanf(vectors, "%d %d", rule_count, cases_per_rule);
    if (scanned != 2 || rule_count != 100 || cases_per_rule != 15)
      $fatal(1, "Expected 100 rules and 15 inputs per rule");
    $fdisplay(csv, "rule_index,case_index,input_bytes,expected_match,actual_match,cycles");
    repeat (8) @(posedge clk);
    @(negedge clk); reset = 0;
    for (rule_index = 1; rule_index <= rule_count; rule_index = rule_index + 1) begin
      scanned = $fscanf(vectors, "%d %d", instruction_count, class_words);
      if (scanned != 2 || instruction_count < 1 || instruction_count > 4096 ||
          class_words < 0 || class_words > 1024)
        $fatal(1, "Invalid configuration for rule %0d", rule_index);
      @(negedge clk);
      for (i = 0; i < 4096; i = i + 1) instruction_mem[i] = INST_END;
      for (i = 0; i < 1024; i = i + 1) bitset_mem[i] = 0;
      for (i = 0; i < instruction_count; i = i + 1) begin
        scanned = $fscanf(vectors, "%h", instruction_mem[i]);
        if (scanned != 1) $fatal(1, "Invalid instruction");
      end
      for (i = 0; i < class_words; i = i + 1) begin
        scanned = $fscanf(vectors, "%h", bitset_mem[i]);
        if (scanned != 1) $fatal(1, "Invalid character-class word");
      end
      for (case_index = 1; case_index <= cases_per_rule; case_index = case_index + 1) begin
        @(negedge clk);
        scanned = $fscanf(vectors, "%d %d", input_length, expected_match);
        if (scanned != 2 || input_length < 1 || input_length > 512 ||
            expected_match != (case_index <= 10))
          $fatal(1, "Invalid input for rule %0d case %0d", rule_index, case_index);
        for (i = 0; i < 1024; i = i + 1) message_mem[i] = 0;
        for (i = 0; i < 512; i = i + 1) offset_mem[i] = 16'hffff;
        words = (input_length + 3) / 4;
        for (i = 0; i < words; i = i + 1) begin
          scanned = $fscanf(vectors, "%h", message_mem[i]);
          if (scanned != 1) $fatal(1, "Invalid message word");
        end
        run_vm(cycles);
        $fdisplay(csv, "%0d,%0d,%0d,%0d,%0d,%0d",
                  rule_index, case_index, input_length, expected_match, actual_match, cycles);
        $fflush(csv);
        measurements = measurements + 1;
        if (actual_match !== expected_match) begin
          mismatches = mismatches + 1;
          $display("MATCH_MISMATCH rule=%0d case=%0d expected=%0d actual=%0d",
                   rule_index, case_index, expected_match, actual_match);
        end
        repeat (2) @(posedge clk);
      end
      $display("Measured rule %0d/100", rule_index);
    end
    $fclose(vectors); $fclose(csv);
    $display("COMPLETED regexVM_new performance: %0d cases, %0d mismatches", measurements, mismatches);
    $finish;
  end
endmodule
