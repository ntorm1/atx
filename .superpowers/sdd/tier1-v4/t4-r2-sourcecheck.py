"""Independent arithmetic and field-level blocking matrix over the retained 40 filing documents. No network."""
import json, math, hashlib, datetime as dt
from collections import Counter
from pathlib import Path
from atx_db.alpha_panel import common as C, ratios as R
from atx_db.alpha_panel.compustat_std import parse_ixbrl

def div(a,b):return None if a is None or b is None or b==0 else a/b
def total(*values):return None if any(x is None for x in values) else sum(values)
def sub(a,b):return None if a is None or b is None else a-b
def avg(a,b):return div(total(a,b),2)
def term(a,b,weight=1):
 x=div(a,b);return None if x is None else weight*x
EXTRA={
 'dltt':('LongTermDebtNoncurrent','LongTermDebtAndCapitalLeaseObligations'),
 'dltt_total':('LongTermDebt',),'cur_ltd':('LongTermDebtCurrent','LongTermDebtAndCapitalLeaseObligationsCurrent'),
 'short_borrow':('ShortTermBorrowings','ShortTermDebtCurrent','ShortTermBankLoansAndNotesPayable'),
 'dlc':('DebtCurrent',),
 'ppent':('PropertyPlantAndEquipmentNet',), 'ap':('AccountsPayableCurrent',),
 're':('RetainedEarningsAccumulatedDeficit',),'xsga':('SellingGeneralAndAdministrativeExpense',),
 'xad':('AdvertisingExpense',),'sstk':('ProceedsFromStockIssuance','ProceedsFromIssuanceOfCommonStock'),
 'intan':('IntangibleAssetsNetIncludingGoodwill',),
 'intano':('IntangibleAssetsNetExcludingGoodwill',),
 'gdwl':('Goodwill',),'mib':('MinorityInterest',),
}
PRIMITIVES={'evm':['me','dltt','dlc','pstk','mib','che','oiadp','dp'],
 'roce':['ni','txt','xint','dltt','dlc','seq','pstk','dltt_p','dlc_p','seq_p','pstk_p'],
 'aftret_eq':['ni','seq','pstk','seq_p','pstk_p'],
 'aftret_invcapx':['ni','xint','dltt','seq','pstk','mib'],
 'pretret_noa':['oiadp','ppent','act','lct','ppent_p','act_p','lct_p'],
 'pretret_earnat':['oiadp','ppent','act','ppent_p','act_p'],
 'equity_invcap':['seq','pstk','dltt','mib'],'debt_invcap':['dltt','seq','pstk','mib'],
 'totdebt_invcap':['dltt','dlc','seq','pstk','mib'],'capital_ratio':['dltt','seq','pstk'],
 'int_debt':['xint','dltt','dltt_p'],'int_totdebt':['xint','dltt','dlc','dltt_p','dlc_p'],
 'debt_ebitda':['dltt','dlc','oiadp','dp'],'short_debt':['dlc','dltt'],
 'lt_debt':['dltt','lt'],'cash_debt':['oancf','dltt','dlc'],'lt_ppent':['lt','at','intan'],
 'dltt_be':['dltt','seq','txditc','pstk'],'debt_assets':['dltt','dlc','at'],
 'debt_capital':['ap','dlc','dltt','seq','pstk'],
 'cash_conversion':['invt','rect','ap','cogs','sale'],'inv_turn':['cogs','invt','invt_p'],
 'pay_turn':['cogs','invt','invt_p','ap','ap_p'],'rect_turn':['sale','rect','rect_p'],
 'sale_invcap':['sale','dltt','seq','pstk','mib'],'adv_sale':['xad','sale'],
 'altman_z':['act','lct','at','re','oiadp','me','lt','sale'],
 'ohlson_o':['at','lt','act','lct','ni','oancf','ni_p'],
 'piotroski_f':['ib','ib_p','at','at_p','at_p2','oancf','dltt','dltt_p','act','lct','act_p','lct_p','sstk','sale','cogs','sale_p','cogs_p'],
 'beneish_m':['rect','rect_p','sale','sale_p','cogs','cogs_p','act','act_p','ppent','ppent_p','at','at_p','dp','dp_p','xsga','xsga_p','lct','lct_p','dltt','dltt_p','ib','oancf'],
}
PRIMITIVES.update({
 'ps':['me','sale'],'pcf':['me','oancf'],'pe_issuer_ni':['me','ni'],'pe_issuer_ib':['me','ib'],
 'bm':['seq','txditc','pstk','me'],'ptb':['seq','pstk','me'],'divyield_paid':['dvc','me'],
 'dpr':['dvc','ib'],'npm':['ni','sale'],'opmad':['oiadp','sale'],'gpm':['sale','cogs'],
 'roe':['ni','seq','txditc','seq_p','txditc_p'],'at_turn':['sale','at','at_p'],
 'accrual':['ib','oancf','at','at_p'],'debt_at':['lt','at'],'curr_ratio':['act','lct'],
 'de_ratio':['lt','seq'],'fcf_ocf':['oancf','capx'],'rd_sale':['xrd','sale'],
 'gprof':['sale','cogs','at'],'sale_equity':['sale','seq'],'sale_nwc':['sale','act','lct'],
 'curr_debt':['lct','lt'],'invt_act':['invt','act'],'rect_act':['rect','act'],
 'cash_lt':['che','lt'],'cash_ratio':['che','lct'],'quick_ratio':['act','invt','lct'],
 'ocf_lct':['oancf','lct'],'ptpm':['pi','sale'],'efftax':['txt','pi'],
 'cfm':['ib','dp','sale'],'opmbd':['oiadp','dp','sale'],'roa':['oiadp','dp','at','at_p'],
 'intcov':['ib','xint'],'intcov_ratio':['ni','txt','xint'],
 'aftret_equity':['ni','seq','seq_p'],'profit_lct':['oiadp','dp','lct'],
})

