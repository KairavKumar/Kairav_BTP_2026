from pathlib import Path
import subprocess,time,json
R=Path(__file__).resolve().parent
# Wait for the currently running tuning sweep; never run two iCTS processes together.
while not (R/'icts/fan64_BUF_X16_min4/execution.json').exists():time.sleep(5)
with (R/'icts_defaultroot.log').open('w') as f:subprocess.run(['python3',str(R/'run_icts_defaultroot.py')],stdout=f,stderr=subprocess.STDOUT,check=True)
names=json.loads((R/'icts_tuned_configs.json').read_text())+json.loads((R/'icts_defaultroot_configs.json').read_text())
with (R/'icts_evaluate_all.log').open('w') as f:subprocess.run(['python3',str(R/'evaluate_icts.py'),*names],stdout=f,stderr=subprocess.STDOUT,check=True)
print('ICTS_TUNING_AND_EVALUATION_COMPLETE',flush=True)
