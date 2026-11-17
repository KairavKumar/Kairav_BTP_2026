"""Run a new local VCS/PX campaign using explicit authorized external models."""
from pathlib import Path
import argparse,subprocess,shutil,os,re,json
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cell-models',type=Path,required=True);p.add_argument('--library-db',type=Path,required=True);p.add_argument('--name',required=True);a=p.parse_args()
models=a.cell_models.expanduser().resolve();db=a.library_db.expanduser().resolve()
for f in [models,db]:
 if not f.is_file():p.error(f'Missing external model: {f}')
for tool in ['vcs','pt_shell']:
 if shutil.which(tool) is None:p.error(f'{tool} is not on PATH')
out=(ROOT/'runs'/a.name).resolve()
if not out.is_relative_to(ROOT/'runs'):p.error('Output must stay within runs/')
out.mkdir(parents=True,exist_ok=False);(out/'runs').mkdir()
shutil.copy2(ROOT/'src/simulation/tb_layer.sv',out/'tb_layer.sv')
for mode in ['fvr','null']:shutil.copy2(ROOT/'results/present/tvla'/f'vectors_{mode}.txt',out/f'vectors_{mode}.txt')
text=(ROOT/'results/present/tvla/power_tvla_selected.tcl').read_text()
text=text.replace('set link_library [list * {/home/c_karfa/Kairav/cts_lab_assessment_20261018/library.db}]','set link_library [list * $::env(NANGATE_DB)]')
assert '/home/c_karfa/' not in text
(out/'power.tcl').write_text(text)
error=re.compile(r'FlexNet Licensing checkout error|Checkout.*failed|Cannot connect to.*license|^Error:|^Fatal:|Error-|Timing violation',re.I|re.M)
def run(command,cwd,log,env=None,marker=None):
 with log.open('w') as f:result=subprocess.run(command,cwd=cwd,stdout=f,stderr=subprocess.STDOUT,env=env)
 text=log.read_text(errors='replace')
 if result.returncode or error.search(text) or (marker and marker not in text):raise RuntimeError(f'Execution or validation failed; inspect {log}')
for case,folder in [('stock','stock'),('mixed_e40_critical_leaves','candidate')]:
 local=out/case;shutil.copytree(ROOT/'results/present/lab_exports'/folder,local);compile_dir=local/'compile';compile_dir.mkdir()
 run(['vcs','-full64','-sverilog','-timescale=1ns/1fs','+neg_tchk','+define+NTC','-sdf','typ:tb_layer.uut:../design.sdf','-top','tb_layer',str(models),'../design.v','../../tb_layer.sv','-o','simv'],compile_dir,compile_dir/'compile.log')
 for mode in ['fvr','null']:
  work=out/'runs'/f'{case}_{mode}';work.mkdir()
  run([str(compile_dir/'simv'),f'+VECTORS={out}/vectors_{mode}.txt','+sdfverbose'],work,work/'simulation.log',marker='FUNCTIONAL_PASS traces=1024')
  run(['pt_shell','-f','power.tcl'],out,work/'power.log',env=dict(os.environ,ASSESS_CASE=case,ASSESS_MODE=mode,NANGATE_DB=str(db)),marker='ASSESSMENT_POWER_COMPLETE')
  if not (work/'power.out').is_file():raise RuntimeError('Missing power output')
  print('Completed',case,mode,flush=True)
(out/'protocol.json').write_text((ROOT/'results/present/tvla/protocol.json').read_text())
print('New campaign saved in',out,'; audit hashes, warnings and window coverage before reporting.')
