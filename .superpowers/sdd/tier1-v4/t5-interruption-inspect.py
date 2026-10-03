"""Read-only, guarded inspection of interrupted Task5; never infer prep validity."""
import hashlib
import json
from pathlib import Path
import pyarrow.parquet as pq

ROOT = Path('C:/atx')
BASE = ROOT/'atx-db/data/alpha_panel/v1'
OUT = ROOT/'.superpowers/sdd/tier1-v4/logs/t5-interruption-inspection-v1.json'

def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
    return h.hexdigest()

def main():
    if OUT.exists():raise RuntimeError('Refuse overwrite of inspection')
    source=BASE/'fundamentals_v10';target=BASE/'fundamentals_v11'
    man=json.loads((source/'manifest.json').read_text());expected=man['sources']['companyfacts_batches']
    cf=source/'_work/cf';dest=target/'_work/cf'
    assert {p.name for p in cf.glob('batch-????.parquet')}==set(expected)
    assert {p.name for p in dest.glob('batch-????.parquet')}==set(expected)
    receipts=list(cf.glob('batch-????.json'));assert len(receipts)==len(expected)
    seen=set();total=0
    for p in receipts:
        r=json.loads(p.read_text());name=r['file'];assert name in expected and Path(name).name==name
        assert p.stem==Path(name).stem and name not in seen;seen.add(name)
        src=cf/name;tgt=dest/name
        assert sha(src)==r['parquet_sha256']==expected[name]['extract_sha256']==sha(tgt)
        assert src.stat().st_size==r['parquet_bytes']==tgt.stat().st_size
        assert sha(p)==sha(dest/p.name)
        total+=src.stat().st_size
    assert seen==set(expected)
    plan_sha=sha(cf/'plan.json')
    assert plan_sha==man['sources']['companyfacts_extract_plan']['sha256']==sha(dest/'plan.json')
    files=[]
    for p in sorted((target/'_work').rglob('*')):
        if not p.is_file() or 'cf' in p.relative_to(target/'_work').parts:continue
        entry={'path':str(p.relative_to(target)), 'bytes':p.stat().st_size,'sha256':sha(p)}
        if p.suffix=='.parquet':
            try:entry['rows']=pq.ParquetFile(p).metadata.num_rows
            except Exception as e:entry['parquet_error']=str(e)
            entry['payload_verified_by_original_checkpoint']=False
        files.append(entry)
    codes={str(p.relative_to(ROOT)):sha(p) for p in [ROOT/'atx-db/src/atx_db/alpha_panel'/f'{n}.py' for n in ('fundamentals','fund_items','fund_extract','fund_fx','fund_catalog','common','fund_depth')]+[ROOT/'.superpowers/sdd/tier1-v4/t5-publish.py']}
    result={'source_manifest_sha256':sha(source/'manifest.json'),'extract_plan_sha256':plan_sha,
            'extract_members':len(seen),'extract_data_bytes':total,'extract_identity':'PASS',
            'stage_root_files':[p.name for p in target.iterdir() if p.is_file()],
            'interrupted_work_files':files,'code_sha256':codes,
            'acceptance':'NOT_MEASURED','safe_recovery':'Preserve entire _work under a unique forensic sibling; rerun wrapper fund with fresh _work; do not use --skip-prepare.'}
    OUT.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)

if __name__=='__main__':main()
