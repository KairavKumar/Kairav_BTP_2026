from pathlib import Path
import os,subprocess,json,hashlib
R=Path(__file__).resolve().parent;V=R/'vendor/iEDA';binary=V/'scripts/design/sky130_gcd/iEDA'
e=os.environ.copy();e['LD_LIBRARY_PATH']=str(R/'deps/root/usr/lib/x86_64-linux-gnu');e['OMP_NUM_THREADS']='2'
for name in ['fast_fan32_buf8_root8', 'fast_fan64_buf8_root8', 'fast_fan32_buf16_root16', 'fast_fan64_buf16_root16', 'fast_fan16_buf8_root32', 'fast_fan32_buf8_root32']:
 o=R/'icts'/name
 with (o/'run.log').open('w') as f:
  p=subprocess.run([str(binary),'-script',str(o/'run.tcl')],cwd=V,env=e,stdout=f,stderr=subprocess.STDOUT)
 (o/'execution.json').write_text(json.dumps({'exit_code':p.returncode,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()},indent=2))
 print(name,p.returncode,flush=True)
 if p.returncode:break