BLOCKED={
 'capei':{'fields':['ni_fy0','ni_fy1','ni_fy2','ni_fy3','ni_fy4','source_full_year_comparability'],'owner':'Task5','reason':'Five comparable annual NI vintages are not jointly certified by a retained single filing; no calendar-history inference'},
 'pe_exi':{'fields':['security_class_diluted_eps_continuing','adr_ratio','eps_share_basis','split_available_at'],'owner':'Task5 + market','reason':'Diluted per-share security-class/date/split alignment unprovided; outstanding shares and issuer cap cannot substitute'},
 'pe_inc':{'fields':['security_class_diluted_eps_total','adr_ratio','eps_share_basis','split_available_at'],'owner':'Task5 + market','reason':'Same class/date/split alignment unprovided; NI/IB scaling prohibited'},
 'peg_trailing':{'fields':['security_class_diluted_eps_fy0','security_class_diluted_eps_fy3','source_full_year_comparability','split_available_at'],'owner':'Task5 + market','reason':'Certified positive comparable diluted EPS three-year history and split basis unavailable'},
 'divyield':{'fields':['indicated_annual_dividend_rate','dividend_announcement_available_at','security_class_currency_basis'],'owner':'Task13 + market','reason':'Paid historical cash dividends are not an indicated rate'},
}

