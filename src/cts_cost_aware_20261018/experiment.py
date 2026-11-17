"""Construction/STA experiment with explicit reporting-only gate regressions."""
from pathlib import Path
import argparse,csv,hashlib,json,sys,time,os
from collections import defaultdict
import numpy as np
from scipy.optimize import linprog
from scipy.sparse import coo_matrix
D=Path(__file__).resolve().parent;K=D.parent
sys.path.insert(0,str(K/'cts_alignment_20261018'))
import flow_final as flow
flow.BIN=Path(os.environ.get('COST_CTS_BINARY',str(D/'build3/openroad')))
STOCK=K/'cts_alignment_20261018/runs_final_constructor/stock'

def plan(out,gain,bound=80,grouping=None,maximize=False,anchor=None):
    out.mkdir(exist_ok=False,parents=True)
    source_design=Path(anchor) if anchor else STOCK
    clocks={r['pin']:r for r in flow.rows(source_design/'model_clocks.csv')}
    original_clocks={r['pin']:r for r in flow.rows(STOCK/'model_clocks.csv')}
    pins=sorted(clocks);regid={p.rsplit('/',1)[0]:i for i,p in enumerate(pins)}
    profiles=defaultdict(lambda:defaultdict(lambda:[float('inf'),-float('inf')]))
    weights={}
    for r in flow.rows(source_design/'model_sources.csv'):
        g=r['gate'];source=regid.get(r['source'].rsplit('/',1)[0],-1)
        profile=profiles[g][source]
        profile[0]=min(profile[0],float(r['early_ns'])*1000)
        profile[1]=max(profile[1],float(r['late_ns'])*1000)
        weights[g]=float(r['weight'])
    gates=sorted(profiles);N=len(pins);G=len(gates)
    gi={g:N+2*i for i,g in enumerate(gates)}
    abs_start=N+2*G;excess_start=abs_start+N;size=excess_start+G
    rr=[];cc=[];vv=[];rhs=[]
    def add(terms,limit):
        row=len(rhs)
        for col,value in terms:
            rr.append(row);cc.append(col);vv.append(value)
        rhs.append(limit)
    widths={r['gate']:float(r['width_ns'])*1000 for r in flow.rows(STOCK/'gates.csv')}
    for g,by in profiles.items():
        low=gi[g];high=low+1
        for source,(a,b) in by.items():
            add([(low,1)]+([(source,-1)] if source>=0 else []),a)
            add([(high,-1)]+([(source,1)] if source>=0 else []),-b)
        add([(high,1),(low,-1),(excess_start+gates.index(g),-1)],widths[g]+5)
    for i in range(N):
        add([(i,1),(abs_start+i,-1)],0)
        add([(i,-1),(abs_start+i,-1)],0)
    if grouping:
        ids={pin:i for i,pin in enumerate(pins)}
        for group in grouping:
            a=ids[group[0]];ca=clocks[pins[a]]
            at=(float(ca['rise_min_ns'])+float(ca['rise_max_ns']))/2
            for pin in group[1:]:
                b=ids[pin];cb=clocks[pin]
                bt=(float(cb['rise_min_ns'])+float(cb['rise_max_ns']))/2
                add([(a,1),(b,-1)],(bt-at)*1000)
                add([(a,-1),(b,1)],(at-bt)*1000)
    timing_start=len(rhs)
    preferred_margin=30 if anchor else 20
    for r in flow.rows(source_design/'timing.csv'):
        sign=1 if r['mode']=='max' else -1
        terms=defaultdict(float)
        if r['source'] in regid:terms[regid[r['source']]]+=sign
        if r['target'] in regid:terms[regid[r['target']]]-=sign
        add(list(terms.items()),float(r['slack_ns'])*1000-preferred_margin)
    spread={gi[g]+1:weights[g] for g in gates}
    spread.update({gi[g]:-weights[g] for g in gates})
    stock_cost=sum(weights[g]*widths[g] for g in gates)
    add(list(spread.items()),stock_cost*(1-gain/100))
    A=coo_matrix((vv,(rr,cc)),shape=(len(rhs),size)).tocsr()
    objective=np.zeros(size);objective[abs_start:excess_start]=1
    # Prefer limited total regression, without imposing a per-gate 5ps cap.
    objective[excess_start:]=2
    if maximize:
        objective[:]=0
        for index,value in spread.items():objective[index]=value
    bounds=[(-bound+10,bound-10)]*N+[(None,None)]*(2*G)+[(0,None)]*(N+G)
    if anchor:
        for i,pin in enumerate(pins):
            old=original_clocks[pin];current=clocks[pin]
            lower=max((float(old[key])-float(current[key]))*1000-bound+10 for key in ['rise_min_ns','rise_max_ns','fall_min_ns','fall_max_ns'])
            upper=min((float(old[key])-float(current[key]))*1000+bound-10 for key in ['rise_min_ns','rise_max_ns','fall_min_ns','fall_max_ns'])
            bounds[i]=(max(lower,-15),min(upper,15))
            if bounds[i][0]>bounds[i][1]:raise RuntimeError('No remaining movement window for refreshed construction')
    result=linprog(objective,A_ub=A,b_ub=rhs,bounds=bounds,method='highs')
    margin=preferred_margin
    if not result.success:
        for i in range(timing_start,len(rhs)-1):rhs[i]+=preferred_margin-10
        margin=10
        result=linprog(objective,A_ub=A,b_ub=rhs,bounds=bounds,method='highs')
    if not result.success:raise RuntimeError(result.message)
    assert np.max(A@result.x-np.asarray(rhs))<1e-5
    targets={p:(float(clocks[p]['rise_min_ns'])+float(clocks[p]['rise_max_ns']))/2+result.x[i]/1000 for i,p in enumerate(pins)}
    text=['set block [ord::get_db_block]','intprop $block cts_data_alignment 1','intprop $block cts_cost_aware_tree 1','intprop $block cts_align_defer 1','intprop $block cts_data_skew_native_sizing 0','doubleprop $block cts_cost_half_period_ns 1.0']
    bypin=defaultdict(list);fixed=[]
    for g,by in profiles.items():
        for s,(a,b) in by.items():
            if s>=0:
                mid=(float(clocks[pins[s]]['rise_min_ns'])+float(clocks[pins[s]]['rise_max_ns']))/2
                bypin[pins[s]].extend([gates.index(g),(a/1000)-mid,(b/1000)-mid,weights[g]])
            else:fixed.extend([gates.index(g),a/1000,b/1000,weights[g]])
    for p in pins:
        mid=(float(clocks[p]['rise_min_ns'])+float(clocks[p]['rise_max_ns']))/2
        text += [f'set pin [$block findITerm {{{p}}}]',f'doubleprop $pin cts_align_target_ns {targets[p]:.17g}',f'doubleprop $pin cts_align_reference_ns {mid:.17g}',f'doubleprop $pin cts_align_stock_fall_ns {(float(clocks[p]["fall_min_ns"])+float(clocks[p]["fall_max_ns"]))/2:.17g}',f'stringprop $pin cts_align_profile {{'+ ' '.join(map(str,bypin[p]))+'}']
    text.append('stringprop $block cts_align_fixed {'+' '.join(map(str,fixed))+'}')
    encoded=[]
    for r in flow.rows(source_design/'timing.csv'):
        launch=pins[regid[r['source']]] if r['source'] in regid else 'FIXED'
        capture=pins[regid[r['target']]] if r['target'] in regid else 'FIXED'
        encoded.extend([launch,capture,1 if r['mode']=='max' else -1,float(r['slack_ns'])-margin/1000])
    text.append('stringprop $block cts_align_timing_checks {'+' '.join(map(str,encoded))+'}')
    if grouping:
        for index,group in enumerate(grouping):
            for pin in group:text.extend([f'set pin [$block findITerm {{{pin}}}]',f'intprop $pin cts_cost_prescribed {index}'])
    (out/'targets.tcl').write_text('\n'.join(text)+'\n')
    (out/'targets.json').write_text(json.dumps(targets,indent=2))
    achieved_cost=sum(value*result.x[index] for index,value in spread.items())
    report={'status':'PREDICTED_TARGETS_ONLY','source_anchor':str(source_design),'source_response_refreshed':bool(anchor),'requested_reduction_percent':gain,'predicted_reduction_percent':100*(1-achieved_cost/stock_cost),'prescribed_groups':len(grouping) if grouping else None,'movement_bound_ps':bound,'planning_slack_margin_ps':margin,'gate_regressions':'reported; no hard per-gate cap','objective':'weighted spread bound with shared group clocks' if maximize else 'minimum movement plus total excess worsening, subject to requested weighted spread reduction and planning timing constraints'}
    (out/'plan.json').write_text(json.dumps(report,indent=2))
    return out/'targets.tcl'

