import json,math
from pathlib import Path
from atx_db.alpha_panel import common as C
root=C.build_root();con=C.connect(memory='250MB',threads=1)
v=C.read_json(root/'ratios'/'validation.json');out=[]
mp={'dp':'dp_ttm','txt':'txt_ttm','pi':'pi_a0','xint':'xint_ttm','che':'che_q0','invt':'invt_q0','rect':'rect_q0','sale':'sale_ttm','cogs':'cogs_ttm','oiadp':'oiadp_ttm','ni':'ni_ttm','ib':'ib_ttm','oancf':'oancf_ttm','capx':'capx_ttm','dvc':'dvc_ttm','xrd':'xrd_ttm','epsfi':'epsfi_ttm','epsfx':'epsfx_ttm','csho':'csho_b','at':'at_q0','at_p':'at_q4','seq':'seq_q0','seq_p':'seq_q4','act':'act_q0','lct':'lct_q0','pstk':'pstk_q0','pstk_p':'pstk_q4','lt':'lt_q0','be':'be_q0','be_p':'be_q4','txditc':'txditc_a0','txditc_p':'txditc_a1'}
for rec in v.get('handcheck',{}).get('checks',[]):
 if 'ratios' not in rec:continue
 rel=con.execute(f"SELECT * FROM read_parquet('{(root/'ratios'/'fund_state.parquet').as_posix()}') WHERE cik=? AND available_at<CAST(? AS DATE)+INTERVAL22 HOUR ORDER BY available_at DESC LIMIT1".replace('INTERVAL22','INTERVAL 22').replace('LIMIT1','LIMIT 1'),[rec['cik'],rec['session_date']]);s=dict(zip([d[0] for d in rel.description],rel.fetchone()))
 diffs=[]
 for h,st in mp.items():
  a=rec['filing'].get(h);b=s.get(st)
  if a is None or b is None:
   if a!=b:diffs.append({'primitive':h,'source':a,'state':b,'class':'source_absent' if a is None else 'state_absent'})
  elif abs(a-b)>1e-6*max(abs(a),1e-12):diffs.append({'primitive':h,'source':a,'state':b,'class':'upstream_value_difference'})
 bad=[x for x in rec['ratios'] if x['verdict'] not in ('match','both_absent')]
 if bad:out.append({'cik':rec['cik'],'accession':rec['accession'],'datadate':rec['datadate'],'session_date':rec['session_date'],'source_url':rec['primary'],'ratios':bad,'primitive_differences':diffs})
con.close();Path('../.superpowers/sdd/tier1-v4/t4-r1-hand-attribution.json').write_text(json.dumps(out,indent=2,default=str),encoding='utf-8')
from collections import Counter
print(json.dumps(dict(Counter(x['primitive']+':'+x['class'] for r in out for x in r['primitive_differences'])),indent=2))
