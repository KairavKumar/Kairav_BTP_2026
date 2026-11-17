from pathlib import Path
import csv,json,hashlib,importlib.util,re
import numpy as np
R=Path(__file__).resolve().parent;D=R.parent/'cts_cost_aware_20261018';O=R/'power_large'
spec=importlib.util.spec_from_file_location('adapter',R.parent/'OpenROAD/src/cts/BTP_Research/secure_skew_demo/analyze_power.py');a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
collection=json.loads((O/'collection_status.json').read_text());results={}
reference=D/'tvla_dp_candidate_20261024/lab_reports/runs/stock_fvr/trace_manifest.csv'
for name,record in collection['ready'].items():
 f=O/'lab_reports/runs'/name;p=f/'power.out'
 if not p.exists():continue
 assert hashlib.sha256(p.read_bytes()).hexdigest()==record['power_sha256']
 assert (f/'trace_manifest.csv').read_bytes()==reference.read_bytes()
 rows=list(csv.DictReader((f/'trace_manifest.csv').open()));starts=np.rint([float(x['start_ns'])*1000 for x in rows]).astype(int);ends=np.rint([float(x['end_ns'])*1000 for x in rows]).astype(int)
 assert len(rows)==1024 and np.all(ends-starts==80000)
 # Use the same observed 79.98 ns support for all 1024 traces and references.
 end_support=int(ends[-1])-20
 analysis=f/'power_common_support.out'
 last=-1;off=[]
 with p.open() as src,analysis.open('w') as dst:
  for line in src:
   if re.fullmatch(r'\d+',line.strip()):
    t=int(line);assert t>=last;last=t
    if t>=end_support:
     dst.write(str(end_support)+'\n');break
    assert t%5==0,('offgrid inside retained support',t)
   dst.write(line)
  else:raise RuntimeError('Waveform ends before common support')
 power,res=a.read_power_out(analysis,5,end_support);traces=np.stack([power[s//5:(e-20)//5] for s,e in zip(starts,ends)])
 clip={'operation':'common 79.98ns trace support; final 20ps omitted from every trace in every design; raw files unchanged'}
 labels=np.array([int(x['class']) for x in rows]);t,constant,det=a.welch(traces[labels==0],traces[labels==1])
 results[name]={'mean_window_power_mW':float(traces.mean()*1000),'peak_abs_t_exploratory':float(np.max(abs(t))),'crossing_bins_exploratory':int(np.sum(abs(t)>4.5)),'window_ns':79.98,'null_control_run':False,'power_sha256':record['power_sha256'],'terminal_correction':clip,'out_tick_ns':res}
 np.savez_compressed(f/'samples.npz',mean=traces.mean(axis=0),t=t)
 print(name,results[name],flush=True)
 del power,traces
for name in ['stock','mixed_e40_critical_leaves']:
 f=D/'tvla_dp_candidate_20261024/lab_reports/runs'/(name+'_fvr')/'tvla_samples.npz'
 v=np.load(f);mean=(v['mean0'][:-4]+v['mean1'][:-4])/2
 results[name+'_fvr']={'mean_window_power_mW':float(mean.mean()*1000),'peak_abs_t_exploratory':float(np.max(abs(v['t'][:-4]))),'window_ns':79.98,'source':str(f),'reference_original_full80ns_retained':True}
(O/'analysis.json').write_text(json.dumps(results,indent=2))
