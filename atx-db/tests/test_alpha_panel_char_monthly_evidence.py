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
    evidence=E.replication({},[],[],[],{"be_me":{"osap_name":"BM","sign":1}})
    assert len(evidence["outcomes"])==153
    assert evidence["gates"]["jkp_matched_rho_share"]=={"pass":False,"measured":0,"numerator":0,"denominator":153}
    assert evidence["gates"]["osap_median_rho"]["pass"] is False
    assert evidence["gates"]["osap_median_rho"]["denominator"]==1
    assert len(evidence["requested_slots"])==120
    assert evidence["requested_slots"][0]["holding_month"]=="2013-01"
    assert evidence["requested_slots"][-1]["holding_month"]=="2022-12"


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


def test_missing_eligible_label_window_is_explicit_without_invented_dates():
    data,formation=rows();data[0]["entry_date"]=None
    assert E.tercile(data,"be_me",formation)==(None,"missing_eligible_complete_label_window")


def test_ratio_value_diagnostic_keeps_original_date_denominator(tmp_path,monkeypatch):
    import duckdb
    import json
    from atx_db.alpha_panel import char_monthly as M,release_inputs as R
    from atx_db.alpha_panel.char_monthly_registry import FREEZE_SHA256
    con=duckdb.connect(config={"memory_limit":"128MB","threads":"1"});con.execute("SET TimeZone='UTC'")
    generation=tmp_path/"fixture-generation";unit=generation/"month=2022-06";unit.mkdir(parents=True)
    (generation/"manifest.json").write_text("{}",encoding="utf-8")
    (tmp_path/"ratios_v11").mkdir()
    source=tmp_path/"ratios_v11"/"monthly"/"year=2022"/"ratios_monthly.parquet"
    source.parent.mkdir(parents=True)
    payload=unit/"char_monthly.parquet"
    try:
        con.execute("""CREATE TABLE chars AS SELECT DATE '2022-06-30' session_date,i::BIGINT security_id,i::BIGINT cik,true member_equity,
         (i*.1)::DOUBLE be_me,(i*.2)::DOUBLE nix_me,(i*.3)::DOUBLE sale_me,(i*.4)::DOUBLE gp_at,DATE '2021-12-31' accounting_datadate,'USD' accounting_currency,100.0 issuer_me,TIMESTAMPTZ '2022-06-20 00:00:00+00' accounting_available_at FROM range(1,11) t(i)""")
        con.execute(f"COPY chars TO '{payload.as_posix()}' (FORMAT PARQUET)")
        con.execute("""CREATE TABLE ratios AS SELECT session_date,security_id,cik,be_me bm,1/nix_me pe_issuer_ni,1/sale_me ps,gp_at gprof,
          TIMESTAMPTZ '2022-06-20 00:00:00+00' fund_available_at,DATE '2022-03-31' datadate,'USD' curcd,100.0 me FROM chars""")
        con.execute(f"COPY ratios TO '{source.as_posix()}' (FORMAT PARQUET)")
        final=tmp_path/"ratios_v11/manifest.json"
        final.write_text(json.dumps({"files":{"monthly/year=2022/ratios_monthly.parquet":R.identity(source)}}))
        final_pin=R.identity(final)
        (generation/"snapshot.json").write_text(json.dumps({"source":{"manifests":{"ratios_v11/manifest.json":final_pin}}}))
        proof=tmp_path/"basis.parquet"
        con.execute("""CREATE TABLE basis AS SELECT session_date,security_id,cik,formula,accounting_datadate datadate,accounting_available_at available_at,
          accounting_available_at source_available_at,accounting_available_at fund_available_at,accounting_available_at cap_available_at,
          'genuine_annual' flow_basis,'same_annual_snapshot' balance_basis,'USD' currency,issuer_me,'all_eligible_issuer_classes' cap_basis,
          CASE formula WHEN 'bm' THEN 'seq+txditc-pstk' WHEN 'ep' THEN 'ni_including_extraordinary' WHEN 'sp' THEN 'annual_sale' ELSE 'annual_sale_minus_cogs_over_annual_at' END definition
          FROM chars CROSS JOIN (VALUES ('bm'),('ep'),('sp'),('gpa')) t(formula)""")
        con.execute(f"COPY basis TO '{proof.as_posix()}' (FORMAT PARQUET)")
        contract=tmp_path/"basis-contract.json"
        contract.write_text(json.dumps({"version":"char_monthly_ratio_basis_v1","registry_sha256":FREEZE_SHA256,"product":{"source":"independent fixture","clock":"fixture PIT","admission":"reviewed fixture only","files":{"basis.parquet":R.identity(proof)}}}))
        binding={"registry_sha256":FREEZE_SHA256,"ratios":{"manifest":final_pin,"files":{"ratios_v11/monthly/year=2022/ratios_monthly.parquet":R.identity(source)}},"ratio_basis":{"path":"basis-contract.json","identity":R.identity(contract),"expected_sha256":R.identity(contract)["sha256"]}}
        path=tmp_path/"binding.json";path.write_text(json.dumps(binding),encoding="utf-8")
        monkeypatch.setattr(M,"validate_generation",lambda *args:{"files":{"month=2022-06/char_monthly.parquet":R.identity(payload)}})
        invalid={**binding,"ratios":{**binding["ratios"],"manifest":{"sha256":"0"*64,"bytes":1}}}
        path.write_text(json.dumps(invalid),encoding="utf-8")
        with pytest.raises(ValueError,match="admitted final"):E.compare_ratios(tmp_path,generation,path,tmp_path/"comparison.json",con)
        path.write_text(json.dumps(binding),encoding="utf-8")
        result=E.compare_ratios(tmp_path,generation,path,tmp_path/"comparison.json",con)
        assert result["gate"]["pass"] is False
        assert result["gate"]["expected_formula_dates"]==288
        assert result["gate"]["measured"] is None
        assert result["dates"]["2022-06-30"]["bm"]["eligible_denominator"]==10
        assert result["dates"]["2022-06-30"]["bm"]["paired"]==0
        assert result["dates"]["2022-06-30"]["bm"]["complete_compatible_cohort"] is False
    finally:con.close()


