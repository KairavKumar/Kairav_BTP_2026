"""Independent reload certification and matched routing of legal improvements."""
import argparse,csv,json
from pathlib import Path
import flow_final as flow
D,K=flow.D,flow.K

identity_tcl='''
set block [ord::get_db_block]
set ck [dict create]
foreach pin [all_registers -clock_pins] {dict set ck [get_full_name $pin] 1}
set records [dict create]
foreach inst [$block getInsts] {
 set ci 0;set dout 0
 foreach term [$inst getITerms] {
  set net [$term getNet]
  if {$net eq "NULL"} {continue}
  if {[$term isInputSignal] && [$net getSigType] eq "CLOCK"} {set ci 1}
  if {[$term isOutputSignal] && [$net getSigType] ne "CLOCK"} {set dout 1}
 }
 if {$ci && !$dout && ![dict exists $ck "[$inst getName]/CK"]} {continue}
 set rec [list [[$inst getMaster] getName] [$inst getLocation] [$inst getOrient]]
 set terms {}
 foreach term [$inst getITerms] {
  set name "[$inst getName]/[[$term getMTerm] getName]"
  if {[dict exists $ck $name]} {continue}
  set net [$term getNet]
  lappend terms [list $name [expr {$net eq "NULL" ? "NULL" : [$net getName]}]]
 }
 lappend rec [lsort $terms]
 dict set records [$inst getName] $rec
}
set f [open {@OUT@/data_identity.txt} w]
foreach name [lsort [dict keys $records]] {puts $f [list $name [dict get $records $name]]}
close $f
'''

def verify(source,out,stock,bound,budget):
    body=flow.load(source)+identity_tcl.replace('@OUT@',str(out))+f'export_state {{{out}}}\n'
    flow.run_tcl(out,body)
    result=flow.compare(out,stock,bound,budget)
    assert (out/'data_identity.txt').read_bytes()==(stock/'data_identity.txt').read_bytes(),'Independent data identity mismatch'
    result['data_identity']='PASS'
    (out/'certification.json').write_text(json.dumps(result,indent=2))
    return result

def route(source,out):
    # No repair_design/repair_timing: routing must not modify the frozen data.
    body=flow.load(source)+f'''
set_global_routing_layer_adjustment metal2-metal10 0.3
set_routing_layers -signal metal2-metal10 -clock metal6-metal10
pin_access
global_route -guide_file {{{out / 'route.guide'}}} -congestion_iterations 100
detailed_route -output_drc {{{out / 'route_drc.rpt'}}} -no_pin_access -verbose 0
if {{[detailed_route_num_drvs] != 0}} {{error "Routing DRC violations"}}
define_process_corner -ext_model_index 0 typical
extract_parasitics -ext_model_file {{{K / 'OpenROAD/test/Nangate45/Nangate45.rcx_rules'}}}
write_spef {{{out / 'design.spef'}}}
read_spef {{{out / 'design.spef'}}}
''' + identity_tcl.replace('@OUT@',str(out))
    # Placement-based source graph export is deliberately disabled here.
    # export_state does not estimate parasitics, so these metrics use extraction.
    body+=f'export_state {{{out}}} 0\n'
    return flow.run_tcl(out,body)

def main():
    p=argparse.ArgumentParser();p.add_argument('--runs',type=Path,default=D/'runs_final_constructor');p.add_argument('--route',action='store_true');a=p.parse_args()
    out=a.runs/'certification';out.mkdir(exist_ok=False)
    stock=out/'stock'
    flow.run_tcl(stock,flow.load(a.runs/'stock')+identity_tcl.replace('@OUT@',str(stock))+f'export_state {{{stock}}}\n')
    cases=json.loads((a.runs/'matrix_summary.json').read_text());summary={}
    for case,record in cases.items():
        if record['best'] is None: continue
        best=record['best'];dest=out/case
        result=verify(Path(best['folder']),dest,stock,best['bound_ps'],best['regression_budget_ps'])
        assert result['accepted_improvement'],'Winner failed independent reload'
        assert abs(result['weighted_reduction_percent']-best['weighted_reduction_percent'])<1e-7
        summary[case]=result
    (out/'summary.json').write_text(json.dumps(summary,indent=2))
    if a.route and summary:
        routes=a.runs/'routing';routes.mkdir(exist_ok=False)
        route(stock,routes/'stock')
        routed={}
        for case in summary:
            route(out/case,routes/case)
            routed[case]=flow.compare(routes/case,routes/'stock',summary[case]['bound_ps'],summary[case]['regression_budget_ps'])
            assert (routes/case/'data_identity.txt').read_bytes()==(stock/'data_identity.txt').read_bytes()
        (routes/'summary.json').write_text(json.dumps(routed,indent=2))
    print('CERTIFICATION_COMPLETE',len(summary))

if __name__=='__main__':main()
