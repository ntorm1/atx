"""Read-only operation, production-access refusal and economic checks on tiny fixtures."""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import os
import sys
import time
from pathlib import Path

import duckdb
import pytest

from tests.test_market_daily import _fact

pytest_plugins = ("tests.test_fundamental_signal_research",)
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("desk_question_pack", ROOT / "scripts/read_desk_question_pack.py")
assert spec is not None and spec.loader is not None
reader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(reader)


@pytest.fixture(autouse=True)
def bounded_connections(monkeypatch):
    real_connect = duckdb.connect

    def connect(*args, **kwargs):
        config = dict(kwargs.pop("config", {}))
        config.update(memory_limit="256MB", threads="1")
        return real_connect(*args, config=config, **kwargs)

    monkeypatch.setattr(duckdb, "connect", connect)


def query(con, key, **params):
    sql = (ROOT / "sql/research" / reader.SQL_FILES[key]).read_text(encoding="utf-8")
    cursor = con.execute(sql, params)
    columns = [column[0] for column in cursor.description]
    return [dict(zip(columns, row, strict=True)) for row in cursor.fetchall()]


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_real_read_only_connection_rejects_mutation_and_keeps_bytes(tmp_path):
    path = tmp_path / "small.duckdb"
    with duckdb.connect(str(path)) as con:
        con.execute("CREATE TABLE protected(value INTEGER)")
    before = _sha(path)
    with reader.open_read_only(path, "256MB", 1) as con:
        with pytest.raises(duckdb.Error, match="read-only"):
            con.execute("INSERT INTO protected VALUES (1)")
        assert con.execute("SELECT count(*) FROM protected").fetchone()[0] == 0
    assert _sha(path) == before


@pytest.mark.parametrize("protected", ["data/warehouse.duckdb", "warehouse_template.duckdb"])
def test_original_database_paths_are_refused_before_connect(protected, tmp_path, monkeypatch):
    def forbidden(*_args, **_kwargs):
        pytest.fail("protected database opened")

    monkeypatch.setattr(reader, "open_read_only", forbidden)
    output = tmp_path / "out.json"
    assert reader.main(["--db-path", str(ROOT / protected), "--output-json", str(output)]) == 2
    assert not output.exists()


# --- Governed production access -------------------------------------------------------------------

def _guard_receipt(path: Path, **overrides) -> Path:
    """The receipt run_memory_guarded.py writes once it has launched this process."""
    receipt = {"command": [sys.executable, str(ROOT / "scripts/read_desk_question_pack.py"), "--production"],
               "job_limit_gb": 2, "status": "running", "child_pid": os.getpid(),
               "headroom": {"physical_free_gb": 6.5, "commit_free_gb": 9.0}}
    receipt.update(overrides)
    path.write_text(json.dumps(receipt), encoding="utf-8")
    return path


@pytest.fixture
def governed(tmp_path, monkeypatch):
    """A stand-in production warehouse with a complete guard; each refusal case breaks one piece."""
    production = tmp_path / "warehouse.duckdb"
    with duckdb.connect(str(production)) as con:
        con.execute("CREATE TABLE protected(value INTEGER)")
    monkeypatch.setattr(reader, "PRODUCTION_DB", production)
    monkeypatch.setattr(reader, "GUARD_RECEIPT_WAIT_SECONDS", 0)
    monkeypatch.setattr(reader, "job_memory_limit_bytes", lambda: 2 * reader.GIB)
    monkeypatch.setenv(reader.GUARD_RECEIPT_ENV, str(_guard_receipt(tmp_path / "guard.json")))
    return production


def _stale(tmp_path, _monkeypatch):
    old = time.time() - 600
    os.utime(tmp_path / "guard.json", (old, old))


def _low_disk(_tmp_path, monkeypatch):
    monkeypatch.setattr(reader.shutil, "disk_usage", lambda _path: type("Usage", (), {"free": 2 * reader.GIB})())


