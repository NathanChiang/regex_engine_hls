set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../.."]
if {[info exists ::env(REGEX_ENGINE_ROOT)]} { set root [file normalize $::env(REGEX_ENGINE_ROOT)] }
file mkdir "$here/build"
cd "$here/build"
open_project -reset regex_vm_new_csim.prj
set_top regex_vm_new_synth
set flags "-std=c++14 -I$root/include/hw"
add_files "$here/regex_vm_new_synth.cpp" -cflags $flags
add_files -tb "$here/regex_vm_new_diff.cpp" -cflags $flags
open_solution -reset sol
set_part xcvc1902-vsva2197-2MP-e-S
create_clock -period 9
csim_design -clean
exit
