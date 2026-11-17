# Check distinct shared-branch targets and preservation of data-cell placement.
# SPDX-License-Identifier: BSD-3-Clause
source "helpers.tcl"
read_lef Nangate45/Nangate45.lef
read_liberty Nangate45/Nangate45_typ.lib
read_verilog cost_aware_tree.v
link_design cost_aware_tree
initialize_floorplan -die_area {0 0 40 40} -core_area {2 2 38 38} \
  -site FreePDK45_38x28_10R_NP_162NW_34O
create_clock -period 2 clk
set_input_transition .020 [all_inputs]
set_input_delay -clock clk -min .3 [get_ports {a b}]
set_input_delay -clock clk -max .4 [get_ports {a b}]
set_output_delay -clock clk -min .1 [get_ports y]
set_output_delay -clock clk -max .2 [get_ports y]
source Nangate45/Nangate45.rc
set_wire_rc -signal -layer metal3
set_wire_rc -clock -layer metal6
set block [ord::get_db_block]
set row [lindex [$block getRows] 0]
lassign [$row getOrigin] x y
set i 0
set identity {}
foreach name {r0 r1 r2 data_delay meeting} {
  set inst [$block findInst $name]
  $inst setLocation [expr {$x + $i * 20 * [$row getSpacing]}] $y
  $inst setOrient [$row getOrient]
  $inst setPlacementStatus FIRM
  dict set identity $name [list [[$inst getMaster] getName] [$inst getLocation]]
  incr i
}
odb::dbIntProperty_create $block cts_cost_aware_tree 1
odb::dbIntProperty_create $block cts_data_skew_native_sizing 0
odb::dbDoubleProperty_create $block cts_cost_half_period_ns 1
odb::dbDoubleProperty_create $block cts_cost_energy_weight 0
odb::dbDoubleProperty_create $block cts_cost_target_span_ns .01
foreach {pin target} {r0/CK .08 r1/CK .11 r2/CK .08} {
  set term [$block findITerm $pin]
  odb::dbDoubleProperty_create $term cts_align_target_ns $target
  odb::dbDoubleProperty_create $term cts_align_reference_ns 0
  odb::dbDoubleProperty_create $term cts_align_stock_fall_ns 1
}
clock_tree_synthesis -root_buf BUF_X4 -buf_list {BUF_X1 BUF_X2 BUF_X4 BUF_X8} \
  -sink_clustering_enable -sink_clustering_size 4 \
  -sink_clustering_max_diameter 60 -dont_use_dummy_load
detailed_placement
set_propagated_clock [all_clocks]
estimate_parasitics -placement
sta::worst_slack -max
set arrivals {}
foreach {name target} {r0/CK .08 r1/CK .11 r2/CK .08} {
  set pin [sta::get_port_pin_error clock $name]
  set vertex [lindex [$pin vertices] 0]
  set path [sta::vertex_worst_arrival_path_rf $vertex rise max]
  if {$path eq "NULL"} {error "Missing clock path"}
  set arrival [expr {[$path arrival] / 1e-9}]
  if {abs($arrival - $target) > .020} {error "Target-delivery regression"}
  dict set arrivals $name $arrival
}
if {[dict get $arrivals r1/CK] - [dict get $arrivals r0/CK] < .020} {
  error "Intentional clock offset was lost"
}
dict for {name record} $identity {
  set inst [$block findInst $name]
  if {[list [[$inst getMaster] getName] [$inst getLocation]] ne $record} {
    error "Data cell changed"
  }
}
check_placement -verbose
puts "pass COST_AWARE_TARGET_DELIVERY"
