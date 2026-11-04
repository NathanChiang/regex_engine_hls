# VCK190 / Vitis HLS 2021.2 verification

This target is a standalone Vivado IP with AXI-Lite configuration/results and
AXI4-Stream DMA input, targeting `xcvc1902-vsva2197-2MP-e-S` at a 6 ns clock.
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

# Synthesized three-rule/three-message-length performance matrix:
RE_HLS_ACTION=perf vitis_hls -f run_hls.tcl
RE_HLS_ACTION=perf_only vitis_hls -f run_hls.tcl # reuse re_engine_perf/perf RTL

# Start-once production RTL, including AXI-Lite bus transactions:
RE_HLS_RTL_PROJECT=re_engine_direct_array_status python3 run_rtl_mmio.py

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

## Performance C/RTL benchmark

`RE_HLS_ACTION=perf` runs `host/test_perf.sv` against the csynth RTL. It
configures job IDs 1, 2, and 3 at the simple page ranges `0x1000..0x1fff`,
`0x2000..0x2fff`, and `0x3000..0x3fff`. Their synthetic linear rule images
are 80, 528, and 1552 bytes (8, 64, and 192 VM instruction words). Each rule
is crossed with 256, 1024, and 4088-byte messages four times, producing 36
jobs so the dispatcher can overlap all 16 processing units.

The run writes three inspectable files under
`build/perf_cosim_re_engine_perf/`: `perf_workload.log` maps message IDs to
inputs, `perf_message_results.log` records returned words without checking
them, and `perf_engine.log` contains timing, utilization, and DMA-stall data.

Per-message records separate IOVA lookup, chained rule lookup, packet/lookup
join, ready-to-dispatch scheduling, PU input/load, VM execution, and ordered
result wait. Engine snapshots contain all 16 synthesized PU-idle counters and
all-PUs-busy cycles measured from the first DMA SOP, actual internal
packet-handoff cycles (including FIFO backpressure), and cumulative scheduling
delay. The testbench independently
counts every interface `TVALID && !TREADY` cycle. Correlating those intervals
with handoff, all-busy, and scheduler counters identifies ingress-copy pressure
versus processing saturation. The current chained rule-metadata lookup can
correctly measure zero cycles after IOVA completion.

Telemetry leaves through the dedicated 512-bit `perf_out` AXI4-Stream. Its
record layout is documented beside `RePerfRecord` in
`kernel/re_engine_kernel.hpp`. Diagnostic taps and the monitor's internal FIFO
writes are non-blocking, so telemetry cannot stall functional matching. A
dedicated egress process may wait for `perf_out_TREADY`; the supplied testbench
keeps it asserted. Up to 64 messages may be in flight.

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
  Configuration writes update all replicas in parallel. Input, dispatch,
  execution, and ordered result delivery run concurrently. Do not rewrite or
  delete rules while packets using them are in flight. The AXIS input honors
  backpressure; a passive snoop source still needs external buffering or a
  defined drop policy when the queue fills.
- Pending MMIO results do not raise `result_out.halt` in streaming hardware;
  AXIS `TREADY` supplies input backpressure when necessary.
- `perf_out` carries diagnostic message-timing and engine-snapshot records; it
  does not replace the MMIO match-result interface.
- `regexVM_opt` pipelines VM loop iterations at II=3; it does not expose three
  independent message slots. A PU is reusable only after its VM call and
  result copy finish, so its message and capture buffers are no longer in use.
- In hardware, start the AXI-Lite-controlled kernel once (`ap_ctrl_hs`) after
  reset. It then runs continuously. Configuration and result-acknowledgement
  MMIO inputs are reread while it runs; software must not wait for `ap_done`.
  The synthesized register arrays place `result_in` at 0x18 and `cfg_in`
  at 0x20; `cfg_out` is at 0x10 and `result_out` begins at 0x80. Use the
  generated driver header for the exact register map.
- The C simulation uses a finite, one-packet step model for the existing MMIO
  regression harness. The default production-dimension smoke test checks
  configuration, rule lookup, DMA-miss, and malformed-packet handling without
  entering the expensive VM stress cases. `RE_HLS_EXHAUSTIVE_CSIM=1` adds the
  full table-capacity/reference and regex stress regression. Neither C model
  represents concurrent PU timing; the persistent dataflow path is selected
  under `__SYNTHESIS__` and validated by synthesis and the direct RTL bus test.
- After power-on or reset, keep DMA idle and issue CLEAR before populating
  tables. Control state resets explicitly; data memories are invalidated by
  CLEAR instead of a large synchronous RAM reset. CLEAR takes 1025 table
  steps. A worst-case insertion needs a directory scan and two bucket passes;
  the C test allows 4096 steps.
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
For the supplied 2021.2 Vivado flow, `cfg_in` occupies 0x10..0x24,
`cfg_out` is at 0x30, and `result_in` is at 0x38. The result payload is at
0x80..0xbc, the address at 0xc0..0xc4, and ack/valid/halt occupy consecutive
bytes at 0xc8. `cfg_in` packs command/valid/job into bytes 0/1/2..3, then
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
result reads and reset with a pending result.

These checks do not establish routed timing, a bitstream, PCIe/NVMe subsystem
integration, or board operation. HLS resource/timing numbers are estimates.

The interface choices follow the 2021.2 documentation on
[AXI stream side channels](https://docs.amd.com/r/2021.2-English/ug1399-vitis-hls/How-AXI4-Stream-is-Implemented)
and [nonblocking streams](https://docs.amd.com/r/2021.2-English/ug1399-vitis-hls/Non-Blocking-Reads-and-Writes).
The latter explains why automatic co-simulation of a nonblocking
`ap_ctrl_none` top cannot be relied on to finish.
