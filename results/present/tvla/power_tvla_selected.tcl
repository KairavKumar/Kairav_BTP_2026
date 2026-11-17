set case $::env(ASSESS_CASE)
set mode $::env(ASSESS_MODE)
if {$case ni {stock mixed_e40_critical_leaves} || $mode ni {fvr null}} {error "Unexpected assessment"}
set run runs/${case}_${mode}
set_host_options -max_cores 2
set power_enable_analysis true
set power_analysis_mode time_based
set power_enable_clock_cycle_based_glitch true
set power_filter_glitch_pulse_width -1.0
set link_library [list * {/home/c_karfa/Kairav/cts_lab_assessment_20261018/library.db}]
read_verilog $case/design.v
current_design masked_present_layer
link
report_units > $run/units.rpt
read_sdc $case/design.pt.sdc
set_propagated_clock [all_clocks]
read_parasitics $case/design.spef
update_timing
check_timing -verbose > $run/timing_checks.rpt
report_timing -delay_type max -path_type full_clock_expanded -max_paths 10 -significant_digits 9 > $run/primetime_setup.rpt
report_timing -delay_type min -path_type full_clock_expanded -max_paths 10 -significant_digits 9 > $run/primetime_hold.rpt
report_constraint -all_violators > $run/constraint_violations.rpt
report_cell > $run/cells.rpt
read_vcd $run/activity.vcd -strip_path tb_layer/uut
check_power -verbose > $run/power_checks.rpt
set_power_analysis_options -waveform_format out -waveform_output $run/power -waveform_interval 0.005
update_power
report_power > $run/average_power.rpt
report_power -hierarchy > $run/hierarchy_power.rpt
puts ASSESSMENT_POWER_COMPLETE
quit
