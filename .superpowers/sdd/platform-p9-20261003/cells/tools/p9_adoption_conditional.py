"""Prospective G-B7 adapter. DO NOT EXECUTE before the exact adoption release.

Only existing nav_summ series extraction/serialization and frozen native eval;
no statistical calculation or result projection in this adapter.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import re

ROOT = Path('C:/atx-wt/pool-2')
OUT = Path('build-equity/p9-adoption-readers')
EXE = Path('build-equity-rel/bin/atx-research-eval.exe')
PIN = 'e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c'
STEPS = ['R-2=.0529,.0945', 'X-2=.0109,.0603', 'X-3=.1536,.1200',
         'X-5=.3490,.2011', 'Y-S=.0800,.1502',
         'H=.02554011056222616,.2615496420444564']
SDD = Path('.superpowers/sdd/platform-p9-20261003')
HELPERS = [str(SDD / ('cells/tools/' + n)).replace('\\', '/') for n in
           ('p9x_read_filter.py', 'p9_adoption_conditional.py', 'p9_adoption_prepare.py')]


def source_module():
    spec = importlib.util.spec_from_file_location('adoption_source', HELPERS[0])
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def normalized(path):
    return str(Path(path).resolve()).replace('\\', '/').casefold()


def metadata_ok(value, expected):
    command = value.get('command')
    want = expected['command']
    assert isinstance(command, list) and len(command) == len(want)
    assert normalized(command[0]) == normalized(want[0]) and command[1:] == want[1:], 'STOP: source argv mismatch'
    bindings = value.get('bindings')
    assert isinstance(bindings, list) and len(bindings) == len(expected['bindings'])
    for a, b in zip(bindings, expected['bindings']):
        assert set(a) == {'path', 'sha256'}
        assert normalized(a['path']) == normalized(b['path']) and a['sha256'] == b['sha256'], 'STOP: source binding mismatch'


def own_script_spans(text, start, expected, mod):
    value, end = json.JSONDecoder().raw_decode(text, start)
    metadata_ok(value, expected)
    allowed = set(HELPERS + [str((ROOT / p).resolve()) for p in HELPERS])
    spans = []
    decoder = json.JSONDecoder()
    for key, value, a, b in mod.fields(text, start):
        if key not in ('command', 'bindings'):
            continue
        i = a + 1
        while i < b - 1:
            while text[i].isspace() or text[i] == ',':
                i += 1
            if text[i] == ']':
                break
            item, finish = decoder.raw_decode(text, i)
            if key == 'command' and isinstance(item, str) and item in allowed:
                spans.append((i + 1, finish - 1))
            elif key == 'bindings' and isinstance(item, dict):
                for field, path, p, q in mod.fields(text, i):
                    if field == 'path' and isinstance(path, str) and path in allowed:
                        spans.append((p + 1, q - 1))
            i = finish
    return spans, end


def classify(text, kind, expected, mod, receipt_text=None):
    spans = []
    receipt = json.loads(receipt_text) if receipt_text is not None else None
    if kind == 'receipt':
        spans, _ = own_script_spans(text, len(text) - len(text.lstrip()), expected, mod)
    elif receipt_text is not None:
        # Exact bytes, not merely equal parsed JSON, for OWN_SCRIPT in an echo.
        for match in re.finditer(re.escape(receipt_text.strip()), text):
            found, _ = own_script_spans(text, match.start(), expected, mod)
            spans.extend(found)
    # Mask only the exact own-script sprint token within validated complete values.
    # Preserve byte positions/newlines for the original, unchanged source classifier.
    masked = list(text)
    owned = []
    for match in re.finditer('20261003', text):
        if any(a <= match.start() and match.end() <= b for a, b in spans):
            owned.append({'line': text.count('\n', 0, match.start()) + 1,
                          'token': '20261003', 'form': 'compact', 'source': 'OWN_SCRIPT'})
            masked[match.start():match.end()] = 'x' * 8
    masked_text = ''.join(masked)
    masked_receipt = None
    if receipt is not None:
        # Existing receipt predicates operate on the equally masked exact echo.
        masked_receipt_text = receipt_text
        for path in HELPERS + [str((ROOT / p).resolve()) for p in HELPERS]:
            masked_receipt_text = masked_receipt_text.replace(json.dumps(path), json.dumps(path.replace('20261003', 'xxxxxxxx')))
        masked_receipt = json.loads(masked_receipt_text)
    return mod.source_text(masked_text, kind, masked_receipt) + owned


def source(stage):
    mod = source_module()
    proposal = mod.doc(SDD / 'cells/p9-adoption-proposal.json')
    command = next(c for c in proposal['commands'] if c['id'] == stage)
    hashes = {normalized(r['path']): r['sha256'] for r in proposal['source_hashes'] + proposal['input_hashes']}
    for rows in proposal['nav_inputs'].values():
        hashes.update({normalized(r['path']): r['sha256'] for r in rows})
    hashes[normalized(EXE)] = PIN
    argv = command['argv']; paths = [argv[k + 1] for k, token in enumerate(argv) if token == '--bind']
    expected = {'command': command['inner_argv'], 'bindings': [{'path': p, 'sha256': hashes[normalized(p)]} for p in paths]}
    for binding in expected['bindings']:
        assert mod.sha(binding['path']) == binding['sha256'], 'STOP: current source/input hash mismatch'
    receipt_text = Path(command['source_paths'][1]).read_text(encoding='utf-8-sig')
    rows = []
    for path in command['source_paths'] + [command['raw_console']]:
        text = Path(path).read_text(encoding='utf-8-sig')
        kind = 'receipt' if Path(path).name in ('start.json', 'receipt.json') else 'raw'
        hits = classify(text, kind, expected, mod, receipt_text)
        rows.append({'path': path, 'sha256': mod.sha(path), 'hits': hits})
    result = {'stage': stage, 'files': rows, 'other': sum(h['source'] == 'OTHER' for r in rows for h in r['hits'])}
    mod.save(SDD / ('cells/logs/p9-adoption-' + stage + '-source.json'), result)
    print(json.dumps(result, indent=2))
    assert result['other'] == 0, 'STOP: source OTHER'


def controls():
    mod = source_module()
    expected = {'command': ['python.exe', HELPERS[1]],
                'bindings': [{'path': str((ROOT / HELPERS[1]).resolve()), 'sha256': 'a' * 64}]}
    good = dict(expected, started_utc='2026-10-17T12:00:00+00:00', dirty_outside_pathspec=sorted(mod.PNGS))
    encoded = json.dumps(good, indent=2)
    for kind, text in [('receipt', encoded), ('raw', 'prefix\n' + encoded + '\nsuffix')]:
        hits = classify(text, kind, expected, mod, encoded)
        assert sum(h['source'] == 'OWN_SCRIPT' for h in hits) == 2
        assert all(h['source'] != 'OTHER' for h in hits)
    wrong_field = dict(good, unrelated=HELPERS[1])
    assert any(h['source'] == 'OTHER' for h in classify(json.dumps(wrong_field), 'receipt', expected, mod))
    assert any(h['source'] == 'OTHER' for h in classify('outside ' + HELPERS[1], 'raw', expected, mod, encoded))
    unequal_echo = encoded.replace('12:00:00', '12:00:01')
    assert any(h['source'] == 'OTHER' for h in classify(unequal_echo, 'raw', expected, mod, encoded))
    for change in ('root', 'argv', 'hash'):
        bad = json.loads(encoded)
        if change == 'root':
            bad['bindings'][0]['path'] = str(Path('C:/wrong') / HELPERS[1])
        elif change == 'argv':
            bad['command'].append('--unexpected')
        else:
            bad['bindings'][0]['sha256'] = 'b' * 64
        try:
            classify(json.dumps(bad), 'receipt', expected, mod)
        except AssertionError:
            pass
        else:
            raise AssertionError('negative not refused: ' + change)
    return {'positive_receipt_and_equal_echo': 'PASS', 'wrong_field_root_argv_hash_echo_and_free_text': 'PASS',
            'scientific_input_reads': 0, 'statistical_commands': 0}


def main():
    assert Path.cwd().resolve() == ROOT.resolve()
    assert hashlib.sha256(EXE.read_bytes()).hexdigest() == PIN
    sys.path.insert(0, str(ROOT / 'atx-impl/tools'))
    spec = importlib.util.spec_from_file_location('adoption_nav_summ', 'atx-impl/tools/nav_summ.py')
    nav = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(nav)
    nets = nav.net_series(nav.load_daily(Path('build-equity/mega-nav-p9-theme-cov-hedge-gm'),
                                       'modeled-1bn-stale5-v1+swap-fin-v1'))
    series = OUT / 'conditional-input.csv'
    # Byte form copied from the existing EngineStats.csv, no statistics.
    with series.open('x', encoding='ascii', newline='\n') as f:
        f.write('session_ns,net_return\n' + ''.join(f'{int(s)},{float(v)!r}\n' for s, v in nets.items()))
    argv = [str(EXE), '--daily', 'H=' + series.as_posix(), '--protocol', 'v8',
            '--threshold', '0', '--conditional-alpha', '.05']
    for step in STEPS:
        argv += ['--step', step]
    argv += ['--json', '--out', (OUT / 'conditional.json').as_posix()]
    assert not (OUT / 'conditional.json').exists()
    with (OUT / 'conditional-command.json').open('x', encoding='utf-8') as f:
        json.dump({'argv': argv, 'engine_sha256': PIN,
                   'input_sha256': hashlib.sha256(series.read_bytes()).hexdigest()}, f, indent=2)
        f.write('\n')
    # The surrounding bounded runner captures full stdout/stderr, never a console read.
    return subprocess.run(argv, check=False).returncode


if __name__ == '__main__':
    if sys.argv[1:] == ['controls']:
        print(json.dumps(controls(), indent=2))
    elif len(sys.argv) == 3 and sys.argv[1] == 'source':
        source(sys.argv[2])
    elif not sys.argv[1:]:
        raise SystemExit(main())
    else:
        raise SystemExit('usage: p9_adoption_conditional.py [controls|source STAGE]')