def test_formation_audit_cannot_overwrite_values_issuer_population_or_clocks():
    date=dt.date(2022,10,31);early=dt.datetime(2022,10,1,tzinfo=dt.UTC)
    row={"session_date":date,"security_id":1,"cik":10,"issuer_me":100,"be_me":1,"univ_all_common":True,"available_at":E.dt.datetime.combine(date,dt.time(22),dt.UTC)}
    audit={"formation":date,"security_id":1,"cik":10,"me":100,"nyse":True,"primary_security":True,"certified":True,"available_at":early,"source_available_at":early}
    result=E.qualify_formation(row,audit)
    assert result["available_at"]==row["available_at"] and result["be_me"]==1
    for field in ("be_me","univ_all_common","session_date"):
        with pytest.raises(ValueError,match="protected"):E.qualify_formation(row,{**audit,field:999})
    with pytest.raises(ValueError,match="issuer"):E.qualify_formation(row,{**audit,"cik":999})
    with pytest.raises(ValueError,match="clock"):E.qualify_formation(row,{**audit,"source_available_at":None})


def test_reviewed_osap_mapping_full_inventory_pin_and_ambiguity(tmp_path):
    import json
    from atx_db.alpha_panel import release_inputs as R
    from atx_db.alpha_panel.char_monthly_registry import JKP153,FREEZE_SHA256
    metadata=tmp_path/"authoritative.fixture";metadata.write_text("synthetic registry metadata")
    entries={n:{"status":"excluded","reason":"independent metadata has no matching name","metadata_id":"authoritative.fixture"} for n in JKP153}
    entries["be_me"]={"status":"mapped","osap_name":"BM","sign":1,"unit":"decimal","weighting":"vw_cap","country":"usa","metadata_id":"authoritative.fixture"}
    admission=tmp_path/"admission.json";contract=tmp_path/"mapping.json"
    def frozen():
        admission.write_text(json.dumps({"registry_sha256":FREEZE_SHA256,"mapping_sha256":R.digest(entries),"phase":"frozen_before_any_return_run"}))
        contract.write_text(json.dumps({"version":"char_monthly_osap_mapping_v1","registry_sha256":FREEZE_SHA256,"entries":entries,"authoritative_metadata":{"authoritative.fixture":R.identity(metadata)},"admission_record":{"path":"admission.json","identity":R.identity(admission),"expected_sha256":R.identity(admission)["sha256"]}}))
        return {"path":"mapping.json","identity":R.identity(contract),"expected_sha256":R.identity(contract)["sha256"]}
    pin=frozen();assert set(E.admit_osap_mapping(tmp_path,pin))=={"be_me"}
    entries["at_me"]={**entries["be_me"]}
    with pytest.raises(ValueError,match="ambiguous"):E.admit_osap_mapping(tmp_path,frozen())
    entries.pop("at_me")
    with pytest.raises(ValueError,match="original153"):E.admit_osap_mapping(tmp_path,frozen())
    with pytest.raises(ValueError,match="reviewed"):E.admit_osap_mapping(tmp_path,None)
    duplicate=tmp_path/"duplicate.json";duplicate.write_text('{"entries":{"be_me":1,"be_me":2}}')
    with pytest.raises(ValueError,match="duplicate"):E.read_metadata(duplicate)


