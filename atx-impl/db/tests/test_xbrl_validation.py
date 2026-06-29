from __future__ import annotations


def _insert_sec_submission(store) -> None:
    store.con.execute(
        """
        INSERT INTO sec_submissions (
            security_id,
            cik,
            accession_number,
            filing_date,
            report_date,
            acceptance_datetime,
            form,
            primary_document,
            primary_doc_description,
            file_number,
            film_number,
            items,
            size,
            is_xbrl,
            is_inline_xbrl,
            act,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            'SEC-CIK-0000000002',
            '0000000002',
            '0000000002-24-000010',
            DATE '2024-05-03',
            DATE '2024-03-31',
            TIMESTAMP '2024-05-03 18:00:00',
            '10-Q',
            'form10q.htm',
            '10-Q',
            NULL,
            NULL,
            NULL,
            12345,
            true,
            true,
            NULL,
            'https://www.sec.gov/Archives/edgar/data/2/000000000224000010/form10q.htm',
            'test',
            TIMESTAMP '2024-05-03 18:05:00'
        )
        """
    )


def _insert_context(store) -> None:
    store.con.execute(
        """
        INSERT INTO xbrl_filing_contexts (
            filing_context_id,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            report_date,
            acceptance_datetime,
            primary_document,
            context_id,
            entity_identifier_scheme,
            entity_identifier,
            period_type,
            period_start,
            period_end,
            instant_date,
            has_segment,
            has_scenario,
            explicit_member_count,
            typed_member_count,
            dimension_count,
            context_hash,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            'ctx-1',
            'SEC-CIK-0000000002',
            '0000000002',
            '0000000002-24-000010',
            '10-Q',
            DATE '2024-05-03',
            DATE '2024-03-31',
            TIMESTAMP '2024-05-03 18:00:00',
            'form10q.htm',
            'c1',
            'http://www.sec.gov/CIK',
            '0000000002',
            'instant',
            NULL,
            NULL,
            DATE '2024-03-31',
            false,
            false,
            0,
            0,
            0,
            'ctxhash',
            'https://www.sec.gov/Archives/edgar/data/2/000000000224000010/form10q.htm',
            'test',
            TIMESTAMP '2024-05-03 18:05:00'
        )
        """
    )


def _insert_calc_relationships(store) -> None:
    rows = [
        ("rel-cash", "Assets", "CashAndCashEquivalentsAtCarryingValue", 1.0, 1.0),
        ("rel-ar", "Assets", "AccountsReceivableNetCurrent", 1.0, 2.0),
    ]
    for relationship_id, parent, child, weight, order_value in rows:
        store.con.execute(
            """
            INSERT INTO xbrl_taxonomy_relationships (
                relationship_id,
                taxonomy_package_id,
                taxonomy,
                release_year,
                linkbase_type,
                source_file,
                role_uri,
                role_name,
                role_href,
                arcrole,
                from_label,
                to_label,
                parent_href,
                parent_taxonomy,
                parent_concept,
                parent_concept_kind,
                child_href,
                child_taxonomy,
                child_concept,
                child_concept_kind,
                order_value,
                weight,
                priority,
                preferred_label,
                use,
                closed,
                context_element,
                usable,
                target_role,
                touches_observed_concept,
                source_url,
                source_loaded_at
            )
            VALUES (
                ?,
                'pkg-2026',
                'us-gaap',
                2026,
                'calculation',
                'us-gaap-cal.xml',
                'http://fasb.org/us-gaap/role/BalanceSheet',
                'Balance Sheet',
                NULL,
                'http://www.xbrl.org/2003/arcrole/summation-item',
                'from',
                'to',
                NULL,
                'us-gaap',
                ?,
                'line_item',
                NULL,
                'us-gaap',
                ?,
                'line_item',
                ?,
                ?,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                true,
                'https://xbrl.fasb.org/us-gaap/2026/us-gaap-2026.zip',
                TIMESTAMP '2024-01-01 00:00:00'
            )
            """,
            [relationship_id, parent, child, order_value, weight],
        )


