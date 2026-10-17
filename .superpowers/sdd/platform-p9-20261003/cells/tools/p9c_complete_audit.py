"""C completion metadata, hashes and timing only; no result statistics."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0,str(Path.cwd()/'scripts'))
import research_cycle as RC
LOGS=Path('.superpowers/sdd/platform-p9-20261003/cells/logs')
WAVE=Path('build-equity/waves/p9c-info-clock')
def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def doc(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))
def text(p):
    b=Path(p).read_bytes()
    return b.decode('utf-16' if b.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
def write(name,obj):
    (LOGS/name).write_text(json.dumps(obj,indent=2)+'\n',encoding='utf-8')

assert 'OTHER hits: 0' in text(LOGS/'p9c-seal-complete.txt')
assert 'DIFFERS' not in text(LOGS/'p9c-freeze-complete.txt')
exes=[]
for label,directory in [('rel','build-equity-rel/bin'),('dbg','build-equity/bin')]:
    for line in text(LOGS/f'p9m-exe-{label}-complete.txt').splitlines():
        m=re.match(r'([^:]+):.*sha256 ([a-f0-9]{64}) disk = receipt',line)
        if m:
            p=Path(directory)/(m[1]+'.exe')
            assert sha(p)==m[2],str(p)
            exes.append({'path':str(p),'sha256':m[2]})
assert len(exes)==54,len(exes)
write('p9c-frozen-exes.json',exes)
spec=Path('scripts/specs/p9/templates/info-clock-rate-v1-gm.json')
s=RC.load_spec(spec)
parent=RC.load_spec(Path('scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json'))
assert s['fit']==parent['fit'] and s['ic']==parent['ic']
assert s['nav']['rule']=='aim-partial-v5'
flags=s['nav']['flags']
for k,v in [('--cadence','1'),('--trade-fraction','.05'),('--rate','info-clock-rate-v1')]:
    assert flags.count(k)==1 and flags[flags.index(k)+1]==v
for k in ['--capacity-curve','--info-clock-print','--ann-mean-gross']:
    assert flags.count(k)==1
assert '--two-speed' not in flags and '--engine-fit' not in s['fit']['flags']
assert s['runner']['phases']['w']=={'seconds':300,'max_rss_mib':3584}
for k,h in s['exes_sha256'].items():
    assert sha(s['exes'][k])==h
inputs=[]
for k,v in s['inputs'].items():
    if isinstance(v,dict) and 'path' in v and 'sha256' in v:
        assert sha(v['path'])==v['sha256']
        inputs.append({'key':k,'path':v['path'],'sha256':v['sha256']})
for p,h in [('build-equity/train-2020-2023-lo3-fields-v15/manifest.json','26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09'),
 ('build-equity/p9-m2i-dcov-v11/manifest.json','397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7'),
 ('build-equity/p9-m2i-dcov-covv1/manifest.json','ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3'),
 ('build-equity/mega-weights-p9-theme-cov-hedge/composition_weights.json','153943b58587d6023917f410af2088987b185384d25415ae5ceec89850b352ae')]:
    assert sha(p)==h
    inputs.append({'path':p,'sha256':h})
commits=[]
revs=subprocess.check_output(['git','rev-list','--reverse','c2b73ade..HEAD'],text=True).splitlines()
expected=['scripts/specs/p9/templates/info-clock-rate-v1.json','scripts/specs/p9/templates/info-clock-rate-v1-cal.json','scripts/specs/p9/templates/info-clock-rate-v1-gm.json']
assert len(revs)==len(expected)
for rev,path in zip(revs,expected):
    paths=subprocess.check_output(['git','diff-tree','--no-commit-id','--name-only','-r',rev],text=True).splitlines()
    body=subprocess.check_output(['git','show','-s','--format=%B',rev],text=True)
    assert paths==[path] and 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>' in body
    commits.append({'sha':rev,'paths':paths,'trailer':True})
raw=Path('build-equity/trials.jsonl').read_bytes()
lines=raw.splitlines(keepends=True)
assert len(lines)==137
assert hashlib.sha256(b''.join(lines[:136])).hexdigest()=='c98dd0c4d59bada990fb34c7bf5efc3ae59873acc313de771a2d025a449013b7'
assert not Path('build-equity/research-store').exists()
assert not Path('build-equity/mega-candidate-cache-v8-lo3/index.sqlite').exists()
cache=sum(p.is_file() for p in Path('build-equity/mega-candidate-cache-v8-lo3').rglob('*'))
assert cache==686
stages={p.name:doc(p)['seconds'] for p in sorted((WAVE/'receipts').glob('*.json'))}
out={'generated_commits':commits,'cell_spec_sha256':sha(spec),'fit_reused_unchanged':True,
 'fit_flags':s['fit']['flags'],'nav_flags':flags,'nav_rule':s['nav']['rule'],'nav_output':s['nav']['output'],
 'leverage':s['nav']['leverage'],'inputs':inputs,'cache_files':cache,
 'ledger':{'lines':len(lines),'sha256':hashlib.sha256(raw).hexdigest(),'prefix136_unchanged':True},
 'stage_seconds':stages,'frozen_exes':len(exes)}
write('p9c-input-commit-audit.json',out)
roots=[WAVE]+[Path('build-equity')/('cycle-info-clock-rate-v1'+suffix) for suffix in ['', '-cal','-gm']]
phases=doc(WAVE/'receipts/08-judge.json')['outputs']['phases']
roots += [Path(p['run_dir']) for p in phases]
receipts=[]
seen=set()
for root in roots:
    for p in sorted(root.rglob('receipt.json')):
        if p in seen:
            continue
        seen.add(p)
        r=doc(p)
        assert r['outcome']=='completed'
        assert r['limits']['min_free_mib']==512
        assert r['wall_seconds']<=r['limits']['seconds']
        cmd=r['command']
        nav='nav' in cmd and any(str(x).endswith('atx-equity-strategy-targets.exe') for x in cmd)
        if nav:
            for k,v in [('--rule','aim-partial-v5'),('--rate','info-clock-rate-v1'),('--trade-fraction','.05'),('--cadence','1')]:
                assert cmd.count(k)==1 and cmd[cmd.index(k)+1]==v
            for k in ['--ann-mean-gross','--info-clock-print']:
                assert cmd.count(k)==1
        receipts.append({'path':str(p),'sha256':sha(p),'outcome':r['outcome'],
            'wall_seconds':r['wall_seconds'],
            'sampled_peak_tree_rss_bytes':r['sampled_peak_tree_rss_bytes'],
            'peak_mib':r['sampled_peak_tree_rss_bytes']/2**20,'limits':r['limits'],
            'configured_admission_wait_seconds':r['limits'].get('admission_wait_seconds'),
            'historical_admission_waited_seconds':r.get('admission',{}).get('waited_seconds'),
            'reused':not ('info-clock' in p.as_posix()),
            'nav_command':cmd if nav else None})
assert sum(r['nav_command'] is not None for r in receipts)==1
write('p9c-final-receipt-flags.json',receipts)
print(json.dumps({'checks':'PASS','frozen_exes':len(exes),'generated_commits':commits,
 'inputs_checked':len(inputs),'cache_files':cache,'own_bounded_receipts':sum(not r['reused'] for r in receipts),
 'reused_bounded_receipts':sum(r['reused'] for r in receipts),
 'ledger':out['ledger'],'stage_seconds':stages},indent=2))
