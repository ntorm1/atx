"""R15-V: explicit routes, safe unchanged X helpers, ordered scalar reads only."""
import csv
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys
import time

import p9x_read_filter as X
import start_state as SS

ROOT = Path.cwd()
SDD = Path('.superpowers/sdd/platform-p9-20261003')
LOGS = SDD/'cells/logs'
PROPOSAL = SDD/'cells/p9v-vol-target-cov-proposal.json'
CELL = Path('build-equity/mega-nav-p9v-vol-target-cov')
BASE = Path('build-equity/mega-nav-p9x-leverage')
H = Path('build-equity/mega-nav-p9-theme-cov-hedge-gm')
READ = Path('build-equity/p9v-vol-target-cov-readers')
DIRS = {'cycle':Path(str(CELL)+'-run'),
    'mechanics':READ/'mechanics-run1','pair':READ/'pair-run1','book':READ/'book-run1',
    'b8a-h-nav':Path('build-equity/p9v-b8a-h-nav-run'),
    'b8a-book':Path('build-equity/p9v-b8a-book-run'),
    'b8a-risk-v11':Path('build-equity/p9v-b8a-risk-v11-run'),
    'b8a-risk-covv1':Path('build-equity/p9v-b8a-risk-covv1-run')}
LEDGER = '5caaccd36762571fffd9ff6fe1fdddfffca69790d2777755df3424fd0643a2dd'
sha, doc, save, finite = X.sha, X.doc, X.save, X.finite

def own(name):
    return LOGS/('p9v-'+name+'.json')

def committed(path):
    rel=Path(path).as_posix()
    subprocess.run(['git','ls-files','--error-unmatch',rel],check=True,stdout=subprocess.DEVNULL)
    r=subprocess.run(['git','diff','HEAD','--exit-code','--',rel],stdout=subprocess.DEVNULL)
    assert r.returncode==0, 'STOP: prerequisite evidence uncommitted'

def source(stage):
    directory=DIRS[stage]
    receipt=doc(directory/'receipt.json') if (directory/'receipt.json').exists() else None
    rows=[]
    for p in [directory/n for n in ('start.json','receipt.json','stdout.log','stderr.log')]+[LOGS/f'p9v-{stage}-raw.log']:
        if not p.exists(): continue
        kind='receipt' if p.name in ('start.json','receipt.json') else 'cycle' if p==LOGS/'p9v-cycle-raw.log' else 'raw'
        rows.append({'path':p.as_posix(),'sha256':sha(p),'hits':X.source_text(p.read_text(encoding='utf-8-sig'),kind,receipt)})
    assert rows
    out={'stage':stage,'files':rows,'other':sum(h['source']=='OTHER' for row in rows for h in row['hits'])}
    save(own(stage+'-source'),out)
    assert out['other']==0, 'STOP: source OTHER; preserve report'
    return {'stage':stage,'source_files':len(rows),'other':out['other']}

def source_ok(stage):
    d=doc(own(stage+'-source')); assert d['other']==0
    assert all(sha(r['path'])==r['sha256'] for r in d['files'])

def command(stage):
    p=doc(PROPOSAL)
    if stage=='cycle': return p['cycle_command']
    if stage in ('mechanics','pair','book'):
        return p[{'mechanics':'mechanics_command','pair':'paired_bundle_command','book':'book_command'}[stage]]
    d=p['diagnostic_B8a']
    if stage=='b8a-h-nav': return d['step0']
    if stage=='b8a-book': return d['step1']
    key=stage.removeprefix('b8a-risk-'); assert key in ('v11','covv1')
    pin=doc(own('b8a-book-pin')); assert sha(pin['path'])==pin['sha256']
    return [pin['sha256'] if x=='BOOK_CSV_SHA256_AFTER_STEP1' else x for x in d['step2'][key]['proposed_argv']]

