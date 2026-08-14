from __future__ import annotations

import datetime as dt
import json

from atx_db.connection import DuckDBStore
from atx_db.fundamental_reconciliation import (
    RECONCILIATION_RULES,
    RECONCILIATION_TERMS,
    FundamentalReconciliationRefreshOptions,
    refresh_fundamental_reconciliation_serving,
    seed_fundamental_reconciliation_rules,
)
from atx_db.quality._runner import run_warehouse_quality_checks

SECURITY_ID = "SEC-RECON-1"
PERIOD_END = dt.date(2023, 12, 31)
FIRST_AVAILABLE = dt.datetime(2024, 2, 1, 12)
RESTATED_AVAILABLE = dt.datetime(2024, 3, 1, 12)


def _insert_standardized(
    store: DuckDBStore,
    *,
    standardized_id: str,
    item_id: int,
    value: float,
    available_at: dt.datetime,
    basis: str = "instant",
    is_latest_revision: bool = True,
    source_accession: str | None = None,
    input_code: str | None = None,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id,source,upstream_source,security_id,symbol,cik,item_id,
            canonical_code,basis,period_start,period_end,fiscal_year,fiscal_period,
            value,unit_type,source_accession,filed_date,as_of_date,available_at,
            input_codes_json,input_item_ids_json,rule_id,combination_rule,
            is_latest_revision,run_id,source_loaded_at,updated_at,unit,
            revision_group_id
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            standardized_id,
            "test-reconciliation",
            "test-input",
            SECURITY_ID,
            "RECON",
            "0000000001",
            item_id,
            f"item_{item_id}",
            basis,
            None if basis == "instant" else dt.date(2023, 1, 1),
            PERIOD_END,
            2023,
            "FY",
            value,
            "monetary",
            source_accession or f"accession-{standardized_id}",
            available_at.date(),
            PERIOD_END,
            available_at,
            json.dumps([input_code or f"item_{item_id}"]),
            f"[{item_id}]",
            f"rule-{item_id}",
            "identity",
            is_latest_revision,
            "test-run",
            available_at,
            available_at,
            "USD",
            f"{SECURITY_ID}|{item_id}|{basis}|{PERIOD_END.isoformat()}",
        ],
    )


def _seed_balance_sheet_revisions(store: DuckDBStore) -> None:
    _insert_standardized(
        store,
        standardized_id="assets-original",
        item_id=1101,
        value=100.0,
        available_at=FIRST_AVAILABLE,
    )
    _insert_standardized(
        store,
        standardized_id="total-liabilities-equity",
        item_id=1223,
        value=100.0,
        available_at=FIRST_AVAILABLE,
    )
    _insert_standardized(
        store,
        standardized_id="liabilities-original",
        item_id=1201,
        value=60.0,
        available_at=FIRST_AVAILABLE,
    )
    _insert_standardized(
        store,
        standardized_id="equity-nci-original",
        item_id=1222,
        value=40.0,
        available_at=FIRST_AVAILABLE,
        is_latest_revision=False,
    )
    _insert_standardized(
        store,
        standardized_id="stockholders-equity-original",
        item_id=1221,
        value=35.0,
        available_at=FIRST_AVAILABLE,
    )
    _insert_standardized(
        store,
        standardized_id="equity-nci-restated",
        item_id=1222,
        value=35.0,
        available_at=RESTATED_AVAILABLE,
    )


def test_governed_rule_registry_has_expected_scope_and_signs(tmp_store: DuckDBStore) -> None:
    assert len(RECONCILIATION_RULES) == 19
    assert seed_fundamental_reconciliation_rules(tmp_store) == 19

    diagnostic_rules = {rule.rule_id for rule in RECONCILIATION_RULES if rule.mismatch_severity == "diagnostic"}
    assert diagnostic_rules == {"assets_equal_liabilities_stockholders_equity_instant"}

    gross_profit_terms = {
        (term.term_role, term.item_id): term.weight
        for term in RECONCILIATION_TERMS
        if term.rule_id == "gross_profit_components_annual"
    }
    assert gross_profit_terms == {("lhs", 1004): 1.0, ("rhs", 1001): 1.0, ("rhs", 1003): -1.0}
    component_terms = {
        term.item_id: term.is_required
        for term in RECONCILIATION_TERMS
        if term.rule_id == "total_liabilities_equity_components_instant"
    }
    assert component_terms == {1223: True, 1201: True, 1222: True, 1224: False}
    assert tmp_store.con.execute(
        "SELECT mismatch_severity FROM fundamental_reconciliation_rule WHERE rule_id=?",
        ["assets_equal_liabilities_stockholders_equity_instant"],
    ).fetchone() == ("diagnostic",)


