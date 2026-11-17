from pathlib import Path
import sys,json
R=Path(__file__).resolve().parent;K=R.parent;D=K/'cts_cost_aware_20261018'
sys.path.insert(0,str(D));import experiment as e
e.flow.BIN=D/'build_dp_20261024_v4/openroad'
snapshot=r"""
proc snapshot_data {path} {
 set f [open $path w];set block [ord::get_db_block]
 set regs [dict create]
 foreach pin [all_registers -clock_pins] {dict set regs [get_full_name $pin] 1}
 foreach inst [lsort -command {apply {{a b} {string compare [$a getName] [$b getName]}}} [$block getInsts]] {
  set isclk 0
  foreach term [$inst getITerms] {
   set net [$term getNet]
   if {$net ne "NULL" && [$term isInputSignal] && [$net getSigType] eq "CLOCK" && ![dict exists $regs "[$inst getName]/CK"]} {set isclk 1}
  }
  if {$isclk} {continue}
  set row [list [$inst getName] [[$inst getMaster] getName] [$inst getLocation] [$inst getOrient]]
  set terms {}
  foreach term [$inst getITerms] {
   set name "[$inst getName]/[[$term getMTerm] getName]"
   if {[dict exists $regs $name]} {continue}
   set net [$term getNet]
   lappend terms [list $name [expr {$net eq "NULL" ? "NULL" : [$net getName]}]]
  }
  puts $f [concat $row [lsort $terms]]
 }
 close $f
}
"""
out=R/'identity_stock'
if not out.exists():e.flow.run_tcl(out,e.flow.load(e.STOCK)+snapshot+f'set state [strip_clock]\nsnapshot_data {{{out/"data.txt"}}}\nputs ALIGNMENT_EXPORT_PASS\n')
for name in sys.argv[1:]:
 source=R/'icts'/name;out=R/'icts_evaluated'/name
 if not (source/'execution.json').exists() or not (source/'design.def').exists():continue
 if out.exists():
  if (out/'result.json').exists():continue
  out=R/'icts_evaluated'/(name+'_retry1')
  if out.exists():continue
 body=f"""set_thread_count 2
sta::set_thread_count 1
read_liberty {{{K/'OpenROAD/src/cts/BTP_Research/TETRIS-artifact/src/Design Synthesis/nangate45-pdk/stdcells.lib'}}}
read_lef {{{K/'OpenROAD/test/Nangate45/Nangate45.lef'}}}
read_def {{{source/'design.def'}}}
read_sdc {{{e.STOCK/'design.sdc'}}}
source {{{K/'OpenROAD/test/Nangate45/Nangate45.rc'}}}
set_wire_rc -signal -layer metal3
set_wire_rc -clock -layer metal6
set_propagated_clock [all_clocks]
estimate_parasitics -placement
source {{{K/'cts_larger_20261011/native_sizing_inputs.tcl'}}}
set block [ord::get_db_block]
"""+snapshot+f"snapshot_data {{{out/'before_data.txt'}}}\n"
 # All original data instances are fixed; only new CTS cells can be legalized.
 body+='foreach inst [$block getInsts] {set clk 0;foreach term [$inst getITerms] {if {[$term isOutputSignal] && [$term getNet] ne "NULL" && [[$term getNet] getSigType] eq "CLOCK"} {set clk 1}};if {!$clk} {$inst setPlacementStatus FIRM}}\n'
 body+=f'detailed_placement\nset_propagated_clock [all_clocks]\nestimate_parasitics -placement\nsnapshot_data {{{out/"after_data.txt"}}}\nexport_state {{{out}}} 0\n'
 try:
  e.flow.run_tcl(out,body)
  expected=(R/'identity_stock/data.txt').read_bytes()
  checks={n:(out/n).read_bytes()==expected for n in ['before_data.txt','after_data.txt']}
  (out/'identity.json').write_text(json.dumps(checks,indent=2));assert all(checks.values()),checks
  result=e.compare(out);print(name,result['weighted_reduction_percent'],result['metrics'],flush=True)
 except Exception as ex:print(name,type(ex).__name__,str(ex),flush=True)
