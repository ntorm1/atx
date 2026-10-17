"""R15-X-SOURCE/LAUNCH: narrow source, metadata and ordered scalar projections."""
import csv
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path.cwd()
SDD = Path('.superpowers/sdd/platform-p9-20261003')
LOGS = SDD / 'cells/logs'
PROPOSAL = SDD / 'cells/p9x-leverage-proposal.json'
CELL = Path('build-equity/mega-nav-p9x-leverage')
BASE = Path('build-equity/mega-nav-p9-theme-cov-hedge-gm')
READ = Path('build-equity/p9x-leverage-readers')
DIRS = {'cycle': Path(str(CELL) + '-run'), **{k: READ / (k + '-run1') for k in ('mechanics', 'pair', 'book')}}
LEDGER = 'fe1bc97ecf9ccef7f042d637730581c3410792798ef2ab0a4a7a7fa2bfbc05f8'
PNGS = {
    'docs/plans/2026-10-02-x5-equity-curve.png',
    'docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png',
    'docs/plans/2026-10-03-wq-explore-equity-curve.png',
    'docs/plans/2026-10-03-wq-explore-sharpe-vs-turnover.png',
    'docs/plans/2026-10-06-p9-f0-yf0-equity-curve.png'}
HEADER = '# dirty outside the code pathspec (listed, not a stop): '
sys.path.insert(0, str(ROOT / 'scripts'))
import wave_seal as WSL


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(4 << 20), b''):
            h.update(b)
    return h.hexdigest()


