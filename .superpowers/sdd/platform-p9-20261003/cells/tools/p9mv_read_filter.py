"""R15-MV-LAUNCH scalar allowlist. No process or scientific computation beyond registered quotients."""
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import sys

WAVE = Path('build-equity/waves/p9mv-mv-aim')
LOGS = Path('.superpowers/sdd/platform-p9-20261003/cells/logs')
BASE = 'build-equity/mega-nav-p9-theme-cov-hedge-gm'
CELL = 'build-equity/mega-nav-p9-mv-aim-gm'
EVAL = Path('build-equity-rel/bin/atx-research-eval.exe')
PIN = 'e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c'
STAGES = ('preflight', 'register', 'screen', 'spec', 'run', 'match', 'verify', 'judge', 'record')


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def doc(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def metadata(path):
    raw_path = str(path)
    raw = Path(path).read_bytes()
    text = raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig', errors='replace')
    for line in text.splitlines():
        match = re.fullmatch(r'== (preflight|register|screen|spec|run|match|verify|judge|record)(?:: done \([0-9]{2}-[a-z]+\.json\))?', line)
        if match:
            print('stage: ' + match[1])
    for i, stage in enumerate(STAGES, 1):
        path = WAVE / 'receipts' / f'{i:02}-{stage}.json'
        if path.is_file():
            print(json.dumps({'stage': stage, 'receipt': str(path), 'sha256': sha(path),
                              'seconds': doc(path).get('seconds')}))
        for failed in sorted((WAVE / 'receipts').glob(f'{i:02}-{stage}.failed-*.json')):
            print(json.dumps({'stage': stage, 'failed_receipt': str(failed), 'contents': 'withheld'}))
    print(json.dumps({'raw_console': raw_path, 'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}))


def mechanics():
    value = doc(WAVE / 'receipts/07-verify.json')['outputs']['mechanics']
    assert len(value['rows']) == 6
    return {'pass': value['pass'], 'rows': [
        {k: row[k] for k in ('check', 'value', 'limit', 'pass')} for row in value['rows']]}


def bound_bundle():
    assert doc(LOGS / 'p9mv-mechanics-first.json')['pass'] is True
    assert mechanics()['pass'] is True
    judge = doc(WAVE / 'receipts/08-judge.json')['outputs']
    path = WAVE / 'bundle.json'
    assert sha(path) == judge['bundle']['sha256'], 'bundle/receipt hash differs'
    bundle = doc(path)
    assert Path(bundle.get('base', '')).as_posix() == BASE
    assert Path(bundle.get('final', '')).as_posix() == CELL
    assert sha(EVAL) == bundle.get('nav_summ_run', {}).get('engine_exe_sha256') == PIN
    return bundle, judge


def rho():
    bundle, _ = bound_bundle()
    paired = bundle.get('paired')
    assert isinstance(paired, dict) and type(paired.get('sessions')) is int and paired['sessions'] >= 2
    value = paired.get('rho')
    assert type(value) in (int, float) and math.isfinite(value) and -1 <= value <= 1
    assert value != 0.999999, 'STOP: rho threshold boundary'
    if 'degenerate' not in paired and 'one_minus_rho' not in paired:
        flag = False
        provenance = 'inferred FALSE from documented nondegenerate encoding; R15-V2-RHO'
    elif paired.get('degenerate') is True and type(paired.get('one_minus_rho')) in (int, float) and \
            math.isfinite(paired['one_minus_rho']) and 0 <= paired['one_minus_rho'] < 1e-6:
        flag = True
        provenance = 'supplied engine flag and stable difference'
    else:
        raise SystemExit('STOP: inconsistent paired degeneracy encoding')
    keys = ('sessions', 'rho', 'one_minus_rho', 'degenerate')
    return {'path': str(WAVE / 'bundle.json'), 'sha256': sha(WAVE / 'bundle.json'),
            'base': BASE, 'final': CELL, 'engine_exe_sha256': PIN,
            'paired': {k: paired.get(k) for k in keys},
            'not_supplied': [k for k in keys if k not in paired],
            'one_minus_rho_text': paired.get('one_minus_rho', 'not supplied by bundle'),
            'threshold_flag': flag, 'threshold_flag_provenance': provenance}


def selected_mv(summary):
    mv = summary['mv_aim']
    def stats(value):
        return {k: value[k] for k in ('n', 'mean', 'min', 'max')}
    result = {'rule': mv['rule'],
              'risk_model': {k: mv['risk_model'][k] for k in ('directory', 'manifest_sha256')},
              'decisions': {k: mv['decisions'][k] for k in ('formed', 'applied')},
              'warm_up_decisions': {k: mv['warm_up_decisions'][k] for k in ('formed', 'applied')},
              'priced_names': stats(mv['priced_names']), 'unpriced_members': stats(mv['unpriced_members']),
              'specific_vol_ratio': {'pooled': mv['specific_vol_ratio']['pooled'],
                                     'per_decision': stats(mv['specific_vol_ratio']['per_decision'])},
              'special_tier_negative': {k: mv['special_tier_negative'][k]
                                        for k in ('locate_in_aim', 'name_decisions', 'decisions')}}
    result['decisions']['pass_through'] = {k: mv['decisions']['pass_through'][k]
                                         for k in ('no_forecast', 'fewer_than_two_priced', 'flat')}
    eigen = mv['eigen_00_02_share']
    if eigen is None:
        result['eigen_00_02_share'] = None
        result['eigen_00_02_share_unavailable'] = {
            k: mv['eigen_00_02_share_unavailable'][k] for k in ('decisions', 'first_error')}
    else:
        result['eigen_00_02_share'] = {k: stats(eigen[k]) for k in ('mv', 'desired')}
    return result


def prints():
    bundle, judge = bound_bundle()
    assert doc(LOGS / 'p9mv-rho-first.txt')['sha256'] == sha(WAVE / 'bundle.json')
    paired, verdict = bundle['paired'], judge['verdict']
    result = {'decision': {k: verdict[k] for k in ('rule', 'accepted', 'dsr')},
              'checks': {k: verdict['checks'][k] for k in ('dsr_positive', 'mechanics')},
              'paired': {k: paired.get(k) for k in ('dsr', 'memmel_se', 'sr', 'sr_reference')},
              'p_two_sided': paired.get('lw', {}).get('p_value'),
              'p_one_sided': paired.get('lw', {}).get('p_one_sided'), 'books': {}}
    for side, directory in [('parent', BASE), ('cell', CELL)]:
        path = Path(directory) / 'summary.json'
        summary = doc(path)
        primary = summary['primary_scenario']
        scenarios = [s for s in summary['scenarios'] if s['scenario'] == primary]
        assert len(scenarios) == 1
        scenario = scenarios[0]
        book = {'source': str(path), 'sha256': sha(path), 'scenario': primary,
                'ann_vol': scenario['ann_vol'], 'ann_mean_gross': scenario['ann_mean_gross'],
                **{k: judge['book'][side][k] for k in ('tau_per_gross', 'cost_bps_traded', 'x4_net_sharpe')}}
        capacity = Path(directory) / 'capacity_curve.csv'
        with capacity.open(newline='', encoding='utf-8') as stream:
            rows = [r for r in csv.DictReader(stream)
                    if re.fullmatch(r'capacity-x[0-9.]+-v1\+swap-fin-v1', r['book'])]
        multiples = [float(r['multiple']) for r in rows]
        if not rows or len(multiples) != len(set(multiples)) or 1 not in multiples or 4 not in multiples:
            book['capacity'] = 'unavailable: missing or ambiguous registered primary rows'
        else:
            book['capacity'] = [{'multiple': float(r['multiple']), 'book': r['book'],
                                 'net_sharpe': float(r['net_sharpe'])} for r in rows]
            cost = next(r for r in rows if float(r['multiple']) == 1)
            book['cost_bps_per_traded_dollar'] = float(cost['cost_bps_per_traded_dollar'])
        book['capacity_source'], book['capacity_sha256'] = str(capacity), sha(capacity)
        if side == 'cell':
            book['mv_aim'] = selected_mv(summary)
        result['books'][side] = book
    parent, cell = result['books']['parent'], result['books']['cell']
    if all(type(b[k]) in (int, float) and math.isfinite(b[k])
           for b in (parent, cell) for k in ('ann_vol', 'ann_mean_gross')) and \
            parent['ann_vol'] != 0 and parent['ann_mean_gross'] != 0:
        vol, gross = cell['ann_vol'] / parent['ann_vol'], cell['ann_mean_gross'] / parent['ann_mean_gross']
        result['nondeciding_predictions'] = {'ann_vol_ratio': vol, 'vol_ratio_lt_1': vol < 1,
                                             'ann_mean_gross_ratio': gross, 'gross_ratio_in_band': .9 <= gross <= 1.1}
    else:
        result['nondeciding_predictions'] = 'unavailable: nonfinite or zero denominator; not imputed'
    return result


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'metadata':
        metadata(sys.argv[2])
    elif mode in ('mechanics', 'rho', 'prints'):
        value = {'mechanics': mechanics, 'rho': rho, 'prints': prints}[mode]()
        if len(sys.argv) > 2:
            Path(sys.argv[2]).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8', newline='\n')
        print(json.dumps(value, indent=2))
    else:
        raise SystemExit('usage: p9mv_read_filter.py metadata RAW_LOG | mechanics|rho|prints [OUTPUT]')
