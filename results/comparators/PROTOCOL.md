# PRESENT CTS comparator campaign

Original references are immutable. This campaign compares tuned conventional TritonCTS and upstream iEDA/iCTS against the preserved PRESENT 16.4218% weighted-data-skew reduction circuit.

## Common conditions
Same placed masked PRESENT layer, original Nangate45 Liberty, 2 ns clock, original SDC, 5,088 marked gates and original weights. All original data/register instance types, coordinates, orientation and nonclock connections must survive. Each tree is independently constructed from the stock clock-stripped design. No security-aware targets or native optimization are enabled in conventional TritonCTS.

Evaluate all outputs in the same OpenROAD/OpenSTA binary with placement-estimated parasitics: signal metal3, clock metal6. Report 10 ps setup/hold margins and electrical checks, plus the original 80 ps movement bound separately. Reject physical-placement failures. This is not detailed-route or multi-corner signoff.

## TritonCTS tuning
Initial declared sweep: 13 configurations, varying cluster size, root driver and dummy loads with a broad buffer set. Follow-up sweep: 16 configurations using a narrower buffer set and explicit leaf drivers. A final eight configurations use clusters of 64/128 sinks and root strengths X8/X16, for 37 total configurations. All failures are retained; this is a bounded engineering tuning campaign, not exhaustive optimal tuning. Logs and exact commands are stored for every run. Select legal points on the weighted-skew versus clock-area frontier for actual PX evaluation. Include dummy loads in clock-area comparisons.

## iCTS
Pinned upstream source revision is recorded separately. Build dependencies are extracted/installed beneath this campaign directory. Twenty-one valid configurations cover skew bounds, root handling, buffer sets, fanout and transition limits. One earlier configuration with a root buffer outside the allowed buffer list is retained as a rejected setup attempt. Build-only compatibility patches are recorded; CTS algorithm changes are not permitted. The current upstream implementation is being evaluated, not claimed as an exact reproduction of every paper table.

## Power
Run the same full FVR vectors (1,024 transactions, 512/class), VCS cell models, SDF flags and PX configuration used for the preserved reference. The new PX files end just before the last nominal 80 ns window endpoint. Compare on common 79.98 ns support (15,996 bins) in every trace for every design, including preserved stock/DP arrays. Raw waveforms remain unchanged; no retained sample is interpolated. The original 80 ns reference results remain preserved. Full-run PX average and glitch reports are separate metrics. This campaign initially measures FVR power; it is not a new complete FVR/null TVLA validation.

## Integrity
reference_integrity.json verifies preserved reference hashes. Original reports, benchmark files and algorithm sources are unchanged. All new local output is beneath this directory; new commercial-tool jobs are beneath /home/c_karfa/Kairav/cts_comparison_20261112.
