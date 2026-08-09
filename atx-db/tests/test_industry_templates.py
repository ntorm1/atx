from __future__ import annotations

import datetime as dt

import pytest


def _seed_security(store, security_id: str, symbol: str | None = None) -> None:
    store.con.execute(
        """
        INSERT OR IGNORE INTO securities (security_id, primary_symbol, name, source)
        VALUES (?, ?, ?, 'test')
        """,
        [security_id, symbol or security_id.upper(), f"{security_id} Test Co"],
    )


def _seed_sic_classification(store, security_id: str, sic_code: str) -> None:
    taxonomy_row = store.con.execute(
        "SELECT taxonomy_id FROM taxonomy WHERE code = 'SIC'"
    ).fetchone()
    if taxonomy_row is None:
        taxonomy_id = "taxonomy_sic_test"
        store.con.execute(
            """
            INSERT INTO taxonomy (
                taxonomy_id, code, name, provider, version, is_hierarchical, description, source
            )
            VALUES (?, 'SIC', 'Standard Industrial Classification', 'SEC', 'test', true, 'Test SIC taxonomy', 'test')
            """,
            [taxonomy_id],
        )
    else:
        taxonomy_id = str(taxonomy_row[0])

    node_id = f"node_sic_{sic_code}"
    store.con.execute(
        """
        INSERT OR REPLACE INTO taxonomy_node (
            node_id, taxonomy_id, node_code, node_label, parent_node_id, level, sort_order
        )
        VALUES (?, ?, ?, ?, NULL, 3, 0)
        """,
        [node_id, taxonomy_id, sic_code, f"SIC {sic_code}"],
    )
    store.con.execute(
        """
        INSERT OR REPLACE INTO entity_classification (
            classification_id, security_id, taxonomy_id, node_id, node_code,
            is_primary, valid_from, valid_to, as_of_date, available_at, source_loaded_at, run_id, source
        )
        VALUES (?, ?, ?, ?, ?, true, DATE '2020-01-01', NULL, DATE '2020-01-01', now(), now(), 'test', 'test')
        """,
        [f"classification_{security_id}_{sic_code}", security_id, taxonomy_id, node_id, sic_code],
    )


def _insert_company_fact(
    store,
    *,
    security_id: str,
    cik: str,
    taxonomy: str = "us-gaap",
    concept: str,
    unit: str = "USD",
    period_start: dt.date | None = dt.date(2023, 1, 1),
    period_end: dt.date = dt.date(2023, 12, 31),
    value: float,
    filed_date: dt.date = dt.date(2024, 2, 15),
    accession_number: str,
    form: str = "10-K",
) -> None:
    store.con.execute(
        """
        INSERT INTO sec_company_facts (
            source, security_id, cik, taxonomy, concept, unit,
            period_start, period_end, value, filed_date, accession_number,
            form, frame, source_url, source_loaded_at, available_at
        ) VALUES (
            'SEC companyfacts', ?, ?, ?, ?, ?,
            ?, ?, ?, ?, ?,
            ?, NULL, 'https://data.sec.gov/', now(), now()
        )
        """,
        [
            security_id,
            cik,
            taxonomy,
            concept,
            unit,
            period_start,
            period_end,
            value,
            filed_date,
            accession_number,
            form,
        ],
    )


def _refresh_statement_points(store) -> None:
    from atx_db.fundamental_statements import refresh_fundamental_statement_points
    from atx_db.fundamentals import refresh_fundamental_fact_revisions

    refresh_fundamental_fact_revisions(store)
    refresh_fundamental_statement_points(store)


def test_template_for_sic_codes():
    from atx_db.industry_templates import template_for_sic

    assert template_for_sic("6022")[0] == "BK"
    assert template_for_sic("6211")[0] == "BD"
    assert template_for_sic("6311")[0] == "IS"
    assert template_for_sic("4911")[0] == "UT"
    assert template_for_sic("6798")[0] == "RT"
    assert template_for_sic("7372")[0] == "ALL"
    assert template_for_sic(None)[0] == "ALL"


