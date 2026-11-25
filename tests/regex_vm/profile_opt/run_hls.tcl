set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../../.."]
if {[info exists ::env(DEBUG)]} { unset ::env(DEBUG) }
open_project "$here/../build/profile_opt_dut"
set_top regex_vm_opt_perf
add_files "$here/../regex_vm_opt_perf.cpp" -cflags "-std=c++14 -I$root/include/hw"
open_solution -flow_target vivado sol
set_part xcvc1902-vsva2197-2MP-e-S
create_clock -period 6
config_compile -pipeline_loops 0
config_array_partition -throughput_driven off
csynth_design
exit
