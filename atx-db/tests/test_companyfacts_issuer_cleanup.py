"""Issuer-bounded replacement, including legacy NULLs and large unrelated history."""

from __future__ import annotations

import datetime as dt

import duckdb
import pandas as pd
import pytest

from atx_db.connection import DuckDBStore
from atx_db.fundamentals import FACT_IDENTIFIER_MATCH_METHOD, SOURCE_NAME, SecCompanyFactsDataset
from atx_db.warehouse import insert_frame

CIK = "0000000001"
FALLBACK = "SEC-CIK-0000000001"
KEY_A = ("filing-a", "us-gaap", "Assets", "USD", dt.date(2023, 12, 31), dt.date(2023, 1, 1))
KEY_B = ("filing-b", *KEY_A[1:])
KEY_NULL_ID = ("filing-null-id", *KEY_A[1:])
KEY_COLUMNS = "accession_number, taxonomy, concept, unit, period_end, period_start"


@pytest.fixture
def cleanup_store(tmp_path):
    # Reduced legacy schema deliberately permits NULL IDs and all six NULL keys;
    # current production NOT NULL contracts are covered by test_companyfacts_zip.
    store = DuckDBStore(tmp_path / "cleanup.duckdb")
    store.connection = duckdb.connect(str(store.path), config={"threads": 1})
    store._configure_session(store.con)
    store.con.execute("""CREATE TABLE sec_company_facts (
        source VARCHAR, security_id VARCHAR, cik VARCHAR, run_id VARCHAR,
        accession_number VARCHAR, taxonomy VARCHAR, concept VARCHAR, unit VARCHAR,
        period_end DATE, period_start DATE)""")
    store.con.execute("""CREATE TABLE fundamental_points (
        source VARCHAR, security_id VARCHAR, run_id VARCHAR,
        accession_number VARCHAR, taxonomy VARCHAR, metric VARCHAR, unit VARCHAR,
        period_end DATE, period_start DATE)""")
    store.con.execute("CREATE TABLE sec_company_tickers (cik VARCHAR, security_id VARCHAR)")
    store.con.execute("""CREATE TABLE identifier_resolution_candidates (
        candidate_id VARCHAR PRIMARY KEY, source_dataset_id VARCHAR, match_method VARCHAR,
        source_key_type VARCHAR, source_key_value VARCHAR)""")
    try:
        yield store
    finally:
        store.connection.close()


def _fact(store, marker, *, security_id="OLD-A", cik="1", key=KEY_A):
    store.con.execute(
        f"INSERT INTO sec_company_facts (source, security_id, cik, run_id, {KEY_COLUMNS}) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [SOURCE_NAME, security_id, cik, marker, *key],
    )


def _point(store, marker, *, security_id="OLD-A", source=SOURCE_NAME, key=KEY_A):
    store.con.execute(
        f"INSERT INTO fundamental_points (source, security_id, run_id, {KEY_COLUMNS.replace('concept', 'metric')}) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [source, security_id, marker, *key],
    )


def _empty_replace(store, cik=CIK):
    return SecCompanyFactsDataset()._replace_facts(store, pd.DataFrame(), pd.DataFrame(), "PASSED", cik=cik)


def _assert_no_cleanup_tables(store):
    assert store.con.execute("""SELECT table_name FROM duckdb_tables()
        WHERE temporary AND table_name IN (
            'companyfacts_old_fact_keys', 'companyfacts_legacy_ids', 'companyfacts_point_delete_keys')""").fetchall() == []


