from experiment import D,plan
import json
groups=json.loads((D/'groups_span20.json').read_text())
for gain in [15,20]:
    plan(D/f'plan_group609_gain{gain}',gain,grouping=groups)
    print('GROUPED_PLAN_READY',gain,flush=True)
