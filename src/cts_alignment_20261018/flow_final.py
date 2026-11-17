"""Reproducible data-aware construction experiment; all writes stay in this folder."""
from pathlib import Path
from collections import defaultdict
import argparse, csv, hashlib, json, os, subprocess, time
import numpy as np
from scipy.optimize import linprog
from scipy.sparse import coo_matrix, vstack, hstack

D = Path(__file__).resolve().parent
K = D.parent
BIN = Path(os.environ.get('ALIGNMENT_BINARY', str(D / 'build11/openroad')))
STOCK = K / 'cts_larger_20261011/baseline'

def rows(path):
    return list(csv.DictReader(Path(path).open()))

def run_tcl(out, body, binary=None):
    out.mkdir(exist_ok=False, parents=True)
    script = f'set K {{{K}}}\nsource {{{D / "common_final.tcl"}}}\n' + body
    (out / 'run.tcl').write_text(script)
    started = time.monotonic()
    with (out / 'run.log').open('w') as log:
        result = subprocess.run([str(binary or BIN), '-no_init', '-no_splash', '-exit', str(out / 'run.tcl')], stdout=log, stderr=subprocess.STDOUT)
    text = (out / 'run.log').read_text()
    status = {'exit_code': result.returncode, 'seconds': time.monotonic() - started, 'exported': 'ALIGNMENT_EXPORT_PASS' in text}
    (out / 'execution.json').write_text(json.dumps(status, indent=2))
    if result.returncode or not status['exported']:
        raise RuntimeError(f'OpenROAD failed: {out}/run.log')
    return status

def load(db=STOCK):
    return f'load_design {{{db / "design.odb"}}} {{{db / "design.sdc"}}}\nsource {{{K / "cts_larger_20261011/native_sizing_inputs.tcl"}}}\n'

def export(out, db=STOCK):
    return run_tcl(out, load(db) + f'export_state {{{out}}}\n')

def construct(out, targets=None):
    body = load() + 'set block [ord::get_db_block]\nintprop $block cts_data_skew_native_sizing 0\nset state [strip_clock]\n'
    if targets:
        body += f'source {{{targets}}}\n'
    cts = ('-root_buf BUF_X32 -buf_list {BUF_X2 BUF_X4 BUF_X8 BUF_X16 BUF_X32} -sink_clustering_buffer BUF_X4 -dont_use_dummy_load' if targets else '-root_buf BUF_X4 -buf_list {BUF_X2 BUF_X4}')
    body += 'clock_tree_synthesis '+cts+' -sink_clustering_enable -sink_clustering_size 20 -sink_clustering_max_diameter 100\ndetailed_placement\nset_propagated_clock [all_clocks]\nestimate_parasitics -placement\n'
    if targets: body += 'construct_cts_data_alignment -buf_list {BUF_X2 BUF_X4 BUF_X8 BUF_X16}\n'
    body += 'check_data $state\n'
    body += f'export_state {{{out}}}\n'
    return run_tcl(out, body)

