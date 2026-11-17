"""Rebuild owned CTS objects and relink against the preserved dependency build."""
from pathlib import Path
import json,os,shutil,subprocess,hashlib
D=Path(__file__).resolve().parent;K=D.parent
out=D/'build';out.mkdir(exist_ok=False)
base=K/'cts_engine_20261015/build_validated'
commands=json.loads((base/'commands.json').read_text())
manifest={'compile':[],'sha256':{}}
names=['DataSkewOptimizer.cpp','HTreeBuilder.cpp','TritonCTS.cpp','CostAwareTreeBuilder.cpp']
for name in names:
    cmd=commands['compile'].copy();cmd[-1]=str(K/'OpenROAD/src/cts/src'/name)
    cmd[cmd.index('-o')+1]=str(out/(name+'.o'))
    manifest['compile'].append(cmd)
    with (out/(name+'.log')).open('w') as log:
        subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
    print('COMPILED',name,flush=True)
archive=out/'libcts_lib.a';shutil.copy2(base/'libcts_lib.a',archive)
subprocess.run(['ar','r',str(archive)]+[str(out/(n+'.o')) for n in names],check=True)
subprocess.run(['ranlib',str(archive)],check=True)
link=[str(archive) if x==str(base/'libcts_lib.a') else x for x in commands['link']]
link=['-flto=8' if x.startswith('-flto=') else x for x in link]
link[link.index('-o')+1]=str(out/'openroad')
manifest['link']=link
for p in (K/'OpenROAD/src/cts/src').iterdir():
    if p.is_file() and (p.name.startswith(('Cost','Data')) or p.name in ['TritonCTS.cpp','HTreeBuilder.cpp','TechChar.h']):
        manifest['sha256'][p.name]=hashlib.sha256(p.read_bytes()).hexdigest()
(out/'commands.json').write_text(json.dumps(manifest,indent=2))
(out/'tmp').mkdir()
with (out/'link.log').open('w') as log:
    subprocess.run(link,cwd=K/'cts_engine_work_20261011/build/src',env=dict(os.environ,TMPDIR=str(out/'tmp')),stdout=log,stderr=subprocess.STDOUT,check=True)
print('COST_AWARE_BUILD_PASS',flush=True)