def construct(out,targets,energy=20,leaf=32,span=.020,calibration=None,diameter=60,leaf_master=None,leaf_max_slew=None):
    body=flow.load(STOCK)+'set block [ord::get_db_block]\nintprop $block cts_data_skew_native_sizing 0\nset state [strip_clock]\n'
    body+=f'source {{{targets}}}\ndoubleprop $block cts_cost_energy_weight {energy}\ndoubleprop $block cts_cost_target_span_ns {span}\n'
    if calibration:body+=f'source {{{calibration}}}\n'
    if leaf_master:body+=f'stringprop $block cts_cost_leaf_master {{{leaf_master}}}\n'
    if leaf_max_slew:body+=f'doubleprop $block cts_cost_leaf_max_slew_ns {leaf_max_slew}\n'
    body+=f'clock_tree_synthesis -root_buf BUF_X4 -buf_list {{BUF_X1 BUF_X2 BUF_X4 BUF_X8 BUF_X16}} -sink_clustering_enable -sink_clustering_size {leaf} -sink_clustering_max_diameter {diameter} -dont_use_dummy_load\ndetailed_placement\nset_propagated_clock [all_clocks]\nestimate_parasitics -placement\ncheck_data $state\nexport_state {{{out}}}\n'
    body+='''
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
set file [open {@CALIBRATION@} w]
puts $file [list stringprop $block cts_cost_slews $measured]
puts $file "# Physical root-wire delays: rise=$root_rise ns fall=$root_fall ns"
puts $file "# Positive common model corrections: rise=$rb ns fall=$fb ns; requested targets are unchanged."
puts $file "doubleprop \\$block cts_cost_common_rise_bias_ns $rb"
puts $file "doubleprop \\$block cts_cost_common_fall_bias_ns $fb"
puts $file "doubleprop \\$block cts_cost_root_rise_ns [expr {$root_rise+$rb}]"
puts $file "doubleprop \\$block cts_cost_root_fall_ns [expr {$root_fall+$fb}]"
close $file
'''.replace('@CALIBRATION@',str(out/'calibration.tcl'))
    # Store a command using the variable name, never a stale SWIG block handle.
    body=body.replace('puts $file [list stringprop $block cts_cost_slews $measured]','puts $file "stringprop \\$block cts_cost_slews {$measured}"')
    flow.run_tcl(out,body)