def receipt(stage):
    source_ok(stage)
    r=doc(DIRS[stage]/'receipt.json'); p=doc(PROPOSAL)
    a=p['native_bounded_nav_argv'] if stage=='cycle' else command(stage)
    argv=a[a.index('--')+1:]
    assert Path(r['command'][0]).resolve()==Path(argv[0]).resolve() and r['command'][1:]==argv[1:]
    secs,rss=(600,2048) if stage.startswith('b8a-') else (180,1536)
    assert r['limits']=={'seconds':float(secs),'max_rss_mib':rss,'min_free_mib':512}
    assert r['outcome']=='completed' and r['exit_code']==0
    assert r['wall_seconds']<=secs and r['sampled_peak_tree_rss_bytes']<=rss*2**20
    return {'path':(DIRS[stage]/'receipt.json').as_posix(),'sha256':sha(DIRS[stage]/'receipt.json'),
        'argv_sha256':r['argv_sha256'],'source_sha':r['source_sha'],'wall_seconds':r['wall_seconds'],
        'peak_mib':r['sampled_peak_tree_rss_bytes']/2**20,'limits':r['limits']}

def pins():
    p=doc(PROPOSAL)
    assert sha(PROPOSAL)=='4b83b32e3bf3173bdc0c4bffd367a22f6dadf6dfcfae688828a7a955fc24aa83'
    assert sha(p['template_path'])==p['template_sha256']
    for path,pin in p['source_files'].items(): assert sha(path)==pin,path
    from cycle.spec import load_spec
    s=load_spec(Path(p['template_path'])); x=load_spec(Path('scripts/specs/p9/templates/p9x-leverage.json'))
    for k in ('fit','ic','card','fields','runner'): assert s[k]==x[k],k
    assert '--engine-fit' not in s['fit']['flags']
    assert s['nav']['rule']=='aim-partial-v5' and s['nav']['leverage']=='2.0'
    for f in ('--ann-mean-gross','--info-clock-print','--risk-model','--risk-model-sha256','--vol-target'): assert s['nav']['flags'].count(f)==1
    assert '--target-rule' not in s['nav']['flags'] and '--rate' not in s['nav']['flags']
    for v in s['inputs'].values():
        if isinstance(v,dict) and 'path' in v and 'sha256' in v: assert sha(v['path'])==v['sha256']
    for tag in ('p9-3f','p9-3g'):
        for e in doc(f'build-equity/mega-{tag}-receipt.json')['Executables'].values(): assert sha(e['Path'])==e['Sha256'].lower()
    for key,v in p['stores'].items():
        root=Path('build-equity/p9-m2i-dcov-'+key)
        assert sha(root/'manifest.json')==v['manifest_sha256'] and sha(root/'model.atxcov')==v['container']['file_sha256']
    assert doc('build-equity/train-2020-2023-lo3/manifest.json')['universe']['delisting']['returns_applied'] is False
    assert 'DIFFERS' not in SS.ucrt()
    assert sha('build-equity/trials.jsonl')==LEDGER
    assert not Path('build-equity/research-store').exists()
    c=Path('build-equity/mega-candidate-cache-v8-lo3')
    assert not list(c.rglob('index.sqlite')) and sum(x.is_file() for x in c.rglob('*'))==686
    assert all(not Path('build-equity/train-2020-2023-lo3-fields-'+v).exists() for v in ('v9','v10','v13','v14','v15a'))
    return {'pins':'PASS','frozen_exes':54,'ledger_sha256':LEDGER,'host':SS.host()}

