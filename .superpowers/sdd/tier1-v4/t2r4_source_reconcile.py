"""Canonical source publication plus complete original/key/status revalidation.

The canonical loaders SHA-verify every retained raw read. Their assessment binds each
processed primary/header to the original identity and filing clock. This avoids an
additional full-corpus parser pass while recording the actual final-code result.
"""
import json,os,hashlib
from pathlib import Path
from collections import Counter
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K
from atx_db import sec_http as H
art=Path('../.superpowers/sdd/tier1-v4');root=C.build_root()
initial_path=art/'t2r3-intermediate-parser-revalidation.json'
initial=C.read_json(initial_path if initial_path.exists() else art/'t2r3-retained-parser-revalidation.json')
baseline={(r['cik'],r['accession']):dict(r) for r in initial['rows']}
for change in initial['changed_status']:
    baseline[(change['cik'],change['accession'])]['status']=change['old_status']
# Historical intermediate evidence preserved unchanged.
assert initial['attempt_count']==2998
assert C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME)==initial['raw_ledger_sha256']
assert C.sha256_file(root/'issuer_classes/plan.json')==initial['source_plan_sha256']
def checkpoint(stage,rule):
    path=root/stage/'manifest.json';manifest=C.read_json(path)
    codes={}
    for name,value in manifest['code'].items():
        if not isinstance(value,dict):continue
        source=C.PACKAGE_ROOT/'src/atx_db/alpha_panel'/name
        if source.exists():codes[name]=hashlib.sha256(source.read_bytes().replace(b'\r\n',b'\n')).hexdigest()==value['sha256_lf']
    actual=C.output_hashes(root/stage)
    result={'manifest_sha256':C.sha256_file(path),'manifest_files':manifest['files'],'actual_files':actual,
            'files_match':actual==manifest['files'],'partial_files':[str(path) for path in (root/stage).rglob('*.partial')],
            'code_matches':codes,'plan_matches':C.sha256_file(root/stage/'plan.json')==manifest.get('plan_sha256',manifest.get('source_plan_sha256')),
            'raw_ledger_matches':C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME)==manifest['raw_ledger_sha256'],
            'rule_matches':manifest['rule']==rule,'network_matches':C.sha256_file(Path(H.__file__))==manifest['network_contract_sha256']}
    if stage=='issuer_classes':result['assessment_matches']=C.sha256_file(root/stage/'assessment.json')==manifest['assessment_sha256']
    result['reusable_completed_publication']=bool(codes) and all(codes.values()) and result['files_match'] and not result['partial_files'] and all(result[key] for key in ('plan_matches','raw_ledger_matches','rule_matches','network_matches')) and result.get('assessment_matches',True)
    return result
snapshots={stage:checkpoint(stage,rule) for stage,rule in ((I.STAGE,I.RULE),(K.STAGE,K.RULE))}
prior=C.read_json(art/'t2r2-after-verification.json')['publications']
for stage,snapshot in snapshots.items():
    snapshot['files_changed_from_round2']=prior[stage]['actual_files']!=snapshot['actual_files']
    snapshot['round2_manifest_sha256']=prior[stage]['manifest_sha256']
C.write_json_atomic(art/'t2r4-source-publication-snapshot.json',{'guard_job':os.environ.get('ATX_GUARD_JOB'),'publications':snapshots})
print('Source checkpoint reuse',{stage:snapshot['reusable_completed_publication'] for stage,snapshot in snapshots.items()},flush=True)
assert snapshots[I.STAGE]['reusable_completed_publication'],'history publication not reusable'
assert snapshots[K.STAGE]['reusable_completed_publication'],'class publication not reusable'
assessment=C.read_json(root/'issuer_classes/assessment.json')
current={(r['cik'],r['accession']):r for r in assessment['rows']}
assert len(current)==len(assessment['rows'])==len(baseline)==704
assert current.keys()==baseline.keys()
store=H.FetchLedgerStore(I.RAW).load();sources=[];changes=[]
for key,row in current.items():
    prior=baseline[key]
    assert row['primary_url']==prior['primary_url'] and row['header_url']==prior['header_url']
    for field,urlfield in (('primary_sha256','primary_url'),('header_sha256','header_url')):
        receipt=store.lookup(row[urlfield]);assert receipt and receipt.ok
        assert row[field]==receipt.sha256
        # Byte verification is repeated even when a fully pinned publish-last
        # checkpoint allows skipping a duplicate parser scan after guard stop.
        store.read(receipt,maximum=K.PRIMARY_MAXIMUM if field=='primary_sha256' else 2_000_000)
    assert row['available_at']>=row['accepted_at']
    sources.append({key:row.get(key) for key in ('cik','accession','primary_url','header_url','primary_sha256','header_sha256','accepted_at','available_at','status','reason')})
    if row['status']!=prior['status']:changes.append({'cik':key[0],'accession':key[1],'old_status':prior['status'],'new_status':row['status']})
out={'rule':K.RULE,'observations':len(current),'status_counts':dict(Counter(str(row['status']) for row in current.values())),
     'reason_counts':dict(Counter(row['reason'] for row in current.values())),'changed_status':changes,'processed_sources':sources,
     'method':'Canonical issuer_classes.load reran final parser on every SHA-verified retained header+primary. Exact704key/status diff compares pre-round3 published statuses; assessment primary/headerSHA matches retained receipts; original conservative clocks checked. This supersedes intermediate343.7s/2drop result after source-based completed-election correction.',
     'superseded_intermediate_sha256':C.sha256_file(art/'t2r3-intermediate-parser-revalidation.json'),
     'prior_manifest_sha256':initial['prior_manifest_sha256'],'final_manifest_sha256':C.sha256_file(root/'issuer_classes/manifest.json'),
     'source_plan_sha256':C.sha256_file(root/'issuer_classes/plan.json'),'raw_ledger_sha256':C.sha256_file(I.RAW/H.FETCH_LEDGER_NAME),
     'parser_code_sha256':C.sha256_file(Path(K.__file__)),'attempt_count':sum(1 for line in (I.RAW/'http-attempts.jsonl').read_text(encoding='utf-8').splitlines() if line),
     'completed_publication_checkpoint_reused':{stage:snapshot['reusable_completed_publication'] for stage,snapshot in snapshots.items()},
     'guard_job':os.environ.get('ATX_GUARD_JOB')}
C.write_json_atomic(art/'t2r4-retained-parser-revalidation.json',out)
# Any measured change is reviewed before universe publication.
assert out['attempt_count']==2998 and out['raw_ledger_sha256']==initial['raw_ledger_sha256']
print('Final canonical cohort',len(current),'status counts',out['status_counts'],'changed status',len(changes),flush=True)