def test_reconciliation_reacts_to_restatement_and_preserves_diagnostic_scope(
    tmp_store: DuckDBStore,
) -> None:
    _seed_balance_sheet_revisions(tmp_store)

    rows = tmp_store.con.execute(
        """
        SELECT status,previous_status,update_type,revision_sequence,revision_count,
               is_latest_revision,available_at,valid_to,input_values_json,
               input_filing_status,context_verification_status,mismatch_reason,
               is_hard_failure
        FROM v_fundamental_reconciliation
        WHERE rule_id='total_liabilities_equity_components_instant'
        ORDER BY available_at
        """
    ).fetchall()
    assert [(row[0], row[1], row[2]) for row in rows] == [
        ("reconciled", None, "original"),
        ("mismatch", "reconciled", "restated"),
    ]
    assert [(row[3], row[4], row[5]) for row in rows] == [(1, 2, False), (2, 2, True)]
    assert rows[0][7] == RESTATED_AVAILABLE
    assert rows[1][7] is None
    assert len(json.loads(rows[1][8])) == 3
    assert [(row[9], row[10], row[11], row[12]) for row in rows] == [
        ("mixed_filing_vintage", "mixed_filing_vintage", "within_tolerance", False),
        ("mixed_filing_vintage", "mixed_filing_vintage", "mixed_filing_vintage", False),
    ]

    diagnostic = tmp_store.con.execute(
        """
        SELECT status,mismatch_severity,residual,is_applicable
        FROM v_fundamental_reconciliation
        WHERE rule_id='assets_equal_liabilities_stockholders_equity_instant'
        """
    ).fetchone()
    assert diagnostic == ("diagnostic_difference", "diagnostic", 5.0, True)


