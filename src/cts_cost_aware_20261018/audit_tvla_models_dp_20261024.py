"""Audit full-run printed PX load warnings against unchanged original Liberty."""
from pathlib import Path
import json,re
D=Path(__file__).resolve().parent
old=D.parent/'cts_lab_assessment_20261018/audit_models.py'
namespace={'__file__':str(old)}
exec(compile(old.read_text().split('summary={}')[0],str(old),'exec'),namespace)
bounds=namespace['bounds']
pattern=r"load ([\d.]+) at pin '([^']+)' of cell '([^']+)' is out of load range \(([^)]+)\) of lib_cell '([^']+)'"
summary={}
for name in ['stock_fvr','stock_null','mixed_e40_critical_leaves_fvr','mixed_e40_critical_leaves_null']:
    folder=D/'tvla_dp_candidate_20261024/lab_reports/runs'/name
    text=(folder/'power_checks.rpt').read_text()
    bad=[];count=0
    for load,pin,inst,printed,cell in re.findall(pattern,text):
        count+=1
        if not all(lo<=float(load)<=hi for lo,hi in bounds(cell,pin)):
            bad.append({'cell':cell,'pin':pin,'instance':inst,'load_ff':float(load),'bounds_ff':bounds(cell,pin)})
    sim=(folder/'simulation.log').read_text()
    assert re.search(r'^FUNCTIONAL_PASS traces=1024$',sim,re.M)
    assert not re.search(r'Timing violation|Error-|Fatal:',sim,re.I)
    summary[name]={'functional_traces':1024,'capacitance_warnings_audited':count,
                   'actual_load_extrapolations':bad,
                   'other_out_of_range_lines':[line for line in text.splitlines() if 'out of' in line.lower() and not re.search(pattern,line)],
                   'scope':'Original Liberty fF power-table load range check of printed warnings; not full model/PVT signoff.'}
(D/'tvla_dp_candidate_20261024/model_audit.json').write_text(json.dumps(summary,indent=2))
print(json.dumps({name:{'audited':v['capacitance_warnings_audited'],'actual_extrapolations':len(v['actual_load_extrapolations'])} for name,v in summary.items()},indent=2))