def additional(h,m):
 g=h.get;ceq=sub(g('seq'),g('pstk'));ceqp=sub(g('seq_p'),g('pstk_p'))
 invested=total(g('dltt'),ceq,g('pstk'),g('mib'));debt=total(g('dltt'),g('dlc'));ebitda=total(g('oiadp'),g('dp'))
 capemp=total(debt,ceq);capempp=total(g('dltt_p'),g('dlc_p'),ceqp)
 out={'evm':div(total(m['me'],debt,g('pstk'),g('mib'),None if g('che') is None else -g('che')),ebitda),
 'roce':div(total(g('ni'),g('txt'),g('xint')),avg(capemp,capempp)),
 'aftret_eq':div(g('ni'),avg(ceq,ceqp)),'aftret_invcapx':div(total(g('ni'),g('xint')),invested),
 'pretret_noa':div(g('oiadp'),avg(sub(total(g('ppent'),g('act')),g('lct')),sub(total(g('ppent_p'),g('act_p')),g('lct_p')))),
 'pretret_earnat':div(g('oiadp'),avg(total(g('ppent'),g('act')),total(g('ppent_p'),g('act_p')))),
 'equity_invcap':div(ceq,invested),'debt_invcap':div(g('dltt'),invested),'totdebt_invcap':div(debt,invested),
 'capital_ratio':div(g('dltt'),total(g('dltt'),ceq,g('pstk'))),'int_debt':div(g('xint'),avg(g('dltt'),g('dltt_p'))),
 'int_totdebt':div(g('xint'),avg(debt,total(g('dltt_p'),g('dlc_p')))),'debt_ebitda':div(debt,ebitda),
 'short_debt':div(g('dlc'),debt),'lt_debt':div(g('dltt'),g('lt')),'cash_debt':div(g('oancf'),debt),
 'lt_ppent':div(g('lt'),sub(g('at'),g('intan'))),'dltt_be':div(g('dltt'),g('be')),'debt_assets':div(debt,g('at')),
 'debt_capital':div(total(g('ap'),debt),total(g('ap'),debt,ceq,g('pstk'))),
 'cash_conversion':None if any(g(x) is None for x in ('invt','rect','ap','sale','cogs')) or g('sale')==0 or g('cogs')==0 else 365*(g('invt')/g('cogs')+g('rect')/g('sale')-g('ap')/g('cogs')),
 'inv_turn':div(g('cogs'),avg(g('invt'),g('invt_p'))),'pay_turn':div(total(g('cogs'),sub(g('invt'),g('invt_p'))),avg(g('ap'),g('ap_p'))),
 'rect_turn':div(g('sale'),avg(g('rect'),g('rect_p'))),'sale_invcap':div(g('sale'),invested),'adv_sale':div(g('xad'),g('sale')),
 'aftret_equity':div(g('ni'),avg(g('seq'),g('seq_p'))),'profit_lct':div(ebitda,g('lct')),
 'altman_z':total(term(sub(g('act'),g('lct')),g('at'),1.2),term(g('re'),g('at'),1.4),term(g('oiadp'),g('at'),3.3),term(m['me'],g('lt'),.6),term(g('sale'),g('at')))}
 out['ohlson_o']=None
 if all(g(x) is not None for x in PRIMITIVES['ohlson_o']) and g('at')>0 and g('act')!=0 and g('lt')!=0 and abs(g('ni'))+abs(g('ni_p'))!=0:
  out['ohlson_o']=-1.32-.407*math.log(g('at')/1e6)+6.03*g('lt')/g('at')-1.43*(g('act')-g('lct'))/g('at')+.0757*g('lct')/g('act')-1.72*(g('lt')>g('at'))-2.37*g('ni')/g('at')-1.83*g('oancf')/g('lt')+.285*(g('ni')<0 and g('ni_p')<0)-.521*(g('ni')-g('ni_p'))/(abs(g('ni'))+abs(g('ni_p')))
 out['piotroski_f']=None;out['beneish_m']=None
 if all(g(x) is not None for x in PRIMITIVES['piotroski_f']):
  roa=div(g('ib'),g('at_p'));oldroa=div(g('ib_p'),g('at_p2'));cfo=div(g('oancf'),g('at_p'));lev=div(g('dltt'),avg(g('at'),g('at_p')));oldlev=div(g('dltt_p'),avg(g('at_p'),g('at_p2')));liq=div(g('act'),g('lct'));oldliq=div(g('act_p'),g('lct_p'));gm=div(sub(g('sale'),g('cogs')),g('sale'));oldgm=div(sub(g('sale_p'),g('cogs_p')),g('sale_p'));turn=div(g('sale'),g('at_p'));oldturn=div(g('sale_p'),g('at_p2'))
  if all(x is not None for x in (roa,oldroa,cfo,lev,oldlev,liq,oldliq,gm,oldgm,turn,oldturn)):
   out['piotroski_f']=sum((roa>0,cfo>0,roa>oldroa,cfo>roa,lev<oldlev,liq>oldliq,g('sstk')<=0,gm>oldgm,turn>oldturn))
 if all(g(x) is not None for x in PRIMITIVES['beneish_m']):
  terms=[div(div(g('rect'),g('sale')),div(g('rect_p'),g('sale_p'))),div(div(sub(g('sale_p'),g('cogs_p')),g('sale_p')),div(sub(g('sale'),g('cogs')),g('sale'))),div(sub(1,div(total(g('act'),g('ppent')),g('at'))),sub(1,div(total(g('act_p'),g('ppent_p')),g('at_p')))),div(g('sale'),g('sale_p')),div(div(g('dp_p'),total(g('dp_p'),g('ppent_p'))),div(g('dp'),total(g('dp'),g('ppent')))),div(div(g('xsga'),g('sale')),div(g('xsga_p'),g('sale_p'))),div(div(total(g('lct'),g('dltt')),g('at')),div(total(g('lct_p'),g('dltt_p')),g('at_p'))),div(sub(g('ib'),g('oancf')),g('at'))]
  if all(x is not None for x in terms):out['beneish_m']=-4.84+.920*terms[0]+.528*terms[1]+.404*terms[2]+.892*terms[3]+.115*terms[4]-.172*terms[5]-.327*terms[6]+4.679*terms[7]
 return out

def extract(facts,end,concepts,instant):
 for concept in concepts:
  found=[f for f in facts if f['concept']==concept and not f['dim'] and f['end']==end and f['instant']==instant and (instant or 350<=(dt.date.fromisoformat(f['end'])-dt.date.fromisoformat(f['start'])).days<=380)]
  if found:return max(f['value'] for f in found),{'concept':concept,'end':end,'facts':found}
 return None,{'missing_concepts':list(concepts),'end':end,'kind':'instant' if instant else 'comparable_annual_duration350..380'}

