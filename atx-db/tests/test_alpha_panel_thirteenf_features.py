from atx_db.alpha_panel.thirteenf_features import ownership_snapshot
import datetime as dt
import json
import pytest


def snapshot(current, previous, **kwargs):
    args = dict(current_managers={"a", "b"}, previous_managers={"a", "b"},
                cohort_complete=True, previous_complete=True, denominator_shares=100,
                prior_to_current_split=1, denominator_available=True)
    args.update(population_complete=True,table_absence_qualified=True,previous_table_absence_qualified=True)
    args.update(kwargs)
    return ownership_snapshot(current, previous, **args)


def test_split_is_not_buying_and_known_closed_position():
    result = snapshot({"a": 40}, {"a": 20, "b": 5}, prior_to_current_split=2)
    assert result["inst_shares_change"] == -10
    assert result["inst_closed_positions"] == 1
    assert result["inst_breadth_change"] == -1


def test_arrival_is_not_buying_and_incomplete_is_not_zero():
    result = snapshot({"a": 20, "b": 10}, {"a": 20}, previous_managers={"a"})
    assert result["inst_shares_change"] is None
    assert result["common_manager_share_change"] == 0
    assert result["change_reason"] == "reporting_cohort_changed"
    result = snapshot({}, {}, cohort_complete=False)
    assert result["inst_shares"] is None and result["inst_n_holders"] is None
    assert result["inst_shares_change"] is None
    assert result["common_manager_share_change"] is None


def test_zero_unknown_types_and_overownership_are_preserved():
    assert snapshot({}, {})["inst_ownership"] == 0
    result = snapshot({"a": 110}, {}, manager_types={})
    assert result["inst_ownership"] == 1.1 and result["ownership_above_one"]
    assert result["shares_by_filer_type"] == {"unknown": 110}
    assert snapshot({"a": 10}, {}, denominator_available=False)["inst_ownership"] is None
    partial=snapshot({}, {},population_complete=False)
    assert partial["reported_holding_zero"] is True
    assert partial["inst_ownership"] is None and partial["inst_shares"] is None


def test_native_vintage_producer_requires_explicit_bindings_and_clocks(tmp_path, monkeypatch):
    from atx_db.alpha_panel.thirteenf_features import build
    from atx_db.alpha_panel import common as C
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    snapshots = tmp_path / "snapshots.jsonl"
    bindings = tmp_path / "bindings.json"
    bindings.write_text("{}")
    snapshots.write_text("")
    with pytest.raises(ValueError, match="explicit final"):
        build(snapshots, bindings)
    artifact = tmp_path / "source.parquet"
    artifact.write_bytes(b"fixture source identity")
    manifest = tmp_path / "manifest.json"
    manifest.write_text('{"status":"complete"}')
    binding = dict(manifest=str(manifest), sha256=C.sha256_file(manifest),
                   outputs=[dict(path=str(artifact), bytes=artifact.stat().st_size, sha256=C.sha256_file(artifact))])
    bindings.write_text(json.dumps({name: binding for name in ("universe", "fundamentals_v11", "thirteenf_quality")}))
    row = dict(security_id=101, period_q="2020-03-31", available_at="2020-05-15T21:00:00Z",
               current={"a": 20}, previous={"a": 10}, current_managers=["a"], previous_managers=["a"],
               cohort_complete=True, previous_complete=True, denominator_shares=100,
               prior_to_current_split=1, denominator_available=True, source_accessions=["fixture"],
               evidence=[dict(available_at="2020-05-15T20:00:00Z")])
    snapshots.write_text(json.dumps(row) + "\n")
    assert build(snapshots, bindings)["rows"] == 1
    row["evidence"][0]["available_at"] = "2020-05-16T20:00:00Z"
    snapshots.write_text(json.dumps(row) + "\n")
    with pytest.raises(ValueError, match="future or undated"):
        build(snapshots, bindings)


