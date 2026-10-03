"""V11 source semantics and bounded acquisition on offline fixtures."""
import datetime as dt
import json
import pytest
from atx_db.alpha_panel import fund_items as F, fund_depth as D
from atx_db.alpha_panel import fundamentals as FU
import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

DAY=dt.date;CLOCK=dt.datetime(2021,2,23,21,3);END=DAY(2020,12,31);START=DAY(2020,1,1)

def kfacts(*facts):
    k=F.Knowledge(source_backed_only=True)
    for name,start,end,value in facts:k.apply(F.CID[name],start,end,value,CLOCK)
    return k

def test_own_statement_scope_beats_off_statement_generic_without_value_preference():
    k=kfacts(('Revenues',START,END,0),('RevenueFromContractWithCustomerExcludingAssessedTax',START,END,3728134000))
    s=F.Snapshot(k);ctx=F.Ctx(pre={END:{'is_sale_contract':True,'is_sale_generic':False}})
    assert F._sale(s,ctx,END,False)==(3728134000,'main_statement_contract')
    # When generic is also actually presented its economic priority remains; no positive/max preference.
    assert F._sale(F.Snapshot(k),F.Ctx(pre={END:{'is_sale_contract':True,'is_sale_generic':True}}),END,False)==(0,'chain')
    assert F._sale(F.Snapshot(k),F.Ctx(),END,False)==(0,'chain')


def test_custom_caption_cannot_impersonate_standard_revenue_presentation(tmp_path):
    p=tmp_path/'pre.parquet'
    pq.write_table(pa.Table.from_pylist([
        {'adsh':'a','stmt':'IS','tag':'RevenueFromContractWithCustomerExcludingAssessedTax',
         'version':'a','plabel':'Sales','inpth':False},
        {'adsh':'a','stmt':'IS','tag':'Revenues','version':'a','plabel':'Sales','inpth':False},
    ]),p)
    with duckdb.connect() as con:
        row=con.execute(FU.pre_flags_sql(p.as_posix())).fetchdf().iloc[0]
    assert not row['is_sale_contract'] and not row['is_sale_generic']

def test_missing_debt_tax_equity_components_do_not_become_zero_or_balance_plugs():
    k=kfacts(('Assets',None,END,1000),('Liabilities',None,END,600),('LongTermDebtCurrent',None,END,40))
    b=F.ItemCalc(F.Snapshot(k)).bs(END)
    assert b['seq'] is None and b['debt'] is None and b['st'] is None and b['ltd_nc'] is None
    out=F.compute_items(k,END,{}, {},F.Ctx())
    assert all(out[name] is None for name in ('dlc','dltt','txditc','pstk','gdwl','intan','mib'))
    assert out['zero_filled']=='' and out['catalog_zero_filled']==''

def test_explicit_debt_components_and_reported_zero_preserved():
    k=kfacts(('Assets',None,END,1000),('LongTermDebt',None,END,300),('LongTermDebtCurrent',None,END,40),
             ('ShortTermBorrowings',None,END,0),('DeferredIncomeTaxLiabilitiesNet',None,END,0))
    b=F.ItemCalc(F.Snapshot(k)).bs(END)
    assert b['debt']==300 and b['st']==40 and b['ltd_nc']==260
    history=F.history_values(k,END,F.Ctx())
    assert history['txditc']==(0,False) and history['dltt']==(260,False)


def test_parent_equity_and_net_operating_assets_require_reported_components():
    k=kfacts(('Assets',None,END,1000),
             ('StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest',None,END,400))
    b=F.ItemCalc(F.Snapshot(k)).bs(END)
    assert b['seq'] is None and b['noa'] is None
    k.apply(F.CID['StockholdersEquity'],None,END,390,CLOCK)
    b=F.ItemCalc(F.Snapshot(k)).bs(END)
    assert b['seq']==390 and b['mib']==10 and b['noa'] is None


def test_cash_and_deferred_revenue_require_both_components_in_event_and_history():
    k=kfacts(('CashAndCashEquivalentsAtCarryingValue',None,END,10),
             ('ContractWithCustomerLiabilityCurrent',None,END,20))
    assert F.compute_items(k,END,{}, {},F.Ctx())['che'] is None
    assert F.compute_items(k,END,{}, {},F.Ctx())['drev'] is None
    assert 'che' not in F.history_values(k,END,F.Ctx())
    assert 'drev' not in F.history_values(k,END,F.Ctx())
    k.apply(F.CID['ShortTermInvestments'],None,END,0,CLOCK)
    k.apply(F.CID['ContractWithCustomerLiabilityNoncurrent'],None,END,0,CLOCK)
    out=F.compute_items(k,END,{}, {},F.Ctx());h=F.history_values(k,END,F.Ctx())
    assert out['che']==10 and out['drev']==20 and h['che']==(10,False) and h['drev']==(20,False)
    legacy=F.Knowledge()
    legacy.apply(F.CID['CashAndCashEquivalentsAtCarryingValue'],None,END,10,CLOCK)
    assert F.ItemCalc(F.Snapshot(legacy)).bs(END)['che']==10

