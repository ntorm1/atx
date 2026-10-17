"""P9-MV completion metadata/hash audit; no scientific values are emitted."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path.cwd() / 'scripts'))
import research_cycle as RC

LOGS = Path('.superpowers/sdd/platform-p9-20261003/cells/logs')
WAVE = Path('build-equity/waves/p9mv-mv-aim')


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def doc(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def txt(path):
    raw = Path(path).read_bytes()
    return raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig')


def save(name, value):
    (LOGS / name).write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8', newline='\n')


assert 'OTHER hits: 0' in txt(LOGS / 'p9mv-seal-complete.txt')
assert 'DIFFERS' not in txt(LOGS / 'p9mv-freeze-complete.txt')
exes = []
for tag in ('p9-3f', 'p9-3g'):
    for name, record in doc(f'build-equity/mega-{tag}-receipt.json')['Executables'].items():
        assert sha(record['Path']) == record['Sha256']
        exes.append({'path': record['Path'], 'sha256': record['Sha256'], 'tag': tag})
assert len(exes) == 54
save('p9mv-frozen-exes.json', exes)
spec = Path('scripts/specs/p9/templates/mv-aim-v1-gm.json')
s = RC.load_spec(spec)
parent = RC.load_spec(Path('scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json'))
assert s['fit'] == parent['fit'] and s['ic'] == parent['ic'] and s['card'] == parent['card']
assert s['nav']['rule'] == 'aim-partial-v5'
flags = s['nav']['flags']
expected = [('--cadence', '1'), ('--trade-fraction', '.05'), ('--target-rule', 'mv-aim-v1'),
            ('--risk-model', 'build-equity/p9-m2i-dcov-covv1'),
            ('--risk-model-sha256', 'ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3')]
for key, value in expected:
    assert flags.count(key) == 1 and flags[flags.index(key) + 1] == value
for key in ('--capacity-curve', '--ann-mean-gross', '--info-clock-print'):
    assert flags.count(key) == 1
assert not any(k in flags for k in ('--rate', '--two-speed', '--adv-hold-q', '--hold-band', '--construction-grid'))
assert '--engine-fit' not in s['fit']['flags']
assert s['runner'] == parent['runner']
inputs = []
for key, value in s['inputs'].items():
    if isinstance(value, dict) and 'path' in value and 'sha256' in value:
        assert sha(value['path']) == value['sha256']
        inputs.append({'key': key, 'path': value['path'], 'sha256': value['sha256']})
for path, pin in [
    ('build-equity/train-2020-2023-lo3-fields-v15/manifest.json', '26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09'),
    ('build-equity/p9-m2i-dcov-covv1/manifest.json', 'ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3'),
    ('build-equity/p9-m2i-dcov-covv1/model.atxcov', '0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a'),
    ('build-equity/p9-m2i-dcov-v11/manifest.json', '397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7')]:
    assert sha(path) == pin
    inputs.append({'path': path, 'sha256': pin})
assert doc('build-equity/train-2020-2023-lo3/manifest.json')['universe']['delisting']['returns_applied'] is False
commits = []
revs = subprocess.check_output(['git', 'rev-list', '--reverse', '3a317b85..HEAD'], text=True).splitlines()
expected_paths = ['scripts/specs/p9/templates/mv-aim-v1' + suffix + '.json' for suffix in ('', '-cal', '-gm')]
assert len(revs) == 3
for rev, path in zip(revs, expected_paths):
    paths = subprocess.check_output(['git', 'diff-tree', '--no-commit-id', '--name-only', '-r', rev], text=True).splitlines()
    body = subprocess.check_output(['git', 'show', '-s', '--format=%B', rev], text=True)
    assert paths == [path] and 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>' in body
    commits.append({'sha': rev, 'paths': paths, 'trailer': True})
raw = Path('build-equity/trials.jsonl').read_bytes()
lines = raw.splitlines(keepends=True)
assert len(lines) == 138
assert hashlib.sha256(b''.join(lines[:137])).hexdigest() == '21522bb42c9adf7859dbf042878338b3b0374b2f920055a8f2e59980c2a9a76b'
assert not Path('build-equity/research-store').exists()
cache = Path('build-equity/mega-candidate-cache-v8-lo3')
assert not (cache / 'index.sqlite').exists()
assert sum(p.is_file() for p in cache.rglob('*')) == 686
for version in ('v9', 'v10', 'v13', 'v14', 'v15a'):
    assert not Path('build-equity/train-2020-2023-lo3-fields-' + version).exists()
stages = {p.name: doc(p)['seconds'] for p in sorted((WAVE / 'receipts').glob('*.json'))}
out = {'generated_commits': commits, 'cell_spec_sha256': sha(spec), 'fit_reused_unchanged': True,
       'fit_flags': s['fit']['flags'], 'nav_flags': flags, 'nav_rule': s['nav']['rule'],
       'nav_output': s['nav']['output'], 'inputs': inputs, 'cache_files': 686,
       'ledger': {'lines': len(lines), 'sha256': hashlib.sha256(raw).hexdigest(), 'prefix137_unchanged': True},
       'stage_seconds': stages, 'frozen_exes': len(exes)}
save('p9mv-input-commit-audit.json', out)
roots = [WAVE] + [Path('build-equity') / ('cycle-mv-aim-v1' + suffix) for suffix in ('', '-cal', '-gm')]
roots += [Path(p['run_dir']) for p in doc(WAVE / 'receipts/08-judge.json')['outputs']['phases']]
receipts, seen = [], set()
for root in roots:
    for path in sorted(root.rglob('receipt.json')):
        if path in seen:
            continue
        seen.add(path)
        r = doc(path)
        assert r['outcome'] == 'completed'
        assert r['limits']['min_free_mib'] == 512 and r['wall_seconds'] <= r['limits']['seconds']
        cmd = r['command']
        nav = 'nav' in cmd and any(str(x).endswith('atx-equity-strategy-targets.exe') for x in cmd)
        if nav:
            for key, value in [('--rule', 'aim-partial-v5')] + expected:
                assert cmd.count(key) == 1 and cmd[cmd.index(key) + 1] == value
            assert cmd.count('--ann-mean-gross') == cmd.count('--info-clock-print') == 1
            assert '--rate' not in cmd
        receipts.append({'path': str(path), 'sha256': sha(path), 'outcome': r['outcome'],
                         'wall_seconds': r['wall_seconds'], 'limits': r['limits'],
                         'peak_mib': r['sampled_peak_tree_rss_bytes'] / 2**20,
                         'reused': 'mv-aim' not in path.as_posix(),
                         'historical_admission_waited_seconds': r.get('admission', {}).get('waited_seconds'),
                         'nav_command': cmd if nav else None})
assert sum(r['nav_command'] is not None for r in receipts) == 1
save('p9mv-final-receipt-flags.json', receipts)
print(json.dumps({'checks': 'PASS', 'frozen_exes': len(exes), 'generated_commits': commits,
                  'inputs_checked': len(inputs), 'cache_files': 686, 'ledger': out['ledger'],
                  'own_bounded_receipts': sum(not r['reused'] for r in receipts),
                  'reused_bounded_receipts': sum(r['reused'] for r in receipts), 'stage_seconds': stages}, indent=2))