COMPOSITE_CERTIFICATION={'piotroski_f': {'status': 'BLOCKED full original-model certification', 'producer': 'Task5 + market', 'fields': ['source_full_period_comparability_fy0_fy1_fy2', 'assets_beginning_fy0_fy1', 'common_equity_issuance_event_or_share_count_definition'], 'reason': 'FY-2 assets/issuance and comparable histories missing; cash SSTK alone does not certify every issuance event'}, 'altman_z': {'status': 'BLOCKED full original-model certification', 'producer': 'Task5 + Task7', 'fields': ['certified_source_EBIT_definition', 'source_retained_earnings', 'source_full_period_comparability', 'model_population_applicability'], 'reason': 'Implemented OIADP term must not be assumed identical to original EBIT; raw score arithmetic does not certify original manufacturing-model population'}, 'ohlson_o': {'status': 'BLOCKED full original-model certification', 'producer': 'Task5 + Task7', 'fields': ['GNP_price_level_deflator', 'source_funds_from_operations_identity', 'source_full_period_comparability', 'comparable_prior_NI'], 'reason': 'Nominal asset log and OCF proxy differ from original model inputs; missing fields have no assumed equivalence'}, 'beneish_m': {'status': 'BLOCKED full original-model certification', 'producer': 'Task5 + Task7', 'fields': ['source_total_accruals_original_definition', 'source_full_period_comparability_fy0_fy1', 'certified_gross_profit_debt_SGA_depreciation_source_scope'], 'reason': 'IB-minus-OCF accrual proxy and source histories/bases need original-model equivalence certification'}}

