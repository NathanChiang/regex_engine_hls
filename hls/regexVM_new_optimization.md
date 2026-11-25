# VCK190 regex VM optimization

The production streaming kernel uses 16 independent PUs. Each PU retains 16
private rule slots, an 8192-entry backtracking stack, a 4096-byte message RAM,
and capture storage for up to 512 configured groups. The accepted packet limit
is 4088 bytes. A rule slot is 2048 bytes **including** its two header words,
instructions, and character-class data; the 4096-instruction legacy constant
does not increase this per-rule capacity.

## Memory and execution organization

The original narrow interfaces cannot deliver six unrelated instructions per
cycle. The streaming worker now stores messages as 512-bit words and classes
as 256-bit bitmaps. A message window reads at most two words and supplies 24
aligned bytes. At a 64-byte boundary, one port reads the previous word for
line-anchor semantics and the other reads the current word. No third read is
needed. Each class lookup selects one of eight 32-bit words before selecting a
bit; directly indexing a 256-bit `ap_uint` produced very large HLS shifters.

Rule configuration retains compact 64-bit bytecode. Once all required rule
words are present, every PU compiles one 512-bit descriptor per instruction
address. Configuration acknowledgement waits for all PUs to finish. This
preserves the external configuration format and supports payload words arriving
in either order after the header. A later edit to a complete rule recompiles
it. Reconfiguration must be performed with no messages using that rule in flight,
as required by the existing configuration protocol.

A descriptor retains the current and next raw instructions in bits 0..127.
Bits 128..255 describe recognized greedy/possessive loops and optional literal,
capture-start, mandatory-class-byte, and capture-end prefixes/suffixes. Bits
256..311 describe a conservative lazy-loop suffix prefix, its remote literal
alternative, and its body class. Remaining bits are zero. Runtime performs one
descriptor read, at most two class reads, and two message reads per transition.
Configuration uses a rolled single-read bytecode loop rather than asking a
dual-port memory for eight reads at once.

The VM scans up to 24 class bytes per transition. A range stack entry retains
every greedy retry position in LIFO order. Lazy class loops skip only bytes that
belong to their body class and cannot start either supported literal suffix arm.
Unsupported control-flow, nullable suffixes, and capture-changing prefixes use
the scalar interpreter. These are bytecode optimizations; the Apache expression
is not hard-coded. Straight-line literal batching defaults to two instructions.

The worker also loads one 512-bit DMA beat per clock and clears only the
configured capture groups plus the result prefix. Previously, a 12-group rule
paid the same approximately 515-cycle initialization as a 512-group rule.

The 64-row message RAM and 64-beat packet FIFOs use distributed RAM. Wide,
shallow BRAMs wasted physical tiles in this configuration; distributed RAM
uses the device's available LUT capacity instead. The packet FIFO still holds
one maximum-size message. Descriptor, raw-instruction, and backtracking-stack
storage use UltraRAM; class bitmaps and captures retain BRAM.

Class configuration commits a complete 256-bit bitmap through
`replaceClassChunk`. Assigning a variable 64-bit range directly to a BRAM
element passed C simulation but lost earlier chunks in RTL when writes arrived
in descending order. A dedicated ascending/descending C/RTL test reproduces
the failure and verifies the full-word replacement. Separate C/RTL tests check
the configuration controller and descriptor generation, including live edits
and rules containing 254 instructions.

Result-slot validity is explicitly reset. Resetting only control registers
left stale RAM flags after a warm reset, allowing an old result to retire under
a reused message ID. The validity/tag reads are issued together in the
collector. See AMD's [array reset documentation](https://docs.amd.com/r/2021.2-English/ug1399-vitis-hls/Initializing-and-Resetting-Arrays).