def test_dated_mapping_preserves_future_and_ambiguous_unmatched_value(tmp_path):
    import duckdb
    from atx_db.alpha_panel.thirteenf_features import dated_link_sql
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    h, l = tmp_path / "h.parquet", tmp_path / "l.parquet"
    con.execute("""CREATE TABLE h AS SELECT 'accession' AS accession,1 AS infotable_sk,'CUSIP' AS cusip,
        DATE '2020-03-31' AS period_of_report,100.0 AS value_usd,
        TIMESTAMPTZ '2020-05-15T20:00:00Z' AS available_at""")
    con.execute("""CREATE TABLE l AS SELECT 'CUSIP' AS cusip,101 AS security_id,
        DATE '2019-01-01' AS valid_from,NULL::DATE AS valid_to,
        TIMESTAMP '2020-05-16 20:00:00' AS available_at,true AS cusip_check_ok,'fixture' AS basis""")
    con.execute(f"COPY h TO '{h.as_posix()}' (FORMAT parquet)")
    con.execute(f"COPY l TO '{l.as_posix()}' (FORMAT parquet)")
    cutoff = dt.datetime(2020,5,16,0,tzinfo=dt.UTC)
    query = dated_link_sql(h.as_posix(), l.as_posix(), cutoff)
    result = con.execute(f"SELECT security_id,link_quality,unmatched_value_usd FROM ({query})").fetchone()
    assert result == (None,"dated_link_missing",100)
    con.execute("UPDATE l SET available_at=TIMESTAMP '2020-05-15 21:00:00'")
    con.execute("INSERT INTO l SELECT * REPLACE(102 AS security_id) FROM l")
    con.execute(f"COPY l TO '{l.as_posix()}' (FORMAT parquet)")
    result = con.execute(f"SELECT security_id,link_quality,unmatched_value_usd FROM ({query})").fetchone()
    assert result == (None,"share_class_ambiguous",100)
    con.close()


def test_dated_denominator_rejects_class_ambiguity_conflict_and_late_source(tmp_path):
    import duckdb
    from atx_db.alpha_panel.thirteenf_features import denominator_sql
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    m,s = tmp_path / "m.parquet",tmp_path / "s.parquet"
    con.execute("""CREATE TABLE m AS SELECT 'accession' AS accession,security_id AS infotable_sk,
        security_id,DATE '2020-03-31' AS period_of_report FROM (VALUES(101),(102),(103),(104)) t(security_id)""")
    con.execute("""CREATE TABLE s AS SELECT security_id,DATE '2020-03-31' AS session_date,
        100.0 AS shrout,'SEC' AS shrout_source,DATE '2020-03-31' AS shrout_asof,
        security_id=102 AS shrout_conflict,2 AS issuer_equity_lines,security_id!=103 AS shares_sec_line_level,
        CASE WHEN security_id=104 THEN TIMESTAMP '2020-05-16 00:00:00'
             ELSE TIMESTAMP '2020-05-15 21:00:00' END AS available_at
        FROM (VALUES(101),(102),(103),(104)) t(security_id)""")
    con.execute(f"COPY m TO '{m.as_posix()}' (FORMAT parquet)")
    con.execute(f"COPY s TO '{s.as_posix()}' (FORMAT parquet)")
    sql = denominator_sql(m.as_posix(),s.as_posix(),dt.datetime(2020,5,16,tzinfo=dt.UTC))
    rows = {r[0]:r[1:] for r in con.execute(f"SELECT security_id,denominator_shares,denominator_quality FROM ({sql})").fetchall()}
    assert rows[101] == (100,"candidate_source_backed")
    assert rows[102] == (None,"denominator_conflict_or_unknown")
    assert rows[103] == (None,"denominator_share_class_ambiguous")
    assert rows[104] == (None,"denominator_source_missing")
    con.close()


def test_native_coverage_preserves_full_denominator_strict_clock_and_latest_quarter(tmp_path,monkeypatch):
    import duckdb
    from atx_db.alpha_panel.thirteenf_features import measure_coverage
    from atx_db.alpha_panel.common import sha256_file
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT",str(tmp_path / "lake"))
    con=duckdb.connect()
    member,expected,features=[tmp_path / (name+".parquet") for name in ("member","expected","features")]
    con.execute("""CREATE TABLE m AS SELECT DATE '2020-05-18' AS session_date,security_id,
        TIMESTAMPTZ '2020-05-15T22:00:00Z' AS cutoff FROM (VALUES(101),(102),(103)) t(security_id)""")
    con.execute("""CREATE TABLE f AS SELECT * FROM (VALUES
        (101,DATE '2020-03-31',TIMESTAMPTZ '2020-05-15T21:00:00Z',0.0,0.0,0,0,true,'QUALIFIED'),
        (101,DATE '2019-12-31',TIMESTAMPTZ '2020-05-15T21:30:00Z',9.0,9.0,9,9,true,'QUALIFIED'),
        (103,DATE '2020-03-31',TIMESTAMPTZ '2020-05-15T22:00:00Z',1.0,1.0,1,1,true,'QUALIFIED'))
        t(security_id,period_q,available_at,inst_ownership,inst_ownership_change,inst_n_holders,
          inst_breadth_change,cohort_complete,quality_status)""")
    for table,path in (("m",member),("m",expected),("f",features)):
        con.execute(f"COPY {table} TO '{path.as_posix()}' (FORMAT parquet)")
    specfile=tmp_path / "coverage.json"
    item=dict(path=str(member),path_sha256=sha256_file(member),expected_keys=str(expected),expected_keys_sha256=sha256_file(expected))
    spec={name:item.copy() for name in ("member_equity","univ_all_common","univ_r3000p")}
    specfile.write_text(json.dumps(spec))
    receipt=measure_coverage(specfile,features,tmp_path / "coverage-out.json")
    level=next(row for row in receipt["reports"]["member_equity"] if row["metric"]=="inst_ownership")
    assert level["benchmark_cells"]==3 and level["qualified_cells"]==1
    assert level["known_zero_cells"]==1 and level["no_visible_feature_cells"]==2
    con.execute(f"COPY (SELECT * FROM m WHERE security_id!=102) TO '{member.as_posix()}' (FORMAT parquet)")
    for item in spec.values():
        item["path_sha256"]=sha256_file(member)
    specfile.write_text(json.dumps(spec))
    with pytest.raises(ValueError,match="expected key set mismatch"):
        measure_coverage(specfile,features,tmp_path / "coverage-fail.json")
    con.close()


