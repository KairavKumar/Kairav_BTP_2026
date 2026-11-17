"""Export unique>=15% native-legal frontier candidates for matched power."""
import hashlib,json,os,shutil
from pathlib import Path
D=Path(__file__).resolve().parent
from power_campaign_20261024 import export,flow
ROOT=D/'dp_power_20261024'
if __name__=='__main__':
    ROOT.mkdir(exist_ok=True)
    # Reload/export only: original measurement engine, no new construction.
    flow.BIN=D/'build7/openroad'
    paths=[D/'dp_frontier_20261024/mixed_legacy_e5']+sorted((D/'dp_frontier_20261024/v4').glob('mixed_*'))
    rows=json.loads((ROOT/'summary.json').read_text()) if (ROOT/'summary.json').exists() else []
    existing={r['name'] for r in rows}
    seen={hashlib.sha256((Path(r['source'])/'design.v').read_bytes()).hexdigest() for r in rows}
    for path in paths:
        if not (path/'result.json').exists():continue
        if path.name in existing:continue
        report=json.loads((path/'result.json').read_text())
        if not report['legal'] or report['weighted_reduction_percent']<15:continue
        digest=hashlib.sha256((path/'design.v').read_bytes()).hexdigest()
        if digest in seen:continue
        seen.add(digest)
        name=path.name
        if name=='mixed_legacy_control':continue
        (ROOT/name).mkdir()
        shutil.copy2(path/'result.json',ROOT/name/'result.json')
        export(path,ROOT/(name+'_export'))
        rows.append({'name':name,'status':'EXPORTED','source':str(path),'result':report})
    (ROOT/'summary.json').write_text(json.dumps(rows,indent=2))
    print('PREPARED',[(r['name'],r['result']['weighted_reduction_percent']) for r in rows])