def test_replication_axes_and_entire_independent_overlap_denominator(monkeypatch):
    calendar=[]
    for n in range(120):
        y,m=divmod(2012*12+11+n,12)
        end=(dt.date(y+1,1,1) if m==11 else dt.date(y,m+2,1))-dt.timedelta(days=1)
        while end.weekday()>=5:end-=dt.timedelta(days=1)
        calendar.append(end)
    mapping={"be_me":{"osap_name":"BM","sign":1},"at_me":{"osap_name":"AM","sign":1}}
    result=E.replication({},[],[],[],mapping,calendar)
    assert len(result["requested_slots"])==120
    assert result["requested_slots"][0]["formation"]=="2012-12-31"
    assert result["gates"]["osap_median_rho"]["denominator"]==2
    assert result["gates"]["osap_median_rho"]["pass"] is False
    panels={date:[{"cik":1,"univ_all_common":True,"classification":"primary","primary_security":True,"be_me":float(i)}] for i,date in enumerate(calendar)}
    benchmark=[]
    for i,date in enumerate(calendar):
        holding=list(E.requested_axes(calendar))[i]
        benchmark.append({"formation":date,"month":dt.date(*holding,1),"feature":"BM","ret":float(i),"unit":"decimal","weighting":"vw_cap","country":"usa"})
    # Isolate matching/denominators from separately tested portfolio formation.
    monkeypatch.setattr(E,"tercile",lambda rows,feature,formation:(float(calendar.index(formation)),None) if feature=="be_me" else (None,"missing_feature"))
    result=E.replication(panels,[],[],benchmark,mapping,calendar)
    assert result["gates"]["osap_median_rho"]["measured"]==pytest.approx(1)
    assert result["gates"]["osap_median_rho"]["matched"]==1
    assert result["gates"]["osap_median_rho"]["denominator"]==2
    assert result["gates"]["osap_median_rho"]["pass"] is False
    bad={"formation":calendar[0],"cik":1,"entry_date":dt.date(2013,2,1),"exit_date":dt.date(2013,2,28),"ret":.1,"return_certified":True}
    with pytest.raises(ValueError,match="axes"):E.replication({},[bad],[],[],mapping,calendar)
    with pytest.raises(ValueError,match="axis"):E.replication({dt.date(2012,11,30):[]},[],[],[],mapping,calendar)