def test_refresh_entity_industry_templates_routes_six_profiles(tmp_store):
    from atx_db.industry_templates import refresh_entity_industry_templates

    fixtures = {
        "sec_ind": None,
        "sec_bank": "6022",
        "sec_bd": "6211",
        "sec_ins": "6311",
        "sec_utility": "4911",
        "sec_reit": "6798",
    }
    for security_id, sic_code in fixtures.items():
        _seed_security(tmp_store, security_id)
        if sic_code is not None:
            _seed_sic_classification(tmp_store, security_id, sic_code)

    assert refresh_entity_industry_templates(tmp_store) == 6
    routed = dict(
        tmp_store.con.execute(
            """
            SELECT security_id, industry_template
            FROM entity_industry_template
            ORDER BY security_id
            """
        ).fetchall()
    )
    assert routed == {
        "sec_ind": "ALL",
        "sec_bank": "BK",
        "sec_bd": "BD",
        "sec_ins": "IS",
        "sec_utility": "UT",
        "sec_reit": "RT",
    }


def test_statement_refresh_uses_materialized_routes_for_utility_and_broker(tmp_store):
    _seed_security(tmp_store, "sec_utility", "UTIL")
    _seed_sic_classification(tmp_store, "sec_utility", "4911")
    _insert_company_fact(
        tmp_store,
        security_id="sec_utility",
        cik="0000004901",
        concept="RegulatedAndUnregulatedOperatingRevenue",
        value=1_000.0,
        accession_number="0000004901-24-000001",
    )

    _seed_security(tmp_store, "sec_bd", "BDLR")
    _seed_sic_classification(tmp_store, "sec_bd", "6211")
    _insert_company_fact(
        tmp_store,
        security_id="sec_bd",
        cik="0000006211",
        concept="PayablesToBrokerDealersAndClearingOrganizations",
        period_start=None,
        value=250.0,
        accession_number="0000006211-24-000001",
    )

    _refresh_statement_points(tmp_store)
    metrics = {
        tuple(row)
        for row in tmp_store.con.execute(
            """
            SELECT security_id, canonical_metric, statement_type
            FROM fundamental_statement_points
            WHERE security_id IN ('sec_utility', 'sec_bd')
            """
        ).fetchall()
    }
    assert ("sec_utility", "utility_operating_revenue", "utility_statement") in metrics
    assert ("sec_bd", "payables_broker_dealers", "broker_dealer_statement") in metrics


def test_reit_ffo_affo_derives_from_us_gaap_inputs(tmp_store):
    _seed_security(tmp_store, "sec_reit", "REIT")
    _seed_sic_classification(tmp_store, "sec_reit", "6798")
    accession = "0000006798-24-000001"
    for concept, unit, value in (
        ("NetIncomeLoss", "USD", 100.0),
        ("DepreciationDepletionAndAmortization", "USD", 25.0),
        ("WeightedAverageNumberOfDilutedSharesOutstanding", "shares", 10.0),
    ):
        _insert_company_fact(
            tmp_store,
            security_id="sec_reit",
            cik="0000006798",
            concept=concept,
            unit=unit,
            value=value,
            accession_number=accession,
        )

    _refresh_statement_points(tmp_store)
    values = dict(
        tmp_store.con.execute(
            """
            SELECT canonical_metric, value
            FROM fundamental_statement_points
            WHERE security_id = 'sec_reit'
              AND canonical_metric IN ('ffo', 'affo', 'ffo_per_share', 'affo_per_share')
            ORDER BY canonical_metric
            """
        ).fetchall()
    )
    assert values["ffo"] == pytest.approx(125.0)
    assert values["affo"] == pytest.approx(125.0)
    assert values["ffo_per_share"] == pytest.approx(12.5)
    assert values["affo_per_share"] == pytest.approx(12.5)


def test_reported_reit_ffo_wins_over_derivation(tmp_store):
    _seed_security(tmp_store, "sec_reit_reported", "RFFO")
    _seed_sic_classification(tmp_store, "sec_reit_reported", "6798")
    accession = "0000006798-24-000002"
    for taxonomy, concept, unit, value in (
        ("us-gaap", "NetIncomeLoss", "USD", 100.0),
        ("us-gaap", "DepreciationDepletionAndAmortization", "USD", 25.0),
        ("nareit", "FundsFromOperations", "USD", 200.0),
        ("us-gaap", "WeightedAverageNumberOfDilutedSharesOutstanding", "shares", 10.0),
    ):
        _insert_company_fact(
            tmp_store,
            security_id="sec_reit_reported",
            cik="0000006798",
            taxonomy=taxonomy,
            concept=concept,
            unit=unit,
            value=value,
            accession_number=accession,
        )

    _refresh_statement_points(tmp_store)
    rows = tmp_store.con.execute(
        """
        SELECT canonical_metric, taxonomy, concept, value
        FROM fundamental_statement_points
        WHERE security_id = 'sec_reit_reported'
          AND canonical_metric IN ('ffo', 'affo', 'ffo_per_share')
        ORDER BY canonical_metric, taxonomy
        """
    ).fetchall()
    by_metric = {row[0]: row for row in rows}
    assert by_metric["ffo"][1:4] == ("nareit", "FundsFromOperations", 200.0)
    assert by_metric["affo"][3] == pytest.approx(200.0)
    assert by_metric["ffo_per_share"][3] == pytest.approx(20.0)
    assert sum(1 for row in rows if row[0] == "ffo") == 1


