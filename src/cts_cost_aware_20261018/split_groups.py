from experiment import D,plan
import json
groups=json.loads((D/'stock_leaf_groups.json').read_text())
targets=json.loads((D/'plan20/targets.json').read_text())
for span in [20,10,5]:
    split=[]
    for group in groups:
        ordered=sorted(group,key=lambda p:targets[p]);part=[]
        for pin in ordered:
            if part and targets[pin]-targets[part[0]]>span/1000:
                split.append(part);part=[]
            part.append(pin)
        if part:split.append(part)
    (D/f'groups_span{span}.json').write_text(json.dumps(split))
    folder=D/f'plan_group_span{span}_best'
    try:
        plan(folder,0,grouping=split,maximize=True)
        print((folder/'plan.json').read_text(),flush=True)
    except Exception as error:
        print('GROUP_PLAN_INFEASIBLE',span,len(split),str(error),flush=True)
plan(D/'plan_group_stock_gain5',5,grouping=groups)
print('COARSE_GAIN5_READY',flush=True)