def launch(stage):
    assert stage in DIRS
    assert sha('build-equity/trials.jsonl')==LEDGER
    if stage in ('pair','book') or stage.startswith('b8a-'):
        committed(own('mechanics-first')); assert doc(own('mechanics-first'))['pass']
    if stage in ('book',): committed(own('rho-first'))
    if stage in ('b8a-book','b8a-risk-v11','b8a-risk-covv1'):
        assert doc(own('b8a-identity'))['identical']
    a=command(stage)
    if stage=='cycle':
        from cycle.phases import Cycle
        from cycle.spec import load_spec
        from cycle.resolver import Resolver
        c=Cycle(load_spec(Path(doc(PROPOSAL)['template_path'])),Resolver(ROOT))
        steps=c.steps()
        prior={s.phase:s for s in steps if s.phase in ('u','fit','card','w')}
        assert set(prior)=={'u','fit','card','w'} and all(s.done for s in prior.values()), 'STOP: unexpected fresh inherited phase'
        assert all(s.state in ('done','skipped') for s in steps if s.phase=='ref'), 'STOP: unexpected fresh ref'
        reuse={k:{'state':s.state,'output':s.output,'receipt':s.run_dir,
            'receipt_sha256':sha(Path(s.run_dir)/'receipt.json') if s.run_dir else None} for k,s in prior.items()}
        save(own('native-reuse-before'),reuse)
    assert not DIRS[stage].exists() and not (LOGS/f'p9v-{stage}-raw.log').exists(), 'STOP: prior attempt exists'
    assert all(x not in a for x in ('--admission-wait','--host-budget-mib','--auto-attempt'))
    assert shutil.disk_usage('C:/').free>=15*10**9
    print(json.dumps({'stage':stage,'host':SS.host(),'launch':'immediate; bounded; no wait'}),flush=True)
    t=time.monotonic()
    with (LOGS/f'p9v-{stage}-raw.log').open('xb') as f:
        r=subprocess.run(a,stdout=f,stderr=subprocess.STDOUT)
    out={'stage':stage,'exit_code':r.returncode,'outer_seconds':time.monotonic()-t,'command':a}
    save(own(stage+'-launch'),out)
    print(json.dumps(out),flush=True)
    assert r.returncode==0, 'STOP: actual nonzero exit; raw preserved, no retry'

def values(directory):
    expected=['session','book','rebalance','updated','gross','priced_share','sigma_hat','sigma_ref','raw','L_t','multiplier','clip']
    with (directory/'vol_target.csv').open(newline='',encoding='utf-8') as f:
        r=csv.DictReader(f); assert r.fieldnames==expected
        rows=[(v['session'],finite(float(v['L_t']))) for v in r if v['book']==doc(PROPOSAL)['scenario']]
    assert rows and all(1<=x<=2 for _,x in rows)
    return rows

def mechanics():
    rec=[receipt(s) for s in ('cycle','mechanics')]
    raw=(LOGS/'p9v-cycle-raw.log').read_text(encoding='utf-8-sig')
    for stage in ('u','fit','card','w'):
        assert f'== {stage}: done (' in raw, 'STOP: missing native reuse '+stage
    for row in doc(own('native-reuse-before')).values():
        if row['receipt']: assert sha(Path(row['receipt'])/'receipt.json')==row['receipt_sha256']
    p=doc(PROPOSAL); m=doc(READ/'mechanics.json')['navs']['cell']; lim=p['limits']
    assert m['dir']==CELL.as_posix() and m['scenario']==p['scenario']
    assert m['daily_csv_sha256']==sha(CELL/('daily_'+p['scenario']+'.csv'))
    rows=[]
    for k,b,absolute in [('mean_gross_leverage_all_rows',[lim['gross_low'],lim['gross_high']],False),
        ('mean_net_leverage_all_rows',lim['abs_net_max'],True),('tau_gmv_mean',lim['tau_mean_max'],False),
        ('tau_gmv_p95',lim['tau_p95_max'],False),('max_return_identity_error',1e-9,False),('max_cash_book_relative_error',1e-9,False)]:
        v=finite(m['accounting'][k] if k.startswith('max_') else m[k]); t=abs(v) if absolute else v
        rows.append({'check':k,'value':v,'limit':b,'pass':b[0]<=t<=b[1] if isinstance(b,list) else t<=b})
    exposure=[{'scenario':r['scenario'],**{k:finite(r['exposure'][k]) for k in ('mean_gross_leverage','max_gross_leverage')}} for r in doc(CELL/'summary.json')['scenarios']]
    assert len(exposure)==5
    lt=values(CELL)
    out={'rows':rows,'pass':all(r['pass'] for r in rows),'exposure':exposure,'cap_hold':all(r['max_gross_leverage']<=2 for r in exposure),
        'L_t_cap_hold':True,'L_t_rows':len(lt),'mechanics_sha256':sha(READ/'mechanics.json'),
        'summary_sha256':sha(CELL/'summary.json'),'vol_target_sha256':sha(CELL/'vol_target.csv'),'receipts':rec}
    save(own('mechanics-first'),out)
    assert out['pass'] and out['cap_hold'], 'STOP: mechanics or cap'
    return out

