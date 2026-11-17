"""Resolve every active STA source to checked RTL dependencies, never by guesses."""
import json
import re
import sys
from pathlib import Path

here=Path(__file__).resolve().parent
demo=here.parent/'OpenROAD/src/cts/BTP_Research/secure_skew_demo'
sys.path.insert(0,str(demo))
from identify_security_gates import identify

graph=json.loads((here/'baseline_graph/graph.json').read_text())
rtl=json.loads((here/'rtl_provenance.json').read_text())
sources={}
used=sorted({r['source_pin'] for r in graph['arrivals']})
for pin in used:
    match=re.fullmatch(r'lanes_(\d+)__u_(.+)_reg/QN?',pin)
    if match:
        lane,name=match.groups()
        if name not in rtl['register_labels']: raise ValueError('Unmapped register: '+pin)
        labels=rtl['register_labels'][name]
    else:
        match=re.fullmatch(r'(x[01]|randomness)\[(\d+)\]',pin)
        if not match: raise ValueError('Unmapped timing source: '+pin)
        port,index=match.groups();index=int(index)
        if port=='randomness':
            lane=str(index//23);labels=[f'random:rand_{index%23+1}']
        else:
            lane=str(index//4);labels=[f'secret:{index%4}:{port[-1]}']
    deps=[]
    for label in labels:
        if label.startswith('secret:'):
            _,bit,share=label.split(':')
            deps.append({'secret':f'lane_{lane}_bit_{bit}','share':share})
    if deps: sources[pin]={'kind':'secret','dependencies':deps}
    elif any(x.startswith('random:') for x in labels): sources[pin]={'kind':'random'}
    else: sources[pin]={'kind':'public'}
annotation={'sources':sources,'evidence':{
    'method':'Conservative RTL assignment dependency union propagated through registers',
    'source_sha256':rtl['source_sha256'],
    'scope':'Fixed 40-cycle transaction protocol; no Boolean cancellation or statistical independence proof',
    'all_used_sources_resolved':True,
}}
(here/'security_annotations.json').write_text(json.dumps(annotation,indent=2)+'\n')
report=identify(graph,annotation)
(here/'security_gates.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report['coverage'],indent=2))
