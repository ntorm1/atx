"""Current hand-original binding and parser-context recheck, existing receipts only."""
from pathlib import Path
from collections import Counter
from atx_db.alpha_panel import common as C,issuer_history as I,issuer_classes as K,universe as U
from atx_db import sec_http as H
art=Path('../.superpowers/sdd/tier1-v4');e=C.read_json(art/'t2r2-hand-source-evidence.json');sample=C.read_json(C.stage_dir('validation')/'universe_hand_check_sample.json');hand=C.read_json(U.HAND_CHECK)
assert e['sample_sha256']==sample['sample_sha256']==hand['sample_sha256']
stores=[H.FetchLedgerStore(Path(e['raw_root'])).load(),H.FetchLedgerStore(I.RAW).load()];rows=[];changes=[]
for row in e['rows']:
    sources=[]
    for source in row['sources']:
        store=next(s for s in stores if s.lookup(source['url']) and s.lookup(source['url']).ok);rec=store.lookup(source['url'])
        assert rec.sha256==source['raw_sha256'];raw=store.read(rec,maximum=K.PRIMARY_MAXIMUM)
        assert str(source['filing_date'])<str(row['session_date'])
        result=K.reit_assertion(raw,row['name'],row['cik'])
        if result['status']!=source.get('reit_assertion',{}).get('status'):changes.append({'ticker':row['ticker'],'stratum':row['stratum'],'url':source['url'],'old_assertion':source.get('reit_assertion'),'current_assertion':result})
        sources.append({'url':source['url'],'raw_sha256':rec.sha256,'filing_date':source['filing_date'],'parser_assertion':result})
    rows.append({'security_id':row['security_id'],'session_date':row['session_date'],'stratum':row['stratum'],'ticker':row['ticker'],'sources':sources})
out={'sample_sha256':sample['sample_sha256'],'sample_file_sha256':C.sha256_file(C.stage_dir('validation')/'universe_hand_check_sample.json'),'hand_file_sha256':C.sha256_file(U.HAND_CHECK),'source_artifact_sha256':C.sha256_file(art/'t2r2-hand-source-evidence.json'),'rows':rows,'changed_parser_assertions':changes,'method':'Original raw SHA reverified and full-context source parser rerun. These parser results are independent of retained manual cover/status judgments; a parser unknown is not a manual negative. Claim membership must remain unchanged on final rebuild before binding is accepted.','guard_job':__import__('os').environ.get('ATX_GUARD_JOB')}
C.write_json_atomic(art/'t2r4-hand-binding.json',out);print('bound hand originals',len(rows),'parser changes',len(changes))
