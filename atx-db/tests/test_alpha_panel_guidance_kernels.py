import datetime as dt
import pytest
from atx_db.alpha_panel import guidance_kernels as K
UTC=dt.UTC
@pytest.fixture
def prior():return {'cik':1,'accession':'g1','measure':'EPS','basis':'GAAP','accounting_basis_id':'gaap-total','period_type':'Q','fiscal_year':2021,'fiscal_quarter':2,'period_end':dt.date(2021,6,30),'fiscal_calendar_id':'c','currency':'USD','unit':'USD_PER_SHARE','share_basis':'diluted','share_basis_id':'s','value_type':'range','low':1.,'high':1.2,'qualification_reason':'qualified','available_at':dt.datetime(2021,4,1,12,tzinfo=UTC)}
def test_revisions_strict_issue_order_negative_values_no_text_prior_fallback(prior):
 current={**prior,'accession':'g2','available_at':dt.datetime(2021,5,1,12,tzinfo=UTC),'low':-.2,'high':-.1,'prior_low':999.,'prior_high':999.}
 assert K.revision(current,[prior,current])['guid_revision']==pytest.approx(-1.25)
 assert K.revision(current,[current])['guid_revision'] is None
 future={**prior,'available_at':dt.datetime(2021,6,1,tzinfo=UTC)}
 assert K.prior(current,[future,current])[0] is None
@pytest.mark.parametrize('field,value',[('basis','ADJUSTED'),('accounting_basis_id','continuing-operations'),('currency','CAD'),('unit','USD'),('share_basis','basic'),('share_basis_id','new-split'),('fiscal_year',2022),('fiscal_quarter',3),('period_end',dt.date(2021,9,30)),('fiscal_calendar_id','other'),('period_type','FY')])
def test_independent_comparability_axes_never_substitute(prior,field,value):
 actual={**prior,'available_at':dt.datetime(2021,7,15,12,tzinfo=UTC),field:value}
 assert K.prior(actual,[prior])[0] is None

def test_equal_latest_ambiguity_and_latest_invalid_do_not_fallback(prior):
 actual={**prior,'available_at':dt.datetime(2021,7,15,12,tzinfo=UTC)}
 bad={**prior,'available_at':dt.datetime(2021,5,1,tzinfo=UTC),'qualification_reason':'unqualified','low':None,'high':None}
 assert K.prior(actual,[prior,bad])[0] is None
 duplicate={**prior,'accession':'other'}
 assert K.prior(actual,[prior,duplicate])[0] is None

def test_floor_ceiling_unknown_clock_and_complete_price_basis(prior):
 assert K.midpoint({**prior,'value_type':'floor'}) is None
 actual={**prior,'accession':'actual','available_at':dt.datetime(2021,7,15,12,tzinfo=UTC),'actual_value':1.5}
 date=dt.date(2021,7,14);price={'session_date':date,'close_at':dt.datetime(2021,7,14,20,tzinfo=UTC),'available_at':dt.datetime(2021,7,14,21,tzinfo=UTC),'close':10.,'currency':'USD','basis_id':'s','qualification_reason':'qualified'}
 assert K.surprise(actual,[prior],price,date)['guid_surprise']==pytest.approx(.04)
 for changed in ({'available_at':actual['available_at']},{'close_at':actual['available_at']},{'basis_id':'wrong'},{'close':0},{'currency':'CAD'},{'qualification_reason':'unknown'}):
  assert K.surprise(actual,[prior],{**price,**changed},date)['guid_surprise'] is None
 assert K.prior({**actual,'available_at':None},[prior])[0] is None
