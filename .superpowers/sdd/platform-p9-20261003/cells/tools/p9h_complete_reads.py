"""Registered P9-H post-rho fields and ledger metadata only. No process or statistic engine invocation."""
import csv
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path.cwd()
LOGS = ROOT / '.superpowers/sdd/platform-p9-20261003/cells/logs'
WAVE = ROOT / 'build-equity/waves/p9h-cov-hedge-attempt2'


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(name, value):
    path = LOGS / name
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print(f'{name}: sha256 {sha(path)}')


assert (LOGS / 'p9h-attempt2-rho-first.txt').is_file(), 'rho-first log required'
assert read(WAVE / 'receipts/07-verify.json')['outputs']['mechanics']['pass'] is True
judge = read(WAVE / 'receipts/08-judge.json')['outputs']
bundle = read(WAVE / 'bundle.json')
assert sha(WAVE / 'bundle.json') == judge['bundle']['sha256']
paired_keys = ('sessions', 'rho', 'dsr', 'memmel_se', 't', 'lw', 'cbb_ci95', 'sr', 'sr_reference')
prints = {'decision': judge['verdict'], 'paired': {k: bundle['paired'].get(k) for k in paired_keys}, 'books': {}}
for side, rel in (('cell', 'build-equity/mega-nav-p9-theme-cov-hedge-gm'),
                  ('parent', 'build-equity/mega-nav-p9l-label-terminal-gm')):
    nav = ROOT / rel
    summary = read(nav / 'summary.json')
    primary = summary['primary_scenario']
    scenario = next(s for s in summary['scenarios'] if s['scenario'] == primary)
    with (nav / 'capacity_curve.csv').open(newline='', encoding='utf-8') as stream:
        capacity = list(csv.DictReader(stream))
    def row(mult):
        rows = [r for r in capacity if float(r['multiple']) == mult]
        if len(rows) > 1:
            rows = [r for r in rows if r['book'] == primary]
        assert len(rows) == 1, 'capacity row missing or ambiguous'
        return rows[0]
    prints['books'][side] = {
        'path': rel, 'scenario': primary, 'summary_sha256': sha(nav / 'summary.json'),
        'capacity_csv_sha256': sha(nav / 'capacity_curve.csv'),
        'ann_vol_cpp': scenario['ann_vol'], 'ann_mean_gross_cpp': scenario['ann_mean_gross'],
        'turnover_per_gross': judge['book'][side]['tau_per_gross'],
        'cost_bps_per_traded_dollar_cpp_x1': float(row(1)['cost_bps_per_traded_dollar']),
        'net_sharpe_cpp_x4': float(row(4)['net_sharpe']),
    }
cell, parent = prints['books']['cell'], prints['books']['parent']
vol = cell['ann_vol_cpp'] / parent['ann_vol_cpp']
gross = cell['ann_mean_gross_cpp'] / parent['ann_mean_gross_cpp']
prints['predictions'] = {'realised_vol_ratio': vol, 'vol_ratio_lt_1': vol < 1,
                         'gross_alpha_ratio': gross, 'gross_ratio_in_band': .9 <= gross <= 1.1,
                         'expected_dsr_band': [.05, .15], 'ex_ante': 'CUT'}
save('p9h-registered-prints.json', prints)
print(json.dumps(prints, indent=2))

trace_path = ROOT / 'build-equity/mega-p9w-train-theme-cov-hedge-1/summary.json'
trace_doc = read(trace_path)
traces = []
def trace_only(value, pointer=''):
    if isinstance(value, dict):
        for key, child in value.items():
            location = pointer + '/' + key
            if key == 'composition_hedge_trace':
                traces.append({'pointer': location, 'composition_hedge_trace': child})
            else:
                trace_only(child, location)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            trace_only(child, pointer + '/' + str(index))
trace_only(trace_doc)
assert traces, 'registered composition hedge trace missing'
save('p9h-hedge-traces.json', {'source': str(trace_path.relative_to(ROOT)),
                            'source_sha256': sha(trace_path), 'traces': traces})
print('registered composition_hedge_trace blocks:', len(traces))

sys.path.insert(0, str(ROOT / 'atx-impl/tools'))
import backtest_integrity as BI
ledger = ROOT / 'build-equity/trials.jsonl'
raw = ledger.read_bytes()
lines = raw.splitlines(keepends=True)
before = 'b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61'
assert len(lines) == 135 and hashlib.sha256(b''.join(lines[:134])).hexdigest() == before
records = BI.ledger_read(ledger)
new = records[-1]
assert new['kind'] == 'construction' and new['cell'] == 'build-equity/mega-nav-p9-theme-cov-hedge-gm'
save('p9h-ledger-completion.json', {'before_lines': 134, 'before_sha256': before,
     'after_lines': 135, 'after_sha256': sha(ledger), 'prefix_unchanged': True,
     'new_row': {k: new.get(k) for k in ('trial_id', 'kind', 'cell', 'count', 'origin', 'prev_sha256')},
     'counts': BI.ledger_counts(records), 'N_hand': sum(BI.trial_counts(records)),
     'M': BI.campaign_registry_count(records), 'N_tot': sum(BI.trial_counts(records)) + BI.campaign_registry_count(records)})
accounting = '\n'.join(BI.appendix_a(records, 'build-equity/trials.jsonl')) + '\n' + BI.appendix_a_v8(records) + '\n'
(LOGS / 'p9h-accounting.txt').write_text(accounting, encoding='utf-8')
print(accounting)
