set experiment /home/c_karfa/Kairav/cts_larger_20261011
if {[file normalize [pwd]] ne $experiment} {error "Wrong working directory"}
if {[file exists mapped]} {error "Preserve existing mapped results"}
file mkdir mapped
set_app_var target_library /home/c_karfa/Kairav/secure_skew_demo_20261010/nangate_demo.db
set_app_var link_library [list * $target_library]
set_host_options -max_cores 2
read_verilog {present_hls.v masked_present_layer.v}
current_design masked_present_layer
link
uniquify
# Flatten into standard cells while retaining source net names for verified annotations.
ungroup -all -flatten
create_clock -name clk -period 2 [get_ports clk]
set_clock_uncertainty 0.02 [get_clocks clk]
set data [remove_from_collection [all_inputs] [get_ports clk]]
set_input_delay -min 0.10 -clock clk $data
set_input_delay -max 0.20 -clock clk $data
set_input_transition 0.02 [all_inputs]
set_output_delay -min 0 -clock clk [all_outputs]
set_output_delay -max 0.20 -clock clk [all_outputs]
set_load 2 [all_outputs]
set_app_var compile_enable_register_merging false
compile -map_effort low
change_names -hierarchy -rules verilog
check_design > mapped/check_design.rpt
report_area > mapped/area.rpt
report_timing > mapped/timing.rpt
write -format verilog -hierarchy -output mapped/synth.v
write_sdc mapped/design.sdc
exit
