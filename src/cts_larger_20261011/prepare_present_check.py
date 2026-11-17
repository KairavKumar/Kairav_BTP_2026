"""Copy the owned TETRIS PRESENT RTL and generate independent functional checks."""
from pathlib import Path
import shutil

here = Path(__file__).resolve().parent
src = here.parent / 'OpenROAD/src/cts/BTP_Research/TETRIS-artifact/src/Testing/PRESENT/Share_1/sbox.v'
shutil.copyfile(src, here / 'present_hls.v')
connections = ['.clk(clk)', '.dec_1(1\'b0)']
for bit in range(4):
    connections += [f'.x{bit}_0(a[{bit}])', f'.x{bit}_1(b[{bit}])',
                    f'.y{bit}_0(y0[{bit}])', f'.y{bit}_1(y1[{bit}])']
connections += [f'.rand_{i}(r[{i-1}])' for i in range(1,24)]
tb = (here/'check_comar.sv').read_text()
start = tb.index('  sbox dut(')
end = tb.index('  function',start)
tb = tb[:start] + '  sbox dut('+',\n    '.join(connections)+');\n' + tb[end:]
tb = tb.replace('module check_comar;', 'module check_hls;').replace('reg [5:0] r=0;', 'reg [22:0] r=0;')
tb = tb.replace('r=q;', "r=(q*23'h32ad73)^23'h52c319;")
tb = tb.replace('repeat(12)', 'repeat(40)').replace('COMAR stable truth failure','HLS stable truth failure')
(here/'check_hls.sv').write_text(tb)