_REFUSALS = {
    "no_production_flag": ([], None),
    "no_guard_env": (["--production"], lambda _t, m: m.delenv(reader.GUARD_RECEIPT_ENV)),
    "guard_receipt_absent": (["--production"],
                             lambda t, m: m.setenv(reader.GUARD_RECEIPT_ENV, str(t / "absent.json"))),
    "guard_not_running": (["--production"], lambda t, _m: _guard_receipt(t / "guard.json", status="completed")),
    "guard_names_other_process": (["--production"], lambda t, _m: _guard_receipt(
        t / "guard.json", child_pid=max(os.getpid(), os.getppid()) + 1)),
    "guard_receipt_stale": (["--production"], _stale),
    "guard_cap_above_2gib": (["--production"], lambda t, _m: _guard_receipt(t / "guard.json", job_limit_gb=4)),
    "guard_command_not_this_reader": (["--production"], lambda t, _m: _guard_receipt(
        t / "guard.json", command=["python", "scripts/warehouse_activate.py", "--production"])),
    "not_in_a_memory_capped_job": (["--production"],
                                   lambda _t, m: m.setattr(reader, "job_memory_limit_bytes", lambda: None)),
    "job_cap_above_receipt": (["--production"],
                              lambda _t, m: m.setattr(reader, "job_memory_limit_bytes", lambda: 3 * reader.GIB)),
    "memory_override": (["--production", "--memory-limit", "256MB"], None),
    "low_spill_disk": (["--production"], _low_disk),
}


@pytest.mark.parametrize("case", sorted(_REFUSALS))
def test_production_path_is_refused_without_flag_and_live_guard(case, governed, tmp_path, monkeypatch, capsys):
    flags, breaker = _REFUSALS[case]
    if breaker is not None:
        breaker(tmp_path, monkeypatch)

    def forbidden(*_args, **_kwargs):
        pytest.fail("production database opened without the governed access checks")

    monkeypatch.setattr(reader, "open_read_only", forbidden)
    before = _sha(governed)
    output = tmp_path / "out.json"
    assert reader.main(["--db-path", str(governed), "--output-json", str(output), *flags]) == 2
    assert not output.exists() and _sha(governed) == before
    assert "desk pack refused" in capsys.readouterr().err


def test_relocated_runner_refuses_live_data_paths_and_warehouse_sized_files(tmp_path, monkeypatch):
    # A copy of the runner (RX3 pinned export, pool worktree) has its own ROOT; the
    # governed warehouse and every atx-db/data directory must stay protected.
    monkeypatch.setattr(reader, "ROOT", tmp_path / "exports" / ("0" * 40) / "atx-db")
    monkeypatch.setattr(reader, "open_read_only", lambda *_a, **_k: pytest.fail("database opened"))
    live_data = tmp_path / "live" / "atx-db" / "data"
    live_data.mkdir(parents=True)
    backup = live_data / "warehouse.pre-migrate.duckdb"
    backup.write_bytes(b"x")
    fixture = tmp_path / "fixture.duckdb"
    fixture.write_bytes(b"x" * 2048)
    monkeypatch.setattr(reader, "L1_MAX_DATABASE_BYTES", 1024)
    for index, database in enumerate((backup, reader.PRODUCTION_DB, fixture)):
        output = tmp_path / f"out{index}.json"
        assert reader.main(["--db-path", str(database), "--output-json", str(output)]) == 2
        assert not output.exists()


def test_production_flag_never_reads_another_database(governed, tmp_path, monkeypatch):
    other = tmp_path / "fixture.duckdb"
    with duckdb.connect(str(other)) as con:
        con.execute("CREATE TABLE t(value INTEGER)")
    monkeypatch.setattr(reader, "open_read_only", lambda *_a, **_k: pytest.fail("opened"))
    output = tmp_path / "out.json"
    assert reader.main(["--production", "--db-path", str(other), "--output-json", str(output)]) == 2
    assert not output.exists()


def test_job_memory_probe_reports_a_cap_or_none():
    cap = reader.job_memory_limit_bytes()
    assert cap is None or cap > 0


