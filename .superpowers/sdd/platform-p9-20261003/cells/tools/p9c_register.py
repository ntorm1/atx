"""R15-C-READY exact registration; no research execution."""
import copy
import hashlib
import json
from pathlib import Path
import sys

base = Path('scripts/specs/p9')
p, r = base/'templates/info-clock.json', base/'rules/info-clock-rate-v1.json'
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
assert sha(p) == '95d4104a421c32f91877649f1ec9dfbdb409c9a121d6f6e2377e5e25e19b42d5'
assert sha(r) == '7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152'
old, old_rule = json.loads(p.read_text()), json.loads(r.read_text())
new, rule = copy.deepcopy(old), copy.deepcopy(old_rule)
assert old['parent'] is None and len(old['requires']) == 5
new['parent'], new['requires'] = 'theme-cov-hedge-v1-attempt2-gm.json', []
p.write_text(json.dumps(new, indent=2)+'\n', encoding='utf-8', newline='\n')
rule['manifest_fragment']['rule_cell']['template_sha256'] = sha(p)
r.write_text(json.dumps(rule, indent=2)+'\n', encoding='utf-8', newline='\n')
m = json.loads((base/'waves/p9m-mom-volman.json').read_text())
m.update(wave='p9c-info-clock', description='P9-C prereg B.4, R15-C-READY: info-clock-rate-v1 on accepted P9-H; registered constants, pm7-34, pm6-6-engine. R15-RAM-NOW immediate bounded route.', out_dir='build-equity/waves/p9c-info-clock')
m['expect']['n_before'] = 65
m['acceptance']['printed'] = ['capacity-4x-higher', 'cost-bps-lower']
m['rule_cell'], m['rules'] = rule['manifest_fragment']['rule_cell'], rule['manifest_fragment']['rules']
w = base/'waves/p9c-info-clock.json'
assert not w.exists()
w.write_text(json.dumps(m, indent=2)+'\n', encoding='utf-8', newline='\n')
def diff(a,b,path=''):
    if isinstance(a,dict) and isinstance(b,dict):
        return [d for k in sorted(set(a)|set(b)) for d in diff(a.get(k),b.get(k),path+'/'+k)]
    return [] if a == b else [(path,a,b)]
td, rd = diff(old,new), diff(old_rule,rule)
assert [d[0] for d in td] == ['/parent','/requires']
assert [d[0] for d in rd] == ['/manifest_fragment/rule_cell/template_sha256']
sys.path.insert(0,str(Path.cwd()/'scripts'))
import research_cycle as RC
s = RC.load_spec(p)
f = s['nav']['flags']
assert s['nav']['rule'] == 'aim-partial-v5'
for flag,value in [('--cadence','1'),('--trade-fraction','.05')]:
    assert f.count(flag) == 1 and f[f.index(flag)+1] == value
for flag in ['--ann-mean-gross','--info-clock-print','--capacity-curve']:
    assert f.count(flag) == 1
assert '--rate' not in f and '--two-speed' not in f and '--engine-fit' not in s['fit']['flags']
assert s['runner']['phases']['w'] == {'seconds':300,'max_rss_mib':3584}
body = ['P9-C registered under R15-C-READY; all unlisted leaves unchanged.',p.as_posix()]
body += [json.dumps({'pointer':d[0],'before':d[1],'after':d[2]}) for d in td]
body += [r.as_posix()]+[json.dumps({'pointer':d[0],'before':d[1],'after':d[2]}) for d in rd]
body += ['New manifest '+w.as_posix(),json.dumps(m,indent=2),'Inherited NAV flags: '+json.dumps(f)]
for path in [p,r,w]:
    line = path.as_posix()+' SHA256 '+sha(path)
    body.append(line)
    print(line)
Path('.superpowers/sdd/platform-p9-20261003/cells/p9c-registration-commit.txt').write_text('\n'.join(body)+'\n',encoding='utf-8')
print('Exact closed delta and inherited parent/flags/caps PASS; rule-owned rate remains native derivation')
