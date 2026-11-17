from pathlib import Path
import sys,json,hashlib
R=Path(__file__).resolve().parent;D=R.parent/'cts_cost_aware_20261018'
sys.path.insert(0,str(D));import experiment as e
e.flow.BIN=D/'build_dp_20261024_v4/openroad'
for name in sys.argv[1:]:
 source=R/'triton'/name;out=R/'exports'/name
 if out.exists():continue
 body=e.flow.load(source)+f"estimate_parasitics -placement -spef_file {{{out/'design.spef'}}}\nwrite_sdf -include_typ -digits 9 {{{out/'design.sdf'}}}\nexport_state {{{out}}} 0\n"
 e.flow.run_tcl(out,body)
 (out/'design.pt.sdc').write_text('\n'.join(x for x in (out/'design.sdc').read_text().splitlines() if not x.startswith('current_design '))+'\n')
 before=e.flow.rows(source/'metrics.csv')[0];after=e.flow.rows(out/'metrics.csv')[0]
 for k in ['cost_ns','setup_ns','hold_ns','area_um2']:assert abs(float(before[k])-float(after[k]))<1e-7
 (out/'provenance.json').write_text(json.dumps({'source':str(source),'metrics':after,'sha256':{n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in ['design.v','design.sdf','design.spef','design.pt.sdc']}},indent=2))
 print('EXPORTED',name,flush=True)
