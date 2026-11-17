set K {/home/kairav1902/MlEda/FMSV_project/Kairav}
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_alignment_20261018/common_final.tcl}
load_design {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40/design.odb} {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40/design.sdc}
source {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_larger_20261011/native_sizing_inputs.tcl}
set block [ord::get_db_block]
set edits {clkbuf_cost_239_clk BUF_X4 clkbuf_cost_246_clk BUF_X4 clkbuf_cost_249_clk BUF_X4 clkbuf_cost_256_clk BUF_X4 clkbuf_cost_267_clk BUF_X4 clkbuf_cost_305_clk BUF_X4 clkbuf_cost_309_clk BUF_X4 clkbuf_cost_326_clk BUF_X4 clkbuf_cost_327_clk BUF_X4}

set snapshot {};set statuses {}
foreach inst [$block getInsts] {
 if {[dict exists $edits [$inst getName]]} {continue}
 dict set snapshot [$inst getName] [list [$inst getLocation] [$inst getOrient] [[$inst getMaster] getName]]
 dict set statuses [$inst getName] [$inst getPlacementStatus]
 $inst setPlacementStatus FIRM
}
dict for {name master} $edits {replace_cell $name $master}
detailed_placement
dict for {name state} $snapshot {
 set inst [$block findInst $name]
 if {[list [$inst getLocation] [$inst getOrient] [[$inst getMaster] getName]] ne $state} {error "Frozen cell changed"}
 $inst setPlacementStatus [dict get $statuses $name]
}
set_propagated_clock [all_clocks]
estimate_parasitics -placement
export_state {/home/kairav1902/MlEda/FMSV_project/Kairav/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40_critical_leaves}
