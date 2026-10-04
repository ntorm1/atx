import datetime as dt
import math
import pytest
from atx_db.alpha_panel import golden_evaluation as E, golden_labels as L, golden_registry as R


def test_native_evaluator_streams_conditionally_qualified_returns(tmp_path,monkeypatch):
    # Synthetic immutable products only. Admission replacement is confined to
    # this test; production has fixed full native public validators.
    from atx_db.alpha_panel import golden_panel as P,golden_sources as S
    golden=tmp_path/'golden';labels=tmp_path/'labels'
    g=golden/'generations'/'fixture';l=labels/'generations'/'fixture'
    con=P.connect(tmp_path/'fixture-db')
    try:
        con.execute("CREATE TABLE features AS SELECT DATE '2021-01-04'+t::INTEGER session_date,i::BIGINT security_id,i::DOUBLE be_me,true be_me_qualified,TIMESTAMPTZ '2020-12-31 22:00:00+00' be_me_source_at,(1000.0+i)::DOUBLE me,true me_qualified,TIMESTAMPTZ '2020-12-31 22:00:00+00' me_source_at,TIMESTAMPTZ '2021-01-03 22:00:00+00'+t*INTERVAL '1 day' available_at,1 exchcd_like,true univ_all_common,true univ_r3000p,i::DOUBLE ret_12_1,true ret_12_1_qualified,(i%7)::DOUBLE rvol_21d,true rvol_21d_qualified FROM range(1,101) ids(i) CROSS JOIN range(3) days(t)")
        con.execute("CREATE TABLE labels AS SELECT session_date feature_date,security_id,5 horizon,session_date+1 entry_date,session_date+6 exit_date,((security_id+day(session_date))*.001)::DOUBLE AS value,true label_qualified,'qualified_complete_window' reason,true univ_all_common,true univ_r3000p FROM features")
        P.C.copy_to_parquet(con,'SELECT * FROM features',g/'daily/year=2021/panel.parquet')
        P.C.copy_to_parquet(con,'SELECT * FROM labels',l/'year=2021/labels.parquet')
    finally:con.close()
    gm={'files':{'daily/year=2021/panel.parquet':S.pin(g/'daily/year=2021/panel.parquet')}}
    lm={'files':{'year=2021/labels.parquet':S.pin(l/'year=2021/labels.parquet')}}
    P.C.write_json_atomic(g/'manifest.json',gm);P.C.write_json_atomic(l/'manifest.json',lm)
    monkeypatch.setattr(P,'validate_generation',lambda stage,path,con:gm)
    monkeypatch.setattr(L,'validate_generation',lambda stage,path,con:lm)
    config={**R.evaluation_config(),'primary':[{'name':'be_me','direction':1}],
            'exploratory':[{'name':'be_me','direction':1}],'horizons':[5],'minimum_dates':2}
    monkeypatch.setattr(E,'evaluation_config',lambda:config)
    config_path=tmp_path/'fixture-config.json';P.C.write_json_atomic(config_path,config)
    output=tmp_path/'result.json'
    result=E.evaluate_native(golden,'fixture',labels,'fixture',config_path,S.pin(config_path)['sha256'],output)
    assert result['statistical_evidence_measured'] is True
    assert result['primary']['family_size']==2
    assert result['primary']['sensitivities']['be_me:5']['turnover']['usable_dates']==3
    assert len((tmp_path/'result.date-outcomes.jsonl').read_text().splitlines())==12
    assert result['consumer_acceptance']=='PENDING' and result['feature_label_coverage']=='FAIL'


def calendar(start=dt.date(2022,1,3),count=100):
    return [start+dt.timedelta(days=i) for i in range(count)]


def test_complete_label_next_close_missing_delisting_and_seal():
    dates=calendar()
    obs={d:{"ret":.01,"source_qualified":True} for d in dates}
    result=L.label_window(dates,0,5,obs)
    assert result["value"]==pytest.approx(1.01**5-1)
    assert result["entry_date"]==dates[1] and result["exit_date"]==dates[6]
    obs[dates[3]]["delisted"]=True
    assert "delisting" in L.label_window(dates,0,5,obs)["reason"]
    obs[dates[3]].update(dlret=-.5,dlret_qualified=True)
    assert L.label_window(dates,0,5,obs)["reason"]=="residual_cash_convention_pending"
    obs[dates[3]]["residual_cash_qualified"]=True
    for d in dates[4:7]:obs[d].update(cash_return=.001,cash_return_qualified=True)
    assert L.label_window(dates,0,5,obs)["value"]==pytest.approx(1.01**2*.5*1.001**3-1)
    with pytest.raises(ValueError,match="seal"):
        L.label_window(calendar(dt.date(2022,12,29)),0,5,obs)


def rows():
    feature=dt.date(2021,1,4);cutoff=dt.datetime(2021,1,3,22,tzinfo=dt.UTC)
    return [{"security_id":i,"value":float(i),"ret":i*.0001,"me":100+i,"nyse":True,
             "entry_date":feature+dt.timedelta(days=1),"exit_date":feature+dt.timedelta(days=6),
             "source_at":cutoff-dt.timedelta(days=1),"cutoff":cutoff,"me_at":cutoff,
             "label_qualified":True,"source_qualified":True,"adv_qualified":True} for i in range(1,101)]


def test_quantiles_cap_equalweight_turnover_and_forward_rejection():
    config=R.evaluation_config();data=rows();date=dt.date(2021,1,4)
    result=E.evaluate_date(data,date,5,1,config)
    assert result["IC"]==pytest.approx(1) and result["spread"]>0
    assert result["turnover"]==pytest.approx(2)
    assert max(abs(w) for w in result["weights"].values())<=.05+1e-8
    next_result=E.evaluate_date(data,date,5,1,config,result["weights"])
    assert next_result["turnover"]==0
    data[0]["exit_date"]=dt.date(2023,1,2)
    data[0]["label_qualified"]=False # invalid observation must reject before null filtering
    with pytest.raises(ValueError,match="seal"):E.evaluate_date(data,date,5,1,config)
    data=rows();data[0]["source_at"]=data[0]["cutoff"]
    with pytest.raises(ValueError,match="leak"):E.evaluate_date(data,date,5,1,config)


def test_ties_missing_dates_overlap_hac_and_conservative_fdr():
    assert E.spearman([1,1,2,3],[2,2,3,4])==pytest.approx(1)
    assert E.spearman([1,1],[1,2]) is None
    values=[math.sin(i/8)*.02+.002 for i in range(200)]
    result=E.hac(values,63)
    assert result["lag"]==62 and result["standard_error"]>0
    missing=list(values);missing[4]=None
    assert E.hac(missing,5)["dates"]==199
    assert E.hac([1]*125,5)["status"]=="non_testable"
    assert E.fdr([.01,None,.04])==pytest.approx([.03,1,.06])
    report=E.summarize({},R.evaluation_config())
    assert report["family_size"]==78 and all(t["q"]==1 for t in report["tests"])
    assert all(g["status"]=="FAIL" for g in report["gate_status"].values())


def test_sample_exclusions_and_benchmark_boundary():
    data=rows();data[0]["ret"]=None
    result=E.evaluate_date(data,dt.date(2021,1,4),21,1,R.evaluation_config())
    assert result["reason"]=="minimum_names" and result["names"]==99
    data=rows();data[0]["benchmark_dates"]=(dt.date(2023,1,1),)
    with pytest.raises(ValueError,match="benchmark"):E.evaluate_date(data,dt.date(2021,1,4),5,1,R.evaluation_config())
