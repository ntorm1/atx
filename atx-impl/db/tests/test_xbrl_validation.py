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


def _insert_calc_edge(store, relationship_id, parent, child, weight, order_value) -> None:
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


def _insert_calc_relationships(store) -> None:
    for relationship_id, parent, child, weight, order_value in (
        ("rel-cash", "Assets", "CashAndCashEquivalentsAtCarryingValue", 1.0, 1.0),
        ("rel-ar", "Assets", "AccountsReceivableNetCurrent", 1.0, 2.0),
    ):
        _insert_calc_edge(store, relationship_id, parent, child, weight, order_value)


def _insert_dimensional_context(
    store,
    *,
    filing_context_id: str,
    context_id: str,
    axis_qname: str,
    member_qname: str,
) -> None:
    """Insert an instant context carrying one explicit segment dimension
    (a single ``*Axis`` -> member) plus its ``xbrl_filing_dimensions`` row.

    Same period (2024-03-31 instant) as the default ``_insert_context`` context
    so the only thing that differs is the dimensional signature.
    """

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
            ?,
            'SEC-CIK-0000000002',
            '0000000002',
            '0000000002-24-000010',
            '10-Q',
            DATE '2024-05-03',
            DATE '2024-03-31',
            TIMESTAMP '2024-05-03 18:00:00',
            'form10q.htm',
            ?,
            'http://www.sec.gov/CIK',
            '0000000002',
            'instant',
            NULL,
            NULL,
            DATE '2024-03-31',
            true,
            false,
            1,
            0,
            1,
            ?,
            'https://www.sec.gov/Archives/edgar/data/2/000000000224000010/form10q.htm',
            'test',
            TIMESTAMP '2024-05-03 18:05:00'
        )
        """,
        [filing_context_id, context_id, "ctxhash-" + context_id],
    )
    store.con.execute(
        """
        INSERT INTO xbrl_filing_dimensions (
            filing_dimension_id,
            filing_context_id,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            context_id,
            context_element,
            member_kind,
            dimension_qname,
            dimension_taxonomy,
            dimension_concept,
            member_qname,
            member_taxonomy,
            member_concept,
            typed_member_value,
            member_text,
            ordinal,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            ?,
            ?,
            'SEC-CIK-0000000002',
            '0000000002',
            '0000000002-24-000010',
            '10-Q',
            DATE '2024-05-03',
            TIMESTAMP '2024-05-03 18:00:00',
            'form10q.htm',
            ?,
            'segment',
            'explicit',
            ?,
            'us-gaap',
            ?,
            ?,
            'us-gaap',
            ?,
            NULL,
            NULL,
            0,
            'https://www.sec.gov/Archives/edgar/data/2/000000000224000010/form10q.htm',
            'test',
            TIMESTAMP '2024-05-03 18:05:00'
        )
        """,
        [
            "dim-" + filing_context_id,
            filing_context_id,
            context_id,
            axis_qname,
            axis_qname.split(":")[-1],
            member_qname,
            member_qname.split(":")[-1],
        ],
    )


def _insert_fact(
    store,
    fact_id: str,
    ordinal: int,
    concept: str,
    value: float,
    *,
    filing_context_id: str = "ctx-1",
    context_ref: str = "c1",
) -> None:
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
            ?,
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
            ?,
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
        [
            fact_id,
            filing_context_id,
            ordinal,
            concept,
            concept,
            context_ref,
            value,
            value,
            value,
            ordinal,
        ],
    )


def _seed_validation_fixture(store, *, parent_value: float) -> None:
    _insert_sec_submission(store)
    _insert_context(store)
    _insert_calc_relationships(store)
    _insert_fact(store, "fact-assets", 1, "Assets", parent_value)
    _insert_fact(store, "fact-cash", 2, "CashAndCashEquivalentsAtCarryingValue", 400.0)
    _insert_fact(store, "fact-ar", 3, "AccountsReceivableNetCurrent", 600.0)


def _seed_dimensional_fixture(store) -> None:
    """One filing carrying BOTH a dimensional artifact and a genuine footing error.

    Artifact group (parent ``Assets`` in the default/no-dimension context):
      Assets(default) = 1000, its calc children are Cash + PropertyPlantAndEquipmentNet.
      Cash is reported in the default context (400) but PPE is reported ONLY broken out
      across ``PropertyPlantAndEquipmentByTypeAxis`` members (Land 250 + Building 350).
      So the default-context parent has an INCOMPLETE comparable child set: PPE is
      absent from the default signature yet present under other signatures. The
      dimension-aware check must recognise this as a dimensional artifact and resolve
      it (``passed``) rather than reporting a $600 footing failure. Note 400 + 250 + 350
      = 1000 foots at the consolidated level, but the consolidated PPE total is never
      reported in the default context.

    Genuine group (parent ``Liabilities`` in the default context):
      Liabilities(default) = 900, children AccountsPayable(300) + LongTermDebt(500) are
      BOTH present in the same default context. 300 + 500 = 800 != 900. A complete,
      fully comparable child set that still does not foot -> genuine footing error that
      the dimension-aware check must STILL catch (``failed``).
    """

    _insert_sec_submission(store)
    _insert_context(store)  # default (no-dimension) context ctx-1 / c1
    _insert_dimensional_context(
        store,
        filing_context_id="ctx-ppe-land",
        context_id="c-ppe-land",
        axis_qname="us-gaap:PropertyPlantAndEquipmentByTypeAxis",
        member_qname="us-gaap:LandMember",
    )
    _insert_dimensional_context(
        store,
        filing_context_id="ctx-ppe-bldg",
        context_id="c-ppe-bldg",
        axis_qname="us-gaap:PropertyPlantAndEquipmentByTypeAxis",
        member_qname="us-gaap:BuildingMember",
    )

    _insert_calc_edge(store, "rel-assets-cash", "Assets", "CashAndCashEquivalentsAtCarryingValue", 1.0, 1.0)
    _insert_calc_edge(store, "rel-assets-ppe", "Assets", "PropertyPlantAndEquipmentNet", 1.0, 2.0)
    _insert_calc_edge(store, "rel-liab-ap", "Liabilities", "AccountsPayableCurrent", 1.0, 1.0)
    _insert_calc_edge(store, "rel-liab-ltd", "Liabilities", "LongTermDebtNoncurrent", 1.0, 2.0)

    # Artifact: parent + one child in default; the other child only dimensional.
    _insert_fact(store, "fact-assets", 1, "Assets", 1000.0)
    _insert_fact(store, "fact-cash", 2, "CashAndCashEquivalentsAtCarryingValue", 400.0)
    _insert_fact(
        store,
        "fact-ppe-land",
        3,
        "PropertyPlantAndEquipmentNet",
        250.0,
        filing_context_id="ctx-ppe-land",
        context_ref="c-ppe-land",
    )
    _insert_fact(
        store,
        "fact-ppe-bldg",
        4,
        "PropertyPlantAndEquipmentNet",
        350.0,
        filing_context_id="ctx-ppe-bldg",
        context_ref="c-ppe-bldg",
    )

    # Genuine footing error: complete comparable child set in the default context.
    _insert_fact(store, "fact-liab", 5, "Liabilities", 900.0)
    _insert_fact(store, "fact-ap", 6, "AccountsPayableCurrent", 300.0)
    _insert_fact(store, "fact-ltd", 7, "LongTermDebtNoncurrent", 500.0)


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


def test_xbrl_dimensional_artifact_resolves_and_genuine_error_persists(tmp_store):
    """Dimension-aware grouping resolves the artifact but keeps the real error.

    The OLD (opaque-context) check fails BOTH parents: Assets sums to only 400
    (Cash) against a 1000 total, and Liabilities sums to 800 against 900. The NEW
    dimension-aware check must positively explain the Assets shortfall as a
    dimensional artifact (PPE reported only under a *Axis) and flip it to passed,
    while STILL catching the Liabilities footing error.
    """

    from db.xbrl_validation import refresh_xbrl_validation_results

    _seed_dimensional_fixture(tmp_store)
    refresh_xbrl_validation_results(tmp_store)

    status_by_parent = dict(
        tmp_store.con.execute(
            """
            SELECT parent_concept, status
            FROM xbrl_validation_results
            WHERE rule_family = 'calculation_linkbase'
            """
        ).fetchall()
    )

    # Dimensional artifact: parent total in default context, PPE child only broken
    # out across the *ByTypeAxis members -> incomplete comparable set -> resolved.
    assert status_by_parent.get("Assets") == "passed"
    # Genuine footing error: complete child set in the same default context that
    # simply does not foot (300 + 500 = 800 != 900) -> still caught.
    assert status_by_parent.get("Liabilities") == "failed"


def test_xbrl_dimensional_evidence_json_records_verdict(tmp_store):
    """Every triaged failure carries per-row dimensional evidence + comparability verdict."""

    import json

    from db.xbrl_validation import refresh_xbrl_validation_results

    _seed_dimensional_fixture(tmp_store)
    refresh_xbrl_validation_results(tmp_store)

    evidence_by_parent = dict(
        tmp_store.con.execute(
            """
            SELECT parent_concept, dimensional_evidence_json
            FROM xbrl_validation_results
            WHERE rule_family = 'calculation_linkbase'
            """
        ).fetchall()
    )

    assets = json.loads(evidence_by_parent["Assets"])
    assert assets["verdict"] == "resolved_dimensional_artifact"
    assert assets["parent_signature"] == "DEFAULT"
    missing_concepts = {m["concept"]: m for m in assets["missing_children"]}
    assert "PropertyPlantAndEquipmentNet" in missing_concepts
    assert missing_concepts["PropertyPlantAndEquipmentNet"]["present_in_other_context"] is True

    liabilities = json.loads(evidence_by_parent["Liabilities"])
    assert liabilities["verdict"] == "genuine_footing_error"
    assert liabilities["missing_children"] == []


def test_dqc_0015_negative_concept_subset_flags_negative_fact(tmp_store):
    """DQC_0015 subset: curated non-negative us-gaap concepts cannot be negative."""

    from db.xbrl_validation import refresh_xbrl_validation_results

    _insert_sec_submission(tmp_store)
    _insert_context(tmp_store)
    _insert_fact(tmp_store, "fact-negative-assets", 1, "Assets", -1.0)

    assert refresh_xbrl_validation_results(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT rule_family, rule_code, status, severity, parent_concept, parent_value, message
        FROM xbrl_validation_results
        """
    ).fetchone()

    assert row[0:6] == ("dqc", "DQC_0015", "failed", "error", "Assets", -1.0)
    assert "DQC_0015 subset" in row[6]
    assert "negative numeric value" in row[6]