def test_template_coverage_and_quality_gates(tmp_store):
    from atx_db.fundamental_statements import seed_fundamental_statement_map
    from atx_db.industry_templates import (
        refresh_entity_industry_templates,
        refresh_industry_template_coverage,
    )
    from atx_db.quality import run_warehouse_quality_checks

    for security_id, sic_code in (
        ("sec_ind", None),
        ("sec_bank", "6022"),
        ("sec_bd", "6211"),
        ("sec_ins", "6311"),
        ("sec_utility", "4911"),
        ("sec_reit", "6798"),
    ):
        _seed_security(tmp_store, security_id)
        if sic_code is not None:
            _seed_sic_classification(tmp_store, security_id, sic_code)

    seed_fundamental_statement_map(tmp_store)
    refresh_entity_industry_templates(tmp_store)
    assert refresh_industry_template_coverage(tmp_store) == 6

    clean = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=(
                "industry_template_exactly_one_route",
                "industry_template_required_item_coverage",
            ),
        )
    }
    assert clean["industry_template_exactly_one_route"].status == "passed"
    assert clean["industry_template_exactly_one_route"].severity == "critical"
    assert clean["industry_template_required_item_coverage"].status == "passed"
    assert clean["industry_template_required_item_coverage"].severity == "critical"

    tmp_store.con.execute(
        """
        INSERT INTO entity_industry_template (
            route_id, source, security_id, symbol, industry_template,
            matched_taxonomy, matched_node_code, match_reason,
            valid_from, valid_to, as_of_date, available_at,
            is_latest_revision, run_id, source_loaded_at
        )
        VALUES (
            'duplicate_route_sec_bank',
            'derived_industry_templates_v1',
            'sec_bank',
            'SEC_BANK',
            'ALL',
            'SIC',
            '6022',
            'test_duplicate',
            DATE '2020-01-01',
            NULL,
            current_date,
            now(),
            true,
            'test',
            now()
        )
        """
    )
    duplicate = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("industry_template_exactly_one_route",),
        )
    }
    assert duplicate["industry_template_exactly_one_route"].status == "failed"

    tmp_store.con.execute("DELETE FROM entity_industry_template WHERE route_id = 'duplicate_route_sec_bank'")
    tmp_store.con.execute(
        """
        DELETE FROM fundamental_statement_map
        WHERE industry_template = 'UT'
          AND item_id = 1803
        """
    )
    refresh_industry_template_coverage(tmp_store)
    missing = {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            check_names=("industry_template_required_item_coverage",),
        )
    }
    assert missing["industry_template_required_item_coverage"].status == "failed"
    assert missing["industry_template_required_item_coverage"].observed_value == 1.0


def test_migrations_0110_0113_catalog_industry_template_tables(tmp_store):
    versions = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert {110, 111, 112, 113} <= versions

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE table_name IN (
                'industry_template',
                'industry_template_item',
                'entity_industry_template',
                'industry_template_coverage'
            )
            """
        ).fetchall()
    }
    assert tables == {
        "industry_template",
        "industry_template_item",
        "entity_industry_template",
        "industry_template_coverage",
    }

    cataloged = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN (
                'industry_template',
                'industry_template_item',
                'entity_industry_template',
                'industry_template_coverage'
            )
            """
        ).fetchall()
    }
    assert cataloged == tables


def test_formula_registry_loads_industry_family_rows(tmp_store):
    from atx_db.formula_library import seed_formula_registry

    seed_formula_registry(tmp_store)
    codes = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT formula_code
            FROM formula_registry
            WHERE family IN ('bank', 'broker_dealer', 'insurance', 'utility', 'reit')
            """
        ).fetchall()
    }
    assert {
        "bank_net_interest_margin",
        "broker_dealer_net_capital_to_receivables",
        "insurance_combined_ratio",
        "utility_rate_base_return",
        "reit_ffo_per_share",
        "reit_affo_per_share",
    } <= codes