def test_prior_annual_tax_and_income_arrive_at_comparative_filing_clock():
    prior=DAY(2019,12,31)
    rows=[(CLOCK,'a',F.CID['Assets'],None,END,1000,'USD','10-K',CLOCK.date(),2020,'FY','fsds_accepted_utc'),
          (CLOCK,'a',F.CID['NetIncomeLoss'],START,END,20,'USD','10-K',CLOCK.date(),2020,'FY','fsds_accepted_utc'),
          (CLOCK,'a',F.CID['NetIncomeLoss'],DAY(2019,1,1),prior,10,'USD','10-K',CLOCK.date(),2020,'FY','fsds_accepted_utc'),
          (CLOCK,'a',F.CID['DeferredIncomeTaxLiabilitiesNet'],None,prior,138703000,'USD','10-K',CLOCK.date(),2020,'FY','fsds_accepted_utc')]
    h=[];F.company_events(1,rows,CLOCK,{},history=h,source_backed_only=True)
    assert ('annual_ni_ttm',prior,None,'a',10,CLOCK,False) in h
    assert ('txditc',prior,None,'a',138703000,CLOCK,False) in h
    h2=[];F.company_events(1,[],CLOCK,{},history=h2,source_backed_only=True)
    assert h2==[]

def test_presented_omission_and_nil_omission_are_not_reported_zero():
    k=kfacts(('NetIncomeLoss',START,END,20),('NetCashProvidedByUsedInOperatingActivities',START,END,50))
    ctx=F.Ctx(pre={END:{'has_cf':True,'has_is':True,'is_rd':False,'cf_capx':False}})
    out=F.compute_items(k,END,{}, {},ctx)
    assert out['capx_ttm'] is None and out['xrd_ttm'] is None and out['dvc_ttm'] is None
    assert F.nil_zeros(k,[(F.CID['PaymentsToAcquirePropertyPlantAndEquipment'],DAY(2019,1,1),DAY(2019,12,31))],END,CLOCK,{})==0

class Limiter:
    def __init__(self):self.calls=0
    def acquire(self,label):self.calls+=1;return self.calls
    def record_response(self,*a,**kw):pass

def test_attempt_counter_survives_restart_and_stops_before_thirteenth_http(tmp_path):
    lim=Limiter();b=D.AttemptBudget(tmp_path,lim)
    for i in range(12):b.acquire('https://www.sec.gov/Archives/test')
    b2=D.AttemptBudget(tmp_path,lim)
    with pytest.raises(RuntimeError,match='cap exhausted'):b2.acquire('retry')
    assert lim.calls==12 and json.loads(b.path.read_text())['attempts']==12
    assert len((tmp_path/'attempts.jsonl').read_text().splitlines())==12


def test_missing_or_regressed_counter_cannot_reset_consumed_allocation(tmp_path):
    ledger=tmp_path/'attempts.jsonl'
    ledger.write_text(json.dumps({'attempt':2})+'\n',encoding='utf8')
    with pytest.raises(RuntimeError,match='reconciliation'):
        D.AttemptBudget(tmp_path,Limiter())
    (tmp_path/'attempt-counter.json').write_text(json.dumps({'attempts':1,'raw_bytes':0}),encoding='utf8')
    with pytest.raises(RuntimeError,match='behind'):
        D.AttemptBudget(tmp_path,Limiter())


def test_original_local_name_collision_and_blank_do_not_certify_source_values():
    html='''<html xmlns:g="http://fasb.org/us-gaap/2020-01-31" xmlns:x="https://example.com/custom">
      <xbrli:context id="c"><xbrli:period><xbrli:instant>2020-12-31</xbrli:instant></xbrli:period></xbrli:context>
      <ix:nonFraction name="x:Assets" contextRef="c" unitRef="USD">1000</ix:nonFraction>
      <ix:nonFraction name="g:Assets" contextRef="c" unitRef="USD">0</ix:nonFraction>
      <ix:nonFraction name="g:Liabilities" contextRef="c" unitRef="USD"></ix:nonFraction></html>'''
    facts=D.qualified_original_facts(html)
    assert facts[0]['qualified_name']=='x:Assets' and facts[0]['standard_taxonomy'] is None
    assert facts[1]['standard_taxonomy']=='us-gaap' and facts[1]['value']==0 and facts[1]['source_numeric_qualified']
    assert not facts[2]['source_numeric_qualified']
    conflicting=html.replace('</html>','<div xmlns:g="https://example.com/custom"></div></html>')
    assert D.qualified_original_facts(conflicting)[1]['standard_taxonomy'] is None


def test_extract_reuse_verifies_receipt_and_excludes_mutable_parts(tmp_path):
    src=tmp_path/'old';cf=src/'_work/cf';cf.mkdir(parents=True)
    data=cf/'batch-0000.parquet';data.write_bytes(b'fixture')
    (src/'manifest.json').write_text(json.dumps({'sources':{'companyfacts_batches':
        {data.name:{'extract_sha256':D.C.sha256_file(data)}}}}))
    (cf/'batch-0000.json').write_text(json.dumps({'file':data.name,'parquet_bytes':7,'parquet_sha256':D.C.sha256_file(data)}))
    (cf/'unapproved.partial').write_bytes(b'junk')
    result=D.reuse_extract(src,tmp_path/'new')
    assert result['files']==2 and not (tmp_path/'new/_work/cf/unapproved.partial').exists()
    data.write_bytes(b'changed')
    with pytest.raises(ValueError,match='receipt hash'):D.reuse_extract(src,tmp_path/'another')