def bundle():
    committed(own('mechanics-first')); m=doc(own('mechanics-first'))
    assert m['pass'] and m['cap_hold'] and m['mechanics_sha256']==sha(READ/'mechanics.json') and m['summary_sha256']==sha(CELL/'summary.json')
    r=receipt('pair'); b=doc(READ/'pair.json'); p=doc(PROPOSAL)
    assert Path(b['base'])==BASE and Path(b['final'])==CELL
    exe=sha('build-equity-rel/bin/atx-research-eval.exe')
    assert b['nav_summ_run']['engine_exe_sha256']==exe==p['template']['change']['set']['exes_sha256']['eval']
    files={str(f):sha(f) for d in (BASE,CELL) for f in (d/'summary.json',d/('daily_'+p['scenario']+'.csv'))}
    return b,{'path':str(READ/'pair.json'),'sha256':sha(READ/'pair.json'),'files':files,'receipt':r,'engine_exe_sha256':exe}

def rho():
    b,out=bundle(); p=b['paired']; r=finite(p['rho'])
    assert type(p['sessions']) is int and p['sessions']>=2 and -1<=r<=1 and r!=.999999
    if 'degenerate' not in p and 'one_minus_rho' not in p:
        flag=False; provenance='inferred FALSE; missing optional fields under R15-V2-RHO, stable difference not supplied'
    elif p.get('degenerate') is True and 0<=finite(p['one_minus_rho'])<1e-6:
        flag=True; provenance='supplied stable difference and native flag'
    else: raise AssertionError('STOP: inconsistent degeneracy encoding')
    out.update(paired={k:p.get(k) for k in ('sessions','rho','one_minus_rho','degenerate')},
        missing=[k for k in ('one_minus_rho','degenerate') if k not in p],threshold_flag=flag,provenance=provenance)
    save(own('rho-first'),out); return out

def prints():
    committed(own('rho-first')); b,bound=bundle(); r=doc(own('rho-first'))
    assert r['sha256']==bound['sha256'] and r['files']==bound['files']
    rec=receipt('book'); book=doc(READ/'book.json')['navs']; p=doc(PROPOSAL)
    dirs={'parent':H,'cell':CELL,'x':BASE}; assert set(book)==set(dirs)
    for key,d in dirs.items():
        assert Path(book[key]['dir'])==d and book[key]['scenario']==p['scenario']
        assert book[key]['daily_csv_sha256']==sha(d/('daily_'+p['scenario']+'.csv'))
    books={k:{f:finite(book[k][f]) for f in ('net_annual','net_sharpe','x4_net_sharpe','ann_vol','max_drawdown')} for k in dirs}
    lt={}
    for key,d in [('x',BASE),('cell',CELL)]:
        v=values(d); lt[key]={'rows':len(v),'mean_L_t':sum(x for _,x in v)/len(v),
            'consecutive_changes':sum(a[1]!=b[1] for a,b in zip(v,v[1:])),'sha256':sha(d/'vol_target.csv')}
    lt['mean_change_V_minus_X']=lt['cell']['mean_L_t']-lt['x']['mean_L_t']
    pair=X.project_pair(b['paired'],r['threshold_flag'])
    accepted=pair['dsr']>0 and doc(own('mechanics-first'))['pass']
    selection={'annual_above_H':books['cell']['net_annual']>books['parent']['net_annual'],
        'Sharpe_within_point100_H':books['cell']['net_sharpe']>=books['parent']['net_sharpe']-.100,
        'mechanics':doc(own('mechanics-first'))['pass']}
    out={'books':books,'paired':pair,'paired_deciding':True,'accepted':accepted,'selection_checks':selection,
        'P9_F_proposed':'V' if accepted and all(selection.values()) else 'X','L_t':lt,'book_receipt':rec,
        'book_sha256':sha(READ/'book.json'),'degenerate':r['threshold_flag'],
        'degenerate_line':'pair degenerate: the two stores give the same book to within 1e-6 of correlation' if r['threshold_flag'] else None}
    save(own('registered-prints'),out); return out

