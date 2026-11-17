from pathlib import Path
import sys,json,hashlib,itertools
R=Path(__file__).resolve().parent; K=R.parent; D=K/'cts_cost_aware_20261018'
sys.path.insert(0,str(D))
import experiment as e
e.flow.BIN=D/'build_dp_20261024_v4/openroad'
configs=[dict(name='stock_settings',root='BUF_X4',buffers='BUF_X2 BUF_X4',size=20,diameter=100,dummy=True)]
for size,root,dummy in itertools.product([10,20,40],['BUF_X2','BUF_X4'],[False,True]):
 configs.append(dict(name=f'mixed_s{size}_{root}_dummy{int(dummy)}',root=root,buffers='BUF_X1 BUF_X2 BUF_X4 BUF_X8 BUF_X16',size=size,diameter=100,dummy=dummy))
(R/'triton_protocol.json').write_text(json.dumps({'benchmark':'PRESENT','configs':configs,'binary':str(e.flow.BIN),'binary_sha256':hashlib.sha256(e.flow.BIN.read_bytes()).hexdigest(),'measurement':'same frozen stock, gate weights, placement RC and 10ps setup/hold margin; record 80ps movement separately','selection':'retain legal Pareto points in measured weighted skew and clock area; actual PX power required before claiming power benefit'},indent=2))
results={}
for c in configs:
 out=R/'triton'/c['name']
 if (out/'result.json').exists():results[c['name']]=json.loads((out/'result.json').read_text());continue
 if out.exists():continue
 body=e.flow.load(e.STOCK)+"set block [ord::get_db_block]\n"
 for prop in ['cts_cost_aware_tree','cts_data_alignment','cts_data_skew_native_sizing','cts_align_defer']:
  body+=f'intprop $block {prop} 0\n'
 body+='set state [strip_clock]\n'
 body+=f"clock_tree_synthesis -root_buf {c['root']} -buf_list {{{c['buffers']}}} -sink_clustering_enable -sink_clustering_size {c['size']} -sink_clustering_max_diameter {c['diameter']} "+('' if c['dummy'] else '-dont_use_dummy_load')+'\n'
 body+='detailed_placement\nset_propagated_clock [all_clocks]\nestimate_parasitics -placement\ncheck_data $state\n'
 body+=f'export_state {{{out}}} 0\n'
 try:
  e.flow.run_tcl(out,body)
  r=e.compare(out);m=r['metrics'];r['timing_electrical_pass']=float(m['setup_ns'])>=.01 and float(m['hold_ns'])>=.01 and int(m['electrical_pass'])==1
  r['movement_80ps_pass']=r['max_clock_movement_ps']<=80+1e-6
  r['data_identity_pass']='DATA_IDENTITY_PASS' in (out/'run.log').read_text()
  (out/'result.json').write_text(json.dumps(r,indent=2));results[c['name']]=r
  print(c['name'],r['weighted_reduction_percent'],m,flush=True)
 except Exception as ex:results[c['name']]={'error':str(ex)};print(c['name'],str(ex),flush=True)
 (R/'triton_summary.json').write_text(json.dumps(results,indent=2))
