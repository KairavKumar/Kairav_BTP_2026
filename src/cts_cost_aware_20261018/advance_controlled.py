from experiment import D,construct,compare
import re,json
root=D/'controlled_x4_advance5_v7';root.mkdir(exist_ok=False)
text=(D/'plan_group609_gain20/targets.tcl').read_text()
text=re.sub(r'(doubleprop \$pin cts_align_target_ns )([-\d.eE+]+)',lambda m:m[1]+format(float(m[2])-.005,'.17g'),text)
target=root/'targets.tcl';target.write_text(text)
(root/'plan.json').write_text(json.dumps({'explicit_common_advance_ps':5,'original_plan':str(D/'plan_group609_gain20'),'model_anchor':str(D/'controlled609_g20_x4_v7/round0'),'reason':'Independent PrimeTime hold near miss is input-to-register; advance requested common latency while retaining relative targets. All actual guards unchanged.'},indent=2))
construct(root/'circuit',target,5,leaf=32,span=.020,diameter=100,leaf_master='BUF_X4',calibration=D/'controlled609_g20_x4_v7/round0/calibration.tcl')
print(json.dumps(compare(root/'circuit'),indent=2))
