`timescale 1ns/1ps

module test_instr_cclass_rtl;
  localparam integer INSTR_COUNT_VALUES = 16;
  localparam integer CCLASS_COUNT_VALUES = 16;
  localparam integer SAMPLE_COUNT = INSTR_COUNT_VALUES * CCLASS_COUNT_VALUES;
  localparam integer MIN_INSTRUCTIONS = 17;
  localparam integer MAX_INSTRUCTIONS = 194;
  localparam integer MESSAGE_BYTES = 4096;
  localparam integer HEADER_BYTES = 16;
  localparam integer INSTRUCTION_BYTES = 8;
  localparam integer CCLASS_BYTES = 32;

  localparam [63:0] INST_END       = 64'h0100000000000000;
  localparam [63:0] INST_CCLASS    = 64'h0e00000000000000;
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
  wire bitset_ce0, bitset_ce1;
  reg [31:0] bitset_q0, bitset_q1;
`ifdef VM_NEW
  wire bitset_we0, bitset_we1;
  wire [31:0] bitset_d0, bitset_d1;
`endif

  wire [11:0] instruction_address0, instruction_address1;
  wire instruction_ce0, instruction_ce1;
  reg [63:0] instruction_q0, instruction_q1;
  wire [9:0] message_address0, message_address1;
  wire message_ce0, message_ce1;
  reg [31:0] message_q0, message_q1;
  wire [8:0] offset_address1;
  wire offset_ce1, offset_we1;
  wire [15:0] offset_d1;
`ifndef VM_NEW
  wire [8:0] offset_address0;
  wire offset_ce0;
  reg [15:0] offset_q0, offset_q1;
`endif
  wire done, idle, ready;
  wire [31:0] ignored_return;

  always @(posedge clk) begin
    if (bitset_ce0) begin
      bitset_q0 <= bitset_mem[bitset_address0];
`ifdef VM_NEW
      if (bitset_we0) bitset_mem[bitset_address0] <= bitset_d0;
`endif
    end
    if (bitset_ce1) begin
      bitset_q1 <= bitset_mem[bitset_address1];
`ifdef VM_NEW
      if (bitset_we1) bitset_mem[bitset_address1] <= bitset_d1;
`endif
    end
    if (instruction_ce0)
      instruction_q0 <= instruction_mem[instruction_address0];
    if (instruction_ce1)
      instruction_q1 <= instruction_mem[instruction_address1];
    if (message_ce0) message_q0 <= message_mem[message_address0];
    if (message_ce1) message_q1 <= message_mem[message_address1];
`ifndef VM_NEW
    if (offset_ce0) offset_q0 <= offset_mem[offset_address0];
    if (offset_ce1) offset_q1 <= offset_mem[offset_address1];
`endif
    if (offset_ce1 && offset_we1) offset_mem[offset_address1] <= offset_d1;
  end

`ifdef VM_NEW
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
    .length_r(MESSAGE_BYTES), .ap_return(ignored_return),
    .offsets_address1(offset_address1), .offsets_ce1(offset_ce1),
    .offsets_we1(offset_we1), .offsets_d1(offset_d1)
  );
`else
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
    .length_r(MESSAGE_BYTES), .ap_return(ignored_return),
    .offsets_address0(offset_address0), .offsets_ce0(offset_ce0),
    .offsets_q0(offset_q0), .offsets_address1(offset_address1),
    .offsets_ce1(offset_ce1), .offsets_we1(offset_we1),
    .offsets_d1(offset_d1), .offsets_q1(offset_q1)
  );
`endif

  task automatic build_case(input integer instruction_count,
                            input integer cclass_count);
    integer i;
    reg [63:0] class_instruction;
    begin
      for (i = 0; i < 4096; i = i + 1)
        instruction_mem[i] = INST_END;
      for (i = 0; i < 1024; i = i + 1)
        bitset_mem[i] = 0;

      instruction_mem[0] = INST_BEGIN_BUF;
      for (i = 0; i < cclass_count; i = i + 1) begin
        class_instruction = INST_CCLASS;
        class_instruction[47:32] = i * 8;
        instruction_mem[1 + i] = class_instruction;
        // Each class accepts the input byte 'a' (0x61).
        bitset_mem[i * 8 + 3] = 32'h00000002;
      end
      for (i = 1 + cclass_count; i < instruction_count - 1; i = i + 1)
        instruction_mem[i] = INST_ANYCHAR;
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
  integer instr_index;
  integer cclass_index;
  integer instruction_count;
  integer cclass_count;
  integer instruction_data_bytes;
  integer cclass_data_bytes;
  integer original_rule_bytes;
  integer execution_cycles;
  integer i;

  initial begin
`ifdef VM_NEW
    csv = $fopen("regex_vm_new_instr_cclass_cycles.csv", "w");
`else
    csv = $fopen("regex_vm_opt_instr_cclass_cycles.csv", "w");
`endif
    if (!csv) $fatal(1, "Cannot create instruction/class CSV");
`ifdef VM_NEW
    $fdisplay(csv,
      "sample,instr_index,cclass_index,instr_num,cclass_num,header_bytes,instr_data_bytes,cclass_data_bytes,original_rule_bytes,message_bytes,regexVM_new_cycles");
`else
    $fdisplay(csv,
      "sample,instr_index,cclass_index,instr_num,cclass_num,header_bytes,instr_data_bytes,cclass_data_bytes,original_rule_bytes,message_bytes,regexVM_opt_cycles");
`endif

    for (i = 0; i < 1024; i = i + 1) begin
      bitset_mem[i] = 0;
      message_mem[i] = 32'h61616161;
    end
    for (i = 0; i < 512; i = i + 1) offset_mem[i] = 0;

    repeat (8) @(posedge clk);
    @(negedge clk);
    reset = 0;
    sample = 0;

    for (instr_index = 0; instr_index < INSTR_COUNT_VALUES;
         instr_index = instr_index + 1) begin
      instruction_count = MIN_INSTRUCTIONS +
        (instr_index * (MAX_INSTRUCTIONS - MIN_INSTRUCTIONS)) /
        (INSTR_COUNT_VALUES - 1);
      for (cclass_index = 0; cclass_index < CCLASS_COUNT_VALUES;
           cclass_index = cclass_index + 1) begin
        cclass_count = cclass_index;
        if (sample != 0) begin
          @(negedge clk);
          reset = 1;
          repeat (4) @(posedge clk);
          @(negedge clk);
          reset = 0;
        end

        build_case(instruction_count, cclass_count);
        run_vm(execution_cycles);
        instruction_data_bytes = instruction_count * INSTRUCTION_BYTES;
        cclass_data_bytes = cclass_count * CCLASS_BYTES;
        original_rule_bytes = HEADER_BYTES + instruction_data_bytes +
                              cclass_data_bytes;
        $fdisplay(csv, "%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d,%0d",
                  sample, instr_index, cclass_index, instruction_count,
                  cclass_count, HEADER_BYTES, instruction_data_bytes,
                  cclass_data_bytes, original_rule_bytes, MESSAGE_BYTES,
                  execution_cycles);
        $fflush(csv);
        sample = sample + 1;
      end
    end

    $fclose(csv);
    $display("PASS instruction/cclass RTL matrix: samples=%0d message_bytes=%0d",
             SAMPLE_COUNT, MESSAGE_BYTES);
    $finish;
  end

  initial begin
    repeat (5000000) @(posedge clk);
    $fatal(1, "Instruction/cclass RTL matrix watchdog expired");
  end
endmodule
