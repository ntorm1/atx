"""Metadata-only preparation: streaming identities and prospective argv; never runs readers."""
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path('C:/atx-wt/pool-2')
SDD = Path('.superpowers/sdd/platform-p9-20261003')
PY = 'C:/Program Files/Python312/python.exe'
EXE = 'build-equity-rel/bin/atx-research-eval.exe'
OUT = 'build-equity/p9-adoption-readers'
LEDGER = 'build-equity/trials.jsonl'
SCENARIO = 'modeled-1bn-stale5-v1+swap-fin-v1'
BOOKS = {'H': 'build-equity/mega-nav-p9-theme-cov-hedge-gm',
         'X': 'build-equity/mega-nav-p9x-leverage',
         'YF0': 'build-equity/p9-b0g-yf0-nav', 'R2': 'build-equity/p9-b0-r2-nav'}


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(4 << 20), b''):
            h.update(block)
    return h.hexdigest()


def row(path):
    return {'path': str(path).replace('\\', '/'), 'bytes': Path(path).stat().st_size, 'sha256': sha(path)}


def save(path, obj):
    with Path(path).open('x', encoding='utf-8', newline='\n') as f:
        json.dump(obj, f, indent=2)
        f.write('\n')


def main():
    assert Path.cwd().resolve() == ROOT.resolve()
    assert not Path(OUT).exists(), 'STOP: prospective output already exists'
    ledger = Path(LEDGER).read_bytes()
    assert len(ledger.splitlines()) == 140
    assert hashlib.sha256(ledger).hexdigest() == '086ca77e40806bb8f71039bd5e4b824325dcb2124b845b81ffd5784e364e8f5d'
    assert hashlib.sha256(b''.join(ledger.splitlines(keepends=True)[:139])).hexdigest() == '5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd'
    exes = json.loads((SDD / 'cells/logs/p9mv-frozen-exes.json').read_text())
    assert len(exes) == 54
    for r in exes:
        assert sha(r['path']) == r['sha256'], r['path']
    refs = []
    for name in ['p9-b0-r2-nav', 'p9-b0g-x10-nav', 'p9-b0g-y1-nav', 'p9-b0g-yf0-u', 'p9-b0g-yf0-w', 'p9-b0g-yf0-nav']:
        directory = Path('build-equity') / name
        files = sorted(p for p in directory.rglob('*') if p.is_file())
        listing = ''.join(f'{p.relative_to(directory).as_posix()}\t{p.stat().st_size}\t{sha(p)}\n' for p in files)
        record = SDD / 'gate-logs' / ('b0-files-' + name + '.txt')
        assert listing == record.read_text(encoding='utf-8')
        refs.append(dict(row(record), files=len(files), verified=True))
    assert sum(r['files'] for r in refs) == 126
    pins = {
      'C:/Windows/System32/ucrtbase.dll': '2c08e77f86b0953cffa505bdbe64e87ecb7c088db65d5acb9a2eee65a32565ef',
      'build-equity/train-2020-2023-lo3-fields-v15/manifest.json': '26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09',
      'build-equity/train-2020-2023-lo3/manifest.json': 'e1c6710104594b4777616714195e5ecc78f22fed7820577692b6423612d395f4',
      'build-equity/p9-m2i-dcov-v11/manifest.json': '397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7',
      'build-equity/p9-m2i-dcov-v11/model.atxcov': 'cd48d5749dd7484ca69ae6b0b9df142b73e2beb64c814d3c143982ec59bb2ce3',
      'build-equity/p9-m2i-dcov-covv1/manifest.json': 'ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3',
      'build-equity/p9-m2i-dcov-covv1/model.atxcov': '0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a',
      'scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json': '1d8eecaa077844d093b05a861db18787ead3223317154c25094cfc98f029e88f',
      'scripts/specs/p9/templates/p9x-leverage.json': 'e8a308b593b67c16111f3e4792c92983e9c5cfd412c7e42d485ecfe089b549db',
      'build-equity/cycle-v8y-vol-target-y-1/pbo.json': '2beb256ccd64325d28f0b7f95d3eae8e94b10b4ab57197f48c7681e7876f4ea6',
      'build-equity/cycle-v8ysb-gm/pbo.json': '556192c0161051011587467b86c848b916ae4ff8de3fbfbd79cd1d7b4f23aa10'}
    for path, pin in pins.items():
        assert sha(path) == pin, path
    role = json.loads(Path('build-equity/train-2020-2023-lo3/manifest.json').read_text())
    assert role['universe']['delisting']['returns_applied'] is False
    absent = ['build-equity/research-store', 'build-equity/mega-candidate-cache-v8-lo3/index.sqlite'] + [
      'build-equity/train-2020-2023-lo3-fields-' + v for v in ('v9', 'v10', 'v13', 'v14', 'v15a')]
    assert all(not Path(p).exists() for p in absent)
    sources = ['atx-impl/tools/nav_summ.py', 'atx-impl/tools/backtest_integrity.py', 'atx-impl/tools/dsr_total.py',
      'scripts/run_bounded_research.py', 'scripts/wave_seal.py', 'atx-engine/tools/research_window.py',
      'atx-engine/src/research/eval/eval_report.cpp', 'atx-engine/src/research/eval/eval_cli.cpp',
      'atx-engine/src/research/eval/eval_series.cpp', str(SDD / 'cells/tools/p9x_read_filter.py'),
      str(SDD / 'cells/tools/p9_adoption_conditional.py'), str(SDD / 'cells/tools/p9_adoption_prepare.py')]
    reuse = [SDD / ('cells/logs/' + p) for p in ['p9h-registered-prints.json', 'p9h-mechanics-first.json',
      'p9x-registered-prints.json', 'p9x-mechanics-first.json', 'p9x-final-output-sha256.txt',
      'p9c-clock-prints.json', 'p9mv-registered-prints.json', 'p9v-completion-audit.json',
      'p9v-accounting-v8.txt', 'p9v-final-output-sha256.txt']]
    reuse += [Path('build-equity/waves/p9h-cov-hedge-attempt2/readers/mech-matched.json'),
      Path('build-equity/p9x-leverage-readers/book.json'), Path('build-equity/p9x-leverage-readers/mechanics.json'),
      SDD / 'root-R0-12-report.md', SDD / 'p9-prereg.md']
    inputs = [row(LEDGER)] + [row(p) for p in pins] + [row(p) for p in reuse]
    nav_inputs = {}
    for name, directory in BOOKS.items():
        paths = [Path(directory) / 'summary.json', Path(directory) / 'capacity_curve.csv']
        paths += sorted(Path(directory).glob('daily_*.csv'))
        assert len(paths) == 7
        nav_inputs[name] = [row(p) for p in paths]
    commands = []
    common = [PY, 'atx-impl/tools/nav_summ.py', '--engine-stats', EXE, '--protocol', 'v8',
              '--scenario', SCENARIO, '--block', '21', '--seed', '20260929', '--draws', '4999']
    def command(name, argv, book_names, result, seconds=180, rss=1536):
        binds = [EXE] + sources + [LEDGER]
        binds += [r['path'] for book in book_names for r in nav_inputs[book]]
        binds = list(dict.fromkeys(binds))
        runner = [PY, 'scripts/run_bounded_research.py', '--seconds', str(seconds), '--max-rss-mib', str(rss),
                  '--min-free-mib', '512', '--output', OUT + '/' + name + '-run1']
        for p in binds:
            runner += ['--bind', p]
        runner += ['--'] + argv
        commands.append({'id': name, 'argv': runner, 'inner_argv': argv, 'output': result,
          'source_command': [PY, str(SDD / 'cells/tools/p9_adoption_conditional.py').replace('\\', '/'), 'source', name],
          'raw_console': str(SDD / ('cells/logs/p9-adoption-' + name + '-raw.log')).replace('\\', '/'),
          'source_paths': [OUT + '/' + name + '-run1/' + n for n in ('start.json', 'receipt.json', 'stdout.log', 'stderr.log')],
          'seconds': seconds, 'max_rss_mib': rss, 'min_free_mib': 512})
    command('dsr-years', common + [BOOKS['H'], BOOKS['X'], '--dsr-total', LEDGER, '--dsr-ledger', LEDGER,
      '--ledger-n', LEDGER, '--json', OUT + '/dsr-years.json'], ['H', 'X'], OUT + '/dsr-years.json', 600, 2048)
    for name, a, b in [('r2-h', 'R2', 'H'), ('yf0-h', 'YF0', 'H'), ('r2-yf0', 'R2', 'YF0')]:
        command(name, common + ['--bundle', BOOKS[a], BOOKS[b], '--bundle-json', OUT + '/' + name + '.json'],
                [a, b], OUT + '/' + name + '.json')
    command('conditional', [PY, str(SDD / 'cells/tools/p9_adoption_conditional.py').replace('\\', '/')],
            ['H'], OUT + '/conditional.json')
    for c in commands:
        assert '--ledger' not in c['inner_argv'] and '--pbo' not in c['inner_argv']
    proposal = {'status': 'PREPARATION ONLY; exact plan requires separate PM execution/read release',
      'cwd': ROOT.as_posix(), 'books': BOOKS, 'scenario': SCENARIO, 'ledger': row(LEDGER),
      'counts': {'lines': 140, 'N_c': 69, 'N_hand': 130, 'M': 110, 'N_tot': 240, 'history': 0},
      'source_hashes': [row(p) for p in sources], 'input_hashes': inputs, 'nav_inputs': nav_inputs,
      'commands': commands, 'read_plan': str(SDD / 'cells/p9-adoption-plan.md').replace('\\', '/'),
      'pbo': {'mode': 'reuse historical published scalars only, no new run', 'evidence': [row(p) for p in pins if p.endswith('/pbo.json')]}}
    save(SDD / 'cells/p9-adoption-proposal.json', proposal)
    crt = subprocess.check_output(['powershell', '-NoProfile', '-Command',
      "(Get-Item 'C:/Windows/System32/ucrtbase.dll').VersionInfo.FileVersion"], text=True).strip()
    assert crt.startswith('10.0.26100.7623')
    git = lambda *a: subprocess.check_output(['git', *a], text=True).strip()
    save(SDD / 'cells/logs/p9-adoption-start.json', {'mode': 'metadata only; no result parsing',
      'head': git('rev-parse', 'HEAD'), 'branch': git('branch', '--show-current'),
      'main': git('rev-parse', 'main'), 'origin_main': git('rev-parse', 'origin/main'),
      'status': git('status', '--short'), 'ledger': row(LEDGER), 'original139_prefix': 'verified',
      'frozen_exes': exes, 'references': refs, 'pins': [row(p) for p in pins],
      'crt_version': crt, 'role_returns_applied_is_false': True, 'absent': absent,
      'free_C_bytes': shutil.disk_usage('C:/').free, 'proposal_sha256': sha(SDD / 'cells/p9-adoption-proposal.json')})
    print('Preparation identities PASS: 54 exes, 126 reference files, CRT, both stores/models, v15, ledger140; no statistical command or result exposure.')


if __name__ == '__main__':
    main()
