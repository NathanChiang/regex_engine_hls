#
# Copyright 2019-2020 Xilinx, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set here [file dirname [file normalize [info script]]]
set root [file normalize "$here/../.."]
# The named legacy entry point can also launch the full-kernel performance
# flow without duplicating its synthesis setup:
#   RE_HLS_ACTION=perf vitis_hls -f tests/regex_vm/run_hls.tcl
if {[info exists ::env(RE_HLS_ACTION)] &&
    $::env(RE_HLS_ACTION) in {perf perf_only}} {
  source "$root/hls/run_hls.tcl"
  exit
}
source "$here/settings.tcl"

if {[info exists ::env(REGEX_ENGINE_ROOT)]} { set root [file normalize $::env(REGEX_ENGINE_ROOT)] }
set PROJ "re_test.prj"
set SOLN "sol1"

if {![info exists CLKP]} {
  set CLKP 3.33
}

file mkdir "$here/build"
cd "$here/build"
open_project -reset $PROJ

set cflags "-I$root/include/sw -I$root/include/hw -I$root/third_party/oniguruma/lib/include"
set ldflags "-Wl,-rpath,$root/re_compile/lib/lib -L$root/re_compile/lib/lib -Wl,-rpath,$root/third_party/oniguruma/lib/lib -L$root/third_party/oniguruma/lib/lib -lxfcompile -lonig"
add_files "$here/test.cpp" -cflags $cflags
add_files -tb "$here/test.cpp" -cflags $cflags
set_top dut

open_solution -reset $SOLN




set_part $XPART
create_clock -period $CLKP

if {$CSIM == 1} {
  csim_design -ldflags $ldflags
}

if {$CSYNTH == 1} {
  csynth_design
}

if {$COSIM == 1} {
  cosim_design -ldflags $ldflags
}

if {$VIVADO_SYN == 1} {
  export_design -flow syn -rtl verilog
}

if {$VIVADO_IMPL == 1} {
  export_design -flow impl -rtl verilog
}

exit
