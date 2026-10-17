"""R15-V-COMPLETE only. Native completion; no summ/PBO/monitor statistic reads."""
import collections
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time
import p9v_guard as G
sys.path.insert(0,str(Path.cwd()/'atx-impl/tools'))
import backtest_integrity as BI

P=G.SDD/'cells/p9v-completion-proposal.json'
D={'monitor':Path('build-equity/mega-monitor-p9v-vol-target-cov-run'),
   'completion':Path('build-equity/cycle-p9v-vol-target-cov/summ-run1')}

def proposal():
    assert G.sha(P)=='a628391717d982b58a39a1df99c1032dd8bf152c0312354741dffff95f3d1105'
    return G.doc(P)

def source(stage):
    p=proposal(); directory=D[stage]
    r=G.doc(directory/'receipt.json') if (directory/'receipt.json').exists() else None
    rows=[]
    paths=[directory/n for n in ('start.json','receipt.json','stdout.log','stderr.log')]+[G.LOGS/f'p9v-{stage}-raw.log']
    assert all(x.as_posix() in p['source_files'] for x in paths)
    for path in paths:
        assert path.exists()
        kind='receipt' if path.name in ('start.json','receipt.json') else 'cycle' if path.parent==G.LOGS else 'raw'
        rows.append({'path':path.as_posix(),'sha256':G.sha(path),'hits':G.X.source_text(path.read_text(encoding='utf-8-sig'),kind,r)})
    out={'stage':stage,'files':rows,'other':sum(h['source']=='OTHER' for row in rows for h in row['hits'])}
    G.save(G.own(stage+'-source'),out)
    assert out['other']==0,'STOP: actual completion source OTHER'
    return {'stage':stage,'files':len(rows),'OTHER':out['other']}

def receipt(stage):
    report=G.doc(G.own(stage+'-source')); assert report['other']==0
    assert all(G.sha(v['path'])==v['sha256'] for v in report['files'])
    r=G.doc(D[stage]/'receipt.json'); p=proposal()
    phase=p['phases'][0 if stage=='monitor' else 1]; a=phase['argv']; cmd=a[a.index('--')+1:]
    assert Path(r['command'][0]).resolve()==Path(cmd[0]).resolve() and r['command'][1:]==cmd[1:]
    assert r['outcome']=='completed' and r['exit_code']==0
    assert r['limits']=={'seconds':180.0,'max_rss_mib':1536,'min_free_mib':512}
    assert r['wall_seconds']<=180 and r['sampled_peak_tree_rss_bytes']<=1536*2**20
    return {'path':(D[stage]/'receipt.json').as_posix(),'sha256':G.sha(D[stage]/'receipt.json'),
        'source_sha':r['source_sha'],'argv_sha256':r['argv_sha256'],'wall_seconds':r['wall_seconds'],
        'peak_mib':r['sampled_peak_tree_rss_bytes']/2**20,'limits':r['limits']}

def preserve():
    listing=G.LOGS/'p9v-output-sha256.txt'
    assert G.sha(listing)=='6bf3100a5b992a02fec9542646a3adda30bf54ec281a7d8edba902f7eb0d284a'
    for line in listing.read_text(encoding='utf-8').splitlines():
        name,size,pin=line.split('\t'); path=Path(name)
        assert path.stat().st_size==int(size) and G.sha(path)==pin,name

def launch(stage):
    p=proposal(); assert stage in D and not D[stage].exists()
    assert G.sha('build-equity/trials.jsonl')==G.LEDGER
    preserve()
    if stage=='monitor': G.pins()
    else: receipt('monitor')
    from cycle.phases import Cycle
    from cycle.spec import load_spec
    from cycle.resolver import Resolver
    c=Cycle(load_spec(Path(G.doc(G.PROPOSAL)['template_path'])),Resolver(Path.cwd()))
    steps=c.steps()
    assert all(s.done for s in steps if s.phase in ('u','fit','card','w','nav'))
    if stage=='completion': assert all(s.done for s in steps if s.phase=='monitor')
    for st in steps:
        if st.phase in ('monitor','summ'):
            expected=next(v for v in p['phases'] if v['phase']==st.phase)
            assert st.argv==expected['argv']
    a=p['cycle_monitor_command' if stage=='monitor' else 'cycle_completion_command']
    assert shutil.disk_usage('C:/').free>=15*10**9
    print(json.dumps({'stage':stage,'host':G.SS.host(),'launch':'immediate bounded; no wait'}),flush=True)
    t=time.monotonic()
    with (G.LOGS/f'p9v-{stage}-raw.log').open('xb') as f:
        run=subprocess.run(a,stdout=f,stderr=subprocess.STDOUT)
    result={'stage':stage,'command':a,'exit_code':run.returncode,'outer_seconds':time.monotonic()-t}
    G.save(G.own(stage+'-launch'),result)
    print(json.dumps(result),flush=True)
    assert run.returncode==0,'STOP: actual completion failure; no retry'