def build_targets(anchor, original, out, bound_ps, regression_ps, round_id):
    """Weighted-first LP over fresh, source-preserving windows. Timing rows are
    source/endpoint checks from the preserved complete pair list, remeasured
    separately for each anchor. They are a screen, not certified sensitivities.
    """
    clocks = {r['pin']: r for r in rows(anchor / 'model_clocks.csv')}
    stockclocks = {r['pin']: r for r in rows(original / 'model_clocks.csv')}
    pins = sorted(clocks)
    regs = {p.rsplit('/', 1)[0]: i for i, p in enumerate(pins)}
    idx = {p: i for i, p in enumerate(pins)}
    source_rows = rows(anchor / 'model_sources.csv')
    gates = sorted({r['gate'] for r in source_rows})
    gi = {g: i for i, g in enumerate(gates)}
    stock_widths = {r['gate']: float(r['width_ns']) for r in rows(original / 'gates.csv')}
    planning_regression_ps = max(.01, regression_ps*.2)
    max_stages=6+int(bound_ps/20)
    clock_headroom_ps = 10
    target_rows = {r['pin']:r for r in rows(anchor/'targets.csv')} if round_id else {}
    profiles = defaultdict(dict)
    weights = {}
    for r in source_rows:
        source = r['source'].rsplit('/', 1)[0]
        s = regs.get(source, -1)
        g = r['gate']
        lo, hi = float(r['early_ns']), float(r['late_ns'])
        old = profiles[g].get(s, (np.inf, -np.inf))
        profiles[g][s] = (min(old[0], lo), max(old[1], hi))
        weights[g] = float(r['weight'])
    ceilings={}
    for g,by in profiles.items():
        classes={}
        for s,(lo,hi) in by.items():
            key=('fixed',0) if s<0 else (('group',target_rows[pins[s]]['group']) if round_id else ('register',s))
            previous=classes.get(key,(float('inf'),-float('inf')))
            classes[key]=(min(previous[0],lo),max(previous[1],hi))
        floor=max(hi-lo for lo,hi in classes.values())
        ceilings[g]=min(stock_widths[g]+max(regression_ps,.01)/1000,
                        max(stock_widths[g]+planning_regression_ps/1000,floor+1e-5))
    nc = len(pins)
    n = nc + 2 * len(gates) + 1
    worst = n - 1
    rr, cc, vv, rhs = [], [], [], []
    def add(coeff, limit):
        row = len(rhs)
        for c, v in coeff.items():
            if v:
                rr.append(row); cc.append(c); vv.append(v)
        rhs.append(limit)
    cost = np.zeros(n)
    for g in gates:
        lower, upper = nc + 2 * gi[g], nc + 2 * gi[g] + 1
        for s, (lo, hi) in profiles[g].items():
            a, b = {lower: 1}, {upper: -1}
            if s >= 0:
                a[s] = -1; b[s] = 1
            add(a, lo); add(b, -hi)
        add({upper: 1, lower: -1}, ceilings[g])
        add({upper: 1, lower: -1, worst: -1}, 0)
        cost[upper], cost[lower] = weights[g], -weights[g]
    timing_file = anchor / 'timing.csv'
    if not timing_file.exists():
        raise RuntimeError('Fresh complete setup/hold planning constraints required')
    if round_id:
        if set(target_rows)!=set(pins): raise RuntimeError('Incomplete group closure anchor')
        bygroup=defaultdict(list)
        for pin,row in target_rows.items(): bygroup[row['group']].append(pin)
        for members in bygroup.values():
            a=members[0];ca=clocks[a]
            ra=(float(ca['rise_min_ns'])+float(ca['rise_max_ns']))/2-float(ca['local_ns'])
            for b in members[1:]:
                cb=clocks[b];rb=(float(cb['rise_min_ns'])+float(cb['rise_max_ns']))/2-float(cb['local_ns'])
                add({idx[a]:1,idx[b]:-1},rb-ra)
                add({idx[a]:-1,idx[b]:1},ra-rb)
    timing_start = len(rhs)
    for r in rows(timing_file):
        a = defaultdict(float)
        sign = 1 if r['mode'] == 'max' else -1
        if r['source'] in regs: a[regs[r['source']]] += sign
        if r['target'] in regs: a[regs[r['target']]] -= sign
        add(a, float(r['slack_ns']) - .02)
    bounds = []
    for p in pins:
        low = max(float(stockclocks[p][key]) - (bound_ps-clock_headroom_ps) / 1000 - float(clocks[p][key]) for key in ['rise_min_ns', 'rise_max_ns', 'fall_min_ns', 'fall_max_ns'])
        high = min(float(stockclocks[p][key]) + (bound_ps-clock_headroom_ps) / 1000 - float(clocks[p][key]) for key in ['rise_min_ns', 'rise_max_ns', 'fall_min_ns', 'fall_max_ns'])
        if round_id and int(target_rows[p].get('stages',-1))==0:
            # With fixed groups and a matched branch-input load, a zero-stage
            # branch already uses the fastest available original leaf. Guide
            # closure toward attainable delays rather than requesting another
            # impossible advance. This is a local-construction search bound.
            low=max(low,0.0)
        if round_id and int(target_rows[p].get('stages',-1))>=max_stages:
            midpoint=(float(clocks[p]['rise_min_ns'])+float(clocks[p]['rise_max_ns']))/2
            if float(target_rows[p]['target_ns'])>midpoint+.005:
                high=min(high,0.0)  # Observed saturation of the bounded branch menu.
        if low > high:
            return {'status': 'CLOCK_WINDOW_INFEASIBLE'}
        bounds.append((low, high))
    bounds += [(None, None)] * (n - nc - 1) + [(0, None)]
    A = coo_matrix((vv, (rr, cc)), shape=(len(rhs), n)).tocsr()
    start = time.monotonic()
    # x and all arrivals are ns. Scaling rows keeps picosecond caps meaningful.
    result = linprog(cost, A_ub=A * 1000, b_ub=np.array(rhs) * 1000, bounds=bounds, method='highs', options={'primal_feasibility_tolerance': 1e-9, 'dual_feasibility_tolerance': 1e-9})
    planning_margin_ps = 20
    if not result.success:
        # The extra construction headroom is a preference, never a new hard
        # requirement that can falsely rule out the requested 10 ps problem.
        rhs[timing_start:] = [v+.01 for v in rhs[timing_start:]]
        planning_margin_ps = 10
        result = linprog(cost,A_ub=A*1000,b_ub=np.array(rhs)*1000,bounds=bounds,method='highs',options={'primal_feasibility_tolerance':1e-9,'dual_feasibility_tolerance':1e-9})
    if not result.success:
        report = {'status': 'PLANNING_INFEASIBLE', 'message': result.message}
        (out / 'plan.json').write_text(json.dumps(report, indent=2))
        return report
    # Worst raw gate spread is secondary, never the legacy floor-first objective.
    second_cost = np.zeros(n); second_cost[worst] = 1
    second_A = vstack([A, coo_matrix(cost.reshape(1,-1))]).tocsr()
    second_b = np.r_[rhs,result.fun+1e-6]
    second = linprog(second_cost,A_ub=second_A*1000,b_ub=second_b*1000,bounds=bounds,method='highs',options={'primal_feasibility_tolerance':1e-9})
    if not second.success: raise RuntimeError('Worst-spread tie-break failed')
    # Finally resolve remaining freedom by minimum movement.
    A2 = hstack([A, coo_matrix((A.shape[0], nc))]).tocsr()
    absrows = []
    for i in range(nc):
        absrows.extend([(2*i, i, 1), (2*i, n+i, -1), (2*i+1, i, -1), (2*i+1, n+i, -1)])
    aa = coo_matrix(([v for _, _, v in absrows], ([r for r, _, _ in absrows], [c for _, c, _ in absrows])), shape=(2*nc, n+nc)).tocsr()
    A2 = vstack([A2, coo_matrix(np.r_[cost, np.zeros(nc)].reshape(1, -1)), coo_matrix(np.r_[second_cost,np.zeros(nc)].reshape(1,-1)), aa]).tocsr()
    b2 = np.r_[rhs, result.fun + 1e-6, second.fun+1e-6, np.zeros(2*nc)]
    refinement = linprog(np.r_[np.zeros(n), np.ones(nc)], A_ub=A2*1000, b_ub=b2*1000, bounds=bounds+[(0, None)]*nc, method='highs', options={'primal_feasibility_tolerance': 1e-9})
    x = refinement.x if refinement.success else second.x
    # Audit the actual returned vector, not just solver status.
    assert np.max(A @ x[:n] - rhs) <= 1e-7
    targets = {}
    for p in pins:
        c = clocks[p]
        targets[p] = (float(c['rise_min_ns']) + float(c['rise_max_ns']))/2 + x[idx[p]]
    text = ['set block [ord::get_db_block]', 'intprop $block cts_data_alignment 1', 'intprop $block cts_align_defer 1', 'intprop $block cts_data_skew_native_sizing 0', f'intprop $block cts_align_gate_count {len(gates)}', f'doubleprop $block cts_align_movement_ns {bound_ps/1000}', f'doubleprop $block cts_align_regression_ns {max(regression_ps,.01)/1000}']
    bypin, fixed = defaultdict(list), []
    for g in gates:
        for s, (lo, hi) in profiles[g].items():
            if s < 0:
                fixed.extend([gi[g], lo, hi, weights[g]])
            else:
                p = pins[s]
                midpoint = (float(clocks[p]['rise_min_ns']) + float(clocks[p]['rise_max_ns']))/2
                bypin[p].extend([gi[g], lo-midpoint, hi-midpoint, weights[g]])
    previous_targets = {r['pin']:float(r['target_ns']) for r in rows(anchor/'targets.csv')} if round_id else {}
    for p in pins:
        mid = (float(clocks[p]['rise_min_ns'])+float(clocks[p]['rise_max_ns']))/2
        calibration = np.clip(previous_targets.get(p,mid)-mid,-.020,.020)*.5
        if round_id and int(target_rows[p].get('stages',-1))==0:
            calibration=0.0  # A reachability floor is not a delay-model bias.
        if round_id and int(target_rows[p].get('stages',-1))>=max_stages and previous_targets[p]>mid+.005:
            calibration=0.0
        text += [f'set pin [$block findITerm {{{p}}}]', f'doubleprop $pin cts_align_target_ns {targets[p]:.17g}', f'doubleprop $pin cts_align_build_ns {targets[p]+calibration:.17g}', f'doubleprop $pin cts_align_lower_ns {mid+bounds[idx[p]][0]:.17g}', f'doubleprop $pin cts_align_upper_ns {mid+bounds[idx[p]][1]:.17g}', f'doubleprop $pin cts_align_reference_ns {mid:.17g}', f'doubleprop $pin cts_align_stock_fall_ns {(float(stockclocks[p]["fall_min_ns"])+float(stockclocks[p]["fall_max_ns"]))/2:.17g}', f'doubleprop $pin cts_align_local_ns {float(clocks[p]["local_ns"]):.17g}', 'stringprop $pin cts_align_profile {' + ' '.join(map(str, bypin[p])) + '}']
        if round_id: text.append(f'intprop $pin cts_align_prescribed {int(target_rows[p]["group"])}')
    text.append('stringprop $block cts_align_fixed {' + ' '.join(map(str, fixed)) + '}')
    text.append('stringprop $block cts_align_gate_caps {'+' '.join(f'{gi[g]} {ceilings[g]:.17g}' for g in gates)+'}')
    stock_cost=sum(weights[g]*stock_widths[g] for g in gates)
    text.append(f'doubleprop $block cts_align_merge_penalty_ns {max(0,stock_cost-float(cost@x[:n]))*.1:.17g}')
    encoded_checks=[]
    for row in rows(timing_file):
        launch=pins[regs[row['source']]] if row['source'] in regs else 'FIXED'
        capture=pins[regs[row['target']]] if row['target'] in regs else 'FIXED'
        encoded_checks.extend([launch,capture,1 if row['mode']=='max' else -1,float(row['slack_ns'])-planning_margin_ps/1000])
    text.append('stringprop $block cts_align_timing_checks {'+' '.join(map(str,encoded_checks))+'}')
    text.append('intprop $block cts_align_stable_slew 1')
    text.append('intprop $block cts_align_fast_backbone 1')
    text.append('intprop $block cts_align_backbone_fanout 4')
    text.append(f'intprop $block cts_align_max_stages {max_stages}')
    (out / 'targets.tcl').write_text('\n'.join(text)+'\n')
    (out / 'targets.json').write_text(json.dumps(targets))
    report = {'status': 'MODEL_TARGETS_ONLY', 'round': round_id, 'bound_ps': bound_ps, 'regression_budget_ps': regression_ps, 'predicted_weighted_ns': float(cost @ x[:n]), 'registers': nc, 'gates': len(gates), 'seconds': time.monotonic()-start, 'objective': 'weighted_total_then_worst_spread_then_movement', 'planning_slack_margin_ps':planning_margin_ps,'actual_slack_margin_ps':10,'planning_regression_ps':planning_regression_ps,'clock_headroom_ps':clock_headroom_ps,'merge_loss_fraction':.1,'preserved_groups':bool(round_id),'fast_backbone':True,'max_branch_stages':max_stages,'adaptive_source_floors':True,'physical_result': False}
    (out / 'plan.json').write_text(json.dumps(report, indent=2))
    return report

