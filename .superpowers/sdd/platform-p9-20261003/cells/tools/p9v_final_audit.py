"""Read-only final V integrity, receipt timing and registered fixed-table copy."""
import collections
import hashlib
import json
from pathlib import Path
import p9v_guard as G

SDD=G.SDD
out=G.pins()
refs=[]
for name in G.SS.REFS:
    actual=G.SS.b0_listing(Path('build-equity')/name)
    assert actual==(SDD/'gate-logs'/f'b0-files-{name}.txt').read_text(encoding='utf-8')
    refs.append({'name':name,'files':actual.count('\n'),'listing_sha256':hashlib.sha256(actual.encode()).hexdigest()})
out['references']=refs
out['source_files']=[]
counts=collections.Counter()
for stage in ('cycle','mechanics','pair','book','b8a-h-nav','b8a-book','b8a-risk-v11'):
    G.source_ok(stage)
    s=G.doc(G.own(stage+'-source'))
    out['source_files']+=s['files']
    counts.update(h['source'] for row in s['files'] for h in row['hits'])
out['source_counts']=dict(counts)
assert counts['OTHER']==0
out['timings']=[]
for stage in ('cycle','mechanics','pair','book','b8a-h-nav','b8a-book'):
    r=G.receipt(stage)
    out['timings'].append({'stage':stage,**r,'outer_seconds':G.doc(G.own(stage+'-launch'))['outer_seconds']})
out['failed_diagnostic_launch']=G.doc(G.own('b8a-risk-v11-launch'))
out['diagnostic_status']='not produced: step2 v11 bounded runner error: bind small config/source manifests, not full data payloads'
assert not G.DIRS['b8a-risk-v11'].exists()
assert not G.DIRS['b8a-risk-covv1'].exists()
assert not Path('build-equity/p9v-b8a-risk-v11').exists()
assert not Path('build-equity/p9v-b8a-risk-covv1').exists()
out['fresh_completed_phase_seconds']=sum(r['wall_seconds'] for r in out['timings'])
out['outer_seconds_including_failed_wrapper']=sum(r['outer_seconds'] for r in out['timings'])+out['failed_diagnostic_launch']['outer_seconds']
out['inherited_phase_seconds']=sum(G.doc(Path(r['receipt'])/'receipt.json')['wall_seconds'] for r in G.doc(G.own('native-reuse-before')).values() if r['receipt'])
out['ledger']={'lines':139,'sha256':G.sha('build-equity/trials.jsonl'),'N_c':68,'N_hand':129,'M':110,'N_tot':239,'history_reads':0,'P9_admissions':0}
dirs=[G.CELL,G.READ,Path('build-equity/p9v-b8a-h-nav'),Path('build-equity/p9v-b8a-holdings')]
dirs += [d for d in G.DIRS.values() if d.exists()]
files=set(p for d in dirs for p in d.rglob('*') if p.is_file())
files.add(Path('build-equity/p9v-b8a-book.csv'))
rows=[f'{p.as_posix()}\t{p.stat().st_size}\t{G.sha(p)}' for p in sorted(files)]
listing=G.LOGS/'p9v-output-sha256.txt'
with listing.open('x',encoding='utf-8',newline='\n') as f: f.write('\n'.join(rows)+'\n')
out['output_listing']={'path':listing.as_posix(),'sha256':G.sha(listing),'files':len(files)}
text=(SDD/'p9-prereg.md').read_text(encoding='utf-8')
start=text.index('  - **(c)** D-COV\'s families,')
end=text.index('- **Fixed.** Every `atx-cov-v1` parameter',start)
fixed=text[start:end]
assert '| 50 |' in fixed and 'without structurally null ranks' in fixed and '39 of 51' in fixed
path=SDD/'cells/p9v-dcov-registered-prints.md'
with path.open('x',encoding='utf-8',newline='\n') as f:
    f.write('# P9-V B.8(c)-(e), existing registered D-COV prints\n\nCopied exactly from the prereg B.8 tables at this checkpoint; no model or statistic recomputation.\nEvery row read in both directions: below band over-forecast/over-corrected, above band under-forecast. These lines decide nothing.\n\n'+fixed)
out['fixed_prints']={'path':path.as_posix(),'sha256':G.sha(path)}
G.save(G.own('final-audit'),out)
print(json.dumps({k:out[k] for k in ('pins','frozen_exes','ledger','source_counts','fresh_completed_phase_seconds','outer_seconds_including_failed_wrapper','inherited_phase_seconds','output_listing','diagnostic_status')},indent=2))
