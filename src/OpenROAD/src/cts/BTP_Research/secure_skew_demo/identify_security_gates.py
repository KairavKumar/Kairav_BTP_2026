"""Conservative structural risk map from explicitly annotated STA launch pins.

Annotations describe functional dependency, not statistical independence or
proven leakage. Sequential boundaries require new annotations; no name guesses.
"""
import argparse
import copy
import json
import math
from collections import defaultdict
from pathlib import Path

from clock_graph import owned


def identify(graph, annotations):
    sources = {r['source_pin'] for r in graph['arrivals']}
    labels = annotations['sources']
    missing = sources - labels.keys()
    if missing:
        raise ValueError('Unannotated meeting-cone launch pins: ' + ', '.join(sorted(missing)))
    for pin, label in labels.items():
        if label.get('kind') not in ('secret', 'random', 'public'):
            raise ValueError('Invalid source kind: ' + pin)
        if label['kind'] == 'secret':
            if not label.get('dependencies'):
                raise ValueError('Secret source needs explicit dependencies: ' + pin)
            for dep in label['dependencies']:
                if not isinstance(dep.get('secret'), str) or not dep['secret']:
                    raise ValueError('Invalid secret identity: ' + pin)
                if not isinstance(dep.get('share'), str) or not dep['share']:
                    raise ValueError('Invalid share identity: ' + pin)
        elif label.get('dependencies'):
            raise ValueError('Nonsecret source has secret dependencies: ' + pin)
    cones = defaultdict(lambda: defaultdict(set))
    for row in graph['arrivals']:
        cones[row['gate']][row['pin']].add(row['source_pin'])
    gates = []
    for gate, inputs in sorted(cones.items()):
        deps = {}
        for pin, launches in inputs.items():
            deps[pin] = {(d['secret'], d['share']) for s in launches
                         for d in labels[s].get('dependencies', [])}
        secret_pins = sorted(p for p in inputs if deps[p])
        cross = []
        for i, p in enumerate(sorted(inputs)):
            for q in sorted(inputs)[i+1:]:
                evidence = sorted({(s, a, b) for s, a in deps[p]
                                   for t, b in deps[q] if s == t and a != b})
                if evidence:
                    cross.append(dict(pins=[p, q], dependencies=[list(e) for e in evidence]))
        mixed = sorted(p for p in inputs if any(
            len({share for sec, share in deps[p] if sec == secret}) > 1
            for secret, _ in deps[p]))
        # Priority is a transparent experiment policy, not a leakage score.
        category = ('cross_share_meeting' if cross else
                    'secret_secret_meeting' if len(secret_pins) >= 2 else
                    'secret_other_meeting' if secret_pins else 'no_annotated_secret')
        weight = {'cross_share_meeting': 8., 'secret_secret_meeting': 4.,
                  'secret_other_meeting': 2., 'no_annotated_secret': 1.}[category]
        gates.append(dict(gate=gate, category=category, weight=weight,
            secret_input_pins=secret_pins, upstream_mixed_share_pins=mixed,
            cross_share_evidence=cross,
            input_cones={p: dict(source_pins=sorted(inputs[p]),
                dependencies=[dict(secret=s, share=h) for s, h in sorted(deps[p])])
                for p in sorted(inputs)}))
    return dict(status='STRUCTURAL_POTENTIAL_LEAKAGE_MAP', gates=gates,
        coverage=dict(meeting_gates=len(gates), annotated_launch_pins=len(sources),
            unused_annotations=sorted(labels.keys()-sources)),
        limitations='Timing-cone overapproximation; no Boolean cancellation, sensitization, nonlinear-cell classification, statistical independence, or leakage proof. No inference across registers. Random inputs do not erase dependencies.')


def prioritize(model, report):
    result = copy.deepcopy(model)
    expected = {r['gate'] for r in model['arrivals']}
    actual = {r['gate'] for r in report['gates']}
    if expected != actual or len(actual) != len(report['gates']):
        raise ValueError('Security map/model meeting coverage mismatch')
    result['weights'] = {r['gate']: r['weight'] for r in report['gates']}
    result['security_classification'] = report
    # Leave existing explicit tau values intact. No invented inertial threshold.
    return result


def apply_characterized_ranges(model, report, calibration):
    """Require conservative spread limits at every annotated susceptible gate.

    Bounds must be supplied from characterization, never guessed from a cell's
    propagation delay. Metadata is recorded, not a certificate of its validity.
    """
    required = {r['gate'] for r in report['gates'] if r['category'] != 'no_annotated_secret'}
    ranges = calibration['gates']
    if set(ranges) != required:
        raise ValueError('Characterized range coverage must match all susceptible gates')
    result = copy.deepcopy(model)
    bounds = {}
    for gate, record in ranges.items():
        value = record['max_spread_ns']
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError('Invalid characterized range: '+gate)
        if not record.get('evidence') or not record.get('conditions'):
            raise ValueError('Characterized range needs evidence and conditions: '+gate)
        bounds[gate] = value
    result['required_skew_bounds_ns'] = bounds
    result.setdefault('tolerances_ns', {}).update(bounds)
    result['range_characterization'] = copy.deepcopy(calibration)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--graph', type=Path, required=True)
    parser.add_argument('--annotations', type=Path, required=True)
    parser.add_argument('--model', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--characterized-ranges', type=Path)
    args = parser.parse_args()
    out = owned(args.output_directory)
    out.mkdir(parents=True, exist_ok=False)
    report = identify(json.loads(args.graph.read_text()), json.loads(args.annotations.read_text()))
    model = prioritize(json.loads(args.model.read_text()), report)
    if args.characterized_ranges:
        model = apply_characterized_ranges(model, report,
            json.loads(args.characterized_ranges.read_text()))
    for name, data in [('security_gates.json', report), ('prioritized_model.json', model)]:
        (out/name).write_text(json.dumps(data, indent=2)+'\n')
    print(json.dumps(report['coverage'], indent=2))


if __name__ == '__main__':
    main()
