"""Run target-delivery fixtures with frontier enabled and budget infeasibility."""
import csv,json,os,runpy,sys
from pathlib import Path
D=Path(__file__).resolve().parent
os.environ['COST_CTS_BINARY']=str(D/'build_dp_20261024_v4/openroad')
os.environ['COST_TOY_NAME']='dp_frontier_20261024/toy_v4'
sys.path.insert(0,str(D.parent/'cts_alignment_20261018'))
import flow_final as flow
original=flow.run_tcl
def run(out,body,binary=None):
    body=body.replace('clock_tree_synthesis -root_buf','intprop $block cts_cost_pareto_frontier 1\nclock_tree_synthesis -root_buf')
    body+='''
set p [odb::dbStringProperty_find [ord::get_db_block] cts_cost_frontier_csv]
if {$p ne "NULL"} {
 set f [open {PATH} w];puts $f [$p getValue];close $f
}
'''.replace('PATH',str(out/'frontier.csv'))
    return original(out,body,binary)
flow.run_tcl=run
if __name__=='__main__':
    runpy.run_path(str(D/'toy.py'),run_name='__main__')
    root=D/'dp_frontier_20261024/toy_v4'
    for case in ('equal','different'):
        rows=list(csv.DictReader((root/case/'frontier.csv').open()))
        assert len(rows)>1
        assert len({float(r['energy_fj_per_register']) for r in rows})>1
    script=(root/'equal/run.tcl').read_text()
    script=script.replace('clock_tree_synthesis -root_buf','doubleprop $block cts_cost_energy_budget_fj_per_register 1e-12\nclock_tree_synthesis -root_buf')
    try:original(root/'infeasible_budget',script)
    except RuntimeError:
        assert 'No retained clock tree meets estimated energy budget' in (root/'infeasible_budget/run.log').read_text()
    else:raise AssertionError('Impossible cost budget accepted')
    print('DP_FRONTIER_CHECK_PASS: delivery, cost diversity, unreachable targets and infeasible budget')
