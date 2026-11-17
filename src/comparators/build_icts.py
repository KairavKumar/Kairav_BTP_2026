from pathlib import Path
import os,subprocess
r=Path(__file__).resolve().parent
e=os.environ.copy();e['PATH']=str(r/'deps/root/usr/bin')+':'+e['PATH'];e['LD_LIBRARY_PATH']=str(r/'deps/root/usr/lib/x86_64-linux-gnu');e['CARGO_HOME']=str(r/'deps/cargo');e['CARGO_BUILD_JOBS']='2'
e['RUSTUP_HOME']=str(r/'deps/rustup');e['PATH']=str(r/'deps/cargo/bin')+':'+e['PATH']
e['PKG_CONFIG_PATH']=str(r/'deps/root/usr/lib/x86_64-linux-gnu/pkgconfig')
subprocess.run(['python3',str(r/'configure.py')],env=e,cwd=r/'vendor/iEDA',check=True)
subprocess.run(['cmake','--build',str(r/'vendor/iEDA/build'),'--target','iEDA','-j','6'],env=e,check=True)
