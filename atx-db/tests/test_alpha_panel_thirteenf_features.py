from atx_db.alpha_panel.thirteenf_features import ownership_snapshot
import datetime as dt
import json
import pytest


def snapshot(current, previous, **kwargs):
    args = dict(current_managers={"a", "b"}, previous_managers={"a", "b"},
                cohort_complete=True, previous_complete=True, denominator_shares=100,
                prior_to_current_split=1, denominator_available=True)
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