@pytest.mark.parametrize("field,value",[("flow_basis","q4"),("currency","EUR"),("issuer_me",101),("definition","different_book_equity"),("cap_available_at",None),("fund_available_at",dt.datetime(2022,6,30,22,tzinfo=dt.UTC))])
def test_incompatible_ratio_economic_or_clock_basis_cannot_pair(field,value):
    date=dt.date(2022,6,30);early=dt.datetime(2022,6,1,tzinfo=dt.UTC);fiscal=dt.date(2021,12,31)
    row={"cik":1,"accounting_available_at":early,"accounting_datadate":fiscal,"accounting_currency":"USD","issuer_me":100}
    reference={"cik":1,"fund_available_at":early,"datadate":fiscal,"curcd":"USD","me":100}
    proof={"cik":1,"datadate":fiscal,"flow_basis":"genuine_annual","balance_basis":"same_annual_snapshot","currency":"USD","issuer_me":100,"cap_basis":"all_eligible_issuer_classes","definition":"seq+txditc-pstk",**{k:early for k in ("available_at","source_available_at","fund_available_at","cap_available_at")}}
    assert E.ratio_basis_reasons(row,reference,proof,"bm",date)==[]
    assert E.ratio_basis_reasons(row,reference,{**proof,field:value},"bm",date)


def test_thirty_plus_one_unknown_common_classification_blocks_full_month():
    formation=dt.date(2012,12,31);calendar=[formation,dt.date(2013,1,2),dt.date(2013,1,31)]
    data=[{"session_date":formation,"security_id":i,"cik":i,"univ_all_common":True,"classification":"primary","primary_security":True,"nyse":True,"certified":True,"me":100+i,"be_me":i} for i in range(30)]
    labels=[{"formation":formation,"security_id":i,"cik":i,"entry_date":calendar[1],"exit_date":calendar[2],"ret":i*.001,"return_certified":True} for i in range(30)]
    benchmark=[{"formation":formation,"month":dt.date(2013,1,31),"feature":"be_me","ret":.1,"unit":"decimal","weighting":"vw_cap","country":"usa"}]
    mapping={"be_me":{"osap_name":"BM","sign":1}}
    good=E.replication({formation:data},labels,benchmark,[],mapping,calendar)
    assert good["outcomes"]["be_me"]["months"]==1
    unknown=E.qualify_formation({"session_date":formation,"security_id":30,"cik":30,"univ_all_common":True,"be_me":30},{})
    result=E.replication({formation:data+[unknown]},labels,benchmark,[],mapping,calendar)
    assert result["outcomes"]["be_me"]["months"]==0
    pop=result["formation_populations"][str(formation)]
    assert (pop["full_rows"],pop["common_rows"],pop["primary_rows"],pop["unknown_rows"])==(31,31,30,1)
    assert pop["keyed_reasons"]["30/30"]=="missing_independent_classification"
    assert result["exclusions"][f"{formation}/be_me"]=="unknown_independent_formation_population"
    labels[15]={**labels[15],"ret":None,"return_certified":False,"label_reason":"missing_independent_window_proof"}
    result=E.replication({formation:data},labels,benchmark,[],mapping,calendar)
    assert result["outcomes"]["be_me"]["months"]==0
    assert result["exclusions"][f"{formation}/be_me"]=="missing_independent_complete_monthly_label"


def test_independent_secondary_and_exclusion_proof_are_not_unknown():
    date=dt.date(2012,12,31);early=dt.datetime(2012,12,1,tzinfo=dt.UTC)
    row={"session_date":date,"security_id":2,"cik":1,"issuer_me":100,"univ_all_common":True}
    audit={"formation":date,"security_id":2,"cik":1,"me":100,"available_at":early,"source_available_at":early,"classification":"secondary","classification_proof":"proof","primary_security":False,"certified":True,"nyse":True}
    proof={"formation":str(date),"security_id":2,"cik":1,"classification":"secondary","source_available_at":early.isoformat()}
    secondary=E.qualify_formation(row,audit,{"proof":proof})
    primary={**row,"security_id":1,"classification":"primary"}
    assert E.formation_population([primary,secondary])["unknown_rows"]==0
    assert E.formation_population([secondary])["unresolved_issuers"]==1
    assert E.formation_population([secondary])["unknown_rows"]==0
    excluded=E.qualify_formation(row,{**audit,"classification":"excluded","exclusion_reason":"independently reviewed cohort exclusion"},{"proof":{**proof,"classification":"excluded","exclusion_reason":"independently reviewed cohort exclusion"}})
    assert E.formation_population([excluded])["excluded_rows"]==1
    assert E.qualify_formation(row,audit,{})["classification"] is None


