`timescale 1ns/1ps

module test_opt_general_rtl;
  localparam [63:0] INST_END       = 64'h0100000000000000;
  localparam [63:0] INST_BEGIN_BUF = 64'h2300000000000000;

  reg [31:0] input_length;
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
  wire bitset_ce0, bitset_ce1;
  reg [31:0] bitset_q0, bitset_q1;
  wire [11:0] instruction_address0, instruction_address1;
  wire instruction_ce0, instruction_ce1;
  reg [63:0] instruction_q0, instruction_q1;
  wire [9:0] message_address0, message_address1;
  wire message_ce0, message_ce1;
  reg [31:0] message_q0, message_q1;
  wire [8:0] offset_address0, offset_address1;
  wire offset_ce0, offset_ce1, offset_we1;
  wire [15:0] offset_d1;
  reg [15:0] offset_q0, offset_q1;
  wire done, idle, ready;
  wire [31:0] ignored_match;

  always @(posedge clk) begin
    if (bitset_ce0) bitset_q0 <= bitset_mem[bitset_address0];
    if (bitset_ce1) bitset_q1 <= bitset_mem[bitset_address1];
    if (instruction_ce0)
      instruction_q0 <= instruction_mem[instruction_address0];
    if (instruction_ce1)
      instruction_q1 <= instruction_mem[instruction_address1];
    if (message_ce0) message_q0 <= message_mem[message_address0];
    if (message_ce1) message_q1 <= message_mem[message_address1];
    if (offset_ce0) offset_q0 <= offset_mem[offset_address0];
    if (offset_ce1) offset_q1 <= offset_mem[offset_address1];
    if (offset_ce1 && offset_we1) offset_mem[offset_address1] <= offset_d1;
  end

  regex_vm_opt_perf dut (
    .ap_clk(clk), .ap_rst(reset), .ap_start(start),
    .ap_done(done), .ap_idle(idle), .ap_ready(ready),
    .ap_local_block(), .ap_local_deadlock(),
    .bitset_address0(bitset_address0), .bitset_ce0(bitset_ce0),
    .bitset_q0(bitset_q0), .bitset_address1(bitset_address1),
    .bitset_ce1(bitset_ce1), .bitset_q1(bitset_q1),
    .instructions_address0(instruction_address0),
    .instructions_ce0(instruction_ce0), .instructions_q0(instruction_q0),
    .instructions_address1(instruction_address1),
    .instructions_ce1(instruction_ce1), .instructions_q1(instruction_q1),
    .message_address0(message_address0), .message_ce0(message_ce0),
    .message_q0(message_q0), .message_address1(message_address1),
    .message_ce1(message_ce1), .message_q1(message_q1),
    .length_r(input_length), .ap_return(ignored_match),
    .offsets_address0(offset_address0), .offsets_ce0(offset_ce0),
    .offsets_q0(offset_q0), .offsets_address1(offset_address1),
    .offsets_ce1(offset_ce1), .offsets_we1(offset_we1),
    .offsets_d1(offset_d1), .offsets_q1(offset_q1)
  );

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


  integer csv, sample, i, k, pos, n, exit_pc, cycles;
  integer expected_match, expected_capture_start, expected_capture_end, correct;
  reg [63:0] inst;
  string family;
  function automatic [63:0] op(input [7:0] code, input [3:0] mode,
                               input [3:0] len, input [15:0] a, b, c);
    op = {code, mode, len, a, b, c};
  endfunction
  task automatic put_byte(input integer address, input [7:0] value);
    message_mem[address/4][8*(address%4)+:8] = value;
  endtask
  initial begin
    csv = $fopen("generic_opt_cycles.csv", "w");
    if (!csv) $fatal(1, "Cannot create generic baseline CSV");
    $fdisplay(csv, "family,bytes,expected_match,actual_match,correct,cycles");
    repeat (8) @(posedge clk);
    @(negedge clk); reset = 0;
    for (sample = 0; sample < 15; sample = sample + 1) begin
      for (i = 0; i < 4096; i = i + 1) instruction_mem[i] = INST_END;
      for (i = 0; i < 1024; i = i + 1) begin
        bitset_mem[i] = 0; message_mem[i] = 32'h61616161;
      end
      bitset_mem[3] = 2; // class [a]
      for (i = 0; i < 512; i = i + 1) offset_mem[i] = 16'hffff;
      expected_capture_start = -1; expected_capture_end = -1;
      expected_match = 1;
      if (sample < 6) begin
        n = sample%3 == 0 ? 0 : sample%3 == 1 ? 24 : 4086;
        input_length = n+2;
        put_byte(n+1, "b");
        exit_pc = sample < 3 ? 4 : 5;
        family = sample < 3 ? "class-greedy" : "class-possessive";
        instruction_mem[0] = op(51, 0, 0, 2, 0, 0); // MEM_START
        instruction_mem[1] = op(59, 1, 0, exit_pc, 0, 0); // PUSH
        instruction_mem[2] = op(14, 0, 1, 0, 0, 0); // CCLASS
        instruction_mem[3] = sample < 3 ? op(58,0,0,1,0,0) : op(61,0,0,0,0,0);
        if (sample >= 3) instruction_mem[4] = op(58,0,0,1,0,0);
        instruction_mem[exit_pc] = op(55,0,0,3,0,0); // MEM_END
        instruction_mem[exit_pc+1] = 64'h0702616200000000;
        instruction_mem[exit_pc+2] = op(36,0,0,0,0,0); // END_BUF
        expected_match = sample < 3;
        if (expected_match) begin expected_capture_start = 0; expected_capture_end = n; end
      end else if (sample == 6) begin
        family = "literal-chain"; pos = 0;
        for (i = 0; i < 29; i = i + 1) begin
          n = 1+i%4; inst = op(7,0,n,0,0,0);
          for (k = 0; k < n; k = k + 1) begin
            inst[40-8*k+:8] = "a"+k;
            put_byte(pos, "a"+k); pos = pos+1;
          end
          instruction_mem[i] = inst;
        end
        input_length = pos; instruction_mem[29] = op(36,0,0,0,0,0);
      end else if (sample < 9) begin
        family = "scalar-class-chain"; input_length = sample == 7 ? 28 : 29;
        for (i = 0; i < 1024; i = i + 1) message_mem[i] = 32'h78787878;
        for (i = 0; i < 8; i = i + 1) bitset_mem[i] = 32'hffffffff;
        instruction_mem[0] = INST_BEGIN_BUF;
        for (i = 1; i <= 28; i = i + 1) instruction_mem[i] = op(14,0,1,0,0,0);
        instruction_mem[29] = op(36,0,0,0,0,0);
        expected_match = sample == 7;
      end else if (sample < 12) begin
        family = "star-control-suffix";
        input_length = sample == 9 ? 0 : sample == 10 ? 64 : 4088;
        instruction_mem[0] = op(22,0,0,0,0,0); // ANYCHAR_STAR
        instruction_mem[1] = op(51,0,0,2,0,0);
        instruction_mem[2] = op(14,0,1,0,0,0);
        instruction_mem[3] = op(55,0,0,3,0,0);
        instruction_mem[4] = op(36,0,0,0,0,0);
        expected_match = input_length != 0;
        if (expected_match) begin expected_capture_start = input_length-1; expected_capture_end = input_length; end
      end else begin
        family = "bounded-repeat"; n = sample == 12 ? 1 : sample == 13 ? 4 : 7;
        input_length = n+1; put_byte(n,"b");
        instruction_mem[0] = op(65,0,0,1,2,5);
        instruction_mem[1] = op(14,0,1,0,0,0);
        instruction_mem[2] = op(67,0,0,1,2,5);
        instruction_mem[3] = 64'h0701620000000000;
        instruction_mem[4] = op(36,0,0,0,0,0);
        expected_match = n >= 2 && n <= 5;
      end
      run_vm(cycles);
      repeat (2) @(posedge clk);
      #1; // observe the final synchronous capture store
      correct = ignored_match === expected_match;
      if (expected_match && (offset_mem[0] !== 0 || offset_mem[1] !== input_length)) correct = 0;
      if (expected_match && expected_capture_end >= 0 && offset_mem[3] !== expected_capture_end) correct = 0;
      if (expected_match && expected_capture_start >= 0 && offset_mem[2] !== expected_capture_start) correct = 0;
      if (!correct) $display("BASELINE_MISMATCH %s bytes=%0d actual=%0d expected=%0d",family,input_length,ignored_match,expected_match);
      $fdisplay(csv,"%s,%0d,%0d,%0d,%0d,%0d",family,input_length,expected_match,ignored_match,correct,cycles);
      $fflush(csv);
    end
    $fclose(csv); $display("COMPLETED generic original-VM RTL: 15 cases"); $finish;
  end
  initial begin
    repeat (2000000) @(posedge clk);
    $fatal(1,"Generic original-VM RTL timeout");
  end
endmodule
