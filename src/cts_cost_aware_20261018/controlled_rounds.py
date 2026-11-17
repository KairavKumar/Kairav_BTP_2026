from experiment import D,construct,compare
import argparse,json
p=argparse.ArgumentParser();p.add_argument('--plan',required=True);p.add_argument('--name',required=True);p.add_argument('--leaf-master');p.add_argument('--leaf-slew-ps',type=float);p.add_argument('--rounds',type=int,default=3);a=p.parse_args()
root=D/a.name;root.mkdir(exist_ok=False);calibration=None;results=[]
for i in range(a.rounds):
    out=root/f'round{i}'
    construct(out,D/a.plan/'targets.tcl',5,leaf=32,span=.020,calibration=calibration,diameter=100,leaf_master=a.leaf_master,leaf_max_slew=a.leaf_slew_ps/1000 if a.leaf_slew_ps else None)
    result=compare(out);results.append({'folder':str(out),**result})
    print('ROUND',i,json.dumps(result),flush=True)
    (root/'summary.json').write_text(json.dumps({'cases':results,'best_native':min([r for r in results if r['accepted_improvement']],key=lambda r:-r['weighted_reduction_percent'],default=None)},indent=2))
    calibration=out/'calibration.tcl'