@pytest.mark.parametrize("damage",["none","partial","start_mark","start_identity","intermediate","corporate_action","return","terminal","calendar","missing_window","missing_return","heldout","source_changed"])
def test_complete_monthly_window_start_mark_and_intermediate_proof(tmp_path,damage):
    import json
    from atx_db.alpha_panel import release_inputs as R
    from atx_db.alpha_panel.char_monthly_registry import FREEZE_SHA256
    formation=dt.date(2012,12,31);calendar=[formation,dt.date(2013,1,2),dt.date(2013,1,3),dt.date(2013,1,31)]
    raw=tmp_path/"observations.fixture";raw.write_text("synthetic independently reviewed price/corporate-action observations")
    pin={"path":"observations.fixture","identity":R.identity(raw),"expected_sha256":R.identity(raw)["sha256"]}
    label={"formation":formation,"security_id":1,"cik":1,"entry_date":calendar[1],"exit_date":calendar[-1],"ret":E.K.compound([.01,.02,.03]),"return_certified":True}
    proof={"version":"char_monthly_complete_window_v1","registry_sha256":FREEZE_SHA256,"formation":str(formation),"security_id":1,"cik":1,"terminal_treatment":"none","start_mark":{"session_date":str(formation),"security_id":1,"cik":1,"basis":"previous_month_close_total_return","price":100,"price_source":pin,"corporate_action_source":pin},"observations":[{"session_date":str(day),"security_id":1,"cik":1,"total_return":value,"price_source":pin,"corporate_action_source":pin} for day,value in zip(calendar[1:],[.01,.02,.03])]}
    if damage=="partial":label.update(entry_date=dt.date(2013,1,15),exit_date=dt.date(2013,1,15))
    if damage=="start_mark":proof["start_mark"]["session_date"]="2013-01-02"
    if damage=="start_identity":proof["start_mark"]["security_id"]=999
    if damage=="intermediate":proof["observations"].pop(1)
    if damage=="corporate_action":proof["observations"][1].pop("corporate_action_source")
    if damage=="return":label["ret"]=.01
    if damage=="terminal":proof["terminal_treatment"]="unknown_residual_cash"
    if damage=="calendar":calendar.pop(1)
    if damage=="missing_return":proof["observations"][1]["total_return"]=None
    if damage=="heldout":label["exit_date"]=dt.date(2023,1,1)
    path=tmp_path/"window.json";path.write_text(json.dumps(proof))
    binding={"path":"window.json","identity":R.identity(path),"expected_sha256":R.identity(path)["sha256"]}
    windows={f"{formation}/1":binding}
    if damage=="missing_window":windows={}
    if damage=="source_changed":raw.write_text("changed independently pinned synthetic source")
    if damage=="none":assert E.qualify_label(tmp_path,label,calendar,windows)["label_reason"] is None
    elif damage in ("terminal","missing_window","missing_return"):
        result=E.qualify_label(tmp_path,label,calendar,windows)
        assert result["ret"] is None and result["return_certified"] is False and result["label_reason"]
    else:
        with pytest.raises(ValueError):E.qualify_label(tmp_path,label,calendar,windows)


def test_partial_month_and_wrong_last_calendar_session_refused_before_pairing():
    formation=dt.date(2012,12,31);calendar=[formation,dt.date(2013,1,2),dt.date(2013,1,15),dt.date(2013,1,31)]
    label={"formation":formation,"cik":1,"security_id":1,"entry_date":dt.date(2013,1,15),"exit_date":dt.date(2013,1,15),"ret":.1,"return_certified":True}
    with pytest.raises(ValueError,match="first/last"):E.replication({},[label],[],[],{"be_me":{"osap_name":"BM","sign":1}},calendar)
