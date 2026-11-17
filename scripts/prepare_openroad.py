"""Prepare a pinned OpenROAD checkout with the measured CTS source overlay."""
from pathlib import Path
import argparse,subprocess,shutil
ROOT=Path(__file__).resolve().parents[1]
REV='63fe72c577ef12d2ac2a0a944bbd842f9af8a0a7'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--destination',required=True);a=p.parse_args()
dest=Path(a.destination).expanduser().resolve()
if dest.exists():raise FileExistsError('Choose a new destination; existing checkouts are preserved')
dest.mkdir(parents=True)
def run(*args):subprocess.run(args,cwd=dest,check=True)
run('git','init');run('git','remote','add','origin','https://github.com/The-OpenROAD-Project/OpenROAD.git')
run('git','fetch','--depth','1','origin',REV);run('git','checkout','--detach','FETCH_HEAD')
run('git','submodule','update','--init','--recursive','--depth','1')
shutil.copytree(ROOT/'src/OpenROAD/src/cts',dest/'src/cts',dirs_exist_ok=True)
print('Prepared',dest,'at upstream',REV,'with measured CTS overlay.')
print('Install the checkout\'s documented dependencies, then build with its etc/Build.sh or CMake workflow.')
print('This preparation is not a claim of a validated clean dependency build.')
