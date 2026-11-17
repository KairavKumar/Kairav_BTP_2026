from pathlib import Path
import time,subprocess,json
R=Path(__file__).resolve().parent
while not (R/'icts/defaultroot_fan64_inheriton/execution.json').exists():time.sleep(5)
with (R/'icts_fast.log').open('w') as f:subprocess.run(['python3',str(R/'run_icts_fast.py')],stdout=f,stderr=subprocess.STDOUT)
with (R/'icts_fast_eval.log').open('w') as f:subprocess.run(['python3',str(R/'evaluate_icts.py'),*json.loads((R/'icts_fast_configs.json').read_text())],stdout=f,stderr=subprocess.STDOUT)
