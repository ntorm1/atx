"""Preparation only: streamed integrity and closed, unexecuted P9-V proposal.
No scientific payload values are decoded and no research producer is launched.
"""
import copy
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()
SDD = ROOT / '.superpowers/sdd/platform-p9-20261003'
CELL = SDD / 'cells'
sys.path[:0] = [str(ROOT / 'scripts'), str(ROOT / 'atx-impl/tools'), str(CELL / 'tools')]
import start_state as SS
import backtest_integrity as BI
import research_spec
from cycle.spec import load_spec

def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))

def sha(p):
    return SS.sha_file(Path(p))

def save(p, obj):
    Path(p).write_text(json.dumps(obj, indent=2, allow_nan=False) + '\n', encoding='utf-8')

def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()

def replace_tokens(argv, mapping):
    return [mapping.get(x, x) for x in argv]

PY = 'C:/Program Files/Python312/python.exe'
X = 'build-equity/mega-nav-p9x-leverage'
V = 'build-equity/mega-nav-p9v-vol-target-cov'
H = 'build-equity/mega-nav-p9-theme-cov-hedge-gm'
ROLE = 'build-equity/train-2020-2023-lo3/manifest.json'
FIELDS = 'build-equity/train-2020-2023-lo3-fields-v15/manifest.json'
assert git('branch', '--show-current') == 'feat/platform-p9-20261003'
assert git('rev-parse', 'HEAD') == 'cb1c8e37b2201c6fb02146ad6fec1a49ed071da0'
state = {'head': git('rev-parse', 'HEAD'), 'branch': git('branch', '--show-current'),
         'status': git('status', '--short'), 'model': 'GPT-6 Astra', 'host': SS.host(),
         'crt': SS.ucrt(), 'policy': 'No wait, build, measurement, PNG read/hash or spec mutation.'}
assert 'DIFFERS' not in state['crt']
state['source_diff_from_receipt'] = git('diff', '--name-only', '5f48a25d', 'HEAD', '--',
    'atx-engine', 'atx-impl', 'CMakeLists.txt', 'cmake')
assert not state['source_diff_from_receipt']
state['delta_since_repin'] = git('diff', '--name-only', '2078f877', 'HEAD', '--', '.', ':(exclude).superpowers')
state['executables'] = []
for tag in ('p9-3f', 'p9-3g'):
    receipt = read(f'build-equity/mega-{tag}-receipt.json')
    assert receipt['Source'] == '5f48a25d3730789914941fbc7aa42f4458cea6b1'
    for name, item in sorted(receipt['Executables'].items()):
        digest = sha(item['Path'])
        assert digest.lower() == item['Sha256'].lower(), name
        state['executables'].append({'tag': tag, 'name': name, 'sha256': digest, 'match': True})
assert len(state['executables']) == 54
state['references'] = []
for name in SS.REFS:
    actual = SS.b0_listing(ROOT / 'build-equity' / name)
    expected = (SDD / 'gate-logs' / f'b0-files-{name}.txt').read_text(encoding='utf-8')
    assert actual == expected, name
    state['references'].append({'directory': name, 'files': actual.count('\n'),
        'listing_sha256': hashlib.sha256(actual.encode()).hexdigest(), 'match': True})
assert sum(x['files'] for x in state['references']) == 126
ledger = ROOT / 'build-equity/trials.jsonl'
raw = ledger.read_bytes()
state['ledger'] = {'lines': len(raw.splitlines()), 'sha256': hashlib.sha256(raw).hexdigest(),
    'prefix138_sha256': hashlib.sha256(b''.join(raw.splitlines(keepends=True)[:138])).hexdigest()}
assert state['ledger']['sha256'] == '5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd'
assert state['ledger']['prefix138_sha256'] == 'fe1bc97ecf9ccef7f042d637730581c3410792798ef2ab0a4a7a7fa2bfbc05f8'
records = BI.ledger_read(ledger)
state['accounting'] = {'counts': BI.ledger_counts(records), 'N_hand': sum(BI.trial_counts(records)),
    'M': BI.campaign_registry_count(records), 'history_reads': len(BI.history_read_lines(records))}
state['accounting']['N_tot'] = state['accounting']['N_hand'] + state['accounting']['M']
assert (state['accounting']['N_hand'], state['accounting']['M'], state['accounting']['N_tot']) == (129,110,239)
cache = Path('build-equity/mega-candidate-cache-v8-lo3')
state['cache_files'] = sum(p.is_file() for p in cache.rglob('*'))
assert state['cache_files'] == 686
absent = ['build-equity/research-store', str(cache / 'index.sqlite')]
absent += [f'build-equity/train-2020-2023-lo3-fields-{x}' for x in ('v9','v10','v13','v14','v15a')]
state['absent'] = {p: not Path(p).exists() for p in absent}
assert all(state['absent'].values())
state['stores'] = {}
for key, expected in [('v11','397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7'),
                      ('covv1','ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3')]:
    directory = Path(f'build-equity/p9-m2i-dcov-{key}')
    m = read(directory / 'manifest.json')
    assert sha(directory / 'manifest.json') == expected
    container = m['container']
    assert sha(directory / container['file']) == container['file_sha256']
    assert (directory / container['file']).stat().st_size == container['bytes']
    state['stores'][key] = {'manifest_sha256': expected, 'container': container,
        'producer': {k: m['producer'][k] for k in ('engine_git_sha','executable_sha256')}}
