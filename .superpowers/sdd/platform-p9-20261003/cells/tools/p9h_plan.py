"""Read-only resolved H registration evidence; no execution or data statistics."""
import hashlib
import json
import sys
import subprocess
from pathlib import Path

sys.path.insert(0, str(Path.cwd() / 'scripts'))
import research_cycle as RC

for name in ('label-terminal-gm', 'theme-cov-hedge'):
    p = Path('scripts/specs/p9/templates') / (name + '.json')
    s = RC.load_spec(p)
    print(json.dumps({'path': str(p), 'sha256': hashlib.sha256(p.read_bytes()).hexdigest(),
        'resolved': {k: s.get(k) for k in ('name', 'python', 'inputs', 'fields', 'ic', 'fit', 'nav', 'runner', 'summ', 'exes_sha256')}}, indent=2))

if '--commit-body' in sys.argv:
    paths = ['scripts/specs/p9/templates/theme-cov-hedge.json',
             'scripts/specs/p9/rules/theme-cov-hedge-v1.json']
    diffs = []
    def diff(a, b, ptr=''):
        if isinstance(a, dict) and isinstance(b, dict):
            for k in sorted(set(a) | set(b)):
                diff(a.get(k), b.get(k), ptr + '/' + k)
        elif a != b:
            diffs.append({'pointer': ptr, 'before': a, 'after': b})
    body = ['P9-H registered', '', 'Closed registered JSON diff:']
    for p in paths:
        old = json.loads(subprocess.check_output(['git', 'show', 'HEAD:' + p], text=True))
        now = json.loads(Path(p).read_text(encoding='utf-8'))
        diffs.clear()
        diff(old, now)
        allowed = {'/parent', '/description'} if 'templates' in p else {'/manifest_fragment/rule_cell/template_sha256'}
        assert {d['pointer'] for d in diffs} == allowed
        body += [p, json.dumps(diffs, indent=2)]
    h = RC.load_spec(Path(paths[0]))
    flags = h['nav']['flags']
    assert flags.count('--ann-mean-gross') == flags.count('--info-clock-print') == 1
    assert 'baseline_fields' not in h['inputs']
    body += ['', 'New wave manifest: scripts/specs/p9/waves/p9h-cov-hedge.json. It embeds the registered fragment, uses accepted P9-L, pm7-34 and pm6-6-engine; retains the registered caps and free-memory floor; driver timing receipts only, no engine timing flag.',
             'Planned resolved NAV flags (inherited; each required print flag exactly once):', json.dumps(flags),
             'The derived cell is locked --exes --write by the supported driver spec stage before measurement; internal commit-route authorization is pending, so this registration does not launch it.',
             '', 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>', '']
    Path('.superpowers/sdd/platform-p9-20261003/cells/p9h-registration-commit.txt').write_text('\n'.join(body), encoding='utf-8')