def identity():
    r=receipt('b8a-h-nav'); other=Path('build-equity/p9v-b8a-h-nav')
    def listing(d): return {p.relative_to(d).as_posix():{'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(d.rglob('*')) if p.is_file()}
    a,b=listing(H),listing(other)
    differences=[k for k in sorted(set(a)|set(b)) if a.get(k)!=b.get(k)]
    out={'identical':not differences,'differences':differences,'H':a,'diagnostic':b,'receipt':r}
    save(own('b8a-identity'),out)
    return {'identical':not differences,'differences':differences,'files_H':len(a),'files_diagnostic':len(b)}

def bookpin():
    r=receipt('b8a-book'); p=Path('build-equity/p9v-b8a-book.csv')
    out={'path':p.as_posix(),'sha256':sha(p),'bytes':p.stat().st_size,'receipt':r}
    save(own('b8a-book-pin'),out); return out

def bias():
    out={}
    for k in ('v11','covv1'):
        r=receipt('b8a-risk-'+k); path=Path('build-equity/p9v-b8a-risk-'+k+'/bias_summary.json')
        f=doc(path)['families']['book']; assert f['status']=='ok' and f['series']==f['series_ok']==1
        n=f['observations']; assert type(n) is int and n>=2
        mean=finite(f['full_sample']['b_mean']); median=finite(f['full_sample']['b_median']); assert mean==median
        width=math.sqrt(2/n); inside=f['full_sample']['series_in_band']; assert inside in (0,1)
        out[k]={'b_mean':mean,'b_median':median,'in_band':bool(inside),'observations':n,'band':[1-width,1+width],
            'direction':'over-forecast/over-corrected' if mean<1-width else 'under-forecast' if mean>1+width else 'in band',
            'receipt':r,'sha256':sha(path)}
    out['covv1_nearer_one']=abs(out['covv1']['b_mean']-1)<abs(out['v11']['b_mean']-1)
    save(own('b8a-prints'),out); return out

def controls():
    # Only synthetic source/projection inputs. No unchanged-tool suite.
    text=json.dumps({'started_utc':'2026-10-17T12:00:00Z','dirty_outside_pathspec':sorted(X.PNGS)})
    assert all(h['source']!='OTHER' for h in X.source_text(text,'receipt'))
    assert any(h['source']=='OTHER' for h in X.source_text(text.replace('started_utc','ended_utc'),'receipt'))
    assert all(h['source']!='OTHER' for h in X.source_text(X.HEADER+', '.join(sorted(X.PNGS)),'cycle'))
    assert any(h['source']=='OTHER' for h in X.source_text(X.HEADER+', '.join(sorted(X.PNGS)),'raw'))
    assert any(h['source']=='OTHER' for h in X.source_text(text.replace('2026-10-17T','2026-10-18T'),'receipt'))
    p={'dsr':1,'memmel_se':2,'t':3,'lw':{'p_value':.5,'p_one_sided':.4,'secret':'CANARY'},'cbb':'CANARY'}
    assert 'CANARY' not in json.dumps(X.project_pair(p,False)) and X.project_pair(p,True)['t']=='n/a'
    assert len(DIRS)==8 and len(set(DIRS.values()))==8
    assert len({str(d/n) for d in DIRS.values() for n in ('start.json','receipt.json','stdout.log','stderr.log')}|{str(LOGS/f'p9v-{s}-raw.log') for s in DIRS})==40
    return {'V_routing_scalar_source_controls':'PASS','scientific_input_reads':0,'source_files':40}

if __name__=='__main__':
    mode=sys.argv[1]
    if mode=='launch': launch(sys.argv[2])
    else:
        result=source(sys.argv[2]) if mode=='source' else {'pins':pins,'mechanics':mechanics,'rho':rho,
            'prints':prints,'identity':identity,'bookpin':bookpin,'bias':bias,'controls':controls}[mode]()
        if len(sys.argv)>2 and mode!='source': save(Path(sys.argv[2]),result)
        print(json.dumps(result,indent=2,allow_nan=False))
