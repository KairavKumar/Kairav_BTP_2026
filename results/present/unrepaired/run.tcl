set K {/home/kairav1902/MlEda/FMSV_project/Kairav}
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_alignment_20261018/common_final.tcl}
load_design {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_alignment_20261018/runs_final_constructor/stock/design.odb} {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_alignment_20261018/runs_final_constructor/stock/design.sdc}
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_larger_20261011/native_sizing_inputs.tcl}
set block [ord::get_db_block]
intprop $block cts_data_skew_native_sizing 0
set state [strip_clock]
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40_targets.tcl}
doubleprop $block cts_cost_energy_weight 40
doubleprop $block cts_cost_target_span_ns 0.02
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/controlled609_g20_x4_v7/round0/calibration.tcl}
clock_tree_synthesis -root_buf BUF_X4 -buf_list {BUF_X1 BUF_X2 BUF_X4 BUF_X8 BUF_X16} -sink_clustering_enable -sink_clustering_size 32 -sink_clustering_max_diameter 100 -dont_use_dummy_load
detailed_placement
set_propagated_clock [all_clocks]
estimate_parasitics -placement
check_data $state
export_state {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40}

set witness [odb::dbStringProperty_find $block cts_cost_witness]
set measured {}
set root_rise 0
set root_fall 0
set sum_rise_error 0
set sum_fall_error 0
set count 0
foreach {tag name} [$witness getValue] {
 set pin [sta::get_port_pin_error cost_input "$name/A"]
 set vertex [lindex [$pin vertices] 0]
 lappend measured $tag [expr {[$vertex slew rise max]/1e-9}] [expr {[$vertex slew fall max]/1e-9}]
 if {$tag eq "root"} {
  set rise_path [sta::vertex_worst_arrival_path_rf $vertex rise max]
  set fall_path [sta::vertex_worst_arrival_path_rf $vertex fall max]
  set half [odb::dbDoubleProperty_find $block cts_cost_half_period_ns]
  set root_rise [expr {[$rise_path arrival]/1e-9}]
  set root_fall [expr {[$fall_path arrival]/1e-9 - [$half getValue]}]
 }
}
foreach clock_pin [all_registers -clock_pins] {
 set name [get_full_name $clock_pin]
 set term [$block findITerm $name]
 set target [odb::dbDoubleProperty_find $term cts_align_target_ns]
 set reference [odb::dbDoubleProperty_find $term cts_align_reference_ns]
 set fall_reference [odb::dbDoubleProperty_find $term cts_align_stock_fall_ns]
 set vertex [lindex [$clock_pin vertices] 0]
 set r [sta::vertex_worst_arrival_path_rf $vertex rise max]
 set f [sta::vertex_worst_arrival_path_rf $vertex fall max]
 set sum_rise_error [expr {$sum_rise_error+[$r arrival]/1e-9-[$target getValue]}]
 set desired_fall [expr {[$fall_reference getValue]+[$target getValue]-[$reference getValue]}]
 set sum_fall_error [expr {$sum_fall_error+[$f arrival]/1e-9-$desired_fall}]
 incr count
}
set old_rise_bias [odb::dbDoubleProperty_find $block cts_cost_common_rise_bias_ns]
set old_fall_bias [odb::dbDoubleProperty_find $block cts_cost_common_fall_bias_ns]
set rb [expr {$old_rise_bias eq "NULL" ? 0.0 : [$old_rise_bias getValue]}]
set fb [expr {$old_fall_bias eq "NULL" ? 0.0 : [$old_fall_bias getValue]}]
set rb [expr {max(0.0,min(.040,$rb+$sum_rise_error/$count))}]
set fb [expr {max(0.0,min(.040,$fb+$sum_fall_error/$count))}]
set file [open {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40/calibration.tcl} w]
puts $file "stringprop \$block cts_cost_slews {$measured}"
puts $file "# Physical root-wire delays: rise=$root_rise ns fall=$root_fall ns"
puts $file "# Positive common model corrections: rise=$rb ns fall=$fb ns; requested targets are unchanged."
puts $file "doubleprop \$block cts_cost_common_rise_bias_ns $rb"
puts $file "doubleprop \$block cts_cost_common_fall_bias_ns $fb"
puts $file "doubleprop \$block cts_cost_root_rise_ns [expr {$root_rise+$rb}]"
puts $file "doubleprop \$block cts_cost_root_fall_ns [expr {$root_fall+$fb}]"
close $file

set p [odb::dbStringProperty_find [ord::get_db_block] cts_cost_frontier_csv]
if {$p ne "NULL"} {set f [open {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40/frontier.csv} w];puts $f [$p getValue];close $f}
