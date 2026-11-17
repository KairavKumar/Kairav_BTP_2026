from pathlib import Path
import json,csv,os,re
import numpy as np
R=Path(__file__).resolve().parent;K=R.parent;D=K/'cts_cost_aware_20261018'
os.environ['MPLCONFIGDIR']=str(R/'runtime/matplotlib')
import matplotlib;matplotlib.use('Agg')
import matplotlib.pyplot as plt
rows=[]
for method,folder in [('TritonCTS','triton'),('iCTS','icts_evaluated')]:
 for p in sorted((R/folder).glob('*/result.json')):
  d=json.loads(p.read_text());m=d['metrics'];h=d['overhead']
  rows.append({'method':method,'name':p.parent.name,'weighted_reduction_percent':d['weighted_reduction_percent'],'cost_ns':float(m['cost_ns']),'setup_ps':float(m['setup_ns'])*1000,'hold_ps':float(m['hold_ns'])*1000,'movement_ps':d['max_clock_movement_ps'],'clock_area_including_dummy_um2':float(h['clock_area_including_dummy_um2']),'clock_cells_including_dummy':int(h['clock_cells_including_dummy']),'electrical_pass':int(m['electrical_pass']),'native_legal':d['legal'],'result':str(p)})
stock=K/'cts_alignment_20261018/runs_final_constructor/stock'
for method,name,p in [('Stock','stock',stock),('Our method','preserved_dp',D/'dp_frontier_20261024/v4/mixed_e40_critical_leaves')]:
 m=next(csv.DictReader((p/'metrics.csv').open()));h=next(csv.DictReader((p/'overhead.csv').open()));d=json.loads((p/'result.json').read_text()) if (p/'result.json').exists() else {}
 rows.append({'method':method,'name':name,'weighted_reduction_percent':d.get('weighted_reduction_percent',0),'cost_ns':float(m['cost_ns']),'setup_ps':float(m['setup_ns'])*1000,'hold_ps':float(m['hold_ns'])*1000,'movement_ps':d.get('max_clock_movement_ps',0),'clock_area_including_dummy_um2':float(h['clock_area_including_dummy_um2']),'clock_cells_including_dummy':int(h['clock_cells_including_dummy']),'electrical_pass':int(m['electrical_pass']),'native_legal':True,'result':str(p)})
powers={}
for folder in ['power','power_large','power_icts']:
 p=R/folder/'analysis.json'
 if p.exists():powers.update(json.loads(p.read_text()))
base=powers.get('stock_fvr',{}).get('mean_window_power_mW')
for row in rows:
 name={'stock':'stock','preserved_dp':'mixed_e40_critical_leaves'}.get(row['name'],row['name'])+'_fvr'
 if name in powers:
  v=powers[name];row['mean_window_power_mW']=v['mean_window_power_mW'];row['power_change_percent']=100*(v['mean_window_power_mW']/base-1);row['power_window_ns']=v['window_ns'];row['exploratory_peak_abs_t']=v['peak_abs_t_exploratory'];row['power_evidence']=name
(R/'comparison.json').write_text(json.dumps({'rows':rows,'power_reference_mW':base,'scope':'PRESENT fixed placement, original marked weights, placement-estimated RC; power common 79.98ns support; FVR-only comparators'},indent=2))
keys=list(dict.fromkeys(k for row in rows for k in row))
with (R/'comparison.csv').open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=keys);w.writeheader();w.writerows(rows)
fig,ax=plt.subplots(1,2,figsize=(11.5,4.3),layout='constrained')
colors={'Stock':'#555555','Our method':'#bc541d','TritonCTS':'#216b9a','iCTS':'#23806c'}
for method in ['TritonCTS','iCTS','Stock','Our method']:
 for legal in [False,True]:
  group=[v for v in rows if v['method']==method and v['native_legal']==legal]
  if not group:continue
  ax[0].scatter([v['clock_area_including_dummy_um2'] for v in group],[v['weighted_reduction_percent'] for v in group],c=colors[method],marker='o' if legal else 'x',s=28 if method in ['TritonCTS','iCTS'] else 65,alpha=1 if legal else .4,label=method+(' (fails checks)' if not legal else ''))
 group=[v for v in rows if v['method']==method and 'power_change_percent' in v]
 if group:ax[1].scatter([v['power_change_percent'] for v in group],[v['weighted_reduction_percent'] for v in group],c=colors[method],s=55,label=method)
for a in ax:
 a.axhline(0,c='#aaaaaa',lw=.6);a.grid(alpha=.2);a.set_ylabel('Weighted data-skew reduction vs stock (%)');a.legend(fontsize=7)
