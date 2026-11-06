set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../.."]
if {[info exists ::env(REGEX_ENGINE_ROOT)]} {
  set root [file normalize $::env(REGEX_ENGINE_ROOT)]
}

set part "xcvc1902-vsva2197-2MP-e-S"
set period 6.0
if {[info exists ::env(RE_VM_PERF_CLOCK_NS)]} {
  set period $::env(RE_VM_PERF_CLOCK_NS)
}

file mkdir "$here/build"
cd "$here/build"

proc synth_vm {project top source include_root part period} {
  open_project -reset $project
  set_top $top
  add_files $source -cflags "-std=c++14 -I$include_root"
  open_solution -reset sol
  set_part $part
  create_clock -period $period
  csynth_design
  close_project
}

set only ""
if {[info exists ::env(RE_VM_PERF_ONLY)]} {
  set only $::env(RE_VM_PERF_ONLY)
}

if {$only ne "new"} {
  synth_vm regex_vm_opt_perf regex_vm_opt_perf \
    "$here/regex_vm_opt_perf.cpp" "$root/include/hw" $part $period
}
if {$only ne "opt"} {
  synth_vm regex_vm_new_perf regex_vm_new_perf \
    "$here/regex_vm_new_perf.cpp" "$root/include/hw" $part $period
}

exit
