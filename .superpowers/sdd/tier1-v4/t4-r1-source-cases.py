import json
from pathlib import Path
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel.compustat_std import parse_ixbrl
root=C.build_root();a=C.read_json(Path('../.superpowers/sdd/tier1-v4/t4-r1-hand-attribution.json'));out=[];con=C.connect(memory='200MB',threads=1)
for r in a:
 if not any(x['verdict']=='MISMATCH' for x in r['ratios']):continue
 raw=root.parents[1]/'raw'/'sec_ratios_handcheck';file=next(raw.glob(r['accession']+'-*.htm'));facts=parse_ixbrl(file.read_text(encoding='utf-8'));sel=[f for f in facts if f['instant'] and not f['dim'] and ('Cash' in f['concept'] or 'DeferredIncomeTaxLiabilitiesNet'==f['concept'] or 'DeferredTaxLiabilitiesNoncurrent'==f['concept'] or 'ShortTermInvest' in f['concept'] or 'SecuritiesCurrent' in f['concept']) and f['end']<=r['datadate']]
 rel=con.execute(f"SELECT * FROM read_parquet('{(root/'ratios'/'fund_state.parquet').as_posix()}') WHERE cik=? AND available_at<CAST(? AS DATE)+INTERVAL 22 HOUR ORDER BY available_at DESC LIMIT 1",[r['cik'],r['session_date']]);s=dict(zip([d[0] for d in rel.description],rel.fetchone()));out.append({'key':{k:r[k] for k in ('cik','accession','datadate','session_date')},'source_facts':sel,'state':{k:s.get(k) for k in ('seq_q0','seq_q4','wrds_be_q0','wrds_be_q4','txditc_a0','txditc_a1','che_q0','lt_q0','lct_q0')}})
con.close();Path('../.superpowers/sdd/tier1-v4/t4-r1-source-cases.json').write_text(json.dumps(out,indent=2,default=str),encoding='utf-8');print(json.dumps([{'key':r['key'],'state':r['state'],'source_facts':list({(f['concept'],f['end'],f['value']):(f['concept'],f['end'],f['value']) for f in r['source_facts']}.values())} for r in out],indent=2,default=str))
