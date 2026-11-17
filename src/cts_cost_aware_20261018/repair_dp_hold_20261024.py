"""Timing-directed hold recovery from saved independent PT paths; no data edits."""
import json
from pathlib import Path
from power_campaign_20261024 import D,flow,compare
ROOT=D/'dp_frontier_20261024/v4'
if __name__=='__main__':
    for label,edits in [('root32',{'clkbuf_0_clk':'BUF_X32'}),('critical_leaves',{f'clkbuf_cost_{n}_clk':'BUF_X4' for n in (239,246,249,256,267,305,309,326,327)})]:
        out=ROOT/('mixed_e40_'+label)
        body=flow.load(ROOT/'mixed_e40')+'set block [ord::get_db_block]\nset edits {'+' '.join(k+' '+v for k,v in edits.items())+'}\n'+r'''
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
'''+f'export_state {{{out}}}\n'
        flow.run_tcl(out,body)
        report=compare(out)
        (out/'edits.json').write_text(json.dumps(edits,indent=2))
        print(label,json.dumps({k:report[k] for k in ('weighted_reduction_percent','legal','max_clock_movement_ps','metrics')}),flush=True)
