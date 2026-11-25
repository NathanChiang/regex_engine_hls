# Physical primitive utilization check for the complete post-HLS kernel.
# RE_HLS_RTL_PROJECT defaults to re_engine; RE_HLS_RTL_SOLUTION defaults to smoke.
# Run with Vivado 2021.2: vivado -mode batch -source hls/run_vivado_synth.tcl
set here [file dirname [file normalize [info script]]]
set project re_engine
if {[info exists ::env(RE_HLS_RTL_PROJECT)]} { set project $::env(RE_HLS_RTL_PROJECT) }
set solution smoke
if {[info exists ::env(RE_HLS_RTL_SOLUTION)]} { set solution $::env(RE_HLS_RTL_SOLUTION) }
set rtl "$here/build/$project/$solution/syn/verilog"
if {![file exists "$rtl/reEngineKernel.v"]} { error "Missing HLS RTL: $rtl" }
set work "$here/build/vivado_$project"
file mkdir $work
cd $work
set_param general.maxThreads 4
create_project -in_memory -part xcvc1902-vsva2197-2MP-e-S
set_property target_language Verilog [current_project]
foreach data [glob -nocomplain "$rtl/*.dat"] { file copy -force $data $work }
foreach ip_script [glob -nocomplain "$rtl/*_ip.tcl"] { source $ip_script }
if {[llength [get_ips -quiet]]} {
    generate_target synthesis [get_ips]
    synth_ip [get_ips]
}
add_files -norecurse [glob "$rtl/*.v"]
set constraints [open "$work/clock.xdc" w]
puts $constraints {create_clock -period 6.000 -name ap_clk [get_ports ap_clk]}
close $constraints
read_xdc "$work/clock.xdc"
synth_design -top reEngineKernel -mode out_of_context -flatten_hierarchy rebuilt
report_utilization -file "$work/utilization.rpt"
report_utilization -hierarchical -file "$work/utilization_hierarchical.rpt"
report_timing_summary -max_paths 10 -file "$work/timing_summary.rpt"
write_checkpoint -force "$work/reEngineKernel_synth.dcp"
exit
