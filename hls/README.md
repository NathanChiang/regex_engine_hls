# VCK190 / Vitis HLS 2021.2 verification

This target is a standalone Vivado IP with AXI-Lite configuration/results and
AXI4-Stream DMA input, targeting `xcvc1902-vsva2197-2MP-e-S` at a 4 ns
(250 MHz) clock with 0.2 ns uncertainty, giving HLS a 3.8 ns logic budget.
The legacy Alveo OpenCL host path and `conn_u*.cfg` files do not connect this
interface. Integrate the exported IP in the VCK190 PL design.

## Linux commands

After sourcing the 2021.2 tool settings, run from this directory:

```sh
# No Oniguruma, OpenCL, or regex compiler libraries needed:
RE_HLS_ACTION=csim vitis_hls -f run_hls.tcl # production dimensions, config/lookup/DMA smoke test
RE_HLS_ACTION=csim RE_HLS_QUICK_CSIM=1 vitis_hls -f run_hls.tcl # reduced dimensions + rule-replica checks
RE_HLS_ACTION=csim RE_HLS_EXHAUSTIVE_CSIM=1 vitis_hls -f run_hls.tcl # slow table and full regex stress regression
RE_HLS_ACTION=synth vitis_hls -f run_hls.tcl
RE_HLS_ACTION=synth_only vitis_hls -f run_hls.tcl # skip long C regression

# Compile the test.cpp workload/reference, synthesize the production kernel,
# then drive the generated RTL through its real AXI-Lite and AXIS interfaces:
RE_HLS_ACTION=cosim RE_HLS_COSIM_LINES=1000 vitis_hls -f run_hls.tcl
RE_HLS_ACTION=cosim_only RE_HLS_COSIM_LINES=1000 vitis_hls -f run_hls.tcl # reuse existing csynth RTL

# Start-once production RTL, including AXI-Lite bus transactions:
RE_HLS_RTL_PROJECT=re_engine_direct_array_status python3 run_rtl_mmio.py

# Completion-order, burst/backpressure, and maximum-capture RTL regression:
RE_HLS_RTL_PROJECT=re_engine_direct RE_HLS_RTL_BENCH=streaming python3 run_rtl_mmio.py

# Original log-data/Oniguruma reference comparison (Linux libraries required):
RE_HLS_ACTION=csim RE_HLS_TESTBENCH=log vitis_hls -f run_hls.tcl

# C simulation, synthesis, then package as Vivado IP (no place-and-route):
RE_HLS_ACTION=export vitis_hls -f run_hls.tcl
```

`REGEX_ENGINE_ROOT` optionally specifies this standalone package directory.
Normally it is unnecessary because every path is derived from the script,
independent of the launch directory.
Generated projects/logs live under `build/`; the old `prj/` is preserved.
Production reports are under `build/re_engine/smoke/syn/report/`.
The `synth_only` report is under `build/re_engine_direct/smoke/syn/report/`.
The direct RTL bus test logs are under `build/mmio_<project>/`.
The log-data C/RTL regression logs and vectors are under
`build/cosim_re_engine_cosim/`.
The finite C step model is not an RTL co-simulation reference for the
free-running datapath; the `cosim` action and `run_rtl_mmio.py` both drive the
post-synthesis RTL directly.

The log test requires `libxfcompile` in `../re_compile/lib/lib` and uses the
vendored Oniguruma files in `../third_party/oniguruma/lib`. This
Windows checkout contains Linux libraries, so the dependency-free test is the
default. Both the log CSim and C/RTL regression use those libraries. The log
test uses a separate `re_engine_log` project and the C/RTL test uses
`re_engine_cosim`, preventing multiple test-bench entry points from entering
one project. Use a clean project if changing tool versions.

## Interface contract

- `DmaAxisBeat` is now `ap_axiu<512,65,0,0>`, so the RTL has real `TKEEP`,
  `TSTRB`, `TLAST`, and `TUSER` pins. `TUSER[0]` is SOP and `TUSER[64:1]` is
  the DMA address. Producers should set `TSTRB = TKEEP`. This requires updating
  a producer that previously used the packed custom struct.
- Every non-final beat has all 64 `TKEEP` bits set. The final mask contains
  contiguous low-order ones and is nonzero. Bytes start in `TDATA[7:0]`.
  Missing SOP, unexpected SOP inside a packet, invalid masks, table misses,
  and packets longer than 4088 bytes are drained without a result.
- The synthesized kernel has a persistent dispatcher and sixteen independent
  VM workers. Each accepted packet carries a four-bit rule index. Instruction
  and bitset storage is replicated once per PU, so all sixteen VMs can read
  their selected rules concurrently without copying rule payloads per message.
  Configuration writes update all replicas in parallel. DMA capture, lookup,
  scheduling, payload routing, execution, and result delivery run concurrently.
  DMA packet bodies are captured with II=1 independently of AXI-Lite polling.
  Scheduling reserves a free PU independently of the payload router; a full
  64-beat packet fits in each PU's input FIFO, avoiding backpressure from the
  VM's narrower message-memory loader. Do not rewrite or
  delete rules while packets using them are in flight. The AXIS input honors
  backpressure; a passive snoop source still needs external buffering or a
  defined drop policy when the queue fills.