ax[0].set_xlabel('Clock-cell area including dummy loads (um²)');ax[0].set_title('All measured native candidates')
ax[1].set_xlabel('Measured total-power change vs stock (%)');ax[1].set_title('Full FVR power tests; common 79.98 ns windows');ax[1].axvline(0,c='#aaaaaa',lw=.6)
fig.savefig(R/'comparison.png',dpi=180);fig.savefig(R/'comparison.pdf');plt.close(fig)
lines=['# PRESENT CTS comparison','', 'Campaign continued on 16 November 2026. Original reference hashes and fresh-process reloads passed. See PROTOCOL.md for conditions and comparison.csv for all candidate metrics.','']
for method in ['TritonCTS','iCTS']:
 group=[v for v in rows if v['method']==method];legal=[v for v in group if v['native_legal']]
 lines+=['## '+method,'',f'{len(group)} evaluated output trees; {len(legal)} pass native setup/hold >=10 ps, electrical checks and <=80 ps movement. Data identity is checked before scoring.','']
 if legal:
  best=max(legal,key=lambda x:x['weighted_reduction_percent']);lines +=[f"Best achieved weighted-skew reduction: {best['weighted_reduction_percent']:.6f}% ({best['name']}); hold {best['hold_ps']:.3f} ps; clock area including dummy loads {best['clock_area_including_dummy_um2']:.3f} um².",'']
 else:lines+=['No valid operating point is established by this bounded configuration sweep. Failed trees are retained, and no power or superiority claim is made from an invalid implementation. This is not evidence that the published method cannot meet timing under a more complete integration/tuning flow.','']
 lines+=['Measured FVR power points:','']
 for v in group:
  if 'power_change_percent' in v:lines +=[f"- {v['name']}: skew reduction {v['weighted_reduction_percent']:.6f}%; power {v['mean_window_power_mW']:.6f} mW ({v['power_change_percent']:+.3f}% vs stock); native hold {v['hold_ps']:.3f} ps. Independent PT reports are saved with the power campaign."]
 lines+=['']
lines+=['## Interpretation','','The proposed method targets weighted data skew. Conventional CTS can lower total power substantially while leaving that score nearly unchanged. Therefore the supported claim is a skew/power tradeoff, rather than overall dominance. Compare Pareto points at common cost or achieved skew; this sweep does not establish a globally optimal conventional baseline.','','Clock area uses identical dummy-load accounting. The stock value is 760.228 um² and the preserved DP value is 804.916 um². These differ from driver-only area figures.','','## Measurement boundaries','','Comparator waveforms have tool-generated terminal markers before the final manifest endpoint. The analysis uses the same first 79.98 ns (15,996 bins) of every trace for stock, preserved DP and all comparators. Only timestamps after the retained endpoint are removed in separate analysis files; no in-window sample is interpolated. The original 80 ns published reference statistics remain unchanged.','','Comparator t statistics are exploratory FVR-only values, with no new null-control validation. They do not establish leakage improvement or a percentage security improvement. Full-run PX glitch power is a separate averaging domain.','','## Reproduction','','Use the exact run.tcl in each candidate directory for native construction/evaluation. run_triton.py, run_triton_narrow.py and run_triton_large.py define the 37-configuration TritonCTS sweep. iCTS source commit, binary hash and build-only patch are in icts_provenance.json. Each iCTS directory contains its config and input paths; evaluate_icts.py imports and legalizes only CTS cells while fixing all data cells.','','Power staging, collection and analysis scripts are kept in this campaign directory. Remote jobs use /home/c_karfa/Kairav/cts_comparison_20261112. No original reference output is replaced.']
(R/'ASSESSMENT.md').write_text('\n'.join(lines)+'\n')
print('SUMMARY',len(rows),'rows',len(powers),'power entries',flush=True)

with (R/"ASSESSMENT.md").open("a") as f:f.write('\n\n## Completed and blocked work, 16 November 2026\n\nAll 37 TritonCTS and 21 iCTS output trees have been evaluated locally. Nine TritonCTS trees pass native checks. All 21 iCTS trees preserve data identity but have negative hold slack (the closest tested point is approximately -43.5 ps). Their failure cannot support a claim of superiority over the published algorithm; a valid iCTS timing integration remains unresolved. The tested source is upstream iEDA at the pinned commit, not an exact paper-artifact reproduction.\n\nThree TritonCTS points completed full 1,024-transaction FVR VCS/PX runs and independent PrimeTime timing checks. All three independent PT checks pass the 10 ps setup/hold margin; the verified PT log has no errors. Their power waveforms and manifests were downloaded and checked against remote SHA-256 values.\n\nThe later large-cluster configurations have no measured power result. Both the initial VCS attempt and one retry failed because the lab license server was unreachable; independent PrimeTime also failed license checkout. Evidence: power_large/status_20261116.json. They are not assigned estimated or copied power numbers.\n\nThe preserved DP reference has 16.421776% weighted-skew reduction and approximately +3.57% total-power overhead. The best native-legal tuned TritonCTS score is 0.350780%; its power remains blocked. Among points with completed power testing, the lowest-power setting has -0.083775% skew reduction (a small worsening) and -11.871% total-power change. Thus power tuning is a meaningful baseline and our current demonstrated benefit is weighted skew reduction with a cost.\n\nAll reference hashes pass again after the campaign, and stock/DP metric reload differences are exactly zero. No original report has been updated with incomplete comparator results.\n')
