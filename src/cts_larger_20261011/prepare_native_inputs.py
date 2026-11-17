"""Attach complete checked gate/ref metadata for native C++ sizing and Tcl insertion."""
import json
import sys
from pathlib import Path
here=Path(__file__).resolve().parent
sys.path.insert(0,str(here.parent/'OpenROAD/src/cts/BTP_Research/secure_skew_demo'))
from clock_graph import tcl_literal
manifest=json.loads((here/'baseline_graph/physical_targets.json').read_text())
report=json.loads((here/'security_gates.json').read_text())
lines=['set block [ord::get_db_block]','set reference [dict create]','set gates [dict create]']
for r in manifest['targets']:
    name=tcl_literal(r['clock_pin'])
    lo=r['baseline_min_rise_ns'];hi=r['baseline_max_rise_ns'];delta=r['delta_ns']
    lines += [f'dict set reference {name} [list {lo:.17g} {hi:.17g} {delta:.17g}]',
              f'set term [$block findITerm {name}]',
              'if {$term eq "NULL"} {error "Missing annotated clock pin"}',
              f'odb::dbDoubleProperty_create $term cts_data_skew_baseline_min_ns {lo:.17g}',
              f'odb::dbDoubleProperty_create $term cts_data_skew_baseline_max_ns {hi:.17g}']
for r in report['gates']:
    if r['category']=='no_annotated_secret': continue
    name=tcl_literal(r['gate']);pins=' '.join(tcl_literal(p) for p in r['input_cones'])
    lines += [f'dict set gates {name} [list {r["weight"]} [list {pins}]]',
              f'set inst [$block findInst {name}]',
              'if {$inst eq "NULL"} {error "Missing annotated gate"}',
              f'odb::dbDoubleProperty_create $inst cts_data_skew_gate_weight {r["weight"]}']
    lines += [f'odb::dbIntProperty_create [$block findITerm {tcl_literal(p)}] cts_data_skew_gate_input 1' for p in r['input_cones']]
lines += ['odb::dbIntProperty_create $block cts_data_skew_native_sizing 1',
          'odb::dbIntProperty_create $block cts_data_skew_native_trial_budget 64']
(here/'native_sizing_inputs.tcl').write_text('\n'.join(lines)+'\n')
model=['set reference [dict create]','set gates [dict create]']
model += [line for line in lines if line.startswith('dict set reference ') or line.startswith('dict set gates ')]
eligible=[r['clock_pin'] for r in sorted(manifest['targets'],key=lambda r:(-r['delta_ns'],r['clock_pin'])) if r['delta_ns']>1e-6][:4]
model += ['set eligible [list '+' '.join(tcl_literal(p) for p in eligible)+']']
(here/'gate_model.tcl').write_text('\n'.join(model)+'\n')
proposal=json.loads((here/'baseline_graph/targets.json').read_text())
print(json.dumps({'status':proposal['status'],'targets':len(manifest['targets']),'marked_gates':len(report['gates']),
                 'eligible':eligible,'proposal_keys':list(proposal)},indent=2))
