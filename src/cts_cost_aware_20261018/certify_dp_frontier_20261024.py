"""Fresh-process frozen-data and actual-skew certification of selected tree."""
import json
from experiment import D,STOCK,flow,compare
from verify_final import identity_tcl
flow.BIN=D/'build7/openroad'
if __name__=='__main__':
    root=D/'dp_frontier_20261024/certification';root.mkdir(exist_ok=False)
    candidate=D/'dp_frontier_20261024/v4/mixed_e40_critical_leaves'
    for name,source in [('stock',STOCK),('candidate',candidate)]:
        out=root/name
        flow.run_tcl(out,flow.load(source)+identity_tcl.replace('@OUT@',str(out))+f'export_state {{{out}}}\n')
    assert (root/'stock/data_identity.txt').read_bytes()==(root/'candidate/data_identity.txt').read_bytes()
    result=compare(root/'candidate')
    expected=json.loads((candidate/'result.json').read_text())
    assert abs(result['weighted_reduction_percent']-expected['weighted_reduction_percent'])<1e-7
    assert result['legal'] and result['weighted_reduction_percent']>=15
    result['fresh_process_data_identity']='PASS'
    (root/'certification.json').write_text(json.dumps(result,indent=2))
    print('DP_SELECTED_INDEPENDENT_RELOAD_PASS')
