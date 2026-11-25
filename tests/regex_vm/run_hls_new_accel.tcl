set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../.."]
if {[info exists ::env(REGEX_ENGINE_ROOT)]} { set root [file normalize $::env(REGEX_ENGINE_ROOT)] }
if {[info exists ::env(DEBUG)]} { unset ::env(DEBUG) }
set action csim
if {[info exists ::env(RE_VM_ACTION)]} { set action $::env(RE_VM_ACTION) }
if {$action ni {csim synth cosim cosim_only}} { error "Unknown RE_VM_ACTION: $action" }
file mkdir "$here/build"
cd "$here/build"
set bench apache
if {[info exists ::env(RE_VM_BENCH)]} { set bench $::env(RE_VM_BENCH) }
if {$bench ni {apache diff}} { error "Unknown RE_VM_BENCH: $bench" }
set project regex_vm_new_accel_perf
if {$bench eq "diff"} { set project regex_vm_new_accel_diff }
set variant accel
if {[info exists ::env(RE_VM_VARIANT)]} { set variant $::env(RE_VM_VARIANT) }
if {$variant ni {accel opt}} { error "Unknown RE_VM_VARIANT: $variant" }
if {$variant eq "opt" && $bench ne "apache"} { error "The opt baseline uses the Apache fixture" }
set top regex_vm_new_accel_perf
set tb_define COSIM_ACCEL
if {$variant eq "opt"} {
    set project regex_vm_opt_perf
    set top regex_vm_opt_perf
    set tb_define COSIM_OPT
}
if {[info exists ::env(RE_VM_PROJECT_SUFFIX)]} { append project $::env(RE_VM_PROJECT_SUFFIX) }
open_project $project
if {$bench eq "diff"} { set ::env(RE_VM_CASE_REPORT) "$here/build/$project/cases.csv" }
set_top $top
set flags "-std=c++14 -I$root/include/hw"
if {[info exists ::env(RE_VM_II)]} { append flags " -DXF_REGEX_VM_NEW_WIDE_II=$::env(RE_VM_II)" }
set predecoded 0
if {[info exists ::env(RE_VM_PREDECODED)] && $::env(RE_VM_PREDECODED) eq "1"} {
    set predecoded 1
    append flags " -DXF_REGEX_VM_NEW_PREDECODED"
    set ::env(RE_VM_DESCRIPTOR_EXPORT) "$here/build/$project/instruction_rows.txt"
}
if {[info exists ::env(RE_VM_NOPIPE)] && $::env(RE_VM_NOPIPE) eq "1"} {
    append flags " -DXF_REGEX_VM_NEW_NO_PIPELINE"
}
add_files "$here/$top.cpp" -cflags $flags
if {$bench eq "apache"} {
    add_files -tb "$here/regex_vm_new_apache.cpp" -cflags "$flags -D$tb_define"
} else {
    add_files -tb "$here/regex_vm_new_diff.cpp" -cflags "$flags -DCOSIM_ACCEL"
}
open_solution -flow_target vivado sol
set_part xcvc1902-vsva2197-2MP-e-S
create_clock -period 6
config_compile -pipeline_loops 0
config_array_partition -throughput_driven off
set vectors "$here/fixtures/apache_100/cosim_input.txt"
if {[info exists ::env(RE_VM_VECTORS)]} { set vectors $::env(RE_VM_VECTORS) }
if {$bench eq "diff"} { set vectors "" }
if {$action eq "csim" || ($predecoded && $bench eq "apache" && $action in {synth cosim})} {
    csim_design -clean -argv $vectors
}
if {$action in {synth cosim}} { csynth_design }
if {$action in {cosim cosim_only}} { cosim_design -rtl verilog -trace_level none -argv $vectors }
exit