def test_governed_production_read_binds_and_runs_every_query_inside_the_guard(
        built_warehouse, tmp_path, monkeypatch):
    path = built_warehouse("warehouse.duckdb")
    monkeypatch.setattr(reader, "PRODUCTION_DB", path)
    monkeypatch.setattr(reader, "job_memory_limit_bytes", lambda: 2 * reader.GIB)
    monkeypatch.setenv(reader.GUARD_RECEIPT_ENV, str(_guard_receipt(tmp_path / "guard.json")))
    before = _sha(path)
    receipts = tmp_path / "receipts"
    receipts.mkdir()
    output = receipts / "desk.json"
    assert reader.main(["--production", "--db-path", str(path), "--output-json", str(output),
                        "--mode", "run", "--max-rows", "5"]) == 0
    receipt = json.loads(output.read_text())
    assert (receipt["contract"], receipt["access_mode"]) == ("desk-question-pack-governed-v1", "governed_production")
    assert receipt["read_only"] and not receipt["production_qualified"]
    limits = receipt["limits"]
    assert (limits["memory_limit"], limits["threads"], limits["spill_limit"], limits["timeout_seconds"]) == (
        "1GB", 1, "2GB", 600)
    settings = receipt["effective_settings"]
    assert reader._setting_bytes(settings["memory_limit"]) <= 10 ** 9
    assert 0 < reader._setting_bytes(settings["max_temp_directory_size"]) <= 2 * 10 ** 9
    assert (settings["threads"], settings["enable_external_access"], settings["access_mode"],
            settings["lock_configuration"]) == ("1", "false", "read_only", "true")
    assert receipt["guard"]["child_pid"] == os.getpid() and receipt["guard"]["job_memory_limit_bytes"] == 2 * reader.GIB
    # Every desk query binds and executes on the current (>= 0327) schema template.
    assert int(receipt["schema_version"]) >= 327
    queries = {q["query"]: q for q in receipt["queries"]}
    assert all(q["bind_status"] == "bound" and not q["production_qualified"] for q in queries.values())
    assert {name: q["status"] for name, q in queries.items()} == {
        "q1": "empty", "q2": "empty", "q3": "empty", "q4": "empty", "q5": "inspection_only"}
    assert queries["q5"]["truncated"] and queries["q5"]["returned_rows"] == 5
    assert receipt["status"] == "inspection_complete" and not receipt["database_file_changed"]
    assert _sha(path) == before
    # The private spill directory is removed; only the receipt remains.
    assert [entry.name for entry in receipts.iterdir()] == ["desk.json"]


# --- L1 inspection ---------------------------------------------------------------------------------

def test_runner_caps_rows_and_marks_truncation_on_existing_schema_fixture(built_warehouse, tmp_path):
    path = built_warehouse("desk.duckdb")
    output = tmp_path / "receipt.json"
    before = _sha(path)
    assert reader.main(["--db-path", str(path), "--output-json", str(output),
                        "--query", "q5", "--mode", "run", "--max-rows", "2"]) == 0
    receipt = json.loads(output.read_text())
    result = receipt["queries"][0]
    assert receipt["read_only"] and not receipt["production_qualified"]
    assert receipt["access_mode"] == "l1_inspection" and receipt["guard"] is None
    assert result["truncated"] and result["returned_rows"] == 2
    assert {row["status"] for row in result["rows"]} == {"build_manifest_missing"}
    assert _sha(path) == before
    original = output.read_bytes()
    assert reader.main(["--db-path", str(path), "--output-json", str(output)]) == 2
    assert output.read_bytes() == original


def test_connection_failure_writes_an_unavailable_receipt(tmp_path):
    database = tmp_path / "invalid.duckdb"
    database.write_bytes(b"not a database")
    output = tmp_path / "failure.json"
    assert reader.main(["--db-path", str(database), "--output-json", str(output)]) == 2
    receipt = json.loads(output.read_text())
    assert receipt["status"] == "unavailable" and receipt["queries"] == []
    assert receipt["error_type"] and not receipt["production_qualified"]
    assert database.read_bytes() == b"not a database"


