"""Write the measured selected-DP TVLA assessment without changing design or protocol."""
from pathlib import Path
import json,hashlib
D=Path(__file__).resolve().parent
O=D/'tvla_dp_candidate_20261024'
s=json.loads((O/'tvla_results.json').read_text());r=s['results']
model=json.loads((O/'model_audit.json').read_text())
C='mixed_e40_critical_leaves'
lines=['# Matched TVLA: selected DP-frontier candidate versus stock','',
'Tested circuit: `dp_frontier_20261024/v4/mixed_e40_critical_leaves`, with 16.4217764% measured weighted-data-skew reduction relative to original stock. Same frozen register/data placement; this is the final candidate with nine timing-repair leaf upsizes.','',
'Four fresh VCS/PX runs completed: stock and candidate, each fixed/random (FVR) and random/random (null). Each has 1,024 functional transactions, 512 per class; 80 ns measured windows, 5 ps bins, 500 MHz clock. Both designs use identical vectors and byte-identical manifests per mode. Design exports, vectors and collected raw power have verified SHA-256 hashes.','',
'## Measured results','']
for mode in ['fvr','null']:
 a,b=r['stock_'+mode],r[C+'_'+mode]
 lines.append(f"- {mode.upper()}: stock max |t| = {a['max_abs_t']:.6f}, candidate = {b['max_abs_t']:.6f}; bins above 4.5: stock {a['bins_above_4_5']}, candidate {b['bins_above_4_5']} (out of 16,000 bins).")
a,b=r['stock_fvr'],r[C+'_fvr']
gain=(b['mean_measured_power_W']/a['mean_measured_power_W']-1)*100
lines+=['',f"Mean measured-window FVR power: stock {a['mean_measured_power_W']*1000:.9f} mW; candidate {b['mean_measured_power_W']*1000:.9f} mW ({gain:+.6f}% relative to stock).",'',
'## Interpretation','']
if b['max_abs_t']<a['max_abs_t']:
 lines.append('The candidate has a lower observed full-sample FVR peak |t| in this matched campaign. This descriptive difference alone does not establish a statistically reliable leakage reduction; independent seeds and stronger sensitivity evidence would be needed.')
else:
 lines.append('The candidate does not improve the observed full-sample FVR peak |t| in this matched campaign. Preserve the positive data-skew/power-overhead engineering result separately; do not claim a demonstrated TVLA improvement.')
if not a['crossed'] and not b['crossed']:
 lines.append('Neither design crosses the conventional full-sample threshold. This campaign therefore does not establish that the candidate removed detectable baseline leakage; non-crossing is not proof of security.')
if r['stock_null']['crossed'] or r[C+'_null']['crossed']:
 lines.append('A full-sample null control crosses the threshold; this limits interpretation and needs investigation before treating an FVR difference as security evidence.')
lines+=['', '## Exploratory prefixes','',
'Prefixes use the first 128, 256 and 512 samples per class. They are correlated subsets of the same data, not independent repetitions. Full-sample results above remain the predeclared endpoint.','']
for name,prefix in s['exploratory_nested_prefixes'].items():
 lines.append('- '+name+': '+ '; '.join(f"{n} total traces: max |t|={v['max_abs_t']:.6f}, crossing bins={v['bins_above_4_5']}" for n,v in prefix.items()))
lines+=['','## Verification and scope','',
'- All four simulation logs pass exactly 1,024 functional traces and contain no reported timing violation, Error- or Fatal diagnostics. PX completion markers are present.','- Model-warning audit: '+ '; '.join(f"{name}: {v['capacitance_warnings_audited']} printed load warnings checked, {len(v['actual_load_extrapolations'])} actual load extrapolations" for name,v in model.items())+'.',
'- Raw power outputs are retained unchanged. Any separate analysis copy modifies only a sole off-grid terminal timestamp outside every measured window; `timestamp_audit.json` and `terminal_clipping.json` document exact operations and hashes.',
'- This is first-order TVLA of aggregate modeled power with glitch modeling enabled, not an isolated sensitive-glitch waveform test. Typical Nangate45 models and placement-estimated SPEF; not routed silicon measurements or multi-corner signoff.',
'- Uses the recorded seeds from the earlier campaign. Fresh execution does not make it an independent-seed replication. Historical stock repeatability and positive-control evidence are explicitly labeled in JSON; old traces are never pooled.',
'- Full t/class-mean arrays: per-run `tvla_samples.npz`; all crossing bins: `threshold_crossings.csv`; plot: `tvla_comparison.png`; protocol and exact numerical results: `protocol.json` and `tvla_results.json`.','']
(O/'ASSESSMENT.md').write_text('\n'.join(lines))
summary={'weighted_data_skew_reduction_percent':16.42177640659623,'fvr_power_overhead_percent':gain,'runs':{k:{f:v[f] for f in ['max_abs_t','bins_above_4_5','mean_measured_power_W']} for k,v in r.items()}}
(O/'summary.json').write_text(json.dumps(summary,indent=2,allow_nan=False))
print(json.dumps(summary,indent=2))