There is no expression-specific dispatch or execution-step cutoff by default.
`XF_REGEX_VM_NEW_MAX_STEPS` can impose an optional deployment budget; its default
zero preserves unrestricted backtracking. Physical stack overflow still reports
status 2. A valid 96-byte failing regression takes 637,095 scalar transitions and
165,951 vector transitions, exceeding the former fixed cutoff. Vector scans do
not remove exponential worst-case backtracking from this ISA.

## Device budget

The HLS target is `xcvc1902-vsva2197-2MP-e-S`, the VCK190 device, with a 6 ns
clock (166.7 MHz). The tool reports 899,840 LUTs, 1,799,680 FFs, 1,934 BRAM18K
units, 463 UltraRAMs, and 1,968 DSPs. A 24-byte scan is replicated in all 16
PUs, so per-PU LUT use matters more than nominal instruction-lane count.
The totals agree with AMD's [AI Core specifications](https://www.amd.com/en/products/adaptive-socs-and-fpgas/versal/ai-core-series.html).
They are whole-device totals; integration must also budget any other PL logic.

UltraRAM must also be budgeted by physical geometry. A block is 4096 x 72 bits.
Conservatively retaining all 512 descriptor bits costs eight blocks per PU;
4096 raw 64-bit instructions cost one, and an 8192 x 54-bit stack costs two.
That is at most 176 blocks across the PUs. The 64 x 513 x 32-bit reorder buffer
needs nine more, for a conservative total of 185/463 (40%). Trimming unused
descriptor bits can reduce this. HLS 2021.2's utilization estimates undercount
some wide/deep inferred URAMs, so its summary alone is not proof of physical fit.
See AMD's [UltraRAM architecture documentation](https://docs.amd.com/r/en-US/am007-versal-memory/UltraRAM-Key-Features)
for block organization and port constraints.

HLS timing is an estimate. RTL simulation establishes behavior and cycle
counts; it does not establish routed timing, a bitstream, or board throughput.

The final 16-PU kernel (`re_engine_direct_general_verified`) was synthesized
with Vivado 2021.2 against the complete HLS RTL. Physical primitive counts are:

| Resource | Used | Device total | Utilization |
| --- | ---: | ---: | ---: |
| LUT | 221935 | 899840 | 24.66% |
| Flip-flop | 130986 | 1799680 | 7.28% |
| BRAM36 tile equivalents | 356 | 967 | 36.81% |
| UltraRAM | 136 | 463 | 29.37% |
| DSP | 0 | 1968 | 0% |

BRAM is 320 RAMB36 plus 72 RAMB18 primitives. The synthesized internal setup
slack is +0.533 ns at 6 ns, with zero unconstrained internal endpoints. This is
an out-of-context synthesis result: external input/output delays and clock
placement are not specified, and placement/routing has not been performed.
The generated reports are under
`hls/build/vivado_re_engine_direct_general_verified/`.

The memory-width tradeoff matters more than bit capacity for shallow buffers.
A 512-bit message RAM with two read ports uses 14 BRAM36 plus one BRAM18 per PU;
the old 512-bit packet FIFO uses seven BRAM36 plus one BRAM18. Moving these to
distributed RAM saves 352 BRAM36 tile equivalents across 16 PUs. The final
full-word class-update implementation saves eight more compared with the first
wide-memory build. Total BRAM fell from 716 to 356 tile equivalents. The actual
per-PU mapping uses 1,024 LUTRAM LUTs plus nine logic LUTs for the message RAM,
and 586 LUTRAM LUTs plus 24 logic LUTs for the packet FIFO. The message RAM's
two read ports require replicated distributed storage. The complete synthesized
kernel still uses less than a quarter of the device's LUTs.

## General workload measurements

The predecoded core passed 1,037 C/RTL differential cases. The results are in
[`regex_vm_new_general_rtl_cycles.csv`](../tests/regex_vm/results/regex_vm_new_general_rtl_cycles.csv)
and the family-level
[`summary`](../tests/regex_vm/results/regex_vm_new_general_rtl_summary.json).
The standalone wrapper excludes loading, configuration, and capture clearing.
The following selected cases use the same bytecode and input in the original
optimized VM and the new core, at the same 6 ns target:

