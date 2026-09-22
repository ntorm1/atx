from __future__ import annotations

import datetime as dt
import json
from collections.abc import Iterator

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.xbrl_catalog import refresh_concept_catalog


@pytest.fixture
def catalog_store() -> Iterator[DuckDBStore]:
    """Only the two catalog tables; no warehouse bootstrap or template copy."""
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(config={"threads": 1, "memory_limit": "128MB"})
    store.con.execute("SET preserve_insertion_order = false")
    store.con.execute(
        """
        CREATE TABLE sec_company_facts (
            source VARCHAR NOT NULL, taxonomy VARCHAR, concept VARCHAR,
            label VARCHAR, description VARCHAR, unit VARCHAR, form VARCHAR,
            fiscal_period VARCHAR, period_end DATE, filed_date DATE,
            available_at TIMESTAMP, security_id VARCHAR, accession_number VARCHAR,
            source_loaded_at TIMESTAMP
        );
        CREATE TABLE xbrl_concept_catalog (
            source VARCHAR NOT NULL, taxonomy VARCHAR NOT NULL, concept VARCHAR NOT NULL,
            label VARCHAR, description VARCHAR, statement_category VARCHAR,
            units_json VARCHAR NOT NULL, forms_json VARCHAR NOT NULL,
            fiscal_periods_json VARCHAR NOT NULL,
            first_period_end DATE, last_period_end DATE,
            first_filed_date DATE, last_filed_date DATE,
            first_available_at TIMESTAMP, last_available_at TIMESTAMP,
            fact_count BIGINT NOT NULL, security_count BIGINT NOT NULL,
            accession_count BIGINT NOT NULL, latest_source_loaded_at TIMESTAMP,
            updated_at TIMESTAMP DEFAULT now(),
            PRIMARY KEY (source, taxonomy, concept),
            CHECK (label IS NULL OR label <> '__reject_publication__')
        )
        """
    )
    try:
        yield store
    finally:
        store.connection.close()


def test_catalog_aggregates_metadata_counts_dates_and_deterministic_json(catalog_store):
    catalog_store.con.executemany(
        "INSERT INTO sec_company_facts VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        [
            ("SEC", "us-gaap", "Assets", None, "first description", "USD", "10-Q", "Q1",
             "2020-03-31", "2020-05-01", "2020-05-01 22:00:00.123456", "a", "acc1", "2025-01-01"),
            ("SEC", "us-gaap", "Assets", "", None, "EUR", "10-K", "FY",
             "2019-12-31", "2020-02-01", "2020-02-01 22:00:00", "b", "acc1", "2025-02-01"),
            ("SEC", "us-gaap", "Assets", "later label", "later description", "USD", "10-Q", "Q1",
             None, "2020-06-01", None, "b", "", "2025-01-01"),
            ("SEC", "us-gaap", "Assets", "later label", "", "", None, "",
             None, None, None, None, None, None),
            ("SEC", "us-gaap", "Assets", "later label", "", None, "", None,
             None, None, None, "", "", None),
            ("SEC", "us-gaap", "Unusual", None, None, 'é/"unit', None, None,
             "1500-01-01", "2500-01-01", None, "a", None, None),
            ("SEC", "ifrs-full", "Assets", "IFRS", None, "USD", "20-F", "FY",
             None, None, None, "a", None, None),
            ("Other", "us-gaap", "Assets", "other source", None, "USD", None, None,
             None, None, None, "a", None, None),
            ("SEC", "", "Assets", None, None, None, None, None, None, None, None, None, None, None),
            ("SEC", None, "Assets", None, None, None, None, None, None, None, None, None, None, None),
            ("SEC", "us-gaap", "", None, None, None, None, None, None, None, None, None, None, None),
            ("SEC", "us-gaap", None, None, None, None, None, None, None, None, None, None, None, None),
        ],
    )

    assert refresh_concept_catalog(catalog_store) == 4
    row = catalog_store.con.execute(
        "SELECT * EXCLUDE (updated_at) FROM xbrl_concept_catalog "
        "WHERE source = 'SEC' AND taxonomy = 'us-gaap' AND concept = 'Assets'"
    ).fetchone()
    assert row == (
        "SEC", "us-gaap", "Assets", "", "first description", "balance_sheet",
        '["EUR", "USD"]', '["10-K", "10-Q"]', '["FY", "Q1"]',
        dt.date(2019, 12, 31), dt.date(2020, 3, 31),
        dt.date(2020, 2, 1), dt.date(2020, 6, 1),
        dt.datetime(2020, 2, 1, 22), dt.datetime(2020, 5, 1, 22, 0, 0, 123456),
        5, 3, 2, dt.datetime(2025, 2, 1),
    )
    unusual = catalog_store.con.execute(
        "SELECT label, description, units_json, forms_json, fiscal_periods_json, "
        "first_period_end, last_filed_date, first_available_at, latest_source_loaded_at "
        "FROM xbrl_concept_catalog WHERE concept = 'Unusual'"
    ).fetchone()
    assert unusual == (
        None, None, json.dumps(['é/"unit']), "[]", "[]",
        dt.date(1500, 1, 1), dt.date(2500, 1, 1), None, None,
    )
    before = catalog_store.con.execute(
        "SELECT * EXCLUDE (updated_at) FROM xbrl_concept_catalog ORDER BY source, taxonomy, concept"
    ).fetchall()
    assert refresh_concept_catalog(catalog_store) == 4
    assert catalog_store.con.execute(
        "SELECT * EXCLUDE (updated_at) FROM xbrl_concept_catalog ORDER BY source, taxonomy, concept"
    ).fetchall() == before


