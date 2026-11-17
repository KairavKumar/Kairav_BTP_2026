"""Explicit additional5ps common target advance; all physical guards retained."""
import json,re
import run_dp_frontier_20261024 as c
if __name__=='__main__':
    name='mixed_advance5';target=c.ROOT/(name+'_targets.tcl')
    text=re.sub(r'(doubleprop \$pin cts_align_target_ns )([-\d.eE+]+)',lambda m:m[1]+format(float(m[2])-.005,'.17g'),c.base)
    with target.open('x') as f:f.write(text+'\nintprop $block cts_cost_pareto_frontier 1\n# Explicit additional common5psadvance\n')
    c.construct(c.ROOT/name,target,5,leaf=32,span=.020,diameter=100,calibration=c.D/'controlled609_g20_x4_v7/round0/calibration.tcl')
    report=c.compare(c.ROOT/name)
    print(json.dumps({k:report[k] for k in ('weighted_reduction_percent','legal','metrics')}),flush=True)