def compare(out,bound=80):
    report=flow.compare(out,STOCK,bound,5)
    # Preserve the old gated result as a comparison, then apply newly authorized
    # reporting-only regression policy explicitly, without changing timing rules.
    report['legacy_5ps_gate_cap_pass']=report['max_regression_ps']<=5+1e-6
    m=report['metrics']
    report['legal']=float(m['setup_ns'])>=.01 and float(m['hold_ns'])>=.01 and int(m['electrical_pass'])==1 and report['max_clock_movement_ps']<=bound+1e-6
    report['accepted_improvement']=report['legal'] and report['weighted_reduction_percent']>0
    report['regression_policy']='report all regressions; no hard per-gate cap'
    report['regression_budget_ps']=None
    comparisons=flow.rows(out/'gate_comparison.csv')
    report['gates_worse_than_ps']={str(limit):sum(float(r['worsening_ps'])>limit+1e-6 for r in comparisons) for limit in [5,10,20,40]}
    report['unweighted_mean_spread_ps']=float(np.mean([float(r['final_ns'])*1000 for r in comparisons]))
    report['stock_unweighted_mean_spread_ps']=float(np.mean([float(r['stock_ns'])*1000 for r in comparisons]))
    report['certification']='native OpenSTA placement-based only; independent PrimeTime pending'
    (out/'result.json').write_text(json.dumps(report,indent=2))
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('phase',choices=['plan','pilot']);p.add_argument('--gain',type=float,default=15);p.add_argument('--energy',type=float,default=20);p.add_argument('--leaf',type=int,default=32);p.add_argument('--name',default='pilot15');a=p.parse_args()
    target=D/f'plan{a.gain:g}'/'targets.tcl'
    if a.phase=='plan':plan(target.parent,a.gain)
    else:
        construct(D/a.name,target,a.energy,a.leaf)
        print(json.dumps(compare(D/a.name),indent=2))
