"""P9-L B.1 printed-only rows, read after the committed mechanics PASS.

Only orientation signs, admission statuses, ic_missing_exit_v1 and the three named
coverage counters enter the output. No other candidate IC or admission statistic is emitted.
"""
import hashlib
import json
from pathlib import Path

def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8'))

def rows(p, key):
    return {x['id']: x[key] for x in read(p)['candidates']}

ob = rows('build-equity/p9-b0g-yf0-u/orientations.json', 'sign')
oc = rows('build-equity/mega-p9l-train-u-label-terminal-1/orientations.json', 'sign')
ab = rows('build-equity/mega-weights-v8x-theme-erc-v8ysb/admission.json', 'status')
ac = rows('build-equity/mega-weights-p9l-label-terminal/admission.json', 'status')
assert ob.keys() == oc.keys() and ab.keys() == ac.keys()
out = {'orientation_candidates': len(ob), 'orientation_flips': [
    {'id': k, 'parent': ob[k], 'cell': oc[k]} for k in sorted(ob) if ob[k] != oc[k]],
    'admission_candidates': len(ab), 'admission_changes': [
    {'id': k, 'parent': ab[k], 'cell': ac[k]} for k in sorted(ab) if ab[k] != ac[k]], 'ic': {}}
keys = ('terminal_imputed_pairs', 'missing_exit_share', 'guard_excluded_pairs')
for phase, directory in [('u', 'mega-p9l-train-u-label-terminal-1'),
                         ('w', 'mega-p9lw-train-label-terminal-2')]:
    role = read(f'build-equity/{directory}/summary.json')['roles'][0]
    out['ic'][phase] = {'candidates': []}
    for c in role['candidates']:
        out['ic'][phase]['candidates'].append({'id': c['id'],
            'ic_missing_exit_v1': c['ic_missing_exit_v1'],
            'coverage': [{'horizon': h['horizon'], **{k: h['coverage'][k] for k in keys}}
                         for h in c['horizons']]})
    out['ic'][phase]['combined_coverage'] = [
        {'horizon': h['horizon'], **{k: h['coverage'][k] for k in keys}}
        for h in role['combined_ic']['horizons']]
p = Path('.superpowers/sdd/platform-p9-20261003/cells/logs/p9l-printed-only.json')
p.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8', newline='\n')
print('orientation candidates', len(ob), 'flips', out['orientation_flips'])
print('admission candidates', len(ab), 'changes', out['admission_changes'])
for phase, d in out['ic'].items():
    print(phase, 'ic_missing_exit_v1 candidate blocks', len(d['candidates']))
    for h in d['combined_coverage']:
        same = all(h == next(x for x in c['coverage'] if x['horizon'] == h['horizon'])
                   for c in d['candidates'])
        print(phase, 'coverage', h, 'same for every candidate and combined', same)
print(p.as_posix(), 'sha256', hashlib.sha256(p.read_bytes()).hexdigest())
