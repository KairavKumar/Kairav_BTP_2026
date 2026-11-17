"""PRESENT/TETRIS total-power experiment; new directories, immutable references."""
import hashlib,json,os
from pathlib import Path
D=Path(__file__).resolve().parent
os.environ['COST_CTS_BINARY']=str(D/'build7/openroad')
from experiment import construct,compare,flow,STOCK
ROOT=D/'power_campaign_20261024'

def export(source,out):
    body=flow.load(source)+f'''
estimate_parasitics -placement -spef_file {{{out/'design.spef'}}}
write_sdf -include_typ -digits 9 {{{out/'design.sdf'}}}
export_state {{{out}}}
'''
    flow.run_tcl(out,body)
    (out/'design.pt.sdc').write_text('\n'.join(x for x in (out/'design.sdc').read_text().splitlines() if not x.startswith('current_design '))+'\n')
    (out/'provenance.json').write_text(json.dumps({'source':str(source),'binary':str(flow.BIN),'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.iterdir() if p.suffix in ('.v','.sdc','.sdf','.spef')}},indent=2))

def resize(out,master,root_master=None):
    # Preserve physical/data identity; only clock buffers directly driving CK.
    body=flow.load(STOCK)+r'''
set block [ord::get_db_block]
set snapshots {}
set statuses {}
set drivers {}
foreach p [all_registers -clock_pins] {
 set term [$block findITerm [get_full_name $p]]
 set net [$term getNet]
 foreach t [$net getITerms] {if {[$t isOutputSignal]} {dict set drivers [[$t getInst] getName] 1}}
}
ROOT_DRIVER
foreach inst [$block getInsts] {
 if {[dict exists $drivers [$inst getName]]} {continue}
 dict set snapshots [$inst getName] [list [$inst getLocation] [$inst getOrient] [[$inst getMaster] getName]]
 dict set statuses [$inst getName] [$inst getPlacementStatus]
 $inst setPlacementStatus FIRM
}
set changed 0
dict for {name unused} $drivers {
 set inst [$block findInst $name]
 if {![regexp {^BUF_X} [[$inst getMaster] getName]]} {error "Unexpected leaf driver"}
 replace_cell $name MASTER
 incr changed
}
ROOT_REPLACE
detailed_placement
dict for {name row} $snapshots {
 set inst [$block findInst $name]
 if {[list [$inst getLocation] [$inst getOrient] [[$inst getMaster] getName]] ne $row} {error "Frozen instance moved"}
 $inst setPlacementStatus [dict get $statuses $name]
}
puts "POWER_LEAF_REPLACEMENTS $changed"
set_propagated_clock [all_clocks]
estimate_parasitics -placement
'''.replace('ROOT_DRIVER','dict set drivers clkbuf_0_clk 1' if root_master else '').replace('ROOT_REPLACE','replace_cell clkbuf_0_clk '+root_master if root_master else '').replace('MASTER',master)+f'export_state {{{out}}}\n'
    flow.run_tcl(out,body)

if __name__=='__main__':
    ROOT.mkdir(exist_ok=False)
    results=[]
    specs=[('shared340_x4','tree','BUF_X4'),('shared340_x8','tree','BUF_X8'),('stock_leaf_x8','resize','BUF_X8'),('stock_leaf_x2','resize','BUF_X2')]
    for name,kind,master in specs:
        out=ROOT/name
        try:
            if kind=='tree':construct(out,D/'plan_group_stock_gain5/targets.tcl',energy=20,leaf=32,span=.020,diameter=100,leaf_master=master)
            else:resize(out,master)
            report=compare(out)
            export(out,ROOT/(name+'_export'))
            results.append({'name':name,'kind':kind,'master':master,'status':'EXPORTED','result':report})
            print(name,json.dumps({k:report[k] for k in ('weighted_reduction_percent','legal','area_ratio','max_clock_movement_ps')}),flush=True)
        except Exception as error:
            results.append({'name':name,'status':'FAILED','error':str(error)})
            print(name,'FAILED',str(error),flush=True)
        (ROOT/'summary.json').write_text(json.dumps(results,indent=2))