def main():
 root=C.build_root();raw=root.parents[1]/'raw'/'sec_ratios_handcheck';v=C.read_json(root/'ratios'/'validation.json');con=C.connect(memory='250MB',threads=1);checks=[];records=[]
 for r in v['handcheck']['checks']:
  if 'filing' not in r:continue
  files=list(raw.glob(r['accession']+'-*.htm'));file=files[0];content=file.read_bytes();facts=parse_ixbrl(content.decode('utf-8','replace'));h=dict(r['filing']);provenance={}
  end=r['datadate'];dates=sorted({f['end'] for f in facts if f['concept']=='Assets' and f['instant'] and not f['dim'] and f['end']<end and 340<=(dt.date.fromisoformat(end)-dt.date.fromisoformat(f['end'])).days<=390});prior=dates[-1] if dates else None
  older=sorted({f['end'] for f in facts if f['concept']=='Assets' and f['instant'] and not f['dim'] and prior and f['end']<prior and 340<=(dt.date.fromisoformat(prior)-dt.date.fromisoformat(f['end'])).days<=390});prior2=older[-1] if older else None
  h['at_p2'],provenance['at_p2']=extract(facts,prior2,('Assets',),True) if prior2 else (None,{'need':'FY-2 Assets instant'})
  for key,concepts in EXTRA.items():
   instant=key not in ('xsga','xad','sstk');h[key],provenance[key]=extract(facts,end,concepts,instant)
   if prior:h[key+'_p'],provenance[key+'_p']=extract(facts,prior,concepts,instant)
  for suffix in ('','_p'):
   if h.get('dltt'+suffix) is None:h['dltt'+suffix]=sub(h.get('dltt_total'+suffix),h.get('cur_ltd'+suffix))
   if h.get('dlc'+suffix) is None:h['dlc'+suffix]=total(h.get('cur_ltd'+suffix),h.get('short_borrow'+suffix))
  if h['intan'] is None:h['intan']=total(h.get('intano'),h.get('gdwl'))
  for key in ('act','lct','invt','rect'):
   if prior:h[key+'_p'],provenance[key+'_p']=extract(facts,prior,R.HAND_ITEMS[key][1],True)
  for key in ('ni','sale','cogs','dp','oancf','ib_cont','do'):
   if prior:h[key+'_p'],provenance[key+'_p']=extract(facts,prior,R.HAND_ITEMS[key][1],False)
  h['ib_p']=h.get('ib_cont_p') if h.get('ib_cont_p') is not None else sub(h.get('ni_p'),h.get('do_p'))
  # No zero assumptions in newly extracted source prerequisites.
  m={'me':r['me'],'prc':r['prc'],'eps_adj':r.get('eps_adj',1)} if 'me' in r else None
  rel=con.execute(f"SELECT * FROM read_parquet('{(root/'ratios'/'monthly'/'year=*'/'ratios_monthly.parquet').as_posix()}') WHERE cik=? AND linkprim='P' AND session_date=CAST(? AS DATE) ORDER BY security_id LIMIT 1",[r['cik'],r['session_date']]);pub=dict(zip([x[0] for x in rel.description],rel.fetchone()));m={k:pub[k] for k in ('me','prc','eps_adj')}
  source=additional(h,m);source.update({k:fn(h,m) for k,fn in R.HAND_RATIOS.items()})
  for metric in list(R.RATIOS)+list(R.COMPOSITES):
   name=metric['name'];missing=[k for k in PRIMITIVES.get(name,[]) if (m.get(k) if k in m else h.get(k)) is None]
   if name in BLOCKED:verdict='blocked_contract';value=None;missing=BLOCKED[name]['fields']
   else:
    value=source.get(name);ours=pub[name];verdict='both_absent' if value is None and ours is None else 'source_absent' if value is None else 'ratio_absent' if ours is None else 'match' if abs(value-ours)<=1e-6*max(abs(value),1e-12) else 'mismatch'
   checks.append({'cik':r['cik'],'accession':r['accession'],'datadate':end,'session_date':r['session_date'],'ratio':name,'source':value,'published':pub[name],'verdict':verdict,'missing_source_fields':missing})
  records.append({'cik':r['cik'],'accession':r['accession'],'datadate':end,'prior':prior,'prior2':prior2,'raw_sha256':hashlib.sha256(content).hexdigest(),'provenance':provenance,'source_inputs':h})
 matrix={}
 for metric in list(R.RATIOS)+list(R.COMPOSITES):
  name=metric['name'];rows=[x for x in checks if x['ratio']==name];counts=dict(Counter(x['verdict'] for x in rows));missing=dict(Counter(k for x in rows for k in x['missing_source_fields']))
  qualification='Ohlson nominal-assets and OCF proxy: exact source model remains blocked on GNP deflator and source funds-from-operations identity' if name=='ohlson_o' else 'Independent implemented-score arithmetic is not original model equivalence certification; retained full-period/history/source definitions still required' if metric in R.COMPOSITES else 'Annual duration/currency/classification producer qualification remains; debt captions require total/current/lease scope certification; missing components never zero-filled'
  matrix[name]={'role':'composite' if metric in R.COMPOSITES else 'extension' if name in ('pe_issuer_ni','pe_issuer_ib','divyield_paid') else 'primary','counts':counts,'status':'blocked_contract' if name in BLOCKED else 'numeric_mismatch' if counts.get('mismatch') else 'partially_checked' if counts.get('match') and any(counts.get(k) for k in ('source_absent','ratio_absent','both_absent')) else 'checked_sample' if counts.get('match') else 'blocked_source_fields','missing_source_fields_counts':missing,'source_primitive_chains':{k:EXTRA.get(k.removesuffix('_p'),R.HAND_ITEMS.get(k.removesuffix('_p'),('derived',['explicit component arithmetic; source_inputs/provenance in keyed records']))) for k in PRIMITIVES.get(name,BLOCKED.get(name,{}).get('fields',[]))},'producer':BLOCKED.get(name,{'owner':'Task5','fields':list(missing),'reason':'Exact missing retained source fields/history above; no zeros/primitive equivalence inferred'}),'qualification':qualification}
 for name,v in matrix.items():
  if 'me' in v.get('source_primitive_chains',{}):v['source_primitive_chains']['me']=['published_market_inputs',['same-date issuer common-line rawclose*shares_daily.shrout; no independent third-party market-cap certification in SEC-only check']]
  if name in BLOCKED:
   for key in v['source_primitive_chains']:v['source_primitive_chains'][key]=['unprovided_producer_field',v['producer']['owner'],key]
 for name,certification in COMPOSITE_CERTIFICATION.items():matrix[name]['source_model_certification']=certification
 out={'scope':'40 retained issuer-date documents; every metric assigned checked or exact blocked field/reason; no network/no universe/no returns','source_manifest_sha256':C.sha256_file(root/'compustat_std'/'manifest.json'),'ratios_manifest_sha256':C.sha256_file(root/'ratios'/'manifest.json'),'counts':dict(Counter(x['verdict'] for x in checks)),'matrix':matrix,'checks':checks,'source_provenance':records}
 con.close();Path('../.superpowers/sdd/tier1-v4/t4-r2-sourcecheck.json').write_text(json.dumps(out,indent=2,default=str));print(json.dumps(out['counts']))
if __name__=='__main__':main()
