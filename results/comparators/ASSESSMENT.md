# PRESENT CTS comparison

Campaign continued on 16 November 2026. Original reference hashes and fresh-process reloads passed. See PROTOCOL.md for conditions and comparison.csv for all candidate metrics.

## TritonCTS

37 evaluated output trees; 9 pass native setup/hold >=10 ps, electrical checks and <=80 ps movement. Data identity is checked before scoring.

Best achieved weighted-skew reduction: 0.350780% (large_s64_BUF_X16_dummy1); hold 23.987 ps; clock area including dummy loads 287.014 um².

Measured FVR power points:

- narrow_s20_BUF_X8_dummy1: skew reduction 0.031721%; power 20.727397 mW (-0.569% vs stock); native hold 22.147 ps. Independent PT reports are saved with the power campaign.
- narrow_s40_BUF_X8_dummy0: skew reduction -0.083775%; power 18.371231 mW (-11.871% vs stock); native hold 25.275 ps. Independent PT reports are saved with the power campaign.
- narrow_s40_BUF_X8_dummy1: skew reduction 0.020972%; power 18.539109 mW (-11.066% vs stock); native hold 25.275 ps. Independent PT reports are saved with the power campaign.

## iCTS

21 evaluated output trees; 0 pass native setup/hold >=10 ps, electrical checks and <=80 ps movement. Data identity is checked before scoring.

No valid operating point is established by this bounded configuration sweep. Failed trees are retained, and no power or superiority claim is made from an invalid implementation. This is not evidence that the published method cannot meet timing under a more complete integration/tuning flow.

Measured FVR power points:


## Interpretation

The proposed method targets weighted data skew. Conventional CTS can lower total power substantially while leaving that score nearly unchanged. Therefore the supported claim is a skew/power tradeoff, rather than overall dominance. Compare Pareto points at common cost or achieved skew; this sweep does not establish a globally optimal conventional baseline.

Clock area uses identical dummy-load accounting. The stock value is 760.228 um² and the preserved DP value is 804.916 um². These differ from driver-only area figures.

## Measurement boundaries

Comparator waveforms have tool-generated terminal markers before the final manifest endpoint. The analysis uses the same first 79.98 ns (15,996 bins) of every trace for stock, preserved DP and all comparators. Only timestamps after the retained endpoint are removed in separate analysis files; no in-window sample is interpolated. The original 80 ns published reference statistics remain unchanged.

Comparator t statistics are exploratory FVR-only values, with no new null-control validation. They do not establish leakage improvement or a percentage security improvement. Full-run PX glitch power is a separate averaging domain.

## Reproduction

Use the exact run.tcl in each candidate directory for native construction/evaluation. run_triton.py, run_triton_narrow.py and run_triton_large.py define the 37-configuration TritonCTS sweep. iCTS source commit, binary hash and build-only patch are in icts_provenance.json. Each iCTS directory contains its config and input paths; evaluate_icts.py imports and legalizes only CTS cells while fixing all data cells.

Power staging, collection and analysis scripts are kept in this campaign directory. Remote jobs use /home/c_karfa/Kairav/cts_comparison_20261112. No original reference output is replaced.


## Completed and blocked work, 16 November 2026

All 37 TritonCTS and 21 iCTS output trees have been evaluated locally. Nine TritonCTS trees pass native checks. All 21 iCTS trees preserve data identity but have negative hold slack (the closest tested point is approximately -43.5 ps). Their failure cannot support a claim of superiority over the published algorithm; a valid iCTS timing integration remains unresolved. The tested source is upstream iEDA at the pinned commit, not an exact paper-artifact reproduction.

Three TritonCTS points completed full 1,024-transaction FVR VCS/PX runs and independent PrimeTime timing checks. All three independent PT checks pass the 10 ps setup/hold margin; the verified PT log has no errors. Their power waveforms and manifests were downloaded and checked against remote SHA-256 values.

The later large-cluster configurations have no measured power result. Both the initial VCS attempt and one retry failed because the lab license server was unreachable; independent PrimeTime also failed license checkout. Evidence: power_large/status_20261116.json. They are not assigned estimated or copied power numbers.

The preserved DP reference has 16.421776% weighted-skew reduction and approximately +3.57% total-power overhead. The best native-legal tuned TritonCTS score is 0.350780%; its power remains blocked. Among points with completed power testing, the lowest-power setting has -0.083775% skew reduction (a small worsening) and -11.871% total-power change. Thus power tuning is a meaningful baseline and our current demonstrated benefit is weighted skew reduction with a cost.

All reference hashes pass again after the campaign, and stock/DP metric reload differences are exactly zero. No original report has been updated with incomplete comparator results.


## License-log provenance correction

The fresh 16 November independent PrimeTime recheck failed license checkout. Retained timing reports/CSVs are from 12 November; their existence was insufficient to establish a successful fresh recheck. See license_diagnostics_20261117/FINDINGS.md for the verified timeline and current connectivity evidence.
