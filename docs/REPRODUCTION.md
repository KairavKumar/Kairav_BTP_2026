# Reproducing the PRESENT results

Run from the repository root on Linux/WSL. Python >=3.11 is required.

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
make verify
make summary
make figures
make tvla
make plan
```

Every replay creates a fresh directory in `runs/`. Existing output directories are rejected; use `python scripts/reproduce.py figures --name figures_second` for a second run. Raw power replay needs roughly 1 GB RAM and several hundred MB of scratch space. It decompresses original waveforms, checks SHA-256, matches manifests, applies only the recorded outside-window terminal adjustment, computes Welch t and asserts agreement with saved values. It does not rerun VCS/PX.

## Native timing and construction

The complete custom CTS module is in `src/OpenROAD/src/cts`. The 17 source files named in the recorded measured build are hash-exact, including a recovered historical constructor. This is an OpenROAD module, not a standalone C++ program. The measured executable identifies as `26Q3-1899-g63fe72c577`. `verification/build/commands.json` records the historical selective compile/link commands and dependency paths. Those paths are provenance, not portable build instructions.

With the original compatible executable:

```bash
make native OPENROAD=/absolute/path/to/openroad
python scripts/reproduce.py native --construct --name reconstruction \
  --openroad /absolute/path/to/openroad
```

The first command reloads saved stock/candidate ODBs and remeasures. The second reconstructs the mixed-strength tree from stock, applies the exact nine recorded leaf repairs, and checks the recorded cost, setup, hold and area. This requires the custom constructor and data-skew APIs; an ordinary installed OpenROAD is insufficient. The supplied Nangate files support the saved physical design. New results never overwrite `results/`.

To rebuild, use a compatible OpenROAD checkout based on revision `63fe72c577`, its own recursive submodules and dependency instructions, and overlay `src/OpenROAD/src/cts` onto its `src/cts`. The module contains CMake and Bazel registrations. A clean independent dependency build has not been validated in this artifact; the measured experiment used the recorded selective rebuild. Do not infer clean-build reproducibility from a successful binary replay.

## Original RTL to physical design

`src/cts_larger_20261011/present_hls.v` is the unchanged TETRIS masked S-box RTL; `masked_present_layer.v` instantiates 16 lanes. `synth_layer.tcl` records Design Compiler mapping, 2 ns clock and I/O constraints. `run_layer_physical.tcl` records the original physical setup. They retain historical paths and must be adapted to a new output root and authorized synthesis setup. The artifact starts native replay from the preserved stock ODB so original placement is held fixed.

## New commercial simulation and power runs

See `docs/COMMERCIAL_TOOLS.md`. VCS, Design Compiler and PrimeTime/PX installations, `.db` libraries, cell simulation models and licenses are external prerequisites. None is bundled. Existing raw power and simulation outputs are included, allowing offline analysis without those tools. New tool runs depend on the lab license service; they are distinct from regenerating figures or recomputing TVLA from completed runs.

## Preparing the source build

`python scripts/prepare_openroad.py --destination /absolute/path/new_openroad_checkout` fetches verified upstream revision 63fe72c577ef12d2ac2a0a944bbd842f9af8a0a7 and recursive submodules, then overlays the preserved CTS module. It requires a new directory. Install that checkout's documented dependencies and use its normal build workflow. This helper is provided separately from the verified binary replay; it does not claim the dependency build was tested here.
