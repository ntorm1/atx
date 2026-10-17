"""R15-M-READY: exact authorized registration and closed-delta evidence, no research run."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

base = Path('scripts/specs/p9')
p, r = base/'templates/mom-volman.json', base/'rules/mom-volman-v1.json'
old, rule = json.loads(p.read_text()), json.loads(r.read_text())
assert hashlib.sha256(p.read_bytes()).hexdigest() == '7e9b26c1bc4a6453447ccbf9fee2612ecb50a293330f239cfc93d035e45db6fb'
assert hashlib.sha256(r.read_bytes()).hexdigest() == '8a7222fa7d99b853abb102f87538fadf2ea4a17b8d22b698a913b64c58e2b493'
before = "with or without --theme-erc theme-erc-v1 (the parents the fitter's --mom-volman accepts, composition_mom_volman.PARENT_RULES)"
after = "with or without --theme-erc theme-erc-v1 or --theme-hedge theme-cov-hedge-v1 (the parents the fitter's --mom-volman accepts, composition_mom_volman.PARENT_RULES)"
assert old['description'].count(before) == 1
new = json.loads(json.dumps(old))
new['parent'] = 'theme-cov-hedge-v1-attempt2-gm.json'
new['description'] = new['description'].replace(before, after)
new['requires'] = []
p.write_text(json.dumps(new, indent=2)+'\n', encoding='utf-8', newline='\n')
rule['manifest_fragment']['rule_cell']['template_sha256'] = hashlib.sha256(p.read_bytes()).hexdigest()
r.write_text(json.dumps(rule, indent=2)+'\n', encoding='utf-8', newline='\n')
m = json.loads((base/'waves/p9h-cov-hedge-attempt2.json').read_text())
m.update(wave='p9m-mom-volman', description='P9-M prereg B.3, R15-M-READY: mom-volman-v1 on accepted P9-H; registered constants, pm7-34, pm6-6-engine. R15-RAM-NOW immediate bounded route.', out_dir='build-equity/waves/p9m-mom-volman')
m['parent']['spec'] = 'scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json'
m['expect']['n_before'] = 64
m['acceptance']['printed'] = ['capacity-4x-higher', 'turnover-per-gross-not-higher']
m['rule_cell'], m['rules'] = rule['manifest_fragment']['rule_cell'], rule['manifest_fragment']['rules']
w = base/'waves/p9m-mom-volman.json'
assert not w.exists()
w.write_text(json.dumps(m, indent=2)+'\n', encoding='utf-8', newline='\n')

def diff(a, b, path=''):
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a)|set(b)):
            out += diff(a.get(k), b.get(k), path+'/'+k)
    elif a != b:
        out.append((path, a, b))
    return out

td = diff(old, new)
rd = diff(json.loads(subprocess.check_output(['git', 'show', 'HEAD:'+r.as_posix()], text=True)), rule)
assert [x[0] for x in td] == ['/description', '/parent', '/requires']
assert [x[0] for x in rd] == ['/manifest_fragment/rule_cell/template_sha256']
sys.path.insert(0, str(Path.cwd()/'scripts'))
import research_cycle as RC
s = RC.load_spec(p)
assert '--engine-fit' not in s['fit']['flags']
assert 'baseline_fields' not in s['inputs']
assert s['nav']['flags'].count('--ann-mean-gross') == s['nav']['flags'].count('--info-clock-print') == 1
assert s['ic']['w_flags']['--max-memory-mib'] == '3584'
assert s['runner']['phases']['w'] == {'seconds': 300, 'max_rss_mib': 3584}
body = ['P9-M registered under R15-M-READY', '', 'Closed JSON delta (all unlisted leaves unchanged):', p.as_posix(),
        '/description: exactly one substring replacement; remainder byte-identical in the decoded string.',
        'BEFORE '+before, 'AFTER '+after]
for ptr, a, b in td:
    if ptr != '/description':
        body.append(json.dumps({'pointer': ptr, 'before': a, 'after': b}))
body += [r.as_posix()]+[json.dumps({'pointer': ptr, 'before': a, 'after': b}) for ptr, a, b in rd]
body += ['New manifest '+w.as_posix(), json.dumps(m, indent=2), 'Inherited NAV flags once: '+json.dumps(s['nav']['flags'])]
for f in (p, r, w):
    row = f.as_posix()+' SHA256 '+hashlib.sha256(f.read_bytes()).hexdigest()
    body.append(row)
    print(row)
body += ['', 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>']
Path('.superpowers/sdd/platform-p9-20261003/cells/p9m-registration-commit.txt').write_text('\n'.join(body)+'\n', encoding='utf-8')
print('closed pointers and inherited flags/caps PASS; generated spec/lock remain native')
