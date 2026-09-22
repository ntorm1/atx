"""Exact CIK inventory selection and load-local cache lifecycle."""
from __future__ import annotations

import json
import zipfile

import duckdb
import pandas as pd
import pytest

from atx_db.connection import DuckDBStore
from atx_db.fundamentals import (
    FACT_IDENTIFIER_MATCH_METHOD,
    SOURCE_NAME,
    SecCompanyFactsDataset,
    SecCompanyFactsOptions,
    _companyfacts_cik_spellings,
)
from atx_db.warehouse import insert_frame

CIK = "0000000001"


@pytest.fixture
def spelling_store(tmp_path):
    store = DuckDBStore(tmp_path / "spellings.duckdb")
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


def _seed(store, marker, cik):
    store.con.execute("""INSERT INTO sec_company_facts VALUES
        (?, ?, ?, ?, NULL, 'us-gaap', 'Assets', 'USD', DATE '2023-12-31', NULL)""",
        [SOURCE_NAME, marker, cik, marker])
    store.con.execute("""INSERT INTO fundamental_points VALUES
        (?, ?, ?, NULL, 'us-gaap', 'Assets', 'USD', DATE '2023-12-31', NULL)""",
        [SOURCE_NAME, marker, marker])


@pytest.mark.parametrize("cached", [False, True])
def test_inventory_replacement_preserves_exact_numeric_spelling_scope(spelling_store, cached):
    store = spelling_store
    valid = ("1", CIK, " 1 ", "00000000000001")
    invalid = (None, "", " ", "1.0", "+1", "-1", "1e0", "1x", "\u0661", "9223372036854775808", "2")
    for index, spelling in enumerate(valid):
        _seed(store, f"remove-{index}", spelling)
    for index, spelling in enumerate(invalid):
        _seed(store, f"keep-{index}", spelling)
    # Repeated facts must not duplicate inventory entries.
    _seed(store, "remove-duplicate", valid[0])
    # Ticker/candidate spellings need not appear anywhere in raw facts.
    store.con.execute("INSERT INTO sec_company_tickers VALUES ('0000000000000001', 'TICKER')")
    store.con.execute("""INSERT INTO fundamental_points VALUES
        (?, 'TICKER', 'remove-ticker', NULL, 'us-gaap', 'Assets', 'USD', DATE '2023-12-31', NULL)""",
        [SOURCE_NAME])
    store.con.execute("""INSERT INTO identifier_resolution_candidates VALUES
        ('remove', 'sec_company_facts', ?, 'CIK', ' 0000000001 ')""", [FACT_IDENTIFIER_MATCH_METHOD])
    spellings = _companyfacts_cik_spellings(store)
    assert set(spellings[1]) == set(valid)
    assert spellings[2] == ("2",)
    assert set(spellings) == {1, 2}
    dataset = SecCompanyFactsDataset()
    for _ in range(2):
        assert dataset._replace_facts(
            store, pd.DataFrame(), pd.DataFrame(), "PASSED", cik=CIK,
            stored_cik_spellings=spellings[1] if cached else None,
        ) == 0
        for table in ("sec_company_facts", "fundamental_points"):
            assert {row[0] for row in store.con.execute(f"SELECT run_id FROM {table}").fetchall()} == {
                f"keep-{index}" for index in range(len(invalid))
            }
        assert store.con.execute("SELECT count(*) FROM identifier_resolution_candidates").fetchone()[0] == 0


def test_empty_inventory_tuple_does_not_fall_back_to_numeric_lookup(spelling_store):
    store = spelling_store
    spellings = _companyfacts_cik_spellings(store)
    assert spellings == {}
    _seed(store, "new-between-independent-calls", "1")
    dataset = SecCompanyFactsDataset()
    # An explicitly supplied snapshot is distinct from requesting a fresh lookup.
    dataset._replace_facts(store, pd.DataFrame(), pd.DataFrame(), "PASSED", cik=CIK,
                           stored_cik_spellings=spellings.get(1, ()))
    for table in ("sec_company_facts", "fundamental_points"):
        assert store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0] == 1
    dataset._replace_facts(store, pd.DataFrame(), pd.DataFrame(), "PASSED", cik=CIK)
    for table in ("sec_company_facts", "fundamental_points"):
        assert store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0] == 0