def test_empty_replacement_keeps_exact_issuer_identity_key_scope(cleanup_store):
    store = cleanup_store
    for marker, sid, cik, key in (
        ("old-a", "OLD-A", "1", KEY_A),
        ("duplicate-a", "OLD-A", CIK, KEY_A),
        ("old-b", "OLD-B", CIK, KEY_B),
        ("old-c", "OLD-C", " 1 ", KEY_A),
        ("old-d", "OLD-D", "00000000000001", KEY_A),
        ("null-keys", "OLD-NULL", "1", (None,) * 6),
        ("null-id", None, "1", KEY_NULL_ID),
        ("other-cik", "OTHER", "2", KEY_A),
        ("junk-cik", "JUNK", "1.0", KEY_A),
        ("signed-cik", "SIGNED", "+1", KEY_A),
    ):
        _fact(store, marker, security_id=sid, cik=cik, key=key)
    store.con.executemany("INSERT INTO sec_company_tickers VALUES (?, ?)", [
        ("1", "TICKER"), (CIK, "TICKER"), (" 1 ", "TICKER-SPACE"),
        ("00000000000001", "TICKER-ZERO"), ("1", None),
        ("2", "OTHER-TICKER"), ("1.0", "JUNK-TICKER"),
    ])
    for sid, key in (
        ("OLD-A", KEY_A), ("OLD-B", KEY_B), ("OLD-C", KEY_A), ("OLD-D", KEY_A),
        ("OLD-NULL", (None,) * 6), ("PASSED", KEY_B), (FALLBACK, KEY_A),
        ("TICKER", KEY_B), ("TICKER-SPACE", KEY_A), ("TICKER-ZERO", KEY_B),
        ("PASSED", (None,) * 6), ("PASSED", KEY_NULL_ID),
    ):
        _point(store, f"remove-{sid}-{key[0]}", security_id=sid, key=key)

    retained = {
        "own-id-other-key", "other-own-id-key", "other-cik", "other-source", "missing-key",
        "null-id", "junk-cik", "signed-cik", "other-ticker", "junk-ticker",
    }
    _point(store, "own-id-other-key", security_id="OLD-A", key=KEY_B)
    _point(store, "other-own-id-key", security_id="OLD-B", key=KEY_A)
    _point(store, "other-cik", security_id="OTHER")
    _point(store, "other-source", source="other source")
    _point(store, "missing-key", security_id=FALLBACK, key=("missing", *KEY_A[1:]))
    _point(store, "null-id", security_id=None, key=KEY_NULL_ID)
    _point(store, "junk-cik", security_id="JUNK")
    _point(store, "signed-cik", security_id="SIGNED")
    _point(store, "other-ticker", security_id="OTHER-TICKER")
    _point(store, "junk-ticker", security_id="JUNK-TICKER")
    for index in range(6):
        key = list(KEY_A)
        key[index] = None
        marker = f"different-key-{index}"
        retained.add(marker)
        _point(store, marker, key=tuple(key))

    candidates = [
        ("remove", "sec_company_facts", FACT_IDENTIFIER_MATCH_METHOD, "CIK", " 1 "),
        ("keep-cik", "sec_company_facts", FACT_IDENTIFIER_MATCH_METHOD, "CIK", "2"),
        ("keep-junk", "sec_company_facts", FACT_IDENTIFIER_MATCH_METHOD, "CIK", "1.0"),
        ("keep-type", "sec_company_facts", FACT_IDENTIFIER_MATCH_METHOD, "ticker", "1"),
        ("keep-method", "sec_company_facts", "other", "CIK", "1"),
        ("keep-dataset", "other", FACT_IDENTIFIER_MATCH_METHOD, "CIK", "1"),
    ]
    store.con.executemany("INSERT INTO identifier_resolution_candidates VALUES (?, ?, ?, ?, ?)", candidates)

    for _ in range(2):  # Empty replacement and retry both keep unrelated evidence.
        assert _empty_replace(store) == 0
        assert {row[0] for row in store.con.execute("SELECT run_id FROM fundamental_points").fetchall()} == retained
        assert store.con.execute("SELECT run_id FROM sec_company_facts ORDER BY run_id").fetchall() == [
            ("junk-cik",), ("other-cik",), ("signed-cik",),
        ]
        assert {row[0] for row in store.con.execute(
            "SELECT candidate_id FROM identifier_resolution_candidates"
        ).fetchall()} == {row[0] for row in candidates if row[0] != "remove"}
        _assert_no_cleanup_tables(store)


