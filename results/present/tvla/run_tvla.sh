#!/usr/bin/env bash
set -euo pipefail
cd /home/c_karfa/Kairav/cts_cost_aware_20261018/dp_power_20261024/frontier_hold
export SYNOPSYS_LC_ROOT=/home/Synopsys_tools/syps/lc/O-2018.06-SP5-1
case="$1"
mode="$2"
run="runs/${case}_${mode}"
mkdir "$run"
(cd "$run" && "../../$case/compile/simv" "+VECTORS=../../vectors_${mode}.txt" +sdfverbose > simulation.log 2>&1)
grep -qx 'FUNCTIONAL_PASS traces=1024' "$run/simulation.log"
if grep -qi 'Timing violation\|Error-\|Fatal:' "$run/simulation.log"; then exit 3; fi
ASSESS_CASE="$case" ASSESS_MODE="$mode" pt_shell -f power_tvla_selected.tcl > "$run/power.log" 2>&1
if grep -q '^Error:\|^Fatal:' "$run/power.log"; then exit 4; fi
grep -q ASSESSMENT_POWER_COMPLETE "$run/power.log"
test -s "$run/power.out"
gzip "$run/activity.vcd"
echo "TVLA_RUN_COMPLETE $case $mode"