def test_dqc_0015_positive_curated_concept_does_not_emit_false_positive(tmp_store):
    """DQC_0015 subset passing fixture: positive sign-constrained fact is quiet."""

    from db.xbrl_validation import refresh_xbrl_validation_results

    _insert_sec_submission(tmp_store)
    _insert_context(tmp_store)
    _insert_fact(tmp_store, "fact-positive-assets", 1, "Assets", 1.0)

    assert refresh_xbrl_validation_results(tmp_store) == 0
    dqc_rows = tmp_store.con.execute(
        "SELECT count(*) FROM xbrl_validation_results WHERE rule_family = 'dqc'"
    ).fetchone()[0]
    assert dqc_rows == 0


def test_dqc_0053_excluded_member_axis_subset_flags_wrong_pair(tmp_store):
    """DQC_0053 subset: curated wrong member-axis pair emits a failed DQC row."""

    from db.xbrl_validation import refresh_xbrl_validation_results

    _insert_sec_submission(tmp_store)
    _insert_dimensional_context(
        tmp_store,
        filing_context_id="ctx-wrong-axis",
        context_id="c-wrong-axis",
        axis_qname="us-gaap:BusinessSegmentsAxis",
        member_qname="us-gaap:LandMember",
    )

    assert refresh_xbrl_validation_results(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT rule_family, rule_code, status, severity, parent_concept, context_ref, message
        FROM xbrl_validation_results
        """
    ).fetchone()

    assert row[0:6] == ("dqc", "DQC_0053", "failed", "error", "BusinessSegmentsAxis", "c-wrong-axis")
    assert "DQC_0053 subset" in row[6]
    assert "excluded axis-member pair" in row[6]


def test_dqc_0053_allowed_member_axis_does_not_emit_false_positive(tmp_store):
    """DQC_0053 subset passing fixture: PPE member on PPE axis is quiet."""

    from db.xbrl_validation import refresh_xbrl_validation_results

    _insert_sec_submission(tmp_store)
    _insert_dimensional_context(
        tmp_store,
        filing_context_id="ctx-allowed-axis",
        context_id="c-allowed-axis",
        axis_qname="us-gaap:PropertyPlantAndEquipmentByTypeAxis",
        member_qname="us-gaap:LandMember",
    )

    assert refresh_xbrl_validation_results(tmp_store) == 0
    dqc_rows = tmp_store.con.execute(
        "SELECT count(*) FROM xbrl_validation_results WHERE rule_family = 'dqc'"
    ).fetchone()[0]
    assert dqc_rows == 0


def test_dqc_subset_refresh_is_idempotent_for_dqc_family(tmp_store):
    """DQC refresh deletes/reinserts only the DQC family and does not accumulate rows."""

    from db.xbrl_validation import XbrlValidationOptions, refresh_xbrl_validation_results

    _insert_sec_submission(tmp_store)
    _insert_context(tmp_store)
    _insert_fact(tmp_store, "fact-negative-assets", 1, "Assets", -1.0)

    options = XbrlValidationOptions(run_id="dqc-idempotent-test")
    assert refresh_xbrl_validation_results(tmp_store, options) == 1
    first_id = tmp_store.con.execute(
        "SELECT validation_id FROM xbrl_validation_results WHERE rule_family = 'dqc'"
    ).fetchone()[0]

    assert refresh_xbrl_validation_results(tmp_store, options) == 1
    rows = tmp_store.con.execute(
        """
        SELECT count(*), min(validation_id), max(run_id)
        FROM xbrl_validation_results
        WHERE rule_family = 'dqc'
        """
    ).fetchone()

    assert rows == (1, first_id, "dqc-idempotent-test")