def test_legacy_id_without_old_fact_evidence_keeps_points(cleanup_store):
    for sid in ("PASSED", FALLBACK, "TICKER"):
        _point(cleanup_store, sid, security_id=sid)
    cleanup_store.con.execute("INSERT INTO sec_company_tickers VALUES ('1', 'TICKER')")
    assert _empty_replace(cleanup_store) == 0
    assert cleanup_store.con.execute("SELECT count(*) FROM fundamental_points").fetchone()[0] == 3
    _assert_no_cleanup_tables(cleanup_store)


def test_small_issuer_replacement_with_large_unrelated_history_under_64mb(cleanup_store):
    store = cleanup_store
    # Distinct, wide filing keys exercise the old correlation's full-history
    # hash state; the replacement needs only one issuer key on the build side.
    history_rows = 250_000
    store.con.execute(f"""INSERT INTO sec_company_facts
        SELECT ?, 'HISTORY-' || i, '2', 'history-' || i,
            repeat('x', 96) || i, 'us-gaap', 'Assets', 'USD', DATE '2023-12-31', NULL
        FROM range({history_rows}) r(i)""", [SOURCE_NAME])
    store.con.execute("""INSERT INTO fundamental_points
        SELECT source, security_id, run_id, accession_number, taxonomy, concept, unit, period_end, period_start
        FROM sec_company_facts""")
    _fact(store, "old")
    _point(store, "old")
    store.con.execute("CHECKPOINT")
    store.con.execute("SET memory_limit = '64MB'")
    assert _empty_replace(store) == 0
    for table in ("sec_company_facts", "fundamental_points"):
        assert store.con.execute(
            f"SELECT count(*), count(*) FILTER (WHERE run_id LIKE 'history-%') FROM {table}"
        ).fetchone() == (history_rows, history_rows)
    _assert_no_cleanup_tables(store)


@pytest.mark.parametrize("failed_table", ["sec_company_facts", "fundamental_points"])
def test_insert_failure_rolls_back_deletes_inserts_candidates_and_temp_tables(cleanup_store, monkeypatch, failed_table):
    store = cleanup_store
    _fact(store, "old")
    _point(store, "old")
    _fact(store, "other", security_id="OTHER", cik="2")
    _point(store, "other", security_id="OTHER")
    store.con.execute("INSERT INTO identifier_resolution_candidates VALUES ('old', 'sec_company_facts', ?, 'CIK', '1')",
                      [FACT_IDENTIFIER_MATCH_METHOD])
    tables = ("sec_company_facts", "fundamental_points", "identifier_resolution_candidates")
    before = {table: store.con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall() for table in tables}
    facts = store.con.execute("SELECT * FROM sec_company_facts WHERE run_id='old'").df()
    points = store.con.execute("SELECT * FROM fundamental_points WHERE run_id='old'").df()
    facts["run_id"] = points["run_id"] = "new"
    facts["security_id"] = points["security_id"] = "NEW-ID"
    facts["cik"] = CIK

    def insert_then_fail(store, frame, table, relation_name):
        result = insert_frame(store, frame, table, relation_name)
        if table == failed_table:
            raise RuntimeError("injected failure after insert")
        return result

    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_then_fail)
    dataset = SecCompanyFactsDataset()
    with pytest.raises(RuntimeError, match="injected failure after insert"):
        dataset._replace_facts(store, facts, points, "PASSED", cik=CIK)
    for table in tables:
        assert store.con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall() == before[table]
    _assert_no_cleanup_tables(store)

    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_frame)
    for _ in range(2):
        assert dataset._replace_facts(store, facts, points, "PASSED", cik=CIK) == 1
        for table in ("sec_company_facts", "fundamental_points"):
            assert store.con.execute(f"SELECT security_id, run_id FROM {table} ORDER BY run_id").fetchall() == [
                ("NEW-ID", "new"), ("OTHER", "other"),
            ]
        assert store.con.execute("SELECT count(*) FROM identifier_resolution_candidates").fetchone()[0] == 0
        _assert_no_cleanup_tables(store)
