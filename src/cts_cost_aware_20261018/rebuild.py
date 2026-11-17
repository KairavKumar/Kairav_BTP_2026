from pathlib import Path
import argparse,json,subprocess,shutil,os,hashlib
D=Path(__file__).resolve().parent;K=D.parent
p=argparse.ArgumentParser();p.add_argument('--name',required=True);a=p.parse_args()
out=D/a.name;out.mkdir(exist_ok=False)
base=D/'build';commands=json.loads((base/'commands.json').read_text())
archive=out/'libcts_lib.a';shutil.copy2(base/'libcts_lib.a',archive)
names=['TritonCTS.cpp','CostAwareTreeBuilder.cpp'];compiled=[]
for name in names:
    cmd=next(c for c in commands['compile'] if Path(c[-1]).name==name).copy()
    cmd[cmd.index('-o')+1]=str(out/(name+'.o'));compiled.append(cmd)
    with (out/(name+'.log')).open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('COMPILED',name,flush=True)
subprocess.run(['ar','r',str(archive)]+[str(out/(n+'.o')) for n in names],check=True)
subprocess.run(['ranlib',str(archive)],check=True)
link=[str(archive) if x==str(base/'libcts_lib.a') else x for x in commands['link']]
link[link.index('-o')+1]=str(out/'openroad');(out/'tmp').mkdir()
manifest={'compile':compiled,'link':link,'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (K/'OpenROAD/src/cts/src').iterdir() if p.is_file() and (p.name.startswith(('Cost','Data')) or p.name in ['TritonCTS.cpp','TechChar.h'])}}
(out/'commands.json').write_text(json.dumps(manifest,indent=2))
with (out/'link.log').open('w') as log:subprocess.run(link,cwd=K/'cts_engine_work_20261011/build/src',env=dict(os.environ,TMPDIR=str(out/'tmp')),stdout=log,stderr=subprocess.STDOUT,check=True)
print('COST_REBUILD_PASS',a.name,flush=True)
