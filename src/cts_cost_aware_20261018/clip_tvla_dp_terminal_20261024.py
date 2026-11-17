"""Clip only an out-of-window PX terminal timestamp; never resample traces."""
from pathlib import Path
import csv,json,hashlib,re
D=Path(__file__).resolve().parent/'tvla_dp_candidate_20261024'
audit=json.loads((D/'timestamp_audit.json').read_text());records={}
for name in ['stock_fvr','stock_null','mixed_e40_critical_leaves_fvr','mixed_e40_critical_leaves_null']:
    folder=D/'lab_reports/runs'/name
    with (folder/'trace_manifest.csv').open() as f:rows=list(csv.DictReader(f))
    end=round(float(rows[-1]['end_ns'])*1000)
    a=audit[name]
    assert a['tick_ns']==.001 and not a['nonmonotonic'] and end%5==0
    if not a['first_off_grid']:
        assert a['last_timestamp_ps']>=end
        continue
    assert len(a['first_off_grid'])==1 and a['first_off_grid'][0]['ps']==a['last_timestamp_ps']
    assert end<a['last_timestamp_ps']<end+5
    assert sum(v for k,v in a['residues_mod5'].items() if k!='0')==1
    source=folder/'power.out';dest=folder/'power_measurement_clipped.out'
    if dest.exists():
        raise FileExistsError('Preserve existing analysis copy; inspect before rerunning')
    changed=0
    with source.open() as src,dest.open('x') as target:
        for line in src:
            if re.fullmatch(r'\d+',line.strip()) and int(line)>end:
                assert int(line)==a['last_timestamp_ps']
                target.write(str(end)+'\n');changed+=1
            else:target.write(line)
    assert changed==1
    records[name]={'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
                   'analysis_file':str(dest),'analysis_sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),
                   'original_terminal_ps':a['last_timestamp_ps'],'measurement_end_ps':end,
                   'operation':'Clip the sole post-window terminal timestamp to the excluded right boundary. No timestamps or values inside any measurement window change; no resampling.'}
(D/'terminal_clipping.json').write_text(json.dumps(records,indent=2))
print(json.dumps(records,indent=2))