def timing(anchor):
    """Requery every preserved source/endpoint pair, with numeric path slack."""
    script = load(anchor) + 'set fp [open {' + str(anchor/'timing.csv') + '} w]\nputs $fp "source,target,mode,slack_ns"\n'
    for r in rows(K / 'cts_larger_20261011/baseline_graph/timing_constraints.csv'):
        script += 'set paths [find_timing_paths -from [sta::get_port_pin_error source {'+r['source_pin']+'}] -to [sta::get_port_pin_error target {'+r['target_pin']+'}] -path_delay '+r['mode']+' -group_path_count 1]\n'
        script += 'if {[llength $paths] != 1} {error "Missing timing pair"}\n'
        script += 'puts $fp "'+r['source']+','+r['target']+','+r['mode']+',[expr {[[lindex $paths 0] slack]/1e-9}]"\n'
    script += 'close $fp\nputs ALIGNMENT_EXPORT_PASS\n'
    return run_tcl(anchor/'timing_query', script)

def compare(out, stock, bound, budget):
    base = {r['gate']: float(r['width_ns']) for r in rows(stock/'gates.csv')}
    final = {r['gate']: float(r['width_ns']) for r in rows(out/'gates.csv')}
    changes = {g: 1000*(final[g]-v) for g, v in base.items()}
    b = rows(stock/'metrics.csv')[0]; m = rows(out/'metrics.csv')[0]
    oldclock = {r['pin']: r for r in rows(stock/'model_clocks.csv')}
    newclock = {r['pin']: r for r in rows(out/'model_clocks.csv')}
    shift = max(abs(float(r[key])-float(oldclock[p][key]))*1000 for p, r in newclock.items() for key in ['rise_min_ns','rise_max_ns','fall_min_ns','fall_max_ns'])
    target = {r['pin']: float(r['target_ns']) for r in rows(out/'targets.csv')}
    errors = [1000*((float(newclock[p]['rise_min_ns'])+float(newclock[p]['rise_max_ns']))/2-v) for p,v in target.items()]
    report = {'weighted_reduction_percent': 100*(float(b['cost_ns'])-float(m['cost_ns']))/float(b['cost_ns']), 'improved': sum(v<-.01 for v in changes.values()), 'regressed': sum(v>.01 for v in changes.values()), 'improved_at_least_1ps': sum(v<=-1 for v in changes.values()), 'max_regression_ps': max(changes.values()), 'max_clock_movement_ps': shift, 'worst_gate_spread_ps': max(final.values())*1000, 'spread_percentiles_ps': {str(p): float(np.percentile(list(final.values()),p)*1000) for p in [50,90,95,99]}, 'target_rms_error_ps': float(np.sqrt(np.mean(np.square(errors)))) if errors else None, 'target_max_error_ps': max(map(abs,errors)) if errors else None, 'groups': len({r['group'] for r in rows(out/'targets.csv')}), 'metrics': m, 'bound_ps': bound, 'regression_budget_ps': budget, 'area_ratio': float(m['area_um2'])/float(b['area_um2']), 'clock_area_ratio': float(m['clock_area_um2'])/float(b['clock_area_um2'])}
    report['legal'] = (float(m['setup_ns'])>=.01 and float(m['hold_ns'])>=.01 and int(m['electrical_pass'])==1 and shift<=bound+1e-6 and max(changes.values())<=max(budget,.01)+1e-6)
    report['accepted_improvement'] = report['legal'] and report['weighted_reduction_percent']>1e-7
    if (out/'overhead.csv').exists(): report['overhead']=rows(out/'overhead.csv')[0]
    with (out/'gate_comparison.csv').open('w') as f:
        w = csv.writer(f); w.writerow(['gate','stock_ns','final_ns','worsening_ps'])
        w.writerows((g,base[g],final[g],changes[g]) for g in base)
    (out/'result.json').write_text(json.dumps(report,indent=2))
    return report