| Generic bytecode/input | Bytes | Original `opt` cycles | New cycles | Original/new |
| --- | ---: | ---: | ---: | ---: |
| Greedy class loop, capture, literal suffix | 4088 | 24547 | 1067 | 23.01x |
| `ANYCHAR_STAR`, captured class, end anchor | 4088 | 12292 | 1085 | 11.33x |
| Greedy class loop, capture, literal suffix | 26 | 175 | 53 | 3.30x |
| Literal chain | 71 | 97 | 113 | 0.86x |
| Scalar class chain | 28 | 94 | 197 | 0.48x |
| Bounded repeat and suffix | 5 | 31 | 89 | 0.35x |

Ratios below 1 mean a slowdown. The larger feedback-loop II hurts short,
scalar/control-heavy programs. These measurements are representative cases,
not an aggregate throughput claim or an exhaustive expression distribution.
Long recognized repetitions benefit most. A small additional baseline run
measures 15 generic cases and checks their output captures; 13 produce the
expected original-VM result. Two synthetic possessive-loop cases expose an
existing original-VM result mismatch. Their timing is retained in the CSV for
inspection, with no speedup ratio. The new core passes the scalar-reference
expectations for these cases in the 1,037-case regression.
The full selected comparison is in
[`regex_vm_generic_comparison.csv`](../tests/regex_vm/results/regex_vm_generic_comparison.csv).

Throughput in bytes/clock uses **input-message bytes**, not compiled-rule
bytes. The 1,288-byte Apache rule contains 95 instructions and 16 character
classes; its 100 benchmark messages average 214.3 input bytes. The new core
averages 250.2 clocks/message versus 1,501.76 for the original optimized core,
a 6.00x improvement on that workload. This standalone comparison excludes the
kernel's initialization, scheduling, lookup, and result stages.

The final complete-kernel RTL run compares every match/capture word against
the same 100 Oniguruma-backed reference vectors, with no correlation drops:

| Average clocks/message | Previous `regexVM_new` integration | Final integration |
| --- | ---: | ---: |
| VM-stage telemetry | 5843.00 | 266.22 |
| Complete kernel | 17892.67 | 1677.68 |
| Scheduler wait | 11186.79 | 6.00 |
| IOVA lookup/wait | 703.62 | 1329.70 |
| PU loading | 77.32 | 9.38 |
| Result buffering | 79.48 | 63.83 |

That is 21.95x lower VM-stage latency and 10.67x lower complete-kernel latency
**relative to the previous new-VM integration**. It is not a 10.67x comparison
against the original optimized VM. The final VM-stage rate is 0.805 input
bytes/clock. The original optimized standalone core comparison remains 6.00x.
IOVA lookup/wait is now about 79% of average kernel latency; adding PUs or
accelerating only the interpreter would not remove that bottleneck.

## Reproducing validation

Use Vitis HLS and Vivado 2021.2. PowerShell examples below run from the repository
root, with the tool executables on PATH. The Apache benchmark consumes
`tests/regex_vm/fixtures/apache_100/cosim_input.txt` and its matching
`cosim_expected.txt`, produced by the existing Oniguruma-backed log regression.
This fixture is used only by the benchmark and is not embedded in the VM.

