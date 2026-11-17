from pathlib import Path
import json,sys,os
D=Path(__file__).resolve().parent;K=D.parent
sys.path.insert(0,str(K/'cts_alignment_20261018'))
import flow_final as flow
flow.BIN=Path(os.environ.get('COST_CTS_BINARY',str(D/'build3/openroad')))
root=D/os.environ.get('COST_TOY_NAME','toy3');root.mkdir(exist_ok=False)
setup=f'''
read_lef {{{K/'OpenROAD/test/Nangate45/Nangate45.lef'}}}
read_liberty {{{K/'OpenROAD/src/cts/BTP_Research/TETRIS-artifact/src/Design Synthesis/nangate45-pdk/stdcells.lib'}}}
read_verilog {{{K/'cts_alignment_20261018/toy.v'}}}
link_design alignment_toy
initialize_floorplan -die_area {{0 0 40 40}} -core_area {{2 2 38 38}} -site FreePDK45_38x28_10R_NP_162NW_34O
create_clock -period 2 clk
set_input_transition .020 [all_inputs]
set_input_delay -clock clk -min .3 [get_ports {{a b}}]
set_input_delay -clock clk -max .4 [get_ports {{a b}}]
set_output_delay -clock clk -min .1 [get_ports y]
set_output_delay -clock clk -max .2 [get_ports y]
source {{{K/'OpenROAD/test/Nangate45/Nangate45.rc'}}}
set_wire_rc -signal -layer metal3
set_wire_rc -clock -layer metal6
set block [ord::get_db_block]
set row [lindex [$block getRows] 0]
lassign [$row getOrigin] x y
set i 0
foreach name {{r0 r1 r2 data_delay meeting}} {{
 set inst [$block findInst $name]
 $inst setLocation [expr {{$x+$i*20*[$row getSpacing]}}] $y
 $inst setOrient [$row getOrient]
 $inst setPlacementStatus FIRM
 incr i
}}
'''
summaries={}
for name,targets in [('equal',[.08,.08,.08]),('different',[.08,.11,.08]),('unattainable',[-1,-1,-1]),('ordinary',None)]:
    out=root/name
    extra=''
    if targets:
        extra='intprop $block cts_data_alignment 1\nintprop $block cts_align_defer 1\nintprop $block cts_cost_aware_tree 1\nintprop $block cts_data_skew_native_sizing 0\ndoubleprop $block cts_cost_half_period_ns 1\ndoubleprop $block cts_cost_energy_weight 0\ndoubleprop $block cts_cost_target_span_ns .01\n'
        for i,target in enumerate(targets):
            extra+=f'set pin [$block findITerm r{i}/CK]\ndoubleprop $pin cts_align_target_ns {target}\ndoubleprop $pin cts_align_reference_ns 0\ndoubleprop $pin cts_align_stock_fall_ns 1\n'
    finish=f'''
clock_tree_synthesis -root_buf BUF_X4 -buf_list {{BUF_X1 BUF_X2 BUF_X4 BUF_X8}} -sink_clustering_enable -sink_clustering_size 4 -sink_clustering_max_diameter 60 -dont_use_dummy_load
detailed_placement
set_propagated_clock [all_clocks]
estimate_parasitics -placement
source {{{K/'OpenROAD/src/cts/src/DataSkewSizing.tcl'}}}
set reference [dict create]
foreach pin {{r0/CK r1/CK r2/CK}} {{dict set reference $pin {{0 0 0}}}}
set gates [dict create meeting [list 1 {{meeting/A meeting/B}}]]
set m [cts_data_skew::measure $reference $gates]
set fp [open {{{out/'metrics.csv'}}} w]
puts $fp "spread_ns,setup_ns,hold_ns,electrical_pass"
puts $fp "[dict get $m gate_score_ns],[dict get $m setup_ns],[dict get $m hold_ns],[dict get $m electrical_pass]"
close $fp
set fp [open {{{out/'clocks.csv'}}} w]
puts $fp "pin,early_ns,late_ns"
dict for {{pin vals}} [dict get $m clocks] {{puts $fp "$pin,[lindex $vals 0],[lindex $vals 1]"}}
close $fp
check_placement -verbose
puts ALIGNMENT_EXPORT_PASS
'''
    try:
        flow.run_tcl(out,setup+extra+finish)
    except RuntimeError:
        log=(out/'run.log').read_text()
        if name=='unattainable' and ('nonnegative' in log or 'unreachable' in log):
            summaries[name]={'status':'UNREACHABLE_REPORTED'}
            print(name,json.dumps(summaries[name]),flush=True)
            continue
        raise
    got={r['pin']:(float(r['early_ns'])+float(r['late_ns']))/2 for r in flow.rows(out/'clocks.csv')}
    summaries[name]={'clock_arrivals_ns':got,'metrics':flow.rows(out/'metrics.csv')[0]}
    if targets:summaries[name]['max_target_error_ps']=max(abs(got[f'r{i}/CK']-target)*1000 for i,target in enumerate(targets))
    print(name,json.dumps(summaries[name]),flush=True)
assert summaries['equal']['max_target_error_ps']<15
assert summaries['different']['max_target_error_ps']<15
assert summaries['unattainable'].get('status')=='UNREACHABLE_REPORTED' or summaries['unattainable']['max_target_error_ps']>1000
(root/'summary.json').write_text(json.dumps(summaries,indent=2))