def test_industry_rule_emits_not_applicable_classification_revision(
    tmp_store: DuckDBStore,
) -> None:
    _insert_standardized(
        tmp_store,
        standardized_id="bank-net-interest",
        item_id=1501,
        value=70.0,
        available_at=FIRST_AVAILABLE,
        basis="annual",
    )
    _insert_standardized(
        tmp_store,
        standardized_id="bank-interest-income",
        item_id=1503,
        value=100.0,
        available_at=FIRST_AVAILABLE,
        basis="annual",
    )
    _insert_standardized(
        tmp_store,
        standardized_id="bank-interest-expense",
        item_id=1504,
        value=-30.0,
        available_at=FIRST_AVAILABLE,
        basis="annual",
    )
    tmp_store.con.executemany(
        """
        INSERT INTO entity_industry_template (
            route_id,source,security_id,symbol,industry_template,matched_taxonomy,
            matched_node_code,match_reason,valid_from,valid_to,as_of_date,available_at,
            is_latest_revision,run_id,source_loaded_at,route_revision_group_id,
            revision_sequence,revision_count,previous_industry_template,update_type,
            knowledge_valid_to
        ) VALUES (?,?,?,?,?,?,?,?,?,NULL,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            (
                "route-bank",
                "test-routes",
                SECURITY_ID,
                "RECON",
                "BK",
                "SIC",
                "6022",
                "fixture_bank",
                dt.date(2020, 1, 1),
                dt.date(2020, 1, 1),
                dt.datetime(2024, 1, 1, 12),
                False,
                "route-test",
                dt.datetime(2024, 1, 1, 12),
                "route-group",
                1,
                2,
                None,
                "original",
                dt.datetime(2024, 4, 1, 12),
            ),
            (
                "route-insurance",
                "test-routes",
                SECURITY_ID,
                "RECON",
                "IS",
                "SIC",
                "6311",
                "fixture_insurance_revision",
                dt.date(2020, 1, 1),
                dt.date(2020, 1, 1),
                dt.datetime(2024, 4, 1, 12),
                True,
                "route-test",
                dt.datetime(2024, 4, 1, 12),
                "route-group",
                2,
                2,
                "BK",
                "restated",
                None,
            ),
        ],
    )

    rows = tmp_store.con.execute(
        """
        SELECT status,is_applicable,industry_template,update_type,previous_status,
               revision_sequence,revision_count,is_latest_revision
        FROM v_fundamental_reconciliation
        WHERE rule_id='bank_net_interest_income_components_annual'
        ORDER BY available_at
        """
    ).fetchall()
    assert rows == [
        ("reconciled", True, "BK", "original", None, 1, 2, False),
        ("not_applicable", False, "IS", "classification_update", "reconciled", 2, 2, True),
    ]


def test_reconciliation_quality_checks_are_registered_and_pass_on_empty_view(
    tmp_store: DuckDBStore,
) -> None:
    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        dataset_ids=("fundamental_reconciliation",),
    )
    assert {result.check_name for result in results} == {
        "duplicate_fundamental_reconciliation_events",
        "bad_fundamental_reconciliation_rows",
        "bad_latest_fundamental_reconciliation_chains",
        "latest_hard_fundamental_reconciliation_mismatch_rate",
        "fundamental_reconciliation_context_verification_rate",
    }
    assert {result.status for result in results} == {"passed"}


def test_verified_same_context_mismatch_is_a_hard_failure(tmp_store: DuckDBStore) -> None:
    accession = "0000000001-24-000001"
    _insert_standardized(
        tmp_store,
        standardized_id="verified-assets",
        item_id=1101,
        value=110.0,
        available_at=FIRST_AVAILABLE,
        source_accession=accession,
        input_code="us-gaap:Assets",
    )
    _insert_standardized(
        tmp_store,
        standardized_id="verified-total-liabilities-equity",
        item_id=1223,
        value=100.0,
        available_at=FIRST_AVAILABLE,
        source_accession=accession,
        input_code="us-gaap:LiabilitiesAndStockholdersEquity",
    )
    _insert_standardized(
        tmp_store,
        standardized_id="verified-liabilities",
        item_id=1201,
        value=60.0,
        available_at=FIRST_AVAILABLE,
        source_accession=accession,
        input_code="us-gaap:Liabilities",
    )
    _insert_standardized(
        tmp_store,
        standardized_id="verified-equity-including-nci",
        item_id=1222,
        value=35.0,
        available_at=FIRST_AVAILABLE,
        source_accession=accession,
        input_code="us-gaap:StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",
    )
    tmp_store.con.execute(
        """
        INSERT INTO xbrl_filing_contexts (
            filing_context_id,security_id,cik,accession_number,form,filing_date,
            report_date,acceptance_datetime,primary_document,context_id,
            entity_identifier_scheme,entity_identifier,period_type,period_start,
            period_end,instant_date,has_segment,has_scenario,explicit_member_count,
            typed_member_count,dimension_count,context_hash,source_url,run_id,
            source_loaded_at
        ) VALUES (
            'recon-context',?,'0000000001',?,'10-K',DATE '2024-02-01',?,
            TIMESTAMP '2024-02-01 11:00:00','form10k.htm','balance-sheet',
            'http://www.sec.gov/CIK','0000000001','instant',NULL,NULL,?,
            false,false,0,0,0,'default-context','https://www.sec.gov/test','test-run',?
        )
        """,
        [SECURITY_ID, accession, PERIOD_END, PERIOD_END, FIRST_AVAILABLE],
    )
    for ordinal, (taxonomy, concept, value) in enumerate(
        (
            ("us-gaap", "Assets", 110.0),
            ("us-gaap", "LiabilitiesAndStockholdersEquity", 100.0),
            ("us-gaap", "Liabilities", 60.0),
            (
                "us-gaap",
                "StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",
                35.0,
            ),
            ("testco", "RedeemableEquityCarryingAmount", 5.0),
        ),
        start=1,
    ):
        tmp_store.con.execute(
            """
            INSERT INTO xbrl_filing_facts (
                filing_fact_id,filing_context_id,security_id,cik,accession_number,
                form,filing_date,acceptance_datetime,primary_document,fact_ordinal,
                fact_kind,qname,taxonomy,concept,context_ref,unit_ref,
                unit_measures_json,unit_numerator_measures_json,
                unit_denominator_measures_json,is_hidden,raw_value,normalized_value,
                numeric_value,is_numeric,source_url,run_id,source_loaded_at
            ) VALUES (
                ?,'recon-context',?,'0000000001',?,'10-K',DATE '2024-02-01',
                TIMESTAMP '2024-02-01 11:00:00','form10k.htm',?,'nonFraction',
                ? || ':' || ?,?,?,'balance-sheet','usd',
                '["iso4217:USD"]','[]','[]',false,?,?,?,true,
                'https://www.sec.gov/test','test-run',?
            )
            """,
            [
                f"recon-fact-{ordinal}",
                SECURITY_ID,
                accession,
                ordinal,
                taxonomy,
                concept,
                taxonomy,
                concept,
                str(value),
                str(value),
                value,
                FIRST_AVAILABLE,
            ],
        )
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_extension_concept_map (
            extension_mapping_id,cik,taxonomy,concept,item_id,mapping_status,
            confidence,sign_multiplier,evidence_type,evidence_json,citation,
            description,valid_from,as_of_date,available_at,is_active
        ) VALUES (
            'fixture-extension-map','0000000001','testco',
            'RedeemableEquityCarryingAmount',1224,'approved',1.0,1.0,
            'same_context_equation','{}','https://example.test/filing',
            'Fixture issuer extension',DATE '1900-01-01',DATE '2024-02-01',
            TIMESTAMP '2024-02-01 11:30:00',true
        )
        """
    )

    row = tmp_store.con.execute(
        """
        SELECT status,input_filing_status,input_accession_count,
               context_verification_status,verified_filing_context_id,
               mismatch_reason,is_hard_failure,context_evidence_json
        FROM v_fundamental_reconciliation_contextual
        WHERE rule_id='assets_equal_total_liabilities_equity_instant'
        """
    ).fetchone()

    assert row[:7] == (
        "mismatch",
        "single_filing",
        1,
        "verified_same_context",
        "recon-context",
        "verified_accounting_mismatch",
        True,
    )
    evidence = json.loads(row[7])
    assert evidence["context_id"] == "balance-sheet"
    assert evidence["dimension_count"] == 0
    assert evidence["matched_term_count"] == 2

    extension_row = tmp_store.con.execute(
        """
        SELECT status,context_verification_status,mismatch_reason,is_hard_failure,
               extension_mapping_applied,extension_inputs_json,lhs_value,rhs_value,residual
        FROM v_fundamental_reconciliation_contextual
        WHERE rule_id='total_liabilities_equity_components_instant'
        """
    ).fetchone()
    assert extension_row[:5] == (
        "reconciled",
        "verified_same_context_with_extension_map",
        "within_tolerance_extension_mapped",
        False,
        True,
    )
    extension_inputs = json.loads(extension_row[5])
    assert extension_inputs[0]["extension_mapping_id"] == "fixture-extension-map"
    assert extension_inputs[0]["concept"] == "RedeemableEquityCarryingAmount"
    assert extension_row[6:] == (100.0, 100.0, 0.0)

    refresh = refresh_fundamental_reconciliation_serving(
        tmp_store,
        FundamentalReconciliationRefreshOptions(run_id="fixture-full-refresh"),
    )
    assert refresh.run_id == "fixture-full-refresh"
    assert (
        refresh.row_count
        == tmp_store.con.execute("SELECT count(*) FROM v_fundamental_reconciliation_contextual").fetchone()[0]
    )
    assert tmp_store.con.execute(
        """
        SELECT status,extension_mapping_applied,residual
        FROM fundamental_reconciliation_serving
        WHERE rule_id='total_liabilities_equity_components_instant'
        """
    ).fetchone() == ("reconciled", True, 0.0)
    assert tmp_store.con.execute(
        """
        SELECT status,source_row_count,serving_row_count,error_message,run_id
        FROM fundamental_reconciliation_builds
        WHERE build_id=?
        """,
        [refresh.build_id],
    ).fetchone() == (
        "completed",
        refresh.row_count,
        refresh.row_count,
        None,
        "fixture-full-refresh",
    )


