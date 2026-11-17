# Method and file map

1. `src/cts_larger_20261011`: 16-lane wrapper, unchanged masked S-box, functional checks, conservative secret/share provenance and fixed 8/4/2 gate marking.
2. `src/cts_alignment_20261018`: physical loading, placement RC estimation, source-aware export and comparisons. OpenSTA supplies early/late timing and setup/hold relationships.
3. `src/cts_cost_aware_20261018/experiment.py`: individual/shared-target LP, timing screens, minimum-movement and regression penalties. `group_plans.py` and `split_groups.py` recover/split stock leaf groups.
4. `src/OpenROAD/src/cts/src/CostAwareTreeBuilder.cpp`: measured historical constructor. Geometry/target hierarchy plus bounded approximate DP chooses shared branches and mixed buffer strengths. It is not a global optimal-tree solver.
5. `results/present/unrepaired/run.tcl` and `candidate/run.tcl`: exact recorded construction and nine-leaf repair commands. Their historical paths are remapped by the portable native replay.
6. `results/present/lab_exports`: identical evaluated circuit exported for VCS/PT/PX.
7. `results/present/tvla`: matched FVR/null protocol, raw power, manifests, arrays, reports and audits.
8. `scripts/reproduce.py`: portable verification, summaries, plot regeneration, raw-power TVLA replay, shared-target LP and native replay.

`src/cts_cost_aware_20261018` also retains historical drivers for review. Their fixed paths/output roots are not portable; prefer `scripts/reproduce.py`. This artifact reports the 16.42% PRESENT point. The 17.60% earlier point, later placement experiments and other benchmarks are not substituted into its results.
