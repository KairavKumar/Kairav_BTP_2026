"""Measure coarse shared-branch schedule freedom before spending clock cells."""
from experiment import D,STOCK,flow,plan
from collections import defaultdict
import json,re
text=(STOCK/'design.v').read_text();groups=defaultdict(list)
pins={r['pin'] for r in flow.rows(STOCK/'model_clocks.csv')}
for cell,name,ports in re.findall(r'\b(\w+)\s+(\S+)\s*\((.*?)\);',text,re.S):
    if name+'/CK' not in pins:continue
    net=re.search(r'\.CK\(([^)]+)\)',ports)
    assert net,name
    groups[net[1]].append(name+'/CK')
assert set(p for g in groups.values() for p in g)==pins
stock_groups=[sorted(g) for n,g in sorted(groups.items())]
(D/'stock_leaf_groups.json').write_text(json.dumps(stock_groups))
plan(D/'plan_stock_groups_best',0,grouping=stock_groups,maximize=True)
print((D/'plan_stock_groups_best/plan.json').read_text(),flush=True)