def main():
    p = argparse.ArgumentParser(); p.add_argument('phase', choices=['stock','control','pilot','matrix']); p.add_argument('--root', type=Path, default=D/'runs_final_constructor'); a=p.parse_args()
    a.root=a.root.resolve()
    if K.resolve() not in a.root.parents: raise RuntimeError('Outputs must remain under Kairav')
    a.root.mkdir(exist_ok=True)
    if a.phase=='stock':
        export(a.root/'stock'); timing(a.root/'stock')
    elif a.phase=='control':
        construct(a.root/'control'); print(json.dumps(compare(a.root/'control',a.root/'stock',40,0),indent=2))
    else:
        cases=[(80,5)] if a.phase=='pilot' else [(b,g) for b in [40,80,160] for g in [0,1,5]]
        summary={}
        for bound,budget in cases:
            case=a.root/f'{"pilot_" if a.phase=="pilot" else ""}b{bound}_g{budget}'; case.mkdir(exist_ok=False)
            anchor=a.root/'stock'; best=None; attempts=[]
            for round_id in range(3):
                planning=case/f'plan{round_id}'; planning.mkdir()
                plan=build_targets(anchor,a.root/'stock',planning,bound,budget,round_id)
                if plan['status']!='MODEL_TARGETS_ONLY': attempts.append(plan); break
                out=case/f'round{round_id}'
                try:
                    construct(out,planning/'targets.tcl')
                    result=compare(out,a.root/'stock',bound,budget)
                    attempts.append({'folder':str(out),**result})
                    print(bound,budget,round_id,json.dumps(result),flush=True)
                    quality=lambda r:(-round(r['weighted_reduction_percent'],9),r['worst_gate_spread_ps'],float(r['metrics']['area_um2']))
                    if result['accepted_improvement'] and (best is None or quality(result)<quality(best)): best={'folder':str(out),**result}
                    if round_id<2:
                        timing(out); anchor=out
                except Exception as e:
                    attempts.append({'status':'FAILED','error':str(e),'folder':str(out)})
                    break
            summary[case.name]={'best':best,'fallback':str(a.root/'stock') if best is None else None,'attempts':attempts}
            (case/'summary.json').write_text(json.dumps(summary[case.name],indent=2))
            (a.root/(a.phase+'_summary.json')).write_text(json.dumps(summary,indent=2))
    print('ALIGNMENT_PHASE_COMPLETE',a.phase,flush=True)

if __name__=='__main__': main()
