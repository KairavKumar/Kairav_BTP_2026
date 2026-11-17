"""Portable replay of the preserved PRESENT artifact. All new outputs use runs/."""
from pathlib import Path
import argparse,csv,gzip,hashlib,importlib.util,json,re,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
PRESENT=ROOT/'results/present'
OLD='/home/kairav1902/MlEda/FMSV_project/Kairav'
def read(p):return json.loads(p.read_text())
def digest(p):return hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
def new_output(name):
 p=ROOT/'runs'/name;p.mkdir(parents=True,exist_ok=False);return p
def module(name,p):
 spec=importlib.util.spec_from_file_location(name,p);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def verify():
 records=read(ROOT/'verification/import_manifest.json')
 for name,record in records.items():
  assert digest(ROOT/name)==record['sha256'],f'Changed file: {name}'
 for name,record in read(ROOT/'verification/build/source_hash_verification.json').items():
  assert digest(ROOT/'src/OpenROAD/src/cts/src'/name)==record['expected'],name
 for folder in (PRESENT/'tvla/runs').iterdir():
  expected=read(PRESENT/'tvla/collection_status.json')['ready'][folder.name]['power_sha256']
  with gzip.open(folder/'power.out.gz','rb') as f:assert hashlib.file_digest(f,'sha256').hexdigest()==expected,folder.name
 original=read(ROOT/'verification/historical_active_approach.json')['references']['present']['sha256']
 for name,h in original.items():assert digest(PRESENT/'candidate'/name)==h,name
 print(f'PASS: {len(records)} imported files, all 17 measured-build source hashes, four raw power hashes and candidate identity.')
def summary():
 tv=read(PRESENT/'tvla/tvla_results.json')['results'];native=read(PRESENT/'candidate/result.json')
 a=tv['stock_fvr'];b=tv['mixed_e40_critical_leaves_fvr']
 print(json.dumps({'weighted_skew_reduction_percent':native['weighted_reduction_percent'],
 'stock_window_power_mW':a['mean_measured_power_W']*1000,'candidate_window_power_mW':b['mean_measured_power_W']*1000,
 'total_power_change_percent':100*(b['mean_measured_power_W']/a['mean_measured_power_W']-1),
 'stock_FVR_peak_abs_t':a['max_abs_t'],'candidate_FVR_peak_abs_t':b['max_abs_t'],
 'full_run_glitch_power_uW':{'stock':30.050,'candidate':22.920},
 'glitch_power_reduction_percent':100*(1-22.920/30.050)},indent=2))
def figures(out):
 import numpy as np
 import matplotlib;matplotlib.use('Agg')
 import matplotlib.pyplot as plt
 fig,axs=plt.subplots(3,1,figsize=(10,7),sharex=True,layout='constrained')
 for case,label in [('stock','Stock'),('mixed_e40_critical_leaves','Proposed CTS')]:
  for i,mode in enumerate(['fvr','null']):
   s=np.load(PRESENT/'tvla/runs'/f'{case}_{mode}'/'tvla_samples.npz');x=np.arange(len(s['t']))*.005
   axs[i].plot(x,s['t'],lw=.7,label=label)
  s=np.load(PRESENT/'tvla/runs'/f'{case}_fvr'/'tvla_samples.npz')
  axs[2].plot(x,(s['mean0']+s['mean1'])*500,lw=.7,label=label)
 for i in [0,1]:
  axs[i].axhline(4.5,color='firebrick',ls='--',lw=.7);axs[i].axhline(-4.5,color='firebrick',ls='--',lw=.7)
  axs[i].set_ylabel(('FVR' if i==0 else 'Random/random')+' Welch t')
 axs[2].set_ylabel('Mean power (mW)');axs[2].set_xlabel('Time in measured window (ns)')
 for ax in axs:ax.legend()
 fig.savefig(out/'tvla_comparison.png',dpi=180);fig.savefig(out/'tvla_comparison.pdf');plt.close(fig)
 print(out)