def test_q1_negative_base_restatement_cutoff_and_missing_ttm(tmp_store):
    con = tmp_store.con
    for end, start, value in (
        ("2025-06-30", "2025-04-01", -1.0),
        ("2026-03-31", "2026-01-01", 0.5),
        ("2026-06-30", "2026-04-01", 2.0),
    ):
        day = dt.date.fromisoformat(end)
        at = dt.datetime.combine(day + dt.timedelta(days=35), dt.time(12))
        _fact(tmp_store, "issuer", "eps_diluted", "quarterly", day, value, at)
        con.execute("""
          UPDATE fundamental_standardized SET cik='0000093410',period_start=?,
            fiscal_year=?,fiscal_period=? WHERE period_end=?
        """, [start, day.year, "Q" + str((day.month + 2) // 3), day])
    # SEC companyfacts fy/fp belong to the FILING: the 2026 Q2 10-Q re-reports the
    # prior-year quarter under fy=2026. That later label must not break the YoY.
    con.execute("""
      INSERT INTO fundamental_standardized
      SELECT * REPLACE ('comparative' AS standardized_id,2026 AS fiscal_year,
        TIMESTAMP '2026-08-05 22:00:00' AS available_at,DATE '2026-08-05' AS as_of_date)
      FROM fundamental_standardized WHERE period_end=DATE '2025-06-30'
    """)
    params = {"cik": "93410", "cutoff": dt.datetime(2026, 9, 20, 22)}
    rows = query(con, "q1", **params)
    latest = rows[0]
    assert latest["yoy_growth"] == 3 and latest["qoq_growth"] == 3
    assert latest["yoy_status"] == "negative_base_absolute_denominator"
    assert latest["yoy_label_status"] == "labels_consistent"
    assert latest["ttm_eps"] is None and latest["basic_missing_at_endpoint"]
    prior = next(r for r in rows if r["period_end"] == dt.date(2025, 6, 30))
    assert (prior["fiscal_year"], prior["selected_filing_fiscal_year"]) == (2025, 2026)
    assert prior["visible_revision_count"] == 2
    con.execute("""
      INSERT INTO fundamental_standardized
      SELECT * REPLACE ('later-null' AS standardized_id,NULL AS value,
        TIMESTAMP '2026-09-21 12:00:00' AS available_at,DATE '2026-09-21' AS as_of_date)
      FROM fundamental_standardized WHERE period_end=DATE '2026-06-30'
    """)
    assert query(con, "q1", **params)[0]["visible_revision_count"] == 1
    after = query(con, "q1", **{**params, "cutoff": dt.datetime(2026, 9, 22, 22)})[0]
    assert after["reported_quarter_eps"] is None and after["yoy_growth"] is None
    assert after["visible_revision_count"] == 2 and after["changed_since_first_visible"]


def test_q2_q4_preserve_null_states_missing_metrics_and_raw_signs(tiny_panel_store):
    con = tiny_panel_store.con
    # The EPS family is fully registered; the margin families are not in this catalog.
    con.execute("""
      INSERT INTO derived_metric_definitions VALUES ('eps_diluted_q_growth_yoy_accel','q',
        'eps_diluted_q_growth_yoy - lag(eps_diluted_q_growth_yoy, 1)','["metric:eps_diluted_q_growth_yoy"]','1')
    """)
    rows = query(con, "q2", cutoff=dt.datetime(2024, 1, 3, 22))
    assert len(rows) == 9
    eps = next(r for r in rows if r["issuer_owner_id"] == "a" and r["metric_code"] == "eps_diluted_q_growth_yoy")
    # The later NULL state replaces the older numeric one; no prior bucket exists.
    assert eps["current_state_id"] == "a_eps_null" and eps["latest_yoy"] is None
    assert eps["status"] == "missing_comparison"
    assert all(r["acceleration"] is None for r in rows)
    assert sum(r["status"] == "metric_definition_missing" for r in rows) == 6
    # c's newest quarter ended 2023-05-31: never presented as current acceleration.
    stale = next(r for r in rows if r["issuer_owner_id"] == "c" and r["metric_code"] == "eps_diluted_q_growth_yoy")
    assert stale["status"] == "stale_latest_quarter" and stale["acceleration"] is None
    quality = query(con, "q4", cutoff=dt.datetime(2024, 1, 3, 22))
    row = next(r for r in quality if r["issuer_owner_id"] == "a")
    assert row["sloan_accruals"] == -0.1 and row["net_debt_ebitda"] == -0.1
    assert row["screen_status"] == "incomplete" and not row["production_qualified"]


_Q2_FAMILIES = (
    ("eps_diluted_q_growth_yoy", "eps_diluted_q_growth_yoy_accel"),
    ("gross_margin_q_change_yoy", "gross_margin_q_change_yoy_accel"),
    ("operating_margin_q_change_yoy", "operating_margin_q_change_yoy_accel"),
)


def test_q2_reads_registered_accelerations_only_on_proven_adjacent_quarters(tmp_store):
    from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
    from atx_db.derived_registry import seed_derived_metric_definitions
    from tests.test_core_metric_breadth import _QUARTERS, _available, _bars, _growth, _ratio, _seed_statement

    # S1: contiguous calendar quarters. S6: a fiscal-year change with a 66-day stub
    # quarter 2021-04-11..2021-06-15, then contiguous quarters again.
    stub_calendar = (*_QUARTERS[:8], dt.date(2021, 4, 10), dt.date(2021, 6, 15), *_QUARTERS[10:12])
    _seed_statement(tmp_store, "S1", _QUARTERS[:12])
    _seed_statement(tmp_store, "S6", stub_calendar)
    for security_id in ("S1", "S6"):
        _bars(tmp_store, security_id)  # no split: one proven per-share basis
    seed_derived_metric_definitions(tmp_store)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(
        metric_codes=tuple(code for family in _Q2_FAMILIES for code in family)))

    def q2(cutoff):
        return {(r["issuer_owner_id"], r["metric_code"]): r for r in query(tmp_store.con, "q2", cutoff=cutoff)}

    def expected(code, index):
        if code == "eps_diluted_q_growth_yoy":
            return _growth("eps_diluted", index) - _growth("eps_diluted", index - 1)
        numerator = "gross_profit__1004" if code.startswith("gross") else "operating_income"
        return ((_ratio(numerator, index) - _ratio(numerator, index - 4))
                - (_ratio(numerator, index - 1) - _ratio(numerator, index - 5)))

    # Cutoff after the 2021-09-30 quarter is public, before 2021-12-31's.
    early = q2(_available(_QUARTERS[10]) + dt.timedelta(days=1))
    assert len(early) == 6 and all(r["period_end"] <= _QUARTERS[10] for r in early.values())
    for code, accel_code in _Q2_FAMILIES:
        s1, s6 = early[("S1", code)], early[("S6", code)]
        assert (s1["accel_code"], s1["accel_value_origin"]) == (accel_code, "quarterly")
        assert s1["status"] == "registered_candidate_lineage_unverified"
        assert s1["acceleration"] == pytest.approx(expected(code, 10))
        assert s1["acceleration"] == s1["latest_yoy"] - s1["prior_quarter_yoy"]
        assert s1["accel_available_at"] == _available(_QUARTERS[10])
        # The prior quarter is the 66-day stub: the engine cannot prove one-quarter
        # adjacency, so both growth values stay visible but no acceleration is shown.
        assert (s6["status"], s6["accel_value_origin"], s6["acceleration"]) == (
            "accel_adjacency_unproven", "incomparable", None)
        assert s6["latest_yoy"] is not None and s6["prior_end"] == dt.date(2021, 6, 15)
    assert early[("S1", "eps_diluted_q_growth_yoy")]["eps_base_status"] == "positive_bases"
    assert early[("S1", "gross_margin_q_change_yoy")]["eps_base_status"] == "not_applicable"

    # Once the next contiguous quarter is public, S6 has a proven adjacent pair again.
    late_cutoff = _available(_QUARTERS[11]) + dt.timedelta(days=1)
    late = q2(late_cutoff)
    for code, _accel_code in _Q2_FAMILIES:
        for security_id in ("S1", "S6"):
            row = late[(security_id, code)]
            assert row["status"] == "registered_candidate_lineage_unverified", (security_id, code)
            assert row["acceleration"] == pytest.approx(expected(code, 11))

    # A registered acceleration that does not equal the growth states it sits on
    # (e.g. a growth revision the acceleration was not re-derived from) is withheld.
    tmp_store.con.execute("""
      UPDATE derived_metric_values SET value=value+0.5
      WHERE security_id='S1' AND metric_code='eps_diluted_q_growth_yoy_accel' AND period_end=?
    """, [_QUARTERS[11]])
    tampered = q2(late_cutoff)[("S1", "eps_diluted_q_growth_yoy")]
    assert (tampered["status"], tampered["acceleration"]) == ("accel_growth_state_mismatch", None)


def test_all_queries_bind_and_empty_history_retains_every_month(tmp_store):
    parameters = {
        "q1": {"cutoff": dt.datetime(2026, 9, 20, 22), "cik": "0000093410"},
        "q2": {"cutoff": dt.datetime(2026, 9, 20, 22)},
        "q3": {"cutoff": dt.datetime(2026, 9, 20, 22), "market_cap_floor": 1e9},
        "q4": {"cutoff": dt.datetime(2026, 9, 20, 22)},
        "q5": {"cutoff": dt.datetime(2026, 9, 20, 22), "build_run_id": "none",
               "evaluation_run_id": "none", "signal_id": "eps_growth_yoy",
               "start_date": dt.date(2015, 1, 1), "end_date": dt.date(2026, 12, 31)},
    }
    for key in ("q1", "q2", "q3", "q4"):
        assert query(tmp_store.con, key, **parameters[key]) == []
    rows = query(tmp_store.con, "q5", **parameters["q5"])
    assert len(rows) == 144 * 2
    assert sum(r["status"] == "future_month" for r in rows) == 6
    assert sum(r["status"] == "month_not_closed_at_cutoff" for r in rows) == 2
    assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)


def _q3_owner(con, owner, cik, *, latest_null_roe=False):
    con.execute("""
      INSERT INTO fundamental_fact_revisions
      (fact_revision_id,revision_group_id,source,security_id,cik,taxonomy,concept,unit,
       period_end,accession_number,filed_date,revision_sequence,revision_count,
       is_latest_revision,is_value_changed,source_url,as_of_date,available_at)
      VALUES (?,?,'fixture',?,?,'us-gaap','NetIncomeLoss','USD','2026-06-30','a',
        '2026-08-01',1,1,true,false,'fixture','2026-08-01','2026-08-01')
    """, [owner, owner, owner, cik])
    for code in ("roic", "roe", "gross_profitability"):
        con.execute("""
          INSERT INTO derived_metric_values
          (derived_value_id,source,security_id,metric_code,metric_window,period_end,
           available_at,inputs_hash,as_of_date,value,value_status,target_bucket,fiscal_period_end)
          VALUES (?,'atx-db declarative derived metrics v1',?,?,'ttm','2026-06-30',
            '2026-08-01','hash','2026-08-01',.1,'valid',8106,'2026-06-30')
        """, [owner + code, owner, code])
    if latest_null_roe:
        con.execute("""
          INSERT INTO derived_metric_values
          SELECT * REPLACE ('latest-null-roe' AS derived_value_id,NULL AS value,
            'missing_input_or_domain' AS value_status,TIMESTAMP '2026-08-20' AS available_at,
            DATE '2026-08-20' AS as_of_date)
          FROM derived_metric_values WHERE security_id=? AND metric_code='roe'
        """, [owner])


def _q3_line(con, security, owner, cik, *, pe, cap=2e9, shares_source="dei", market_owner=None, volume=1_000):
    con.execute("""
      INSERT INTO universe_us_listed_membership
      (membership_id,universe_id,security_id,valid_from,available_at,security_type,
       exchange_code,has_cik,cik,reason,rules_json,decision_count,as_of_date,source)
      VALUES (?,'us_listed_v1',?,'2020-01-01','2020-01-01','common',
        'XNYS',true,?,'fixture','{}',1,'2020-01-01','atx-db us-listed universe builder')
    """, [security, security, cik])
    con.execute("""
      INSERT INTO security_identifier_history
      (security_id,id_type,id_value,valid_from,as_of_date,available_at,source)
      VALUES (?,'CIK',?,'2020-01-01','2020-01-01','2020-01-01','fixture')
    """, [security, cik])
    con.execute("""
      INSERT INTO market_daily_metrics
      (market_daily_id,source,security_id,trade_date,available_at,inputs_hash,as_of_date,
       close,volume,shares_source,owner_security_id,identity_basis,availability_basis,link_method,
       market_cap,pe_ttm,ev_ebitda,pb,fcf_yield,ebit_to_ev,enterprise_value)
      VALUES (?,'atx-db daily market panel v1',?,'2026-09-18','2026-09-18 22:00',
        'hash','2026-09-18',50,?,?,?,'current_ticker_unverified','modeled','current_sec_ticker',
        ?,?,8,2,.05,.1,3000000000)
    """, [security, security, volume, shares_source, market_owner or owner, cap, pe])


def test_q3_share_basis_owner_link_deciles_floor_and_latest_null(tmp_store):
    con = tmp_store.con
    for index in range(12):
        owner, cik = f"issuer_{index:02}", str(index + 1).zfill(10)
        _q3_owner(con, owner, cik, latest_null_roe=index == 10)
        # trading_02's panel owner is the bridge's SEC-CIK key for the same CIK: same issuer, no mismatch.
        _q3_line(con, f"trading_{index:02}", owner, cik, pe=float(10 + index), cap=1e8 if index == 11 else 2e9,
                 shares_source="archive" if index == 9 else "dei",
                 market_owner=f"SEC-CIK-{cik}" if index == 2 else None)
    # A resolved two-class issuer: both class lines carry the class-summed issuer cap.
    _q3_owner(con, "issuer_12", "0000000013")
    _q3_line(con, "class_a", "issuer_12", "0000000013", pe=100.0, cap=5e9, shares_source="class_sum", volume=100)
    _q3_line(con, "class_b", "issuer_12", "0000000013", pe=100.0, cap=5e9, shares_source="class_sum", volume=10_000)
    # Stored multiples under a withheld or missing share basis are never shown
    # (vendor_shares_zero: an A8 final label); the SQL withholds every A8 label.
    from atx_db.market_daily import SHARES_SOURCES_WITHHELD

    q3_sql = (ROOT / "sql/research" / reader.SQL_FILES["q3"]).read_text(encoding="utf-8")
    assert all(f"'{label}'" in q3_sql for label in SHARES_SOURCES_WITHHELD)
    _q3_owner(con, "issuer_14", "0000000015")
    _q3_line(con, "withheld", "issuer_14", "0000000015", pe=5.0, shares_source="vendor_shares_zero")
    _q3_owner(con, "issuer_15", "0000000016")
    _q3_line(con, "unlabeled", "issuer_15", "0000000016", pe=7.0, shares_source=None)
    # The panel joined another owner's fundamentals to this line.
    _q3_owner(con, "issuer_16", "0000000017")
    _q3_line(con, "mismatch", "issuer_16", "0000000017", pe=9.0, market_owner="issuer_00")

    rows = {row["security_id"]: row for row in query(con, "q3", cutoff=dt.datetime(2026, 9, 20, 22),
                                                     market_cap_floor=1e9)}
    assert len(rows) == 17
    first = rows["trading_00"]
    assert (first["issuer_owner_id"], first["share_basis_status"], first["shares_availability_basis"]) == (
        "issuer_00", "verified_dei_shares", "filing_available_at")
    assert (first["identity_basis"], first["link_method"], first["owner_link_availability_basis"]) == (
        "current_ticker_unverified", "current_sec_ticker", "modeled")
    vendor = rows["trading_09"]
    assert (vendor["status"], vendor["share_basis_status"], vendor["shares_availability_basis"]) == (
        "candidate_complete", "unverified_vendor_shares", "vendor_run_clock")
    # One ranked line per issuer: the more traded class line; the other is a diagnostic.
    assert (rows["class_b"]["status"], rows["class_b"]["share_basis_status"]) == (
        "candidate_complete", "unverified_vendor_shares")
    assert (rows["class_a"]["status"], rows["class_a"]["pe_decile"]) == ("secondary_issuer_line", None)
    assert rows["class_b"]["issuer_candidate_lines"] == 2
    ranked = [f"trading_{index:02}" for index in range(10)] + ["class_b"]
    assert [rows[name]["pe_decile"] for name in ranked] == [1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    assert all(rows[name]["ranked_names"] == 11 and rows[name]["ranked_unverified_share_basis"] == 2
               for name in ranked)
    for name, status, basis in (("withheld", "share_basis_withheld", "withheld"),
                                ("unlabeled", "share_basis_unlabeled", "unlabeled")):
        row = rows[name]
        assert (row["status"], row["share_basis_status"], row["pe_decile"]) == (status, basis, None)
        assert row["market_cap"] is None and row["pe_ttm"] is None and row["ev_ebit"] is None
    mismatch = rows["mismatch"]
    assert (mismatch["status"], mismatch["market_owner_security_id"], mismatch["issuer_owner_id"]) == (
        "market_owner_mismatch", "issuer_00", "issuer_16")
    assert rows["trading_10"]["roe"] is None and rows["trading_10"]["status"] == "profitability_missing"
    assert rows["trading_11"]["status"] == "market_cap_floor_or_missing"
    assert all(rows[name]["pe_decile"] is None for name in rows if name not in ranked)
    assert not any(row["production_qualified"] for row in rows.values())


def test_q5_never_substitutes_a_populated_midmonth_for_missing_month_end(tmp_path):
    from atx_db._forward_return_publication import CALCULATION_VERSION
    from atx_db.fundamental_signal_evaluation import (
        FundamentalSignalEvaluationOptions,
        evaluate_fundamental_signals,
    )
    from tests.test_fundamental_signal_evaluation import _warehouse

    store, day, as_of, signal = _warehouse(tmp_path)
    try:
        evaluate_fundamental_signals(store, FundamentalSignalEvaluationOptions(
            build_run_id="build", run_id="eval", as_of_date=as_of,
            run_at=dt.datetime.combine(as_of, dt.time(22, 30), dt.UTC), label_source="labels",
        ))
        rows = query(store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                     start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                     build_run_id="build", evaluation_run_id="eval", signal_id=signal)
        assert len(rows) == 2 and all(r["decision_date"] != day for r in rows)
        assert all(r["status"] == "month_end_not_evaluated" for r in rows)
        assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)
        assert {r["label_version"] for r in rows} == {CALCULATION_VERSION}

        def q5_statuses_with_label_version(version):
            store.con.execute(
                "UPDATE fundamental_signal_evaluation_runs "
                "SET config_json=json_merge_patch(config_json, json_object('label_version', ?)) WHERE run_id='eval'",
                [version])
            return {r["status"] for r in query(
                store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                build_run_id="build", evaluation_run_id="eval", signal_id=signal)}

        # Only the publisher's current label version is read (the SQL literal must track it);
        # sealed v1 (no halt-gap stitch) and v2 (vendor 2021-01-04 artifact) runs are refused.
        assert q5_statuses_with_label_version("forward_return_publication_v1") == {"unsupported_evaluation_contract"}
        assert q5_statuses_with_label_version("forward_return_publication_v2") == {"unsupported_evaluation_contract"}
        assert q5_statuses_with_label_version(CALCULATION_VERSION) == {"month_end_not_evaluated"}
        assert store.con.execute("""
          SELECT sum(eligible_count),sum(labeled_count) FROM fundamental_signal_evaluation_deciles
          WHERE run_id='eval' AND horizon_sessions=21
        """).fetchone() == (200, 199)
        # Prices vanish after the evaluated mid-March session: that session must not
        # become March's "month end" just because it is the last observed one.
        store.con.execute("DELETE FROM market_daily_metrics WHERE trade_date>? AND trade_date<'2024-04-01'", [day])
        rows = query(store.con, "q5", cutoff=dt.datetime.combine(as_of, dt.time(23)),
                     start_date=dt.date(2024, 3, 1), end_date=dt.date(2024, 3, 31),
                     build_run_id="build", evaluation_run_id="eval", signal_id=signal)
        assert all(r["decision_date"] == day for r in rows)
        assert all(r["status"] == "observed_month_end_session_missing" for r in rows)
        assert all(r["complete_cohort_q10_minus_q1"] is None for r in rows)
    finally:
        store.connection.close()