def test_symbol_scoped_reconciliation_refresh_replaces_only_requested_security(
    tmp_store: DuckDBStore,
) -> None:
    _seed_balance_sheet_revisions(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_reconciliation_serving
        SELECT * FROM v_fundamental_reconciliation_contextual
        """
    )
    tmp_store.con.execute("UPDATE fundamental_reconciliation_serving SET label='stale fixture row'")

    refresh = refresh_fundamental_reconciliation_serving(
        tmp_store,
        FundamentalReconciliationRefreshOptions(
            symbols=(" recon ", "RECON"),
            run_id="fixture-symbol-refresh",
        ),
    )

    assert refresh.row_count > 0
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fundamental_reconciliation_serving
        WHERE security_id=? AND label='stale fixture row'
        """,
        [SECURITY_ID],
    ).fetchone() == (0,)
    assert tmp_store.con.execute(
        "SELECT scope_json FROM fundamental_reconciliation_builds WHERE build_id=?",
        [refresh.build_id],
    ).fetchone() == ('{"symbols":["RECON"]}',)


def test_reconciliation_serving_migration_publishes_schema_v1_2(tmp_store: DuckDBStore) -> None:
    assert tmp_store.con.execute(
        "SELECT count(*) FROM schema_migrations WHERE CAST(version AS INTEGER)=277"
    ).fetchone() == (1,)
    assert "is_required" in {
        row[1] for row in tmp_store.con.execute("PRAGMA table_info('fundamental_reconciliation_rule_term')").fetchall()
    }
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM api_field_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS'
          AND schema_code='reconciliation'
          AND schema_version='1.2.0'
          AND field_name IN (
              'input_filing_status','context_verification_status',
              'context_evidence_json','mismatch_reason','is_hard_failure',
              'extension_mapping_applied','extension_inputs_json'
          )
        """
    ).fetchone() == (7,)
    assert tmp_store.con.execute(
        """
        SELECT source_table
        FROM api_schema_catalog
        WHERE dataset_id='ATX.US.FUNDAMENTALS'
          AND schema_code='reconciliation'
          AND schema_version='1.2.0'
        """
    ).fetchone() == ("fundamental_reconciliation_serving",)