def audit():
    preserve(); rec={s:receipt(s) for s in D}
    raw=Path('build-equity/trials.jsonl').read_bytes(); lines=raw.splitlines(keepends=True)
    assert len(lines)==140 and hashlib.sha256(b''.join(lines[:139])).hexdigest()==G.LEDGER
    records=BI.ledger_read(Path('build-equity/trials.jsonl')); last=records[-1]
    assert last['kind']=='construction' and BI.trial_counts(records)[-1]==1
    # Match exact cell identity without exposing any trial statistics.
    expected=G.CELL.as_posix()
    assert last.get('cell')==expected
    assert sum(r.get('cell')==expected for r in records)==1
    counts=BI.ledger_counts(records)
    nc=sum(counts['construction'].values()); nh=sum(BI.trial_counts(records)); mining=BI.campaign_registry_count(records)
    assert (nc,nh,mining)==(69,130,110)
    out={'ledger':{'lines':140,'sha256':hashlib.sha256(raw).hexdigest(),'original139_prefix_sha256':G.LEDGER,
        'trial_id':last['trial_id'],'kind':last['kind'],'count':1,'N_c':nc,'N_hand':nh,'M':mining,'N_tot':nh+mining,
        'history_reads':len(BI.history_read_lines(records))},'receipts':rec,'source_files':[]}
    for tag in ('p9-3f','p9-3g'):
        for item in G.doc(f'build-equity/mega-{tag}-receipt.json')['Executables'].values(): assert G.sha(item['Path'])==item['Sha256'].lower()
    assert 'DIFFERS' not in G.SS.ucrt()
    for name in G.SS.REFS:
        assert G.SS.b0_listing(Path('build-equity')/name)==(G.SDD/'gate-logs'/f'b0-files-{name}.txt').read_text(encoding='utf-8')
    for key,v in G.doc(G.PROPOSAL)['stores'].items():
        d=Path('build-equity/p9-m2i-dcov-'+key)
        assert G.sha(d/'manifest.json')==v['manifest_sha256'] and G.sha(d/'model.atxcov')==v['container']['file_sha256']
    for path,pin in G.doc(G.PROPOSAL)['source_files'].items(): assert G.sha(path)==pin
    assert G.sha('build-equity/train-2020-2023-lo3-fields-v15/manifest.json')=='26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09'
    counts_source=collections.Counter()
    for stage in list(G.DIRS)[:-1]+list(D):
        r=G.doc(G.own(stage+'-source')); assert r['other']==0
        assert all(G.sha(x['path'])==x['sha256'] for x in r['files'])
        out['source_files']+=r['files']; counts_source.update(h['source'] for row in r['files'] for h in row['hits'])
    out['source_counts']=dict(counts_source)
    out['integrity']='54 exes, CRT,126 reference files, v15,both models/manifests,original88 outputs and source pins match'
    files={Path(line.split('\t')[0]) for line in (G.LOGS/'p9v-output-sha256.txt').read_text().splitlines()}
    for d in (Path('build-equity/mega-monitor-p9v-vol-target-cov'),*D.values(),Path('build-equity/cycle-p9v-vol-target-cov')):
        files.update(x for x in d.rglob('*') if x.is_file())
    listing=G.LOGS/'p9v-final-output-sha256.txt'
    with listing.open('x',encoding='utf-8',newline='\n') as f:
        f.write('\n'.join(f'{x.as_posix()}\t{x.stat().st_size}\t{G.sha(x)}' for x in sorted(files))+'\n')
    out['output_listing']={'path':listing.as_posix(),'files':len(files),'sha256':G.sha(listing)}
    with (G.LOGS/'p9v-accounting-v8.txt').open('x',encoding='utf-8',newline='\n') as f:
        f.write('\n'.join(BI.appendix_a(records,'build-equity/trials.jsonl'))+'\n'+BI.appendix_a_v8(records)+'\n')
    G.save(G.own('completion-audit'),out)
    print(json.dumps({k:out[k] for k in ('ledger','receipts','source_counts','integrity','output_listing')},indent=2))

if __name__=='__main__':
    if sys.argv[1]=='launch': launch(sys.argv[2])
    elif sys.argv[1]=='source': print(json.dumps(source(sys.argv[2])))
    else: audit()
