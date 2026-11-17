# Commercial-tool inputs and execution

Use Linux and an authorized tool installation. Never commit login adapters, credentials or license configuration. These commands assume a freshly created working directory under your own project folder.

Inputs supplied: `results/present/lab_exports/{stock,candidate}/design.{v,sdf,spef,pt.sdc}`, `src/simulation/tb_layer.sv`, matched vector files in `results/present/tvla/`, and the saved protocol. The exact recorded runner is `results/present/tvla/run_tvla.sh`; its historical paths identify the successful run. The exact power settings are in `power_tvla_selected.tcl`. Review these before creating a new experiment. The same vectors, testbench, SDF convention and per-mode windows must be used for both circuits.

External inputs: compatible Nangate cell Verilog simulation models, a compiled Nangate `.db` library, and licensed VCS and PrimeTime/PX. The library build recipe is preserved in `src/simulation/library.tcl`. Historical server paths in these scripts must be replaced with your own input paths. Do not overwrite or relabel the preserved results.

1. Compile the netlist and cell models with the SystemVerilog testbench using the flags in the recorded runner.
2. Run stock/candidate FVR and null with the protocol's 1,024 transactions each. Require functional completion and inspect timing diagnostics.
3. Run the saved PX power script with the corresponding netlist, SDC, placement SPEF, `.db` and generated VCD. Keep glitch modeling and output resolution identical.
4. Retain the complete output, per-transaction window manifests, reports and file hashes. Check completion and warnings before analysis.
5. Analyze each new campaign separately using the preserved OUT parser and Welch formula. Do not pool old and new traces.

The repository deliberately supplies recorded commands rather than an automatic remote launcher. It cannot provide commercial licenses or credentials. Both lab compute servers failed to reach the configured license ports in the latest Nov. 17 connectivity check; this does not affect completed paper results.

## Portable local launcher

Run `python scripts/run_commercial.py --name new_campaign --cell-models /absolute/path/stdcells.v --library-db /absolute/path/library.db` from the repository root on the authorized tool machine. It stages saved exports, compiles VCS, and runs all four simulation/PX cases into a fresh runs/ directory. It checks license errors, exit status and completion markers. This new launcher was syntax-checked, not executed while license connectivity is unavailable. It needs your existing tool/Library Compiler environment. Audit warning reports and hashes before reporting a new campaign; it does not replace the preserved completed campaign.
