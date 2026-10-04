import datetime as dt
import pytest
from atx_db.alpha_panel import char_monthly_evidence as E


def rows():
    formation=dt.date(2022,10,31)
    return [{"cik":i,"me":100+i,"nyse":True,"certified":True,"be_me":i,
             "entry_date":dt.date(2022,11,1),"exit_date":dt.date(2022,11,30),"ret":i*.001,
             "return_certified":True} for i in range(30)],formation


def test_capped_vw_terciles_have_expected_direction():
    data,formation=rows()
    value,reason=E.tercile(data,"be_me",formation)
    assert value>0 and reason is None
    data[0]["ret"]=None
    assert E.tercile(data,"be_me",formation)==(None,"missing_or_uncertified_eligible_return")


@pytest.mark.parametrize("key",["entry_date","exit_date"])
def test_boundary_before_ranking_or_aggregation(key):
    data,formation=rows()
    data[0][key]=dt.date(2023,1,1)
    with pytest.raises(ValueError,match="holding|heldout|label window"):
        E.tercile(data,"be_me",formation)


def test_labels_and_benchmarks_cannot_cross_holdout():
    data,formation=rows()
    label={**data[0],"formation":formation,"exit_date":dt.date(2023,1,2)}
    with pytest.raises(ValueError,match="heldout"):E.replication({},[label],[],[])
    with pytest.raises(ValueError,match="heldout"):E.replication({},[],[{"month":dt.date(2023,1,1)}],[])


def test_every_outcome_original_denominator_missing_never_selected_out():
    evidence=E.replication({},[],[],[])
    assert len(evidence["outcomes"])==153
    assert evidence["gates"]["jkp_matched_rho_share"]=={"pass":False,"measured":0,"numerator":0,"denominator":153}
    assert evidence["gates"]["osap_median_rho"]["pass"] is False
    assert evidence["gates"]["osap_median_rho"]["denominator"]==153


def test_spearman_ties_and_constant_series():
    assert E.spearman([(1,3),(1,3),(2,4),(3,5)])==pytest.approx(1)
    assert E.spearman([(1,3),(1,4)]) is None


def test_duplicate_issuer_population_rejected():
    data,formation=rows()
    with pytest.raises(ValueError,match="duplicate issuer"):E.tercile(data+[data[0]],"be_me",formation)


def test_invalid_eligible_cap_never_disappears_from_portfolio():
    data,formation=rows()
    data[0]["me"]=None
    assert E.tercile(data,"be_me",formation)==(None,"invalid_or_uncertified_eligible_issuer_cap")


def test_ratio_value_diagnostic_keeps_original_date_denominator(tmp_path,monkeypatch):
    import duckdb
    import json
    from atx_db.alpha_panel import char_monthly as M,release_inputs as R
    from atx_db.alpha_panel.char_monthly_registry import FREEZE_SHA256
    con=duckdb.connect(config={"memory_limit":"128MB","threads":"1"});con.execute("SET TimeZone='UTC'")
    generation=tmp_path/"fixture-generation";unit=generation/"month=2022-06";unit.mkdir(parents=True)
    (generation/"manifest.json").write_text("{}",encoding="utf-8")
    (tmp_path/"ratios_v11").mkdir()
    source=tmp_path/"ratios_v11"/"monthly.parquet"
    payload=unit/"char_monthly.parquet"
    try:
        con.execute("""CREATE TABLE chars AS SELECT DATE '2022-06-30' session_date,i::BIGINT security_id,i::BIGINT cik,true member_equity,
         (i*.1)::DOUBLE be_me,(i*.2)::DOUBLE nix_me,(i*.3)::DOUBLE sale_me,(i*.4)::DOUBLE gp_at,DATE '2021-12-31' accounting_datadate,'USD' accounting_currency,100.0 issuer_me FROM range(1,11) t(i)""")
        con.execute(f"COPY chars TO '{payload.as_posix()}' (FORMAT PARQUET)")
        con.execute("""CREATE TABLE ratios AS SELECT session_date,security_id,cik,be_me bm,1/nix_me pe_issuer_ni,1/sale_me ps,gp_at gprof,
          TIMESTAMPTZ '2022-06-20 00:00:00+00' fund_available_at,DATE '2022-03-31' datadate,'USD' curcd,100.0 me FROM chars""")
        con.execute(f"COPY ratios TO '{source.as_posix()}' (FORMAT PARQUET)")
        binding={"registry_sha256":FREEZE_SHA256,"ratios":{"files":{"ratios_v11/monthly.parquet":R.identity(source)}}}
        path=tmp_path/"binding.json";path.write_text(json.dumps(binding),encoding="utf-8")
        monkeypatch.setattr(M,"validate_generation",lambda *args:{"files":{"month=2022-06/char_monthly.parquet":R.identity(payload)}})
        result=E.compare_ratios(tmp_path,generation,path,tmp_path/"comparison.json",con)
        assert result["gate"]["pass"] is False
        assert result["gate"]["expected_formula_dates"]==288
        assert result["gate"]["measured"]==pytest.approx(1)
        assert result["dates"]["2022-06-30"]["bm"]["eligible_denominator"]==10
    finally:con.close()
