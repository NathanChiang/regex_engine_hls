# C/RTL regressions for the kernel's configuration path.
set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../.."]
if {[info exists ::env(DEBUG)]} { unset ::env(DEBUG) }
file mkdir "$here/build"
cd "$here/build"
set flags "-std=c++14 -I$root/include/hw -I$root/kernel"
foreach {top bench} {
    class_config_probe test_class_config_probe
    regex_vm_prepare test_prepare
    rule_config_probe test_rule_config_probe
} {
    open_project $top
    set_top $top
    add_files "$here/$top.cpp" -cflags $flags
    add_files -tb "$here/$bench.cpp" -cflags $flags
    open_solution -flow_target vivado sol
    set_part xcvc1902-vsva2197-2MP-e-S
    create_clock -period 6
    config_compile -pipeline_loops 0
    config_array_partition -throughput_driven off
    csim_design -clean
    csynth_design
    cosim_design -rtl verilog -trace_level none
    close_project
}
exit
