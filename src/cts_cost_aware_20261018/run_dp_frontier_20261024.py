"""Fixed609target schedule, mixed leaf sizes, approximate DP cost frontier."""
import csv,json,os,re
from pathlib import Path
D=Path(__file__).resolve().parent
os.environ['COST_CTS_BINARY']=str(D/'build_dp_20261024_v4/openroad')
from experiment import construct,compare,flow
ROOT=D/'dp_frontier_20261024/v4'
base=(D/'controlled_x4_advance5_v7/targets.tcl').read_text()
original=flow.run_tcl
def run(out,body,binary=None):
    body+='''
set p [odb::dbStringProperty_find [ord::get_db_block] cts_cost_frontier_csv]
if {$p ne "NULL"} {set f [open {PATH} w];puts $f [$p getValue];close $f}
'''.replace('PATH',str(out/'frontier.csv'))
    return original(out,body,binary)
flow.run_tcl=run
if __name__=='__main__':
    ROOT.mkdir(exist_ok=False)
    results=[]
    for setting in ('legacy_control','e0','e5','e20','budget25','budget50','advance5'):
        energy=int(setting[1:]) if setting.startswith('e') else (5 if setting in ('legacy_control','advance5') else 0)
        name='mixed_'+setting;target=ROOT/(name+'_targets.tcl')
        extra='' if setting=='legacy_control' else '\nintprop $block cts_cost_pareto_frontier 1\n'
        if setting.startswith('budget'):
            if not (ROOT/'mixed_e0/frontier.csv').exists():
                results.append({'name':name,'error':'No reference frontier; budget trial skipped'})
                (ROOT/'summary.json').write_text(json.dumps(results,indent=2))
                continue
            rows=list(csv.DictReader((ROOT/'mixed_e0/frontier.csv').open()))
            costs=sorted(float(r['energy_fj_per_register']) for r in rows)
            budget=costs[(len(costs)-1)*int(setting[6:])//100]+1e-4
            extra+=f'doubleprop $block cts_cost_energy_budget_fj_per_register {budget}\n'
        schedule=base
        if setting=='advance5':
            schedule=re.sub(r'(doubleprop \$pin cts_align_target_ns )([-\d.eE+]+)',lambda m:m[1]+format(float(m[2])-.005,'.17g'),base)
            extra+='# Explicit additional common target advance5ps for hold recovery\n'
        with target.open('x') as f:f.write(schedule+extra)
        try:
            construct(ROOT/name,target,energy,leaf=32,span=.020,diameter=100,calibration=D/'controlled609_g20_x4_v7/round0/calibration.tcl',leaf_master='BUF_X4' if setting=='legacy_control' else None)
            report=compare(ROOT/name)
            results.append({'name':name,'result':report})
            print(name,json.dumps({k:report[k] for k in ('weighted_reduction_percent','legal','max_clock_movement_ps','metrics')}),flush=True)
        except Exception as error:
            results.append({'name':name,'error':str(error)});print(name,error,flush=True)
        (ROOT/'summary.json').write_text(json.dumps(results,indent=2))
