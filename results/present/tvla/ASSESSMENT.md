# Matched TVLA: selected DP-frontier candidate versus stock

Tested circuit: `dp_frontier_20261024/v4/mixed_e40_critical_leaves`, with 16.4217764% measured weighted-data-skew reduction relative to original stock. Same frozen register/data placement; this is the final candidate with nine timing-repair leaf upsizes.

Four fresh VCS/PX runs completed: stock and candidate, each fixed/random (FVR) and random/random (null). Each has 1,024 functional transactions, 512 per class; 80 ns measured windows, 5 ps bins, 500 MHz clock. Both designs use identical vectors and byte-identical manifests per mode. Design exports, vectors and collected raw power have verified SHA-256 hashes.

## Measured results

- FVR: stock max |t| = 2.895734, candidate = 2.581247; bins above 4.5: stock 0, candidate 0 (out of 16,000 bins).
- NULL: stock max |t| = 3.014094, candidate = 3.056089; bins above 4.5: stock 0, candidate 0 (out of 16,000 bins).

Mean measured-window FVR power: stock 20.896761585 mW; candidate 21.643542569 mW (+3.573668% relative to stock).

## Interpretation

The candidate has a lower observed full-sample FVR peak |t| in this matched campaign. This descriptive difference alone does not establish a statistically reliable leakage reduction; independent seeds and stronger sensitivity evidence would be needed.
Neither design crosses the conventional full-sample threshold. This campaign therefore does not establish that the candidate removed detectable baseline leakage; non-crossing is not proof of security.

## Exploratory prefixes

Prefixes use the first 128, 256 and 512 samples per class. They are correlated subsets of the same data, not independent repetitions. Full-sample results above remain the predeclared endpoint.

- stock_fvr: 256 total traces: max |t|=5.282597, crossing bins=198; 512 total traces: max |t|=4.128897, crossing bins=0; 1024 total traces: max |t|=2.895734, crossing bins=0
- mixed_e40_critical_leaves_fvr: 256 total traces: max |t|=5.316399, crossing bins=45; 512 total traces: max |t|=4.590561, crossing bins=36; 1024 total traces: max |t|=2.581247, crossing bins=0
- stock_null: 256 total traces: max |t|=5.802036, crossing bins=14071; 512 total traces: max |t|=4.869673, crossing bins=37; 1024 total traces: max |t|=3.014094, crossing bins=0
- mixed_e40_critical_leaves_null: 256 total traces: max |t|=5.807270, crossing bins=13090; 512 total traces: max |t|=4.923421, crossing bins=2; 1024 total traces: max |t|=3.056089, crossing bins=0

## Verification and scope

- All four simulation logs pass exactly 1,024 functional traces and contain no reported timing violation, Error- or Fatal diagnostics. PX completion markers are present.
- Model-warning audit: stock_fvr: 14414 printed load warnings checked, 0 actual load extrapolations; stock_null: 14414 printed load warnings checked, 0 actual load extrapolations; mixed_e40_critical_leaves_fvr: 14710 printed load warnings checked, 0 actual load extrapolations; mixed_e40_critical_leaves_null: 14710 printed load warnings checked, 0 actual load extrapolations.
- Raw power outputs are retained unchanged. Any separate analysis copy modifies only a sole off-grid terminal timestamp outside every measured window; `timestamp_audit.json` and `terminal_clipping.json` document exact operations and hashes.
- This is first-order TVLA of aggregate modeled power with glitch modeling enabled, not an isolated sensitive-glitch waveform test. Typical Nangate45 models and placement-estimated SPEF; not routed silicon measurements or multi-corner signoff.
- Uses the recorded seeds from the earlier campaign. Fresh execution does not make it an independent-seed replication. Historical stock repeatability and positive-control evidence are explicitly labeled in JSON; old traces are never pooled.
- Full t/class-mean arrays: per-run `tvla_samples.npz`; all crossing bins: `threshold_crossings.csv`; plot: `tvla_comparison.png`; protocol and exact numerical results: `protocol.json` and `tvla_results.json`.