fields15 = read(FIELDS)
assert sha(FIELDS) == '26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09'
fields11path = Path('build-equity/train-2020-2023-lo3-fields-v11/manifest.json')
fields11 = read(fields11path)
names = read('scripts/specs/v8/lib-v80.json')['fields']['list']
assert len(names) == len(set(names)) == 70
by11 = {f['name']: f for f in fields11['fields']}
by15 = {f['name']: f for f in fields15['fields']}
risk_source = Path('atx-impl/src/strategy_risk_verb.cpp').read_text(encoding='utf-8')
risk_names = sorted(set(re.findall(r'f\.load\("([^"]+)"', risk_source)))
assert len(risk_names) == 11 and set(risk_names) <= set(names)
witness = {'witness_sha256': sha(SDD / 'gate-logs/fields-witness.txt'),
    'v11_manifest_sha256': sha(fields11path), 'v15_manifest_sha256': sha(FIELDS),
    'risk_source_sha256': sha('atx-impl/src/strategy_risk_verb.cpp'), 'risk_fields': risk_names, 'rows': []}
for name in names:
    old, new = by11[name], by15[name]
    reuse = old['reused_from']
    digest = sha(Path(FIELDS).parent / new['file'])
    assert reuse['manifest_sha256'] == 'a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809'
    assert reuse['payload_sha256'] == old['sha256'] == new['sha256'] == fields15['files'][new['file']]['sha256'] == digest
    assert new['point_in_time'] is True
    witness['rows'].append({'name': name, 'sha256': digest, 'risk_read': name in risk_names, 'match': True})
for recipe in ('v11','covv1'):
    m = read(f'build-equity/p9-m2i-dcov-{recipe}/manifest.json')
    assert set(m['fields']['fields_used']) == set(risk_names)
    for name in risk_names:
        assert m['fields']['fields_used'][name]['sha256'] == by15[name]['sha256']
state['fields'] = {'manifest_sha256': sha(FIELDS), 'payloads': len(fields15['files']), 'witnessed': len(witness['rows']), 'risk_fields_covered': len(risk_names)}
assert read(ROLE)['universe']['delisting']['returns_applied'] is False
state['COV9_returns_applied'] = False
save(CELL / 'logs/p9v-fields-witness.json', witness)

xp = read(CELL / 'p9x-leverage-proposal.json')
old = read('scripts/specs/p9/templates/p9x-leverage.json')
template = copy.deepcopy(old)
template.update(name='p9v-vol-target-cov', parent='p9x-leverage.json',
    description='P9-V prereg B.8: replace only the X vol-target risk store by pinned covv1. Hand v1 cycle, no gross matching; independent derivation before trial write.')
template['change']['set']['nav.output'] = V
template['change']['set']['monitor.output'] = 'build-equity/mega-monitor-p9v-vol-target-cov'
template['change']['flags']['nav']['--risk-model'] = 'build-equity/p9-m2i-dcov-covv1'
template['change']['flags']['nav']['--risk-model-sha256'] = state['stores']['covv1']['manifest_sha256']
template['locked']['reference_cell'] = {'path': X+'/summary.json', 'sha256': sha(X+'/summary.json')}
template_path = Path('scripts/specs/p9/templates/p9v-vol-target-cov.json')
assert not template_path.exists()
resolved = research_spec.resolve(template, template_path, load_spec, ROOT)
xresolved = load_spec(Path('scripts/specs/p9/templates/p9x-leverage.json'))
assert resolved['fit'] == xresolved['fit']
assert resolved['ic'] == xresolved['ic']
flags = resolved['nav']['flags']
assert all(flags.count(x) == 1 for x in ('--vol-target','--risk-model','--risk-model-sha256','--ann-mean-gross','--info-clock-print'))
assert resolved['nav']['rule'] == 'aim-partial-v5'
assert '--engine-fit' not in resolved['fit']['flags']
def delta(a,b,p=''):
    if isinstance(a,dict) and isinstance(b,dict):
        return [d for k in sorted(set(a)|set(b)) for d in delta(a.get(k),b.get(k),p+'/'+k)]
    return [] if a == b else [{'path':p,'old':a,'new':b}]
mapping = {'p9x-leverage':'p9v-vol-target-cov', X:V,
    X+'-run':V+'-run','build-equity/p9-m2i-dcov-v11':'build-equity/p9-m2i-dcov-covv1',
    state['stores']['v11']['manifest_sha256']:state['stores']['covv1']['manifest_sha256']}