```powershell
$env:RE_VM_PREDECODED = '1'
$env:RE_VM_VARIANT = 'accel'
$env:RE_VM_ACTION = 'synth'
vitis_hls -f tests/regex_vm/run_hls_new_accel.tcl
python tests/regex_vm/run_new_accel_apache_rtl.py

$env:RE_VM_BENCH = 'diff'
$env:RE_VM_ACTION = 'cosim'
vitis_hls -f tests/regex_vm/run_hls_new_accel.tcl
python tests/regex_vm/summarize_vm_cosim.py regex_vm_new_accel_diff

# Original optimized VM RTL and selected generic comparisons:
$env:RE_VM_VARIANT = 'opt'
$env:RE_VM_BENCH = 'apache'
$env:RE_VM_ACTION = 'synth'
vitis_hls -f tests/regex_vm/run_hls_new_accel.tcl
python tests/regex_vm/run_opt_general_rtl.py

$env:RE_HLS_ACTION = 'synth_only'
vitis_hls -f hls/run_hls.tcl

$env:RE_HLS_RTL_PROJECT = 're_engine_direct'
python hls/run_rtl_mmio.py
python hls/run_cosim.py --project re_engine_direct --solution smoke --lines 100 --vectors-dir tests/regex_vm/fixtures/apache_100
vivado -mode batch -source hls/run_vivado_synth.tcl
```

The differential regression covers narrow, packed-wide, and predecoded APIs;
greedy/possessive/lazy loops; failed alternatives; capture restoration; every
byte value including NUL; all class bitmap word lanes; and message alignment,
window, and maximum-length boundaries. Explicit anchor expectations cover a
legacy scalar interpreter bug when non-consuming instructions repeat at a
32-bit message boundary. The Apache benchmark separately compares the original
`regexVM_opt` and the Oniguruma-backed capture vectors.
The synthesized DUT in the 1,037-case RTL run is the native predecoded core;
the other memory layouts are compared in the host reference/testbench.

The general RTL summary reports each rule family separately: scalar class and
literal chains, bounded repeats, nullable lazy suffixes, step-back, marks, capture
restoration, and others. Apache is one additional workload. Scalar/control-heavy
bytecode can remain slower than `regexVM_opt` because the feedback loop has a
larger II. No universal 10x speedup is claimed. Long recognized repetitions
benefit most.

The production MMIO regression additionally checks warm reset with a pending
result, reversed configuration writes, class and instruction edits, scalar
fallback after changing a loop target, a 4,088-byte packet, and all 513 result
words at the 512-group limit.

The complete final MMIO run passes these checks. Its maximum-packet driver
assigns the full 65-bit `TUSER` address and SOP separately: XSim 2021.2 produced
incorrect upper address bits for the previous wide OR with a runtime one-bit
expression. The incorrect address prevented lookup from completing. An isolated
capture regression reproduced the error and verified the explicit-width fix.

Saved evidence is in
[`regex_vm_new_validation.json`](../tests/regex_vm/results/regex_vm_new_validation.json)
and the compact
[`validation log archive`](../tests/regex_vm/results/regex_vm_new_validation_logs.zip).
Obsolete experiments and duplicate build flows were removed. The retained
[focused test guide](../tests/regex_vm/README.md) documents the current entry
points; the 22 removed source/build-entry files remain recoverable from
[`regex_vm_obsolete_sources.zip`](../tests/regex_vm/results/regex_vm_obsolete_sources.zip).

The existing 1,048-case Oniguruma corpus is also reusable without editing its
patterns:

```sh
VITIS_HLS_INCLUDE=/path/to/Vitis_HLS/2021.2/include bash tests/regex_vm/run_oniguruma_corpus.sh
VITIS_HLS_INCLUDE=/path/to/Vitis_HLS/2021.2/include bash tests/regex_vm/run_oniguruma_corpus.sh --baseline
```

The host run compares all three new VM memory layouts. It reports 439 successes:
396 VM executions and 43 expected compiler-error checks. All 396 executed rules
fit the kernel's 2048-byte slots; the largest is 165 words / 1320 bytes. The other
609 cases are excluded by the existing compiler/search harness, including matches
that start after input offset zero. The original optimized VM reports the same
success/exclusion counts. This does not add unsupported regex syntax or a search
loop to the bytecode compiler/VM.

Standalone VM latency and the kernel's `regexVM_cycles` telemetry have different
boundaries because of the worker wrapper and synthesized event timing. Capture
initialization also affects PU occupancy. Use the full stage breakdown and
complete-kernel measurements when assessing integration performance.
