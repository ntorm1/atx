"""Reconstruct and report Greek diagnostics; no derivative acceptance budgets."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
FIELDS = ['delta', 'gamma', 'vega', 'theta', 'rho', 'phi', 'vanna', 'volga', 'charm']
ENGINES = ['cold_fd', 'cold_al', 'sigma_family', 'sigma_q_family']
CORPUS_SHA = '5c327e9c34737bde3f7c31165f195f144cc32cbac0ce3dd8165d7a6c7e68d5c8'
PROFILES = {
    'refined24': dict(n_collocation=24, n_quadrature=48, n_quad_price=64,
                      max_newton_iter=24, tol=1e-13, profile='Reference'),
    'refined32': dict(n_collocation=32, n_quadrature=64, n_quad_price=64,
                      max_newton_iter=32, tol=1e-14, profile='Reference'),
}
DEFAULT_OPTIONS = dict(n_collocation=12, n_quadrature=24, n_quad_price=48,
                       max_newton_iter=6, tol=1e-10, profile='Reference', explicit=False)


def finite(x):
    return type(x) in (float, int) and math.isfinite(x)


def require(ok, message):
    if not ok:
        raise ValueError(message)


def offsets():
    out = [[0] * 5]
    for axis in range(5):
        for sign in [1, -1]:
            d = [0] * 5
            d[axis] = sign
            out.append(d)
    for axis in [1, 2]:
        for a, b in [(1, 1), (1, -1), (-1, 1), (-1, -1)]:
            d = [0] * 5
            d[0], d[axis] = a, b
            out.append(d)
    return out


def derivatives(stencil):
    h, samples = stencil['h'], stencil['samples']
    require(len(h) == 5 and all(finite(x) and x > 0 for x in h), 'step size')
    require(len(samples) == 19, 'stencil denominator')
    for sample, offset in zip(samples, offsets()):
        require(sample['offset'] == offset and all(type(v) is int for v in sample['offset']), 'stencil identity')
        require(sample['status'] in ['finite', 'refused', 'NoModelAxis', 'nonfinite_success'], 'sample status')
        if sample['status'] == 'finite':
            require(finite(sample['price']) and type(sample['intrinsic_band']) is bool, 'finite sample')
        else:
            require(sample['price'] is None and sample['intrinsic_band'] is None, 'refused sample')
    formulas = {
        'delta': ([1, 2], [1, -1], 2*h[0]),
        'gamma': ([1, 0, 2], [1, -2, 1], h[0]**2),
        'vega': ([3, 4], [1, -1], 2*h[1]),
        'theta': ([5, 6], [-1, 1], 2*h[2]),
        'rho': ([7, 8], [1, -1], 2*h[3]),
        'phi': ([9, 10], [1, -1], 2*h[4]),
        'vanna': ([11, 12, 13, 14], [1, -1, -1, 1], 4*h[0]*h[1]),
        'volga': ([3, 0, 4], [1, -2, 1], h[1]**2),
        'charm': ([15, 16, 17, 18], [-1, 1, 1, -1], 4*h[0]*h[2]),
    }
    result = {}
    for field, (ids, weights, divisor) in formulas.items():
        selected = [samples[i] for i in ids]
        valid = all(x['status'] == 'finite' for x in selected)
        value = math.fsum(w*s['price'] for w, s in zip(weights, selected))/divisor if valid else None
        require(value is None or finite(value), 'nonfinite derivative reconstruction')
        # The center is regime evidence even when it is not a formula operand.
        # A refused center leaves a computable first difference intact, but does
        # not establish that its stencil stays in one exercise regime.
        evidence = [samples[i] for i in sorted(set(ids) | {0})]
        evidence_complete = all(s['status'] == 'finite' for s in evidence)
        result[field] = dict(value=value, sample_statuses=sorted({s['status'] for s in selected}),
                             intrinsic_evidence_complete=evidence_complete,
                             intrinsic_band_changed=len({s['intrinsic_band'] for s in evidence
                                                         if s['status'] == 'finite'}) > 1)
    return result


def validate_levels(levels, case):
    require(len(levels) == 3, 'three step levels required')
    base = [min(.001*case['S'], .02*case['S']*case['sigma']*math.sqrt(case['T'])),
            .001*max(case['sigma'], .1), .002*case['T'], .0001, .0001]
    for level, scale in zip(levels, [1., .5, .25]):
        require(type(level['scale']) is float and level['scale'] == scale, 'step scale')
        require(level['h'] == [scale*x for x in base], 'model step convention')
        derivatives(level)
        for sample in level['samples']:
            if sample['status'] != 'finite':
                continue
            spot = case['S'] + sample['offset'][0]*level['h'][0]
            intrinsic = max(case['K']-spot if case['side'] == 'put' else spot-case['K'], 0.)
            expected = intrinsic > 0 and sample['price'] <= intrinsic + 1e-9*case['K']
            require(sample['intrinsic_band'] == expected, 'price-derived intrinsic evidence')


def validate_report(report, corpus):
    require(report['schema'] in ['atx-american-greek-native-v1',
                                 'atx-american-greek-native-v2'], 'schema')
    typed_cold = report['schema'] == 'atx-american-greek-native-v2'
    require(report['derivative_tolerances'] is None and corpus['derivative_tolerances'] is None, 'no Greek budget')
    require(report['reference_independent_method'] is False and report['continuous_carry_only'] is True, 'scope')
    require(report['requested_fields'] == FIELDS and report['convention'] == corpus['convention'], 'convention')
    require(report['candidate_options'] == DEFAULT_OPTIONS, 'candidate scheme')
    require(report['candidate_options']['explicit'] is False, 'default option semantics')
    require(report['step_scales'] == [1., .5, .25] and report['family_sigma_factors'] == [.8, 1.2]
            and report['family_q_half_width'] == .0005, 'configuration')
    require(all(type(v) is float for v in report['step_scales']), 'numeric step scale')
    inputs = {x['id']: x for x in corpus['cases']}
    ids = report['selected_ids']
    require(type(report['input_case_count']) is int and
            len(inputs) == corpus['case_count'] == report['input_case_count'], 'input denominator')
    require(type(report['case_count']) is int and report['case_count'] == len(ids) == len(report['cases']) > 0
            and len(set(ids)) == len(ids) and set(ids) <= set(inputs), 'selection denominator')
    seen = set()
    for row in report['cases']:
        c = row['input']
        require(all(finite(c[k]) for k in ['S','K','T','sigma','r','q']), 'numeric contract')
        require(c['id'] not in seen and c['id'] in ids and c == inputs[c['id']], 'input identity')
        seen.add(c['id'])
        require(set(row['engines']) == set(ENGINES), 'engine denominator')
        require(set(row['references']) == set(PROFILES), 'reference denominator')
        require(set(row['served_maps']) == {'sigma_family', 'sigma_q_family'}, 'map denominator')
        for name, reference in row['references'].items():
            require(reference['options'] == PROFILES[name], 'reference scheme')
            validate_levels(reference['stencils'], c)
        for levels in row['served_maps'].values():
            validate_levels(levels, c)
        for name, engine in row['engines'].items():
            require(engine['price_status'] in ['success', 'refused'], 'price status')
            require((engine['price_status'] == 'success' and finite(engine['price'])) or
                    (engine['price_status'] == 'refused' and engine['price'] is None), 'price evidence')
            require(set(engine['fields']) == set(FIELDS), 'field denominator')
            if typed_cold and name.startswith('cold'):
                validate_cold_metadata(engine)
            for field, entry in engine['fields'].items():
                require(entry['requested'] is True and type(entry['supported']) is bool and
                        type(entry['reason']) is str and bool(entry['reason']), 'field metadata')
                require((entry['supported'] and finite(entry['value'])) or
                        (not entry['supported'] and entry['value'] is None), 'field evidence')
                require(not entry['supported'] or engine['price_status'] == 'success', 'refused price supports field')
                if name.startswith('sigma'):
                    require(entry['supported'] == (entry['reason'] in ['Served', 'Exercised']), 'typed support')
                elif not typed_cold:
                    require(entry['supported'] == (entry['reason'] == 'ApiValueUnqualified'), 'cold support')
            if name.startswith('sigma'):
                box = dict(anchor=c['K'] if c['side'] == 'put' else c['S'],
                           sigma_lo=.8*c['sigma'], sigma_hi=1.2*c['sigma'], n_sigma=12)
                if name == 'sigma_q_family':
                    box.update(q_lo=c['q']-.0005, q_hi=c['q']+.0005, n_q=4)
                require(engine['box'] == box, 'family box')
        for name, levels in row['served_maps'].items():
            for level in levels:
                for s in level['samples']:
                    d = s['offset']
                    absent = d[2] or d[3] or (name == 'sigma_family' and d[4])
                    require((s['status'] == 'NoModelAxis') == bool(absent), 'served map axis')
        require(row['engines']['cold_fd']['fields']['phi']['reason'] == 'NotInApi', 'FD phi unsupported')
        require(row['engines']['cold_al']['fields']['phi']['reason'] == 'NotInApi', 'AL phi unsupported')
        for field in ['vanna', 'volga', 'charm']:
            require(row['engines']['sigma_q_family']['fields'][field]['reason'] == 'NotInApi', 'tensor field unsupported')
    require(seen == set(ids), 'missing selected case')


def validate_cold_metadata(engine):
    result = engine['native_result_success']
    requested, supported = engine['native_requested_mask'], engine['native_supported_mask']
    reasons = engine['native_reasons']
    require(type(result) is bool and type(requested) is int and type(supported) is int,
            'native cold metadata types')
    require(requested == (511 if result else 0) and 0 <= supported <= 511 and
            (supported & ~requested) == 0, 'native cold requested/support masks')
    allowed = ['NotRequested', 'ComputedEstimate', 'Exercised', 'UnsupportedAxis',
               'StencilCrossing', 'Nonfinite']
    require(type(reasons) is list and len(reasons) == 9 and
            all(type(r) is str and r in allowed for r in reasons), 'native cold reasons')
    for i, reason in enumerate(reasons):
        require(not result or reason != 'NotRequested', 'requested cold field lacks reason')
        require(bool(supported & (1 << i)) == (reason in ['ComputedEstimate', 'Exercised']),
                'native cold support/reason contradiction')
        require(result or reason == 'NotRequested', 'refused result supplied native reason')
    require((engine['price_status'] == 'success') == bool(supported & (1 << 8)),
            'native cold price availability')
    for field, index in zip(FIELDS, [0, 1, 2, 3, 4, None, 5, 6, 7]):
        if index is None:
            continue  # phi is outside AmericanGreeks, retained as NotInApi
        entry = engine['fields'][field]
        require(entry['supported'] == bool(supported & (1 << index)) and
                entry['reason'] == (reasons[index] if result else 'ApiRefused'),
                'native cold field availability')


def stability(series24, series32, field):
    a = [s[field]['value'] for s in series24]
    b = [s[field]['value'] for s in series32]
    complete = all(finite(x) for x in a+b)
    result = dict(refined24=a, refined32=b, complete=complete,
                  intrinsic_evidence_complete=all(s[field]['intrinsic_evidence_complete']
                                                  for s in series24+series32),
                  intrinsic_band_changed=any(s[field]['intrinsic_band_changed'] for s in series24+series32))
    if complete:
        prior, last = abs(b[1]-b[0]), abs(b[2]-b[1])
        extrap = [(4*b[i+1]-b[i])/3 for i in range(2)]
        result.update(last_step_change=last, prior_step_change=prior,
                      change_decreases=last <= prior, profile_change=abs(b[2]-a[2]),
                      richardson=extrap, richardson_change=abs(extrap[1]-extrap[0]))
    return result


def summarize(report, corpus):
    validate_report(report, corpus)
    details = []
    totals = {name: {field: dict(requested=0, supported=0, reference_complete=0,
                               compared=0, intrinsic_band_changed=0, step_change_increases=0,
                               reasons=Counter(), errors=[], map_errors=[])
                     for field in FIELDS} for name in ENGINES}
    for row in report['cases']:
        refs = {k: [derivatives(s) for s in v['stencils']] for k, v in row['references'].items()}
        maps = {k: [derivatives(s) for s in v] for k, v in row['served_maps'].items()}
        diagnostics = {f: stability(refs['refined24'], refs['refined32'], f) for f in FIELDS}
        comparisons = {}
        for name, engine in row['engines'].items():
            comparisons[name] = {}
            for field in FIELDS:
                entry, ref, total = engine['fields'][field], diagnostics[field], totals[name][field]
                total['requested'] += 1
                total['supported'] += int(entry['supported'])
                total['reference_complete'] += int(ref['complete'])
                total['reasons'][entry['reason']] += 1
                total['intrinsic_band_changed'] += int(ref['intrinsic_band_changed'])
                total['step_change_increases'] += int(ref['complete'] and not ref['change_decreases'])
                item = dict(value=entry['value'], reason=entry['reason'], supported=entry['supported'])
                if entry['supported'] and ref['complete']:
                    error = abs(entry['value']-ref['refined32'][-1])
                    item['abs_error_vs_finest_cold'] = error
                    total['compared'] += 1
                    total['errors'].append((error, row['input']['id']))
                if name in maps:
                    values = [x[field]['value'] for x in maps[name]]
                    item['served_map_estimates'] = values
                    if entry['supported'] and all(finite(v) for v in values):
                        error = abs(entry['value']-values[-1])
                        item['abs_error_vs_finest_served_map'] = error
                        total['map_errors'].append((error, row['input']['id']))
                comparisons[name][field] = item
        details.append(dict(input=row['input'], reference=diagnostics, comparisons=comparisons))
    for fields in totals.values():
        for total in fields.values():
            total['refused_or_unsupported'] = total['requested']-total['supported']
            total['reasons'] = dict(sorted(total['reasons'].items()))
            for key in ['errors', 'map_errors']:
                values = sorted(total.pop(key), reverse=True)
                total[key+'_count'] = len(values)
                total[key+'_worst'] = [dict(abs_error=x, id=i) for x, i in values[:10]]
    reference_summary = {}
    for field in FIELDS:
        complete = [x for x in details if x['reference'][field]['complete']]
        reference_summary[field] = dict(
            requested=len(details), complete=len(complete), unavailable=len(details)-len(complete),
            intrinsic_evidence_incomplete=sum(not x['reference'][field]['intrinsic_evidence_complete']
                                              for x in details),
            intrinsic_band_changed=sum(x['reference'][field]['intrinsic_band_changed'] for x in details),
            step_change_increases=sum(not x['reference'][field]['change_decreases'] for x in complete))
        for metric in ['last_step_change', 'profile_change', 'richardson_change']:
            ranked = sorted(complete, key=lambda x:x['reference'][field][metric], reverse=True)
            reference_summary[field][metric+'_worst'] = [
                dict(id=x['input']['id'], value=x['reference'][field][metric]) for x in ranked[:10]]
    return dict(schema='atx-american-greek-summary-v2', case_count=len(details),
                derivative_tolerances=None, qualified=False, reference_independent_method=False,
                note='Numerical diagnostics only. Step/profile drift is not a rigorous error bound.',
                reference_summary=reference_summary, totals=totals, cases=details)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def validate_requested_ids(report, expected):
    require(type(expected) is list and len(expected) > 0 and
            all(type(x) is str and x for x in expected) and len(set(expected)) == len(expected),
            'requested selection identity')
    actual = report['selected_ids']
    require(type(actual) is list and len(actual) == len(expected) and
            all(type(x) is str for x in actual) and len(set(actual)) == len(actual) and
            set(actual) == set(expected), 'execution omitted or added requested cases')


def run(binary, output, selection):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    corpus = HERE/'corpus-v1.json'
    require(digest(corpus) == CORPUS_SHA, 'frozen corpus hash')
    corpus_data = json.loads(corpus.read_text())
    all_ids = [x['id'] for x in corpus_data['cases']]
    binary = Path(binary).resolve()
    archived = output/'atx-vol-greek-audit.exe'
    shutil.copy2(binary, archived)
    selected = '-'
    if selection:
        selected = str(output/'selection.json')
        shutil.copy2(selection, selected)
    expected = json.loads(Path(selected).read_text()) if selection else all_ids
    validate_requested_ids(dict(selected_ids=expected), expected)
    require(set(expected) <= set(all_ids), 'requested case outside frozen corpus')
    native = output/'native.json'
    command = [str(archived), str(corpus), selected, str(native)]
    with (output/'native.log').open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    require(digest(binary) == digest(archived), 'binary changed during run')
    report = json.loads(native.read_text())
    validate_requested_ids(report, expected)
    summary = summarize(report, corpus_data)
    (output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    root = HERE.parents[2]
    manifest = dict(schema='atx-american-greek-run-v1', utc=datetime.now(timezone.utc).isoformat(),
                    source_commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
                    source_status=subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True),
                    python=sys.version, platform=platform.platform(), command=command,
                    corpus_sha256=digest(corpus), derivative_tolerances=None, latency_qualified=False,
                    expected_case_ids=expected,
                    files={p.name:digest(p) for p in output.iterdir() if p.is_file()})
    manifest['runner_sources'] = {str(p.relative_to(root)): digest(p)
        for p in [HERE/'greek_main.cpp', HERE/'greek_report.py', HERE/'test_greek_report.py',
                  root/'atx-vol/CMakeLists.txt']}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary')
    parser.add_argument('--output', required=True)
    parser.add_argument('--selection')
    parser.add_argument('--report')
    parser.add_argument('--require-all', action='store_true',
                        help='require every frozen corpus ID when revalidating --report')
    args = parser.parse_args()
    require(bool(args.report) or not args.require_all, '--require-all applies to --report')
    if args.report:
        report = json.loads(Path(args.report).read_text())
        require(digest(HERE/'corpus-v1.json') == CORPUS_SHA, 'frozen corpus hash')
        corpus = json.loads((HERE/'corpus-v1.json').read_text())
        require(not (args.require_all and args.selection), 'choose full corpus or explicit selection')
        if args.require_all or args.selection:
            expected = ([x['id'] for x in corpus['cases']] if args.require_all else
                        json.loads(Path(args.selection).read_text()))
            validate_requested_ids(report, expected)
        result = summarize(report, corpus)
        Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
    else:
        require(bool(args.binary), '--binary required')
        result = run(args.binary,args.output,args.selection)
    print(json.dumps(dict(case_count=result['case_count'], qualified=False)))
