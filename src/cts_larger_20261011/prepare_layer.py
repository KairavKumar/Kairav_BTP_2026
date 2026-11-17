"""Prepare a 64-bit masked S-box layer and fail-closed RTL provenance map.

This handles only the assignment-only, single-bit generated RTL in present_hls.v.
Dependencies are conservative unions, not a proof of masking independence.
"""
import hashlib
import json
import re
from pathlib import Path

here = Path(__file__).resolve().parent
rtl = (here/'present_hls.v').read_text()
inputs = re.findall(r'\binput\s+(\w+)\s*;', rtl)
regs = re.findall(r'\b(?:output\s+)?reg\s+(\w+)\s*;', rtl)
assigns = dict(re.findall(r'\bassign\s+(\w+)\s*=\s*([^;]+);', rtl))
sequential = dict(re.findall(r'\b(\w+)\s*<=\s*([^;]+);', rtl))
if set(regs) != set(sequential):
    raise ValueError('Unassigned or multiply declared register')
expressions = {**assigns, **sequential}
labels = {}
for name in inputs:
    if name == 'clk': continue
    match = re.fullmatch(r'x([0-3])_([01])', name)
    if match: labels[name] = {f'secret:{match[1]}:{match[2]}'}
    elif name.startswith('rand_'): labels[name] = {f'random:{name}'}
    elif name == 'dec_1': labels[name] = {'public:dec_1'}
    else: raise ValueError(f'Unclassified input {name}')
dependencies = {}
for name,expr in expressions.items():
    # No procedural control or arithmetic is accepted by this deliberately small parser.
    if re.search(r'[^\w\s~^&|()\d\x27]', expr): raise ValueError(f'Unsupported expression {expr}')
    expr = re.sub(r"\d+'[bB][01]+", '', expr)
    dependencies[name] = set(re.findall(r'\b[a-zA-Z_]\w*\b', expr))
while len(labels) < len(inputs)-1 + len(expressions):
    before = len(labels)
    for name,deps in dependencies.items():
        if name not in labels and deps <= labels.keys():
            labels[name] = set().union(*(labels[x] for x in deps))
    if len(labels) == before: raise ValueError(f'Unresolved dependencies {set(expressions)-labels.keys()}')

connections = ['.clk(clk)', '.dec_1(1\'b0)']
for bit in range(4):
    connections += [f'.x{bit}_0(x0[4*lane+{bit}])',f'.x{bit}_1(x1[4*lane+{bit}])',
                    f'.y{bit}_0(y0[4*lane+{bit}])',f'.y{bit}_1(y1[4*lane+{bit}])']
connections += [f'.rand_{i}(randomness[23*lane+{i-1}])' for i in range(1,24)]
wrapper = '''module masked_present_layer(input clk, input [63:0] x0,x1,
  input [367:0] randomness, output [63:0] y0,y1);
genvar lane;
generate for(lane=0;lane<16;lane=lane+1) begin: lanes
  sbox u('''+',\n    '.join(connections)+''');
end endgenerate
endmodule
'''
(here/'masked_present_layer.v').write_text(wrapper)
(here/'rtl_provenance.json').write_text(json.dumps({
    'status':'CONSERVATIVE_RTL_DEPENDENCIES_NOT_MASKING_PROOF',
    'source_sha256':hashlib.sha256(rtl.encode()).hexdigest(),
    'input_labels':{n:sorted(labels[n]) for n in inputs if n!='clk'},
    'register_labels':{n:sorted(labels[n]) for n in sorted(sequential)},
    'all_signal_labels':{n:sorted(v) for n,v in sorted(labels.items())},
    'register_count_per_lane':len(sequential),
},indent=2)+'\n')
print('LAYER_PREPARED registers_per_lane=',len(sequential))
