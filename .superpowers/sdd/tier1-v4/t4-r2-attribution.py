import json,runpy
from pathlib import Path
from collections import Counter
from atx_db.alpha_panel import common as C,ratios as R
A=runpy.run_path('../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.py');calc=A['additional'];root=C.build_root();con=C.connect(memory='250MB',threads=1)
x=C.read_json(Path('../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.json'));out=[]
for rec in x['source_provenance']:
 checks=[r for r in x['checks'] if r['cik']==rec['cik'] and r['accession']==rec['accession'] and r['verdict'] in ('mismatch','ratio_absent')]
 if not checks:continue
 key=checks[0]
 rel=con.execute(f"SELECT r.me,r.fund_available_at,s.* FROM read_parquet('{(root/'ratios'/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}') r JOIN read_parquet('{(root/'ratios'/'fund_state.parquet').as_posix()}') s ON s.cik=r.cik AND s.available_at=r.fund_available_at WHERE r.cik=? AND r.session_date=CAST(? AS DATE) AND r.linkprim='P' ORDER BY r.security_id LIMIT 1",[key['cik'],key['session_date']]);st=dict(zip([z[0] for z in rel.description],rel.fetchone()))
 mapping={k:k+'_ttm' for k in ('sale','cogs','ni','ib','oiadp','dp','xint','oancf','capx','dvc','xrd','txt')}
 mapping.update({k:k+'_q0' for k in ('seq','pstk','dltt','dlc','mib','ppent','act','lct','invt','rect','ap','at','lt','intan','che')})
 mapping.update({k+'_p':k+'_q4' for k in ('seq','pstk','dltt','dlc','mib','ppent','act','lct','invt','rect','ap','at','lt','intan','che')})
 mapping.update({'be':'be_q0','pi':'pi_a0','xad':'xad_a0','re':'re_a0','ni_p':'ni_a1','sstk':'sstk_a0','at_p2':'at_a2'})
 h={k:st.get(v) for k,v in mapping.items()};h['txditc']=A['sub'](st.get('wrds_be_q0'),st.get('seq_q0'));h['txditc_p']=A['sub'](st.get('wrds_be_q4'),st.get('seq_q4'));statecalc=calc(h,{'me':st['me']})
 annual={k:st.get(k+'_a0') for k in ('at','lt','act','lct','ni','oancf','ib','dltt','sstk','sale','cogs','oiadp','re','rect','ppent','dp','xsga')}
 annual.update({k+'_p':st.get(k+'_a1') for k in ('at','act','lct','ni','ib','dltt','sale','cogs','rect','ppent','dp','xsga')});annual['at_p2']=st.get('at_a2')
 scorecalc=calc(annual,{'me':st['me']})
 for row in checks:
  name=row['ratio'];deps=A['PRIMITIVES'][name];basis=annual if name in {r['name'] for r in R.COMPOSITES} else h
  calculated=scorecalc.get(name) if name in {r['name'] for r in R.COMPOSITES} else statecalc.get(name)
  differs=[]
  for k in deps:
   if k=='me':continue
   source=rec['source_inputs'].get(k);state=basis.get(k)
   if source is None and state is None:continue
   if source is None or state is None or abs(source-state)>1e-6*max(abs(source),1e-12):differs.append({'field':k,'source':source,'state':state,'state_column':mapping.get(k,k+'_a0' if name in {r['name'] for r in R.COMPOSITES} else None),'source_chain':rec['provenance'].get(k,dict(R.HAND_ITEMS).get(k))})
  match=calculated is not None and row['published'] is not None and abs(calculated-row['published'])<=1e-6*max(abs(calculated),1e-12)
  causal='input_history_or_source_basis_difference' if differs and match else 'source_available_history_null' if row['published'] is None and differs else 'unresolved_independent_state_arithmetic' if not match else 'unresolved_source_identity'
  out.append(dict(row,state_independent=calculated,state_arithmetic_matches_published=match,primitive_differences=differs,causal_class=causal,producer_need='Task5: reconcile exact retained concepts/components, debt current maturities/lease scope, and source annual comparable/restated history. No absence->zero inference.'))
con.close();result={'counts':dict(Counter(r['causal_class'] for r in out)),'checks':out}
Path('../.superpowers/sdd/tier1-v4/t4-r2-attribution.json').write_text(json.dumps(result,indent=2,default=str));print(json.dumps(result['counts']))
