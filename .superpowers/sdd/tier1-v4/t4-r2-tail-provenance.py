import json
from pathlib import Path
from atx_db.alpha_panel import common as C
root=C.build_root();x=C.read_json(Path('../.superpowers/sdd/tier1-v4/t4-r2-comparability.json'));fund=C.read_json(root/'compustat_std'/'manifest.json')['fund_stage'];con=C.connect(memory='300MB',threads=1)
cf=(root/fund/'_work'/'cf'/'batch-[0-9][0-9][0-9][0-9].parquet').as_posix();ciks=','.join(str(t['cik']) for t in x['ptpm_tail_samples'])
rel=con.execute(f"SELECT cik,taxonomy,concept,period_start,period_end,unit,value,accession_number FROM read_parquet('{cf}') WHERE cik IN ({ciks}) AND period_start IS NOT NULL AND date_diff('day',period_start,period_end) BETWEEN 350 AND 380 AND (regexp_matches(concept,'Revenue|Premium|Pretax|BeforeIncomeTax|InterestIncome') OR concept='NetIncomeLoss')");facts=[dict(zip([d[0] for d in rel.description],r)) for r in rel.fetchall()]
out=[]
for t in x['ptpm_tail_samples']:
 end=str(t['fy0_datadate']);matching=[f for f in facts if f['cik']==t['cik'] and str(f['period_end'])==end]
 rec={'key':{k:t[k] for k in ('cik','fy0_datadate','session_date','ptpm')},'standardized_annual':t['standardized_annual'],'catalog_event_source':t['catalog_event_source'],'original_full_year_facts_all_accessions':matching}
 acc=t['standardized_annual'][0]['accession'] if t['standardized_annual'] else None
 own=[f for f in matching if f['accession_number']==acc]
 rec['exact_accession_full_year_facts']=own
 revenue=[f for f in own if any(k in f['concept'] for k in ('Revenue','Premium','InterestIncomeOperating'))]
 pretax=[f for f in own if 'BeforeIncomeTax' in f['concept'] or 'PretaxIncome' in f['concept']]
 pairs=[{'revenue':sales,'pretax':profit,'raw_ratio':profit['value']/sales['value']} for sales in revenue for profit in pretax if sales['value']!=0 and sales['unit']==profit['unit'] and sales['period_start']==profit['period_start'] and abs(profit['value']/sales['value']-t['ptpm'])<=1e-6*max(abs(t['ptpm']),1e-12)]
 rec['matching_same_unit_source_pairs']=pairs
 rec['classification']='original_source_supported_extreme' if pairs and not t['fx_converted'] else 'fx_supported_ratio_extreme' if pairs and t['fx_converted'] else 'source_history_or_chain_unresolved'
 if t['cik']==30625:rec['producer_issue']='Task5/standardization: zero Revenues fact precedes billions of positive RevenueFromContractWithCustomerExcludingAssessedTax; annual restatement moved zero-based sales to -1000/13000. These denominators do not represent the reported total sales. Preserve original annual/restatement clocks while repairing revenue concept selection; no ratio clipping or plug.'
 if t['cik']==1895618:rec['producer_issue']='Revenue256 is explicitly reported and explains tiny denominator; retained narrow provenance has NetIncomeLoss but no certified pretax concept, so PI identity is not independently certified.'
 if t['cik']==1253986:rec['producer_issue']='Financial issuer: InterestIncomeOperating466.087m but chosen extension sales185k; full financial-revenue versus isolated caption scope remains uncertified. Task5 must certify total financial sales and pretax lineage.'
 out.append(rec)
con.close();Path('../.superpowers/sdd/tier1-v4/t4-r2-tail-provenance.json').write_text(json.dumps(out,indent=2,default=str));print(json.dumps([{**r['key'],'classification':r['classification'],'facts':len(r['original_full_year_facts_all_accessions'])} for r in out]))
