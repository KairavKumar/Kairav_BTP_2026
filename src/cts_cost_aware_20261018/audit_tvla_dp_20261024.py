"""Audit actual PX timestamps before adapting any waveform resampling."""
from pathlib import Path
import json,re,collections
D=Path(__file__).resolve().parent/'tvla_dp_candidate_20261024'
summary={}
for name in ['stock_fvr','stock_null','mixed_e40_critical_leaves_fvr','mixed_e40_critical_leaves_null']:
    path=D/'lab_reports/runs'/name/'power.out'
    tick=None;previous=-1;off=[];back=[];residues=collections.Counter();count=0
    for number,line in enumerate(path.open(),1):
        line=line.strip()
        if line.startswith('.time_resolution'):tick=float(line.split()[1])
        elif re.fullmatch(r'\d+',line):
            assert tick is not None
            value=float(line)*tick*1000
            assert abs(value-round(value))<1e-5
            value=round(value);count+=1;residues[value%5]+=1
            if value%5 and len(off)<10:off.append({'line':number,'ps':value})
            if value<previous and len(back)<10:back.append({'line':number,'ps':value,'previous_ps':previous})
            previous=value
    summary[name]={'tick_ns':tick,'timestamp_count':count,'residues_mod5':dict(residues),
                   'first_off_grid':off,'nonmonotonic':back,'last_timestamp_ps':previous}
(D/'timestamp_audit.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