def _insert_fact(store, fact_id: str, ordinal: int, concept: str, value: float) -> None:
    store.con.execute(
        """
        INSERT INTO xbrl_filing_facts (
            filing_fact_id,
            filing_context_id,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            fact_ordinal,
            fact_kind,
            ix_id,
            qname,
            taxonomy,
            concept,
            context_ref,
            unit_ref,
            unit_measures_json,
            unit_numerator_measures_json,
            unit_denominator_measures_json,
            decimals,
            precision,
            scale,
            sign,
            format,
            continued_at,
            is_hidden,
            raw_value,
            normalized_value,
            numeric_value,
            is_numeric,
            source_line,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            ?,
            'ctx-1',
            'SEC-CIK-0000000002',
            '0000000002',
            '0000000002-24-000010',
            '10-Q',
            DATE '2024-05-03',
            TIMESTAMP '2024-05-03 18:00:00',
            'form10q.htm',
            ?,
            'nonFraction',
            NULL,
            'us-gaap:' || ?,
            'us-gaap',
            ?,
            'c1',
            'usd',
            '["iso4217:USD"]',
            '[]',
            '[]',
            '0',
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            false,
            CAST(? AS VARCHAR),
            CAST(? AS VARCHAR),
            ?,
            true,
            ?,
            'https://www.sec.gov/Archives/edgar/data/2/000000000224000010/form10q.htm',
            'test',
            TIMESTAMP '2024-05-03 18:05:00'
        )
        """,
        [fact_id, ordinal, concept, concept, value, value, value, ordinal],
    )


def _seed_validation_fixture(store, *, parent_value: float) -> None:
    _insert_sec_submission(store)
    _insert_context(store)
    _insert_calc_relationships(store)
    _insert_fact(store, "fact-assets", 1, "Assets", parent_value)
    _insert_fact(store, "fact-cash", 2, "CashAndCashEquivalentsAtCarryingValue", 400.0)
    _insert_fact(store, "fact-ar", 3, "AccountsReceivableNetCurrent", 600.0)


def test_migration_0009_xbrl_validation_table_exists(tmp_store):
    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM information_schema.tables
            WHERE table_schema = 'main'
            """
        ).fetchall()
    }
    assert "xbrl_validation_results" in tables


def test_xbrl_calculation_validation_passes_balanced_sum(tmp_store):
    from db.xbrl_validation import refresh_xbrl_validation_results

    _seed_validation_fixture(tmp_store, parent_value=1000.0)

    assert refresh_xbrl_validation_results(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT status, parent_value, child_weighted_sum, absolute_difference, child_count
        FROM xbrl_validation_results
        """
    ).fetchone()
    assert row == ("passed", 1000.0, 1000.0, 0.0, 2)


def test_xbrl_calculation_validation_fails_unbalanced_sum(tmp_store):
    from db.xbrl_validation import refresh_xbrl_validation_results

    _seed_validation_fixture(tmp_store, parent_value=950.0)

    assert refresh_xbrl_validation_results(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT status, parent_value, child_weighted_sum, absolute_difference
        FROM xbrl_validation_results
        """
    ).fetchone()
    assert row == ("failed", 950.0, 1000.0, 50.0)


def test_xbrl_validation_quality_surfaces_failures(tmp_store):
    from db.quality import run_warehouse_quality_checks
    from db.xbrl_validation import refresh_xbrl_validation_results

    _seed_validation_fixture(tmp_store, parent_value=950.0)
    refresh_xbrl_validation_results(tmp_store)

    results = run_warehouse_quality_checks(tmp_store, record=False)
    failures = [
        result
        for result in results
        if result.check_name == "failed_xbrl_calculation_linkbase_checks"
    ]
    assert failures
    assert failures[0].status == "failed"
    assert failures[0].observed_value == 1.0