- Pending MMIO results do not raise `result_out.halt` in streaming hardware;
  AXIS `TREADY` supplies input backpressure when necessary.
- Results are returned in completion order rather than input serial order.
  Each PU announces a job only after its complete result record is buffered.
  A fair arbiter selects ready records and queues their completion tickets;
  it never waits for an unfinished earlier job. Records with multiple chunks
  remain contiguous. If several PUs are pending, round-robin arbitration
  resolves their order. Finite queues still propagate backpressure when
  software stops consuming results. No serial-number reorder buffer is used.
- Every result chunk carries a 32-bit `message_id` at MMIO `0xcc` in addition
  to the existing DMA address. The sequence starts at zero after hardware
  reset, advances for each SOP packet (including packets subsequently dropped),
  and wraps at 2^32. CLEAR does not restart it. Use the ID to associate results
  when DMA addresses repeat; software must no longer assume arrival order.
  This one sequence counter replaces the measure branch's large diagnostic
  event/counter pipeline. The AXIS and AXI-Lite ports are otherwise unchanged.
- `regexVM_opt` lets HLS choose the pipeline II to meet the clock target;
  the 4 ns / 0.2 ns uncertainty Vitis HLS 2021.2 build achieves II=4.
  Completion arbitration achieves II=2 to register its feedback path. DMA
  packet bodies still achieve II=1. The previous 1.08 ns uncertainty build
  achieved VM II=6 and arbitration II=3.
  A PU is reusable only after its VM call and result copy finish, so its message
  and capture buffers are no longer in use.
- Payload unpacking, VM byte-window selection, and result packing use bounded
  byte selectors or fixed shifts instead of variable-width slice writes. The
  64-beat PU input FIFOs and 64-entry job FIFO use distributed RAM to avoid
  allocating deep BRAMs for shallow, wide queues. Their capacities are unchanged.
  A registered IOVA response separates bucket selection from rule validation.
- In hardware, start the AXI-Lite-controlled kernel once (`ap_ctrl_hs`) after
  reset. It then runs continuously. Configuration and result-acknowledgement
  MMIO inputs are reread while it runs; software must not wait for `ap_done`.
  The synthesized register arrays place `result_in` at 0x18 and `cfg_in`
  at 0x20; `cfg_out` is at 0x10 and `result_out` begins at 0x80. Use the
  generated driver header for the exact register map.
- The C simulation uses a finite, one-packet step model for the existing MMIO
  regression harness. The default test checks random-payload unpacking for
  every length from 0 through 4088 bytes, including guard words and untouched
  memory, and VM match/capture checks for all 256 byte-class values and
  unaligned literals. The production-dimension kernel smoke test checks
  configuration, rule lookup, DMA-miss, and malformed-packet handling without
  entering the expensive VM stress cases. `RE_HLS_EXHAUSTIVE_CSIM=1` adds the
  full table-capacity/reference and regex stress regression. Neither C model
  represents concurrent PU timing; the persistent dataflow path is selected
  under `__SYNTHESIS__` and validated by synthesis and the direct RTL bus test.
- After power-on or reset, keep DMA idle and issue CLEAR before populating
  tables. Control state resets explicitly; data memories are invalidated by
  CLEAR instead of a large synchronous RAM reset. CLEAR takes 1025 table
  steps. A worst-case insertion needs a directory scan and two bucket passes;
  the C test allows 4096 steps. Deletion registers its exact-range comparison
  before updating the scan/control state, taking two steps per directory entry.
- Configure with `valid=0` while selecting a command and filling its arguments,
  then assert `valid`. Hold all fields until `ack` or `full`; completion is
  held until `valid` is lowered. Wait for acknowledgement to clear and
  `cmd_transition_allowed` before switching commands. Preserve a software
  timeout: malformed ranges/invalid job IDs retain the existing no-ack
  rejection behavior.
- Rule IDs are 0..31, each with 256 64-bit words. Missing words read as zero,
  including after delete/reallocate. Headers must have a nonzero instruction
  count and group count, bounded class/group counts, and exactly
  `2 + instructions + 4*classes` words, at most 256. Malformed headers are
  dropped before the config FIFO is filled. Bytecode must still come from the
  trusted regex compiler; this is not an instruction validator or VM timeout.
- IOVA ranges are half-open. Lowest logical directory index wins overlaps.
  Hash buckets hold eight mappings, so `full` may report a bucket collision
  before all 1024 logical entries are used. An unsuccessful precheck makes no
  partial insertion. Deleting a missing mapping is idempotent.
- Results contain sixteen 32-bit words per chunk, padded with zeros in the
  last chunk. The first word is the match status, followed by capture offsets.
  `address` identifies the input DMA address. There is no final-chunk flag;
  software knows the configured capture count and consumes `groups + 1` words.
