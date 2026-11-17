"""Matched first-order TVLA of the generated shared-branch tree and stock."""
from pathlib import Path
import os,csv,hashlib,importlib.util,json
import numpy as np
D=Path(__file__).resolve().parent
OUT=D/'tvla_dp_candidate_20261024'
os.environ['MPLCONFIGDIR']=str(OUT/'runtime/matplotlib')
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
adapter=D.parent/'OpenROAD/src/cts/BTP_Research/secure_skew_demo/analyze_power.py'
spec=importlib.util.spec_from_file_location('validated_tvla_adapter',adapter)
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
results={};samples={};manifests={};prefixes={}
collection=json.loads((OUT/'collection_status.json').read_text())
clipping=json.loads((OUT/'terminal_clipping.json').read_text()) if (OUT/'terminal_clipping.json').exists() else {}
for mode in ['fvr','null']:
    for case in ['stock','mixed_e40_critical_leaves']:
        name=case+'_'+mode;folder=OUT/'lab_reports/runs'/name
        assert name in collection['ready'],('Incomplete run',name)
        with (folder/'trace_manifest.csv').open() as f:rows=list(csv.DictReader(f))
        assert len(rows)==1024 and [int(r['trace_id']) for r in rows]==list(range(1024))
        labels=np.array([int(r['class']) for r in rows])
        assert set(labels)=={0,1} and np.sum(labels==0)==np.sum(labels==1)==512
        starts=np.rint([float(r['start_ns'])*1000 for r in rows]).astype(np.int64)
        ends=np.rint([float(r['end_ns'])*1000 for r in rows]).astype(np.int64)
        assert np.all(ends-starts==80000) and not np.any(starts%5)
        assert np.all(starts[1:]>=ends[:-1])
        manifest=(folder/'trace_manifest.csv').read_bytes()
        if mode in manifests:assert manifest==manifests[mode],('Cross-design label/window mismatch',mode)
        manifests[mode]=manifest
        output=folder/'power.out'
        digest=hashlib.sha256(output.read_bytes()).hexdigest()
        assert digest==collection['ready'][name]['power_sha256']
        analysis_output=output
        if name in clipping:
            record=clipping[name]
            assert record['source_sha256']==digest
            analysis_output=Path(record['analysis_file'])
            assert hashlib.sha256(analysis_output.read_bytes()).hexdigest()==record['analysis_sha256']
            assert record['measurement_end_ps']==int(ends[-1])
        power,resolution=module.read_power_out(analysis_output,5,int(ends[-1]))
        traces=np.stack([power[s//5:e//5] for s,e in zip(starts,ends)])
        a,b=traces[labels==0],traces[labels==1]
        t,constant,deterministic=module.welch(a,b)
        crossing=np.flatnonzero(np.abs(t)>4.5)
        with (folder/'threshold_crossings.csv').open('w',newline='') as f:
            writer=csv.writer(f);writer.writerow(['bin_index','time_ns','welch_t'])
            writer.writerows((int(i),float(i*.005),float(t[i])) for i in crossing)
        peak=int(np.argmax(np.abs(t)))
        result={'case':case,'mode':mode,'trace_count':1024,'class_counts':[512,512],
                'max_abs_t':float(np.max(np.abs(t))) if not deterministic else 'infinite_deterministic_difference',
                'peak_time_ns':peak*.005,'bins_above_4_5':len(crossing),'crossed':bool(len(crossing)),
                'crossing_range_ns':([float(crossing[0]*.005),float(crossing[-1]*.005)] if len(crossing) else None),
                'identical_constant_bins':int(constant),'deterministic_difference_bins':int(deterministic),
                'mean_measured_power_W':float(traces.mean()),'mean_class_power_W':[float(a.mean()),float(b.mean())],
                'bin_ps':5,'window_ns':80,'out_tick_ns':resolution,
                'power_sha256':digest,'manifest_sha256':hashlib.sha256(manifest).hexdigest(),
                'scope':'First-order simulated aggregate PX power, typical corner, placement-estimated SPEF. No security proof.',
                'post_window_terminal_clipping':clipping.get(name)}
        results[name]=result
        samples[name]={'t':t,'mean0':a.mean(axis=0),'mean1':b.mean(axis=0)}
        np.savez_compressed(folder/'tvla_samples.npz',**samples[name])
        (folder/'tvla_summary.json').write_text(json.dumps(result,indent=2,allow_nan=False))
        prefixes[name]={}
        for n in [128,256,512]:
            prefix_t,_,det=module.welch(a[:n],b[:n])
            prefixes[name][str(n*2)]={'max_abs_t':float(np.max(np.abs(prefix_t))) if not det else 'infinite_deterministic_difference',
                                    'bins_above_4_5':int(np.sum(np.abs(prefix_t)>4.5))}
        print(name,json.dumps(result),flush=True)
        del power,traces,a,b
summary={'protocol':json.loads((OUT/'protocol.json').read_text()),'results':results,
         'exploratory_nested_prefixes':prefixes,
         'prefix_caveat':'Matched first N samples per class; correlated exploratory subsets, not independent replication or extra traces.'}
# The original stock data provide a repeatability check, not additional samples.
old=D.parent/'cts_lab_assessment_20261018/lab_reports/runs'
summary['stock_repeatability']={}
for mode in ['fvr','null']:
    p=old/('stock_'+mode)/'tvla_summary.json'
    if p.exists():
        previous=json.loads(p.read_text());current=results['stock_'+mode]
        summary['stock_repeatability'][mode]={'previous_max_abs_t':previous['max_abs_t'],
            'new_max_abs_t':current['max_abs_t'],'same_power_sha256':previous['power_sha256']==current['power_sha256'],
            'same_manifest_sha256':previous['manifest_sha256']==current['manifest_sha256']}
positive=old/'stock_positive/tvla_summary.json'
if positive.exists():
    previous=json.loads(positive.read_text())
    summary['previous_positive_control']={'rerun_in_this_campaign':False,'trace_count':previous['trace_count'],
        'max_abs_t':previous['max_abs_t'],'interpretation':'Previously measured input-sharing-disabled stock sensitivity control; not protected candidate traces.'}
(OUT/'tvla_results.json').write_text(json.dumps(summary,indent=2,allow_nan=False))
x=np.arange(16000)*.005
fig,axes=plt.subplots(3,1,figsize=(12,8),sharex=True,layout='constrained')
for case,label in [('stock','Stock'),('mixed_e40_critical_leaves','DP frontier CTS')]:
    for i,mode in enumerate(['fvr','null']):axes[i].plot(x,samples[case+'_'+mode]['t'],linewidth=.7,label=label)
    s=samples[case+'_fvr'];axes[2].plot(x,(s['mean0']+s['mean1'])*500,label=label,linewidth=.8)
for i in [0,1]:
    axes[i].axhline(4.5,color='firebrick',linestyle='--',linewidth=.8)
    axes[i].axhline(-4.5,color='firebrick',linestyle='--',linewidth=.8)
    axes[i].legend();axes[i].set_ylabel(('FVR' if i==0 else 'Random/random')+' Welch t')
axes[2].legend();axes[2].set_ylabel('Mean power (mW)');axes[2].set_xlabel('Time after stimulus (ns)')
fig.suptitle('Matched stock vs 16.42% data-spread candidate: 512 traces/class, 5 ps bins')
fig.savefig(OUT/'tvla_comparison.png',dpi=150);plt.close(fig)
print('MATCHED_TVLA_ANALYSIS_COMPLETE',flush=True)