@pytest.mark.parametrize("failed_table", ["sec_company_facts", "fundamental_points"])
def test_cached_replacement_rolls_back_and_retries(spelling_store, monkeypatch, failed_table):
    store = spelling_store
    _seed(store, "old", " 1 ")
    _seed(store, "other", "2")
    store.con.execute("""INSERT INTO identifier_resolution_candidates VALUES
        ('old', 'sec_company_facts', ?, 'CIK', '1')""", [FACT_IDENTIFIER_MATCH_METHOD])
    tables = ("sec_company_facts", "fundamental_points", "identifier_resolution_candidates")
    before = {table: store.con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall() for table in tables}
    spellings = _companyfacts_cik_spellings(store)
    facts = store.con.execute("SELECT * FROM sec_company_facts WHERE run_id='old'").df()
    points = store.con.execute("SELECT * FROM fundamental_points WHERE run_id='old'").df()
    facts["cik"] = CIK
    facts["run_id"] = points["run_id"] = "new"

    def insert_then_fail(store, frame, table, relation_name):
        result = insert_frame(store, frame, table, relation_name)
        if table == failed_table:
            raise RuntimeError("injected after insert")
        return result

    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_then_fail)
    dataset = SecCompanyFactsDataset()
    with pytest.raises(RuntimeError, match="injected after insert"):
        dataset._replace_facts(store, facts, points, "PASSED", cik=CIK, stored_cik_spellings=spellings[1])
    for table in tables:
        assert store.con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall() == before[table]
    assert store.con.execute("""SELECT table_name FROM duckdb_tables() WHERE temporary
        AND table_name LIKE 'companyfacts_%'""").fetchall() == []
    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_frame)
    assert dataset._replace_facts(store, facts, points, "PASSED", cik=CIK,
                                  stored_cik_spellings=spellings[1]) == 1
    assert store.con.execute("SELECT cik, run_id FROM sec_company_facts ORDER BY run_id").fetchall() == [
        (CIK, "new"), ("2", "other"),
    ]


def _archive(path, ciks=(1, 2)):
    with zipfile.ZipFile(path, "w") as archive:
        for cik in ciks:
            payload = {"cik": cik, "facts": {"us-gaap": {"Assets": {"units": {"USD": [{
                "end": "2023-12-31", "val": cik, "accn": f"issuer-{cik}",
                "form": "10-K", "filed": "2024-02-15",
            }]}}}}}
            archive.writestr(f"CIK{cik:010d}.json", json.dumps(payload))
    return path


def _options(path):
    return SecCompanyFactsOptions(symbol_source="archive_members", companyfacts_zip=path,
                                  concepts=("Assets",), refresh_derived_surfaces=False)


def test_load_inventory_is_once_per_load_and_observes_later_mutation(tmp_store, tmp_path, monkeypatch):
    dataset = SecCompanyFactsDataset()
    options = _options(_archive(tmp_path / "companyfacts.zip"))
    inventories = []

    def inventory(store):
        result = _companyfacts_cik_spellings(store)
        inventories.append(result)
        return result

    monkeypatch.setattr("atx_db.fundamentals._companyfacts_cik_spellings", inventory)
    assert dataset.load(tmp_store, options).rows_loaded == 2
    assert inventories == [{1: (CIK,), 2: ("0000000002",)}]
    # Reusing the dataset must see a spelling introduced between loads.
    tmp_store.con.execute("UPDATE sec_company_facts SET cik=' 000000000001 ' WHERE cik=?", [CIK])
    assert dataset.load(tmp_store, options).rows_loaded == 2
    assert len(inventories) == 2
    assert set(inventories[1][1]) == {" 000000000001 ", CIK}
    assert tmp_store.con.execute("SELECT cik FROM sec_company_facts ORDER BY cik").fetchall() == [
        (CIK,), ("0000000002",),
    ]
    assert tmp_store.con.execute("SELECT count(*) FROM fundamental_points").fetchone()[0] == 2


def test_load_cache_does_not_advance_after_rollback(tmp_store, tmp_path, monkeypatch):
    options = _options(_archive(tmp_path / "companyfacts.zip", (1,)))
    dataset = SecCompanyFactsDataset()
    dataset.load(tmp_store, options)
    tmp_store.con.execute("UPDATE sec_company_facts SET cik='1'")
    inventories = []

    def inventory(store):
        result = _companyfacts_cik_spellings(store)
        inventories.append(result)
        return result

    def insert_then_fail(store, frame, table, relation_name):
        result = insert_frame(store, frame, table, relation_name)
        if table == "fundamental_points":
            raise RuntimeError("injected after points")
        return result

    monkeypatch.setattr("atx_db.fundamentals._companyfacts_cik_spellings", inventory)
    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_then_fail)
    with pytest.raises(RuntimeError, match="injected after points"):
        dataset.load(tmp_store, options)
    assert inventories == [{1: ("1",)}]
    assert tmp_store.con.execute("SELECT cik FROM sec_company_facts").fetchall() == [("1",)]
    monkeypatch.setattr("atx_db.fundamentals.insert_frame", insert_frame)
    assert dataset.load(tmp_store, options).rows_loaded == 1
    assert inventories == [{1: ("1",)}, {1: ("1", CIK)}]


def test_failed_source_skips_inventory_and_preserves_existing_facts(tmp_store, tmp_path, monkeypatch):
    path = _archive(tmp_path / "companyfacts.zip", (1,))
    dataset = SecCompanyFactsDataset()
    options = _options(path)
    dataset.load(tmp_store, options)
    tmp_store.con.execute("UPDATE sec_company_facts SET cik=' 1 '")
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr(f"CIK{CIK}.json", "{invalid")

    def forbidden_inventory(store):
        pytest.fail("a failed source member should not start replacement inventory")

    monkeypatch.setattr("atx_db.fundamentals._companyfacts_cik_spellings", forbidden_inventory)
    result = dataset.load(tmp_store, options)
    assert result.details["failed_target_count"] == 1
    assert tmp_store.con.execute("SELECT cik FROM sec_company_facts").fetchall() == [(" 1 ",)]
    assert tmp_store.con.execute("SELECT count(*) FROM fundamental_points").fetchone()[0] == 1