def test_pit_cohort_baseline_future_manager_entrants_missing_and_amendments():
    from atx_db.alpha_panel.thirteenf_features import cohort_state
    at=lambda day:dt.datetime(2020,5,day,20,tzinfo=dt.UTC)
    def original(manager,day):
        return dict(filer_cik=manager,form="13F-HR",accession=f"manager{manager}-{day}",
                    available_at=at(day).isoformat(),qualified_original=True)
    prior=[original("0001",10),original("2",21)]
    current=[original("1",15),original("2",19)]
    earlier=cohort_state(prior,current,at(20))
    assert earlier["prior_visible_baseline_manager_ids"]==["1"]
    assert earlier["visible_entrant_manager_ids"]==["2"]
    assert earlier["prior_visible_baseline_complete"]
    assert not earlier["full_population_complete"] and earlier["retrospective_inventory_complete"] is None
    missing=cohort_state(prior,[],at(20))
    assert missing["outstanding_manager_ids"]==["1"] and not missing["prior_visible_baseline_complete"]
    amendment=dict(filer_cik="1",form="13F-HR/A",accession="amendment",available_at=at(20).isoformat(),
                   source_qualified=False,amendment_type="RESTATEMENT")
    assert cohort_state(prior,current+[amendment],at(20))["prior_visible_baseline_complete"]
    later=cohort_state(prior,current+[amendment],at(20)+dt.timedelta(microseconds=1))
    assert not later["prior_visible_baseline_complete"]
    assert cohort_state(prior,current,at(22))["prior_visible_baseline_manager_ids"]==["1","2"]


def test_native_cohort_vintages_preserve_missing_populations_and_rejected_accessions(tmp_path,monkeypatch):
    import pyarrow.parquet as pq
    from atx_db.alpha_panel.thirteenf_features import build_cohort_vintages
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT",str(tmp_path / "lake"))
    filing=tmp_path / "filings.jsonl"
    rows=[dict(accession="prior",form="13F-HR",filer_cik=1,report_date="2019-12-31",
               available_at="2020-02-15T20:00:00Z",qualified_original=True),
          dict(accession="current",form="13F-HR",filer_cik=1,report_date="2020-03-31",
               available_at="2020-05-15T20:00:00Z",qualified_original=True),
          dict(accession="missing",form="13F-HR",filer_cik=2,report_date=None,available_at=None)]
    filing.write_text("".join(json.dumps(row)+"\n" for row in rows))
    manifest=tmp_path / "manifest.json"
    manifest.write_text('{"status":"complete"}')
    receipt=build_cohort_vintages(filing,manifest,event_start=dt.datetime(2020,5,1,tzinfo=dt.UTC),
                                 event_end=dt.datetime(2020,6,1,tzinfo=dt.UTC))
    assert receipt["rows"]==1 and receipt["rejected_filings"]==1
    cohort=pq.read_table(tmp_path / "lake/thirteenf_cohorts/cohorts.parquet").to_pylist()[0]
    assert cohort["prior_visible_baseline_complete"]
    assert not cohort["full_population_complete"] and not cohort["economic_ownership_zero_known"]
    assert cohort["source_accessions"]==["current","prior"]
