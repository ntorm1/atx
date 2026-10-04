"""Frozen Task8 statistical gates and R20 reference populations; synthetic only."""
import copy
import datetime as dt
import math
import numpy as np
import pytest
from atx_db.alpha_panel import char_replication as R
from atx_db.alpha_panel import char_monthly_evidence as E
from atx_db.alpha_panel.char_monthly_registry import JKP153


def evidence():
    entries={n:{'status':'mapped','native_name':n,'sign':1,'weighting':'vw_cap','metadata_id':'synthetic'} for n in JKP153}
    osap={n:{'status':'excluded','reason':'independent synthetic no match','metadata_id':'synthetic'} for n in JKP153}
    for name in ('be_me','age'):osap[name]={'status':'mapped','native_name':name,'sign':1,'weighting':'equal_weight','metadata_id':'synthetic'}
    units={}
    for i,m in enumerate(R.MONTHS):
        value=.02+.01*math.sin(i)
        units[m]={'holding_month':m,'formation':'synthetic-pre2023','population':{},'benchmark_counts':{},'outcomes':{n:{'own':None if n=='eq_dur' else value,'jkp':value*2,'osap':value*1.5 if n in ('be_me','age') else None,'reason':'unsupported_original' if n=='eq_dur' else None} for n in JKP153}}
    return units,entries,osap


def test_all153_120_slots_mean_sign_rho_tstats_and_original_gates():
    units,entries,osap=evidence();report=R.summarize(units,entries,osap)
    assert len(report['requested_slots'])==120 and len(report['outcomes'])==153
    row=report['outcomes']['be_me']['jkp'];xs=[units[m]['outcomes']['be_me']['own'] for m in R.MONTHS]
    assert row['rho']==pytest.approx(1) and row['pairs']==120 and row['mean_sign_agrees']
    assert row['own']['mean']==pytest.approx(np.mean(xs))
    assert row['own']['iid_t']==pytest.approx(np.mean(xs)/np.std(xs,ddof=1)*math.sqrt(120))
    assert row['own']['hac_t'] is not None
    assert report['gates']['jkp_matched_rho_share']['denominator']==153
    assert report['gates']['jkp_mean_sign_share']['numerator']==152
    assert report['outcomes']['eq_dur']['jkp']['rho'] is None
    assert report['gates']['osap_median_rho']['denominator']==2
    assert report['readiness']=='NOT_PRODUCTION_READY' and report['source_quality_certified'] is False


def test_missing_overlap_member_remains_in_osap_denominator():
    units,entries,osap=evidence()
    for m in R.MONTHS:units[m]['outcomes']['age']['osap']=None
    result=R.summarize(units,entries,osap)['gates']['osap_median_rho']
    assert result['measured']==pytest.approx(1) and result['matched']==1 and result['denominator']==2 and not result['pass']


@pytest.mark.parametrize('months',[0,59,60])
def test_minimum60_pairs_missing_and_constant_are_explicit(months):
    units,entries,osap=evidence()
    for i,m in enumerate(R.MONTHS):
        if i>=months:units[m]['outcomes']['be_me']['own']=None
    row=R.summarize(units,entries,osap)['outcomes']['be_me']['jkp']
    assert row['pairs']==months and (row['rho'] is not None)==(months>=60)
    for m in R.MONTHS:units[m]['outcomes']['be_me']['own']=.1
    row=R.summarize(units,entries,osap)['outcomes']['be_me']['jkp']
    assert row['rho'] is None and row['own']['iid_t'] is None and row['own']['reason']=='zero_variance_undefined_t'


def test_full_original_denominators_and_inventory_cannot_shrink():
    units,entries,osap=evidence();units.pop('2013-01')
    with pytest.raises(ValueError):R.summarize(units,entries,osap)
    units,entries,osap=evidence();units['2013-01']['outcomes'].pop('eq_dur')
    with pytest.raises(ValueError):R.summarize(units,entries,osap)
    units,entries,osap=evidence();entries.pop('eq_dur')
    with pytest.raises(ValueError):R.summarize(units,entries,osap)


def test_hac_uses_actual_calendar_lags_not_compressed_missing_months():
    dense=[.01+(i%3)*.002 for i in range(60)]
    gapped=[None]*120
    for i,v in enumerate(dense):gapped[2*i]=v
    a,b=R.statistics(dense),R.statistics(gapped)
    assert a['iid_t']==pytest.approx(b['iid_t']) and a['hac_t']!=pytest.approx(b['hac_t'])


def test_r20_all_nonmicro_characteristic_reference_differs_from_nyse_only():
    formation=dt.date(2022,10,31)
    rows=[{'cik':i,'me':100.+i,'nyse':i<30,'certified':True,'be_me':float(i if i<30 else i+70),
        'entry_date':dt.date(2022,11,1),'exit_date':dt.date(2022,11,30),'return_certified':True,'ret':(i if i<30 else i+70)*.001} for i in range(60)]
    value,reason=E.tercile(rows,'be_me',formation)
    low,cap=np.quantile([r['me'] for r in rows if r['nyse']],[.2,.8])
    def manual(reference):
        lo,hi=np.quantile([r['be_me'] for r in reference],[1/3,2/3]);legs=([r for r in rows if r['be_me']<=lo],[r for r in rows if r['be_me']>hi])
        returns=[sum(min(r['me'],cap)*r['ret'] for r in leg)/sum(min(r['me'],cap) for r in leg) for leg in legs]
        return returns[1]-returns[0],legs
    all_value,legs=manual([r for r in rows if r['me']>low]);nyse_value,_=manual([r for r in rows if r['nyse'] and r['me']>low])
    assert reason is None and value==pytest.approx(all_value) and value!=pytest.approx(nyse_value)
    assert any(r['me']<=low for r in legs[0])  # micro stocks still assigned from same cutoffs
    assert 'ALL country nonmicro' in R.CONFIG['portfolio']['characteristic_reference']


def test_replication_window_rejects_later_evaluation_despite_r21_construction():
    formation=dt.date(2022,12,30)
    rows=[{'cik':i,'me':100.+i,'nyse':True,'certified':True,'be_me':i,'entry_date':dt.date(2023,1,2),'exit_date':dt.date(2023,1,31),'return_certified':True,'ret':.01} for i in range(30)]
    with pytest.raises(ValueError):E.tercile(rows,'be_me',formation)
    assert R.MONTHS[0]=='2013-01' and R.MONTHS[-1]=='2022-12' and len(R.MONTHS)==120