def doc(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def save(path, value):
    with Path(path).open('x', encoding='utf-8', newline='\n') as f:
        f.write(json.dumps(value, indent=2, allow_nan=False) + '\n')


def fields(text, start=0):
    """Top-level JSON field value spans; escaped strings never become field names."""
    decoder = json.JSONDecoder()
    i = start + 1
    while True:
        while text[i].isspace() or text[i] == ',':
            i += 1
        if text[i] == '}':
            return
        key, i = decoder.raw_decode(text, i)
        while text[i].isspace():
            i += 1
        assert text[i] == ':'
        i += 1
        while text[i].isspace():
            i += 1
        begin = i
        value, i = decoder.raw_decode(text, i)
        yield key, value, begin, i


def receipt_spans(text, start=0):
    spans = []
    for key, value, a, b in fields(text, start):
        if key == 'started_utc' and isinstance(value, str) and value.startswith('2026-10-17T'):
            dt.datetime.fromisoformat(value)
            spans.append((a + 1, a + 11, 'OWN_UTC'))
        if key == 'dirty_outside_pathspec' and isinstance(value, list):
            i = a + 1
            while i < b - 1:
                while text[i].isspace() or text[i] == ',':
                    i += 1
                if text[i] == ']':
                    break
                entry, end = json.JSONDecoder().raw_decode(text, i)
                if isinstance(entry, str) and entry in PNGS and text[i:end] == json.dumps(entry):
                    spans.append((i + 1, end - 1, 'OWNER_RECEIPT'))
                i = end
    return spans


def source_text(text, kind, receipt=None):
    spans = []
    if kind == 'receipt':
        json.loads(text)
        spans = receipt_spans(text, len(text) - len(text.lstrip()))
    else:
        if receipt is not None:
            for m in re.finditer(r'^\{', text, re.M):
                try:
                    value, _ = json.JSONDecoder().raw_decode(text, m.start())
                except ValueError:
                    continue
                if value == receipt:
                    spans.extend(receipt_spans(text, m.start()))
        if kind == 'cycle':
            for m in re.finditer(r'^' + re.escape(HEADER) + r'[^\r\n]*', text, re.M):
                entries = m.group()[len(HEADER):].split(', ')
                pos = m.start() + len(HEADER)
                for entry in entries:
                    if entry in PNGS:
                        spans.append((pos, pos + len(entry), 'OWNER_HEADER'))
                    pos += len(entry) + 2
    hits = []
    for form, regex in WSL.FORMS.items():
        for m in regex.finditer(text):
            raw, g = m.group(), m.groups()
            try:
                iso = (dt.date(int(g[0]), int(g[1]), int(g[2])).isoformat() if form in ('iso', 'compact')
                       else WSL.RW.period_begin(int(g[0]), int(g[1]) if form == 'quarter' else None).isoformat())
            except ValueError:
                continue
            if not WSL.sealed(iso):
                continue
            line_a = text.rfind('\n', 0, m.start()) + 1
            line_b = text.find('\n', m.end())
            line = text[line_a:line_b if line_b >= 0 else len(text)]
            source = ('SEED' if raw in WSL.SEEDS else 'SEAL_REFERENCE'
                      if iso == WSL.RW.SEAL_DATE and 'seal' in line.lower() else 'OTHER')
            if source == 'OTHER':
                source = next((label for a, b, label in spans if a <= m.start() and m.end() <= b), 'OTHER')
            hits.append({'line': text.count('\n', 0, m.start()) + 1, 'token': raw, 'form': form, 'source': source})
    return hits


def source(stage):
    directory = DIRS[stage]
    receipt = doc(directory / 'receipt.json') if (directory / 'receipt.json').exists() else None
    paths = [directory / n for n in ('start.json', 'receipt.json', 'stdout.log', 'stderr.log')]
    paths.append(LOGS / f'p9x-{stage}-raw.log')
    rows = []
    for path in paths:
        if not path.exists():
            continue
        kind = 'receipt' if path.name in ('start.json', 'receipt.json') else ('cycle' if stage == 'cycle' and path.parent == LOGS else 'raw')
        hits = source_text(path.read_text(encoding='utf-8-sig'), kind, receipt)
        rows.append({'path': path.as_posix(), 'sha256': sha(path), 'hits': hits})
    assert rows, 'STOP: no own source files'
    out = {'stage': stage, 'files': rows, 'other': sum(h['source'] == 'OTHER' for row in rows for h in row['hits'])}
    save(LOGS / f'p9x-{stage}-source.json', out)
    print(json.dumps(out, indent=2))
    assert out['other'] == 0, 'STOP: source OTHER'


def source_ok(stage):
    value = doc(LOGS / f'p9x-{stage}-source.json')
    assert value['other'] == 0
    for row in value['files']:
        assert sha(row['path']) == row['sha256']


def pins():
    import research_cycle as RC
    d = doc(PROPOSAL)
    assert sha(d['template_path']) == d['template_sha256']
    s = RC.load_spec(Path(d['template_path']))
    h = RC.load_spec(Path('scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json'))
    assert sha('scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json') == '1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f'
    for key in ('fit', 'ic', 'card', 'runner', 'fields'):
        assert s[key] == h[key], 'STOP: inherited ' + key
    locked = d['template']['locked']
    for key in set(s['inputs']) | set(h['inputs']):
        if key in locked:
            assert all(s['inputs'][key][k] == v for k, v in locked[key].items())
            if key == 'reference_cell':
                assert s['inputs'][key]['dir'] == BASE.as_posix()
        else:
            assert s['inputs'].get(key) == h['inputs'].get(key), 'STOP: inherited input ' + key
    assert '--engine-fit' not in s['fit']['flags']
    assert s['nav']['rule'] == 'aim-partial-v5' and s['nav']['leverage'] == '2.0'
    flags = s['nav']['flags']
    for flag in ('--ann-mean-gross', '--info-clock-print', '--capacity-curve', '--vol-target', '--risk-model'):
        assert flags.count(flag) == 1
    assert '--target-rule' not in flags and '--rate' not in flags
    checks = []
    for value in s['inputs'].values():
        if isinstance(value, dict) and 'path' in value and 'sha256' in value:
            assert sha(value['path']) == value['sha256']
            checks.append(value)
    for row in doc(LOGS / 'p9mv-frozen-exes.json'):
        assert sha(row['path']) == row['sha256']
    for path, pin in [(d['store']['path'] + '/manifest.json', d['store']['manifest_sha256']),
                      (d['store']['path'] + '/model.atxcov', d['store']['container']['file_sha256']),
                      (d['mechanics_parent_source']['path'], d['mechanics_parent_source']['sha256'])]:
        assert sha(path) == pin
        checks.append({'path': path, 'sha256': pin})
    assert doc(d['store']['role']['path'])['universe']['delisting']['returns_applied'] is False
    assert sha('build-equity/trials.jsonl') == LEDGER
    assert len(Path('build-equity/trials.jsonl').read_bytes().splitlines()) == 138
    assert not Path('build-equity/research-store').exists()
    cache = Path('build-equity/mega-candidate-cache-v8-lo3')
    assert not (cache / 'index.sqlite').exists() and sum(p.is_file() for p in cache.rglob('*')) == 686
    assert all(not Path('build-equity/train-2020-2023-lo3-fields-' + v).exists() for v in ('v9', 'v10', 'v13', 'v14', 'v15a'))
    return {'status': 'PASS', 'ledger_lines': 138, 'ledger_sha256': LEDGER, 'frozen_exes': 54, 'inputs': checks}


def receipt_check(stage):
    source_ok(stage)
    p = DIRS[stage] / 'receipt.json'
    r = doc(p)
    key = 'native_bounded_nav_argv' if stage == 'cycle' else {'mechanics': 'mechanics_command', 'pair': 'paired_bundle_command', 'book': 'book_command'}[stage]
    a = doc(PROPOSAL)[key]; command = a[a.index('--') + 1:]
    assert Path(r['command'][0]).resolve() == Path(command[0]).resolve()
    assert r['command'][1:] == command[1:]
    assert r['limits'] == {'seconds': 180.0, 'max_rss_mib': 1536, 'min_free_mib': 512}
    assert r['outcome'] == 'completed' and r['exit_code'] == 0
    assert r['wall_seconds'] <= 180 and r['sampled_peak_tree_rss_bytes'] <= 1536 * 2**20
    return {'path': p.as_posix(), 'sha256': sha(p), 'source_sha': r['source_sha'], 'argv_sha256': r['argv_sha256'],
            'wall_seconds': r['wall_seconds'], 'peak_mib': r['sampled_peak_tree_rss_bytes'] / 2**20, 'limits': r['limits']}


def finite(x):
    assert type(x) in (float, int) and math.isfinite(x), 'STOP: nonfinite/missing scalar'
    return x


def lt_values():
    with (CELL / 'vol_target.csv').open(newline='', encoding='utf-8') as f:
        reader = csv.DictReader(f)
        assert reader.fieldnames == ['session', 'book', 'rebalance', 'updated', 'gross', 'priced_share', 'sigma_hat', 'sigma_ref', 'raw', 'L_t', 'multiplier', 'clip']
        values = [finite(float(row['L_t'])) for row in reader if row['book'] == doc(PROPOSAL)['scenario']]
    assert values and all(1 <= x <= 2 for x in values), 'STOP: L_t schema/cap'
    return values


def mechanics():
    receipts = [receipt_check(k) for k in ('cycle', 'mechanics')]
    d = doc(PROPOSAL); m = doc(READ / 'mechanics.json')['navs']['cell']; lim = d['limits']
    assert m['dir'] == CELL.as_posix() and m['scenario'] == d['scenario']
    assert m['daily_csv_sha256'] == sha(CELL / ('daily_' + d['scenario'] + '.csv'))
    rows = []
    for key, bound, absolute in [('mean_gross_leverage_all_rows', [lim['gross_low'], lim['gross_high']], False),
                                ('mean_net_leverage_all_rows', lim['abs_net_max'], True),
                                ('tau_gmv_mean', lim['tau_mean_max'], False), ('tau_gmv_p95', lim['tau_p95_max'], False),
                                ('max_return_identity_error', 1e-9, False), ('max_cash_book_relative_error', 1e-9, False)]:
        value = finite(m['accounting'][key] if key.startswith('max_') else m[key])
        test = abs(value) if absolute else value
        ok = bound[0] <= test <= bound[1] if isinstance(bound, list) else test <= bound
        rows.append({'check': key, 'value': value, 'absolute': absolute, 'limit': bound, 'pass': ok})
    s = doc(CELL / 'summary.json')
    exposure = [{'scenario': x['scenario'], **{k: finite(x['exposure'][k]) for k in ('mean_gross_leverage', 'max_gross_leverage')}} for x in s['scenarios']]
    assert len(exposure) == 5 and d['scenario'] in [x['scenario'] for x in exposure]
    values = lt_values()
    out = {'rows': rows, 'pass': all(x['pass'] for x in rows), 'exposure': exposure,
           'cap_hold': all(x['max_gross_leverage'] <= 2 for x in exposure), 'receipts': receipts,
           'L_t_cap_hold': True, 'L_t_rows': len(values), 'vol_target_sha256': sha(CELL / 'vol_target.csv'),
           'mechanics_sha256': sha(READ / 'mechanics.json'), 'summary_sha256': sha(CELL / 'summary.json')}
    save(LOGS / 'p9x-mechanics-first.json', out)
    return out


def bundle():
    m = doc(LOGS / 'p9x-mechanics-first.json')
    assert m['pass'] and m['cap_hold']
    assert m['mechanics_sha256'] == sha(READ / 'mechanics.json') and m['summary_sha256'] == sha(CELL / 'summary.json')
    receipt = receipt_check('pair')
    b = doc(READ / 'pair.json'); d = doc(PROPOSAL)
    assert Path(b['base']) == BASE and Path(b['final']) == CELL
    pin = d['template']['change']['set']['exes_sha256']['eval']
    assert b['nav_summ_run']['engine_exe_sha256'] == sha('build-equity-rel/bin/atx-research-eval.exe') == pin
    files = {str(p): sha(p) for directory in (BASE, CELL) for p in (directory / 'summary.json', directory / ('daily_' + d['scenario'] + '.csv'))}
    return b, {'path': (READ / 'pair.json').as_posix(), 'sha256': sha(READ / 'pair.json'), 'receipt': receipt, 'files': files, 'engine_exe_sha256': pin}


def rho():
    b, out = bundle(); p = b['paired']; r = finite(p['rho'])
    assert type(p['sessions']) is int and p['sessions'] >= 2 and -1 <= r <= 1 and r != .999999
    if 'degenerate' not in p and 'one_minus_rho' not in p:
        deg = False; provenance = 'inferred FALSE; documented nondegenerate encoding, R15-V2-RHO'
    elif p.get('degenerate') is True and 0 <= finite(p['one_minus_rho']) < 1e-6:
        deg = True; provenance = 'supplied stable one_minus_rho and engine flag'
    else:
        raise AssertionError('STOP: incompatible paired degeneracy encoding')
    out.update(paired={k: p.get(k) for k in ('sessions', 'rho', 'one_minus_rho', 'degenerate')},
               missing=[k for k in ('one_minus_rho', 'degenerate') if k not in p], threshold_flag=deg, provenance=provenance)
    save(LOGS / 'p9x-rho-first.json', out)
    return out


def project_pair(p, degenerate):
    return {'dsr': finite(p['dsr']), 'memmel_se': 'n/a' if degenerate else finite(p['memmel_se']),
            't': 'n/a' if degenerate else finite(p['t']),
            'p_two_sided': 'n/a' if degenerate else finite(p['lw']['p_value']),
            'p_one_sided': 'n/a' if degenerate else finite(p['lw']['p_one_sided'])}


def prints():
    b, bound = bundle(); rho_doc = doc(LOGS / 'p9x-rho-first.json')
    assert rho_doc['sha256'] == bound['sha256'] and rho_doc['files'] == bound['files']
    receipt = receipt_check('book'); book = doc(READ / 'book.json')['navs']; d = doc(PROPOSAL)
    expected = {'parent': BASE, 'cell': CELL, 'yf0': Path('build-equity/p9-b0g-yf0-nav')}
    assert set(book) == set(expected)
    for alias, directory in expected.items():
        assert Path(book[alias]['dir']) == directory and book[alias]['scenario'] == d['scenario']
        assert book[alias]['daily_csv_sha256'] == sha(directory / ('daily_' + d['scenario'] + '.csv'))
    books = {k: {field: finite(book[k][field]) for field in ('net_annual', 'net_sharpe', 'x4_net_sharpe', 'ann_vol', 'max_drawdown')} for k in ('parent', 'cell', 'yf0')}
    values = lt_values()
    books['cell']['mean_L_t'] = sum(values) / len(values)
    books['cell']['L_t_rows'] = len(values)
    books['parent']['mean_L_t'] = 1.1674; books['yf0']['mean_L_t'] = 1.1828
    for k in ('parent', 'yf0'):
        books[k]['L_t_provenance'] = 'fixed registered leverage parameter; not observed exposure'
    checks = {'net_annual_above_parent': books['cell']['net_annual'] > books['parent']['net_annual'],
              'net_sharpe_within_point100': books['cell']['net_sharpe'] >= books['parent']['net_sharpe'] - .100,
              'scaled_mechanics': doc(LOGS / 'p9x-mechanics-first.json')['pass']}
    out = {'books': books, 'paired': project_pair(b['paired'], rho_doc['threshold_flag']), 'checks': checks,
           'accepted': all(checks.values()), 'paired_deciding': False, 'book_receipt': receipt,
           'book_sha256': sha(READ / 'book.json'), 'vol_target_sha256': sha(CELL / 'vol_target.csv')}
    save(LOGS / 'p9x-registered-prints.json', out)
    return out


def controls():
    good = {'started_utc': '2026-10-17T12:00:00+00:00', 'dirty_outside_pathspec': sorted(PNGS)}
    text = json.dumps(good, indent=2)
    assert all(x['source'] != 'OTHER' for x in source_text(text, 'receipt'))
    assert all(x['source'] != 'OTHER' for x in source_text(text, 'raw', good))
    assert all(x['source'] != 'OTHER' for x in source_text(HEADER + ', '.join(sorted(PNGS)), 'cycle'))
    for bad, kind in [(json.dumps({'ended_utc': good['started_utc']}), 'receipt'),
                      (json.dumps({'started_utc': '2026-10-18T12:00:00+00:00'}), 'receipt'),
                      (json.dumps({'other': sorted(PNGS)}), 'receipt'),
                      ('elsewhere ' + sorted(PNGS)[0], 'raw'), (HEADER + ', '.join(sorted(PNGS)), 'raw')]:
        assert any(x['source'] == 'OTHER' for x in source_text(bad, kind))
    p = {'dsr': .2, 'memmel_se': .3, 't': .4, 'lw': {'p_value': .5, 'p_one_sided': .6, 'unregistered': 'CANARY'}, 'cbb': 'CANARY'}
    assert 'CANARY' not in json.dumps(project_pair(p, False)) and project_pair(p, True)['memmel_se'] == 'n/a'
    return {'small_synthetic_source_and_projection_controls': 'PASS', 'scientific_input_reads': 0}


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'source':
        source(sys.argv[2])
    else:
        result = {'pins': pins, 'mechanics': mechanics, 'rho': rho, 'prints': prints, 'controls': controls}[mode]()
        if len(sys.argv) > 2:
            save(Path(sys.argv[2]), result)
        print(json.dumps(result, indent=2, allow_nan=False))