def tvla(out):
 import numpy as np
 parser=module('power_out',ROOT/'src/analysis/power_out.py')
 saved=read(PRESENT/'tvla/tvla_results.json')['results'];clips=read(PRESENT/'tvla/terminal_clipping.json');ready=read(PRESENT/'tvla/collection_status.json')['ready'];results={};manifests={}
 for name in ['stock_fvr','stock_null','mixed_e40_critical_leaves_fvr','mixed_e40_critical_leaves_null']:
  folder=PRESENT/'tvla/runs'/name;work=out/name;work.mkdir();raw=work/'power.out'
  with gzip.open(folder/'power.out.gz','rb') as f,raw.open('wb') as target:
   import shutil;shutil.copyfileobj(f,target)
  assert digest(raw)==ready[name]['power_sha256']
  manifest=(folder/'trace_manifest.csv').read_bytes();mode=name.rsplit('_',1)[1]
  if mode in manifests:assert manifest==manifests[mode]
  manifests[mode]=manifest
  rows=list(csv.DictReader((folder/'trace_manifest.csv').open()));assert len(rows)==1024
  labels=np.array([int(row['class']) for row in rows]);assert np.sum(labels==0)==np.sum(labels==1)==512
  starts=np.rint([float(row['start_ns'])*1000 for row in rows]).astype(np.int64)
  ends=np.rint([float(row['end_ns'])*1000 for row in rows]).astype(np.int64);assert np.all(ends-starts==80000)
  analysis=raw
  if name in clips:
   record=clips[name];assert record['source_sha256']==digest(raw);analysis=work/'analysis.out';changed=0
   with raw.open() as source,analysis.open('x') as target:
    for line in source:
     if re.fullmatch(r'\d+',line.strip()) and int(line)>int(ends[-1]):
      assert int(ends[-1])<int(line)<int(ends[-1])+5;target.write(str(int(ends[-1]))+'\n');changed+=1
     else:target.write(line)
   assert changed==1 and digest(analysis)==record['analysis_sha256']
  power,_=parser.read_power_out(analysis,5,int(ends[-1]));traces=np.stack([power[s//5:e//5] for s,e in zip(starts,ends)])
  a,b=traces[labels==0],traces[labels==1];t,constant,det=parser.welch(a,b);assert not det
  result={'max_abs_t':float(np.max(np.abs(t))),'mean_measured_power_W':float(traces.mean()),'bins_above_4_5':int(np.sum(np.abs(t)>4.5))}
  assert abs(result['max_abs_t']-saved[name]['max_abs_t'])<1e-10
  assert abs(result['mean_measured_power_W']-saved[name]['mean_measured_power_W'])<1e-14
  np.savez_compressed(work/'tvla_samples.npz',t=t,mean0=a.mean(axis=0),mean1=b.mean(axis=0))
  results[name]=result;print(name,json.dumps(result),flush=True)
  del traces,power,a,b
 (out/'recomputed_results.json').write_text(json.dumps(results,indent=2)+'\n')
 print('PASS: raw-power replay reproduces all four recorded full-sample peaks and mean powers.')
def plan(out):
 sys.path.insert(0,str(ROOT/'src/cts_cost_aware_20261018'))
 import experiment
 experiment.STOCK=PRESENT/'stock';experiment.flow.STOCK=PRESENT/'stock'
 groups=read(PRESENT/'planning/groups_span20.json')
 experiment.plan(out/'group609_gain20',20,grouping=groups)
 print(out/'group609_gain20/plan.json')
def native(out,binary,construct):
 binary=Path(binary).expanduser().resolve();assert binary.is_file(),binary
 if construct:
  cases=[('unrepaired',PRESENT/'unrepaired/run.tcl'),('candidate',PRESENT/'candidate/run.tcl')]
 else:cases=[(name,None) for name in ['stock','candidate']]
 mapping={OLD+'/cts_alignment_20261018/runs_final_constructor/stock':str(PRESENT/'stock'),
 OLD+'/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40_critical_leaves':str(out/'candidate'),
 OLD+'/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40':str(out/'unrepaired'),
 OLD+'/cts_cost_aware_20261018/dp_frontier_20261024/v4/mixed_e40_targets.tcl':str(PRESENT/'planning/dp_frontier_20261024/v4/mixed_e40_targets.tcl'),
 OLD+'/cts_cost_aware_20261018/controlled609_g20_x4_v7/round0/calibration.tcl':str(PRESENT/'planning/controlled609_g20_x4_v7/round0/calibration.tcl')}
 for name,script in cases:
  target=out/name;target.mkdir()
  if script:
   text=script.read_text()
   replacements={}
   for i,(a,b) in enumerate(sorted(mapping.items(),key=lambda item:len(item[0]),reverse=True)):
    token=f'@ARTIFACT_PATH_{i}@';text=text.replace(a,token);replacements[token]=b
   text=text.replace(OLD,str(ROOT/'src'))
   for token,value in replacements.items():text=text.replace(token,value)
  else:
   text=f'set K {{{ROOT / "src"}}}\nsource {{{ROOT / "src/cts_alignment_20261018/common_final.tcl"}}}\nload_design {{{PRESENT/name/"design.odb"}}} {{{PRESENT/name/"design.sdc"}}}\nsource {{{ROOT/"src/cts_larger_20261011/native_sizing_inputs.tcl"}}}\nexport_state {{{target}}}\n'
  path=target/'run.tcl';path.write_text(text)
  with (target/'run.log').open('w') as log:subprocess.run([str(binary),'-no_init','-no_splash','-exit',str(path)],stdout=log,stderr=subprocess.STDOUT,check=True)
  assert 'ALIGNMENT_EXPORT_PASS' in (target/'run.log').read_text()
  actual=list(csv.DictReader((target/'metrics.csv').open()))[0];expected=list(csv.DictReader((PRESENT/name/'metrics.csv').open()))[0]
  for field in ['cost_ns','setup_ns','hold_ns','area_um2']:assert abs(float(actual[field])-float(expected[field]))<1e-8,(name,field,actual[field],expected[field])
  print(name,'PASS: recorded cost, setup, hold and area reproduced',flush=True)
if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('command',choices=['verify','summary','figures','tvla','plan','native']);parser.add_argument('--name');parser.add_argument('--openroad');parser.add_argument('--construct',action='store_true');args=parser.parse_args()
 if args.command=='verify':verify()
 elif args.command=='summary':summary()
 else:
  out=new_output(args.name or args.command)
  if args.command=='figures':figures(out)
  elif args.command=='tvla':tvla(out)
  elif args.command=='plan':plan(out)
  elif args.command=='native':
   if not args.openroad:parser.error('--openroad is required; use the preserved executable or a compatible source build')
   native(out,args.openroad,args.construct)