- Keep `result_in=0` while reading a chunk. Raise it to acknowledge exactly
  one chunk and poll `result_out.ack==1`, then lower it and poll
  `result_out.ack==0` before acknowledging another. A held high input cannot
  skip chunks. `result_out.valid` indicates whether another chunk is present.
  The payload precedes metadata in the result struct, and the kernel builds
  each snapshot locally before publishing it.

The interface changes require regenerating the AXI-Lite driver/register map.
For the supplied 2021.2 Vivado flow, `cfg_in` occupies 0x20..0x34,
`cfg_out` is at 0x10, and `result_in` occupies 0x18..0x1c. The result payload is at
0x80..0xbc, the address at 0xc0..0xc4, and ack/valid/halt occupy consecutive
bytes at 0xc8; `message_id` is at 0xcc. `cfg_in` packs command/valid/job into bytes 0/1/2..3, then
aligns the two 64-bit arguments at bytes 8 and 16. Read the generated map
when integrating; do not cast a native host `ap_uint` struct into MMIO bytes.

## What each check establishes

`test_regression.cpp` checks all 8192 rule words, partial writes, entry reuse,
capacity, bucket collisions with atomic rejection, address wrap/boundaries,
overlap priority, and 4000 seeded lookups against a software model. Kernel
tests cover literal match/mismatch, malformed framing, length limits, invalid
rule headers, delayed acknowledgements, and all 33 maximum-size result chunks.

`test_cosim.cpp` generates the same compiled rule, selected log records, DMA
payloads, and Oniguruma reference as `test.cpp`. `test_cosim.sv` then drives
the exact `csynth_design` output through AXI-Lite and AXIS and compares every
match/capture result. No CoSim-only macro changes the synthesized kernel.
This explicit C-reference/RTL flow is required because the start-once
`ap_ctrl_hs` kernel deliberately never asserts `ap_done`; the standard HLS
transaction wrapper cannot modify live MMIO registers while waiting for that
transaction to return. The smaller `tb_mmio.sv` separately checks stable
result reads, IOVA deletion/miss/reuse, unaligned literal/class match and
mismatch with captures/padding, and reset with a pending result.

`tb_streaming.sv` checks that a short second job overtakes a long first job,
then runs 80 messages of 3, 65, and 4088 bytes: 16 maximum-size capture records
followed by 64 smaller records for multiple PU reuse rounds. It checks every
result word, ID, address, padding word, and chunk
boundary while acknowledgements are delayed and held high. Invalid KEEP,
IOVA-miss, and oversized packets are dropped; a warm reset restarts the ID
counter. It also requires a sustained run of at least 32 accepted DMA body
beats at one beat per cycle. Its `streaming_results.csv` and simulator log
are written under `build/mmio_<project>_streaming/`.

These checks do not establish routed timing, a bitstream, PCIe/NVMe subsystem
integration, or board operation. HLS resource/timing numbers are estimates.

The Vitis HLS 2021.2 area run `re_engine_direct_250mhz_u02_area_final` reports
3.790 ns estimated delay at the 4.00 ns target with 0.20 ns clock uncertainty.
All 27 module reports meet the resulting 3.8 ns scheduling budget, with no
HLS timing-violation warnings. The top-level RTL ports and AXI-Lite addresses
match the previous `re_engine_direct_250mhz_registered` build.

| HLS estimate | Previous 1.08 ns uncertainty | Current 0.20 ns uncertainty | Change |
| --- | ---: | ---: | ---: |
| LUT | 212,425 | 111,750 | -47.4% |
| BRAM_18K | 786 | 538 | -31.6% |
| URAM | 48 | 48 | unchanged |
| FF | 95,683 | 96,197 | +0.5% |

The distributed-RAM FIFOs save 248 BRAM_18K units while retaining their
depths. The instruction replicas and 8192-entry VM stacks retain their URAM
mapping and capacities; moving them would increase BRAM or LUT consumption.
The C regression and 273 direct comparisons with the previous VM passed.
The final MMIO and 80-record streaming RTL regressions also passed at a 4 ns
clock with production dimensions. The streaming run accepted 61 consecutive
DMA body beats at II=1. A focused RTL test additionally checked random,
nonuniform payloads at 15 partial/full/multiple/max-beat lengths.

To reproduce the reported build and its RTL checks from this directory:

```sh
RE_HLS_ACTION=synth_only RE_HLS_PROJECT_SUFFIX=_250mhz_u02_area_final vitis_hls -f run_hls.tcl
RE_HLS_RTL_PROJECT=re_engine_direct_250mhz_u02_area_final python3 run_rtl_mmio.py
RE_HLS_RTL_PROJECT=re_engine_direct_250mhz_u02_area_final RE_HLS_RTL_BENCH=streaming python3 run_rtl_mmio.py
```

The interface choices follow the 2021.2 documentation on
[AXI stream side channels](https://docs.amd.com/r/2021.2-English/ug1399-vitis-hls/How-AXI4-Stream-is-Implemented)
and [nonblocking streams](https://docs.amd.com/r/2021.2-English/ug1399-vitis-hls/Non-Blocking-Reads-and-Writes).
The latter explains why automatic co-simulation of a nonblocking
`ap_ctrl_none` top cannot be relied on to finish.