nav = replace_tokens(xp['native_bounded_nav_argv'],mapping)
proposal = {'status':'PREPARATION ONLY; no V spec written or measurement launched',
    'base_head':state['head'], 'template_path':template_path.as_posix(),'template':template,
    'template_sha256': hashlib.sha256((json.dumps(template,indent=2)+'\n').encode()).hexdigest(),
    'closed_template_delta_vs_X':delta(old,template), 'resolved_delta_vs_X':delta(xresolved,resolved),
    'gross_match':'none (protocol; not an unsupported JSON key)', 'limits':xp['limits'],
    'mechanics_parent_source':xp['mechanics_parent_source'],'stores':state['stores'],
    'native_bounded_nav_argv':nav,'cycle_command':[PY,'-u','scripts/research_cycle.py','run',template_path.as_posix(),'--stop-after','nav'],
    'scenario':xp['scenario'], 'inherited_runner':xp['inherited_runner'], 'inherited_fit_flags':xp['inherited_fit_flags']}
for key in ('mechanics_command','paired_bundle_command','book_command'):
    def translate(s):
        s=s.replace('p9x-leverage-readers','p9v-vol-target-cov-readers')
        s=s.replace(X,V)
        if key == 'paired_bundle_command': s=s.replace(H,X)
        return s
    proposal[key]=[translate(s) for s in xp[key]]
proposal['book_command'] = [s.replace('yf0=build-equity/p9-b0g-yf0-nav','x='+X) for s in proposal['book_command']]
proposal['book_command'] = [s.replace('build-equity/p9-b0g-yf0-nav', X) for s in proposal['book_command']]
def bounded(run,cmd,binds,seconds=600,rss=2048):
    return [PY,'scripts/run_bounded_research.py','--seconds',str(seconds),'--max-rss-mib',str(rss),
        '--min-free-mib','512','--output',run]+[z for p in binds for z in ('--bind',p)]+['--']+cmd
diag = 'build-equity/p9v-b8a'
hreceipt = H+'-run/receipt.json'
hc = read(hreceipt)['command']
assert hc.count('--output') == 1 and '--emit-holdings' not in hc
step0 = replace_tokens(hc,{H:diag+'-h-nav'})+['--emit-holdings',diag+'-holdings']
step1 = [PY,'atx-impl/tools/book_diagnostics.py','book-csv','--holdings',diag+'-holdings',
    '--role',ROLE,'--role-sha256',sha(ROLE),'--output',diag+'-book.csv']
diagnostic = {'status':'UNEXECUTED; prerequisite field coverage holds; identity and all actual stages remain unobserved',
    'H_receipt':{'path':hreceipt,'sha256':sha(hreceipt)}, 'H_original_argv':hc,
    'step0': bounded(diag+'-h-nav-run',step0,[hc[0],ROLE,FIELDS,hc[hc.index('--combined')+1]]),
    'step1': bounded(diag+'-book-run',step1,['atx-impl/tools/book_diagnostics.py',ROLE,diag+'-holdings/manifest.json']),
    'book_csv_source_sha256':sha('atx-impl/tools/book_diagnostics.py'),
    'book_csv_is_python_serializer':True, 'book_hash_placeholder':'BOOK_CSV_SHA256_AFTER_STEP1',
    'step2':{}}
for key in ('v11','covv1'):
    rp=f'build-equity/p9-m2i-dcov-{key}-run/receipt.json'
    cmd=read(rp)['command']
    mapped=replace_tokens(cmd,{'build-equity/train-2020-2023-lo3-fields-v10/manifest.json':FIELDS,
        'a4a060ae29c10080ca89b4b29335709558c2ae4842bd29db0398b23a43070809':sha(FIELDS),
        f'build-equity/p9-m2i-dcov-{key}':diag+'-risk-'+key})
    mapped += ['--book-weights',diag+'-book.csv','--book-weights-sha256','BOOK_CSV_SHA256_AFTER_STEP1']
    diagnostic['step2'][key]={'original_receipt':rp,'original_receipt_sha256':sha(rp),'original_argv':cmd,
        'proposed_argv':bounded(diag+'-risk-'+key+'-run',mapped,[cmd[0],ROLE,FIELDS,diag+'-book.csv']),
        'runtime_exe_sha256':sha(cmd[0])}
proposal['diagnostic_B8a']=diagnostic
proposal['source_files']={p:sha(p) for p in ['scripts/research_spec.py','scripts/cycle/phases.py',
    'scripts/cycle/cli.py','scripts/wave_readers.py','atx-impl/tools/nav_summ.py',
    'atx-impl/tools/book_diagnostics.py','atx-impl/src/strategy_risk_verb.cpp',
    str(CELL/'tools/p9x_read_filter.py')]}
save(CELL/'p9v-vol-target-cov-proposal.json',proposal)
save(CELL/'start-state-6.txt',state)
print(json.dumps({'state':sha(CELL/'start-state-6.txt'),'proposal':sha(CELL/'p9v-vol-target-cov-proposal.json'),
    'witness':sha(CELL/'logs/p9v-fields-witness.json'),'template_proposed_sha256':proposal['template_sha256'],
    'frozen_exes':len(state['executables']),'witnessed':len(witness['rows']),'risk_fields':risk_names,'host':state['host']}))