@pytest.mark.parametrize(
    ("taxonomy", "concept", "category"),
    [
        ("DEI", "Revenue", "share_count"),
        ("us-gaap", "CommonStockSharesOutstanding", "share_count"),
        ("us-gaap", "EarningsPerShareDiluted", "per_share"),
        ("us-gaap", "NetCashProvidedByUsedInOperatingActivities", "cash_flow"),
        ("us-gaap", "PaymentsToAcquirePropertyPlantAndEquipment", "cash_flow"),
        ("us-gaap", "PaymentsForRepurchaseOfCommonStock", "cash_flow"),
        ("us-gaap", "PaymentsOfDividends", "cash_flow"),
        ("us-gaap", "LiabilitiesAndStockholdersEquity", "balance_sheet"),
        ("us-gaap", "CommonStocksIncludingAdditionalPaidInCapital", "balance_sheet"),
        ("us-gaap", "RevenueFromContractWithCustomer", "income_statement"),
        ("us-gaap", "NetIncomeLoss", "income_statement"),
        ("custom", "Inventory", "other"),
    ],
)
def test_statement_category_preserves_taxonomy_and_precedence(catalog_store, taxonomy, concept, category):
    catalog_store.con.execute(
        "INSERT INTO sec_company_facts (source, taxonomy, concept) VALUES ('SEC', ?, ?)",
        [taxonomy, concept],
    )
    assert refresh_concept_catalog(catalog_store) == 1
    assert catalog_store.con.execute(
        "SELECT statement_category FROM xbrl_concept_catalog"
    ).fetchone() == (category,)


def test_refresh_replaces_present_keys_and_preserves_absent_keys_and_empty_input(catalog_store):
    catalog_store.con.execute(
        "INSERT INTO sec_company_facts (source, taxonomy, concept) VALUES "
        "('SEC', 'us-gaap', 'Assets'), ('SEC', 'us-gaap', 'Revenue')"
    )
    assert refresh_concept_catalog(catalog_store) == 2
    catalog_store.con.execute("DELETE FROM sec_company_facts WHERE concept = 'Revenue'")
    catalog_store.con.execute("UPDATE sec_company_facts SET label = 'new label'")
    assert refresh_concept_catalog(catalog_store) == 1
    assert catalog_store.con.execute(
        "SELECT concept, label FROM xbrl_concept_catalog ORDER BY concept"
    ).fetchall() == [("Assets", "new label"), ("Revenue", None)]
    catalog_store.con.execute("DELETE FROM sec_company_facts")
    assert refresh_concept_catalog(catalog_store) == 0
    assert catalog_store.con.execute("SELECT count(*) FROM xbrl_concept_catalog").fetchone() == (2,)
    assert catalog_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'xbrl_concept_catalog_load'"
    ).fetchone() == (0,)


def test_catalog_scan_never_materializes_fact_dataframe(catalog_store, monkeypatch):
    def reject_dataframe(*args, **kwargs):
        raise AssertionError("catalog refresh must not materialize a fact DataFrame")

    monkeypatch.setattr(duckdb.DuckDBPyConnection, "df", reject_dataframe)
    monkeypatch.setattr(duckdb.DuckDBPyConnection, "fetchdf", reject_dataframe)
    catalog_store.con.execute(
        """
        INSERT INTO sec_company_facts (source, taxonomy, concept, label, security_id, accession_number)
        SELECT 'SEC', 'us-gaap', 'Assets', repeat('metadata', 32),
            CAST(i % 100 AS VARCHAR), CAST(i % 1000 AS VARCHAR)
        FROM range(100000) AS facts(i)
        """
    )
    assert refresh_concept_catalog(catalog_store) == 1
    assert catalog_store.con.execute(
        "SELECT fact_count, security_count, accession_count FROM xbrl_concept_catalog"
    ).fetchone() == (100000, 100, 1000)


def test_publish_failure_rolls_back_existing_catalog_and_removes_temporary_table(catalog_store):
    catalog_store.con.execute(
        "INSERT INTO sec_company_facts (source, taxonomy, concept) VALUES ('SEC', 'us-gaap', 'Assets')"
    )
    assert refresh_concept_catalog(catalog_store) == 1
    # A destination constraint makes publication fail after its DELETE. The
    # staged table is deliberately unconstrained, matching the production path.
    catalog_store.con.execute("UPDATE sec_company_facts SET label = '__reject_publication__'")
    with pytest.raises(duckdb.ConstraintException):
        refresh_concept_catalog(catalog_store)
    assert catalog_store.con.execute("SELECT fact_count FROM xbrl_concept_catalog").fetchone() == (1,)
    assert catalog_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'xbrl_concept_catalog_load'"
    ).fetchone() == (0,)
