from __future__ import annotations

import uuid
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "XBRL validation"
DQC_RULES_GUIDANCE_URL = "https://xbrl.us/home/priorities/data-quality/rules-guidance/"
DQC_0004_URL = "https://xbrl.us/data-rule/dqc_0004/"
DQC_0015_URL = "https://xbrl.us/data-rule/dqc_0015/"
DQC_0018_URL = "https://xbrl.us/data-rule/dqc_0018/"
DQC_0041_URL = "https://xbrl.us/data-rule/dqc_0041/"
DQC_0053_URL = "https://xbrl.us/data-rule/dqc_0053/"
DQC_0080_URL = "https://xbrl.us/data-rule/dqc_0080/"

# PF-S7 S7-2 ports a deliberately small SQL-native subset of XBRL US DQC_0015.
# The full rule includes a much larger element catalog plus member exclusions;
# this constant is only the sign-constrained concept slice that is common in our
# fixtures/live cached facts and can be checked safely without Arelle.
DQC_0015_NON_NEGATIVE_CONCEPTS: tuple[tuple[str, str], ...] = (
    ("us-gaap", "Assets"),
    ("us-gaap", "AssetsCurrent"),
    ("us-gaap", "CashAndCashEquivalentsAtCarryingValue"),
    ("us-gaap", "AccountsReceivableNetCurrent"),
    ("us-gaap", "InventoryNet"),
    ("us-gaap", "PropertyPlantAndEquipmentNet"),
    ("us-gaap", "Liabilities"),
    ("us-gaap", "LiabilitiesCurrent"),
    ("us-gaap", "Revenues"),
    ("us-gaap", "SalesRevenueNet"),
    ("us-gaap", "CommonStockSharesOutstanding"),
)

# PF-S7 S7-2 fallback for DQC_0053. If xbrl_dimension_edges exposes an explicit
# unusable axis-member edge, SQL flags that first; these curated pairs cover a
# tiny, documented subset when the local normalized dimension graph is not rich
# enough to compute full dimension-domain-member closure.
DQC_0053_EXCLUDED_MEMBER_AXIS_PAIRS: tuple[tuple[str, str, str], ...] = (
    (
        "us-gaap:BusinessSegmentsAxis",
        "us-gaap:LandMember",
        "LandMember belongs to property, plant and equipment breakdowns, not BusinessSegmentsAxis.",
    ),
    (
        "us-gaap:ProductOrServiceAxis",
        "us-gaap:BuildingMember",
        "BuildingMember belongs to property, plant and equipment breakdowns, not ProductOrServiceAxis.",
    ),
)

# PF2-S9 S9-2 widens the SQL-native subset while documenting the boundary:
# DQC_0004 is a same-context Assets = Liabilities + Equity footing; DQC_0080
# mirrors the non-negative concept check for a small IFRS slice; DQC_0018 is a
# curated deprecated-element list; DQC_0041 checks local direct axis-member
# validity. Rules needing full Arelle/XULE semantics remain catalogued as skips
# by migration 0126, not faked here.
DQC_0080_IFRS_NON_NEGATIVE_CONCEPTS: tuple[tuple[str, str], ...] = (
    ("ifrs-full", "Assets"),
    ("ifrs-full", "CurrentAssets"),
    ("ifrs-full", "CashAndCashEquivalents"),
    ("ifrs-full", "Liabilities"),
    ("ifrs-full", "CurrentLiabilities"),
    ("ifrs-full", "Revenue"),
)

DQC_0018_DEPRECATED_CONCEPTS: tuple[tuple[str, str, str, str], ...] = (
    (
        "us-gaap",
        "ExtraordinaryItemsOfIncomeStatement",
        "IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
        "Extraordinary items presentation is deprecated in modern US-GAAP taxonomy reporting.",
    ),
)

DQC_0041_ALLOWED_MEMBER_AXIS_PAIRS: tuple[tuple[str, str], ...] = (
    ("us-gaap:PropertyPlantAndEquipmentByTypeAxis", "us-gaap:LandMember"),
    ("us-gaap:PropertyPlantAndEquipmentByTypeAxis", "us-gaap:BuildingMember"),
)


@dataclass(frozen=True)
class XbrlValidationOptions:
    absolute_tolerance: float = 1.0
    run_id: str | None = None


def refresh_xbrl_validation_results(
    store: DuckDBStore,
    options: XbrlValidationOptions | None = None,
) -> int:
    """Validate filing facts against public calculation-linkbase relationships.

    This is the SQL-native v1 validator, made dimension-aware in PF-S7 S7-0. Each
    fact's ``context_ref`` is resolved through ``xbrl_filing_contexts`` /
    ``xbrl_filing_dimensions`` to a period + dimensional signature (the ordered
    ``*Axis`` -> member set; ``''`` for the default/no-dimension context). A parent
    total foots ONLY over children sharing the same period, unit AND dimensional
    signature -- never summing a member child into a consolidated parent.

    Each emitted row is triaged and carries ``dimensional_evidence_json``:
    a complete child set that foots is ``passed``; a complete set that does not is
    a genuine footing error (``failed`` / ``resolution_status='genuine_error'``);
    an incomplete set whose missing children are all reported under a DIFFERENT
    dimensional context is a resolved dimensional artifact
    (``passed`` / ``resolution_status='resolved_dimensional_artifact'``); an
    incomplete set with a child absent from every context stays failed as
    ``genuine_error``. Complete footings use ``resolved_ok``. ``absolute_tolerance``
    is never widened to make a check pass. A future Arelle/DQC sidecar can append
    additional rule families to the same table. PF-S7 S7-2 appends a small
    SQL-native DQC subset under ``rule_family = 'dqc'``: DQC_0004 same-context
    assets footings, DQC_0015/DQC_0080 non-negative concept facts,
    DQC_0018 deprecated elements, and DQC_0041/DQC_0053 axis-member checks.
    It is a subset, not a full DQC plugin. DQC rows emitted here are violations
    only, so failed DQC rows use ``resolution_status='genuine_error'``.
    """

    options = options or XbrlValidationOptions()
    validation_run_id = str(uuid.uuid4())
    run_id = options.run_id or validation_run_id
    tolerance = float(options.absolute_tolerance)

    with store.transaction():
        store.con.execute("DELETE FROM xbrl_validation_results WHERE rule_family = 'calculation_linkbase'")
        store.con.execute("DELETE FROM xbrl_validation_results WHERE rule_family = 'dqc'")
        store.con.execute(
            """
            INSERT INTO xbrl_validation_results (
                validation_id,
                validation_run_id,
                rule_family,
                rule_code,
                severity,
                status,
                security_id,
                cik,
                accession_number,
                form,
                filing_date,
                acceptance_datetime,
                primary_document,
                role_uri,
                parent_taxonomy,
                parent_concept,
                context_ref,
                unit_ref,
                parent_fact_id,
                parent_value,
                child_weighted_sum,
                absolute_difference,
                tolerance,
                child_count,
                child_facts_json,
                message,
                dimensional_evidence_json,
                resolution_status,
                source_url,
                run_id,
                source_loaded_at
            )
            WITH latest_edges AS (
                SELECT *
                FROM xbrl_taxonomy_relationships
                WHERE linkbase_type = 'calculation'
                  AND parent_concept IS NOT NULL
                  AND child_concept IS NOT NULL
                  AND weight IS NOT NULL
                  AND coalesce(use, '') <> 'prohibited'
                QUALIFY row_number() OVER (
                    PARTITION BY
                        coalesce(role_uri, ''),
                        coalesce(parent_taxonomy, ''),
                        parent_concept,
                        coalesce(child_taxonomy, ''),
                        child_concept
                    ORDER BY release_year DESC, taxonomy_package_id DESC, relationship_id DESC
                ) = 1
            ),
            -- Canonical dimensional signature per context: the ordered set of
            -- axis -> member pairs. Dimensionless contexts have no row here and
            -- resolve to '' (the default/no-dimension signature) below.
            context_dims AS (
                SELECT
                    filing_context_id,
                    string_agg(
                        dimension_qname || '=' || coalesce(member_qname, typed_member_value, ''),
                        ';' ORDER BY dimension_qname, member_qname, typed_member_value
                    ) AS dimensional_signature
                FROM xbrl_filing_dimensions
                GROUP BY filing_context_id
            ),
            -- Every numeric fact tagged with its resolved period + dimensional
            -- signature. context_ref is resolved through xbrl_filing_contexts
            -- (keyed security_id + accession_number + primary_document + context_id)
            -- rather than treated as an opaque string.
            facts_with_sig AS (
                SELECT
                    f.filing_fact_id,
                    f.security_id,
                    f.cik,
                    f.accession_number,
                    f.form,
                    f.filing_date,
                    f.acceptance_datetime,
                    f.primary_document,
                    f.taxonomy,
                    f.concept,
                    f.context_ref,
                    f.unit_ref,
                    f.numeric_value,
                    f.source_url,
                    f.source_loaded_at,
                    ctx.period_type,
                    ctx.period_start,
                    ctx.period_end,
                    ctx.instant_date,
                    coalesce(cd.dimensional_signature, '') AS dimensional_signature
                FROM xbrl_filing_facts f
                JOIN xbrl_filing_contexts ctx
                  ON ctx.security_id = f.security_id
                 AND ctx.accession_number = f.accession_number
                 AND ctx.primary_document = f.primary_document
                 AND ctx.context_id = f.context_ref
                LEFT JOIN context_dims cd
                  ON cd.filing_context_id = ctx.filing_context_id
                WHERE f.is_numeric
                  AND f.numeric_value IS NOT NULL
            ),
            -- One row per (parent fact, applicable calc edge). Expands to the full
            -- expected child set so we can tell a complete footing from one whose
            -- children were broken out into a different dimensional context.
            parent_edges AS (
                SELECT
                    p.filing_fact_id AS parent_fact_id,
                    p.security_id,
                    p.cik,
                    p.accession_number,
                    p.form,
                    p.filing_date,
                    p.acceptance_datetime,
                    p.primary_document,
                    edge.role_uri,
                    p.taxonomy AS parent_taxonomy,
                    p.concept AS parent_concept,
                    p.context_ref,
                    p.unit_ref,
                    p.numeric_value AS parent_value,
                    p.source_url AS parent_source_url,
                    p.source_loaded_at AS parent_source_loaded_at,
                    p.period_type,
                    p.period_start,
                    p.period_end,
                    p.instant_date,
                    p.dimensional_signature AS parent_signature,
                    edge.child_taxonomy AS edge_child_taxonomy,
                    edge.child_concept AS edge_child_concept,
                    edge.weight AS edge_weight,
                    edge.order_value AS edge_order_value
                FROM facts_with_sig p
                JOIN latest_edges edge
                  ON edge.parent_concept = p.concept
                 AND (
                        edge.parent_taxonomy IS NULL
                     OR p.taxonomy IS NULL
                     OR edge.parent_taxonomy = p.taxonomy
                 )
            ),
            -- Attach the comparable child fact (same period/unit AND same
            -- dimensional signature) if present, plus a flag recording whether an
            -- ABSENT child exists under a DIFFERENT dimensional context in the
            -- same filing (the positive evidence that resolves an artifact).
            edge_children AS (
                SELECT
                    pe.*,
                    c.filing_fact_id AS child_fact_id,
                    c.taxonomy AS child_taxonomy,
                    c.numeric_value AS child_value,
                    c.source_loaded_at AS child_source_loaded_at,
                    CASE
                        WHEN c.filing_fact_id IS NULL AND EXISTS (
                            SELECT 1
                            FROM facts_with_sig celse
                            WHERE celse.security_id = pe.security_id
                              AND celse.accession_number = pe.accession_number
                              AND celse.primary_document = pe.primary_document
                              AND celse.concept = pe.edge_child_concept
                              AND (
                                    pe.edge_child_taxonomy IS NULL
                                 OR celse.taxonomy IS NULL
                                 OR pe.edge_child_taxonomy = celse.taxonomy
                              )
                              AND coalesce(celse.unit_ref, '') = coalesce(pe.unit_ref, '')
                              AND celse.period_type = pe.period_type
                              AND coalesce(celse.period_start, DATE '0001-01-01') = coalesce(pe.period_start, DATE '0001-01-01')
                              AND coalesce(celse.period_end, DATE '0001-01-01') = coalesce(pe.period_end, DATE '0001-01-01')
                              AND coalesce(celse.instant_date, DATE '0001-01-01') = coalesce(pe.instant_date, DATE '0001-01-01')
                              AND celse.dimensional_signature <> pe.parent_signature
                        ) THEN 1
                        ELSE 0
                    END AS present_elsewhere
                FROM parent_edges pe
                LEFT JOIN facts_with_sig c
                  ON c.security_id = pe.security_id
                 AND c.accession_number = pe.accession_number
                 AND c.primary_document = pe.primary_document
                 AND c.concept = pe.edge_child_concept
                 AND (
                        pe.edge_child_taxonomy IS NULL
                     OR c.taxonomy IS NULL
                     OR pe.edge_child_taxonomy = c.taxonomy
                 )
                 AND coalesce(c.unit_ref, '') = coalesce(pe.unit_ref, '')
                 AND c.period_type = pe.period_type
                 AND coalesce(c.period_start, DATE '0001-01-01') = coalesce(pe.period_start, DATE '0001-01-01')
                 AND coalesce(c.period_end, DATE '0001-01-01') = coalesce(pe.period_end, DATE '0001-01-01')
                 AND coalesce(c.instant_date, DATE '0001-01-01') = coalesce(pe.instant_date, DATE '0001-01-01')
                 AND c.dimensional_signature = pe.parent_signature
            ),
            -- Aggregate to one row per (parent fact, role). child_weighted_sum is
            -- taken ONLY over children in the parent's own dimensional signature;
            -- we never sum a member child into a consolidated parent.
            calc_groups AS (
                SELECT
                    parent_fact_id,
                    security_id,
                    cik,
                    accession_number,
                    form,
                    filing_date,
                    acceptance_datetime,
                    primary_document,
                    role_uri,
                    parent_taxonomy,
                    parent_concept,
                    context_ref,
                    unit_ref,
                    parent_value,
                    parent_signature,
                    parent_source_url,
                    count(*)::INTEGER AS total_edge_count,
                    count(child_fact_id)::INTEGER AS matched_child_count,
                    coalesce(sum(CASE WHEN child_fact_id IS NOT NULL THEN child_value * edge_weight ELSE 0 END), 0) AS child_weighted_sum,
                    sum(CASE WHEN child_fact_id IS NULL AND present_elsewhere = 0 THEN 1 ELSE 0 END) AS missing_nowhere_count,
                    max(coalesce(child_source_loaded_at, parent_source_loaded_at)) AS source_loaded_at,
                    list(
                        struct_pack(
                            filing_fact_id := child_fact_id,
                            taxonomy := child_taxonomy,
                            concept := edge_child_concept,
                            weight := edge_weight,
                            numeric_value := child_value
                        )
                        ORDER BY edge_order_value NULLS LAST, edge_child_concept, child_fact_id
                    ) FILTER (WHERE child_fact_id IS NOT NULL) AS matched_children,
                    coalesce(
                        list(
                            struct_pack(
                                concept := edge_child_concept,
                                present_in_other_context := (present_elsewhere = 1)
                            )
                            ORDER BY edge_child_concept
                        ) FILTER (WHERE child_fact_id IS NULL),
                        CAST([] AS STRUCT(concept VARCHAR, present_in_other_context BOOLEAN)[])
                    ) AS missing_children
                FROM edge_children
                GROUP BY
                    parent_fact_id,
                    security_id,
                    cik,
                    accession_number,
                    form,
                    filing_date,
                    acceptance_datetime,
                    primary_document,
                    role_uri,
                    parent_taxonomy,
                    parent_concept,
                    context_ref,
                    unit_ref,
                    parent_value,
                    parent_signature,
                    parent_source_url
            ),
            -- Triage each emitted group. Only groups with at least one comparable
            -- child are emitted (matching the old inner-join emission set). A
            -- shortfall is a genuine error UNLESS every missing child is present
            -- under another dimensional context (positively an artifact).
            triaged AS (
                SELECT
                    cg.*,
                    abs(parent_value - child_weighted_sum) AS absolute_difference,
                    CASE
                        WHEN matched_child_count = total_edge_count THEN
                            CASE
                                WHEN abs(parent_value - child_weighted_sum) <= ? THEN 'complete_footing_ok'
                                ELSE 'genuine_footing_error'
                            END
                        WHEN missing_nowhere_count = 0 THEN 'resolved_dimensional_artifact'
                        ELSE 'genuine_missing_child'
                    END AS verdict
                FROM calc_groups cg
                WHERE matched_child_count >= 1
            )
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        ?,
                        'calculation_linkbase',
                        security_id,
                        accession_number,
                        primary_document,
                        coalesce(role_uri, ''),
                        coalesce(parent_taxonomy, ''),
                        parent_concept,
                        context_ref,
                        coalesce(unit_ref, ''),
                        parent_fact_id
                    )
                ) AS validation_id,
                ? AS validation_run_id,
                'calculation_linkbase' AS rule_family,
                'calc_sum_parent_equals_weighted_children' AS rule_code,
                'error' AS severity,
                CASE
                    WHEN verdict IN ('complete_footing_ok', 'resolved_dimensional_artifact') THEN 'passed'
                    ELSE 'failed'
                END AS status,
                security_id,
                cik,
                accession_number,
                form,
                filing_date,
                acceptance_datetime,
                primary_document,
                role_uri,
                parent_taxonomy,
                parent_concept,
                context_ref,
                unit_ref,
                parent_fact_id,
                parent_value,
                child_weighted_sum,
                absolute_difference,
                ? AS tolerance,
                matched_child_count AS child_count,
                CAST(to_json(matched_children) AS VARCHAR) AS child_facts_json,
                CASE verdict
                    WHEN 'complete_footing_ok' THEN 'Calculation linkbase sum matched weighted child facts within tolerance.'
                    WHEN 'genuine_footing_error' THEN 'Calculation linkbase sum did not match weighted child facts; the full child set is present in the same dimensional context, so this is a genuine footing error.'
                    WHEN 'resolved_dimensional_artifact' THEN 'Resolved as dimensional artifact: one or more calculation-linkbase children are reported only under a different dimensional context (segment/scenario axis member), so the parent''s comparable child set is incomplete. Not a footing error; tolerance unchanged.'
                    ELSE 'Calculation linkbase check failed: one or more child concepts are absent from every dimensional context in this filing, so the shortfall cannot be explained as a dimensional artifact.'
                END AS message,
                CAST(
                    to_json(
                        struct_pack(
                            parent_signature := CASE WHEN parent_signature = '' THEN 'DEFAULT' ELSE parent_signature END,
                            verdict := verdict,
                            total_edge_count := total_edge_count,
                            matched_child_count := matched_child_count,
                            missing_children := missing_children
                        )
                    ) AS VARCHAR
                ) AS dimensional_evidence_json,
                CASE verdict
                    WHEN 'complete_footing_ok' THEN 'resolved_ok'
                    WHEN 'resolved_dimensional_artifact' THEN 'resolved_dimensional_artifact'
                    ELSE 'genuine_error'
                END AS resolution_status,
                parent_source_url AS source_url,
                ? AS run_id,
                coalesce(source_loaded_at, now()) AS source_loaded_at
            FROM triaged
            """,
            [tolerance, validation_run_id, validation_run_id, tolerance, run_id],
        )
        _insert_dqc_subset_results(store, validation_run_id=validation_run_id, run_id=run_id)
        _insert_pf2_s9_dqc_results(
            store,
            validation_run_id=validation_run_id,
            run_id=run_id,
            tolerance=tolerance,
        )

    return int(
        store.con.execute(
            "SELECT count(*) FROM xbrl_validation_results WHERE validation_run_id = ?",
            [validation_run_id],
        ).fetchone()[0]
    )


def _sql_values(rows: tuple[tuple[str, ...], ...]) -> str:
    return ",\n                    ".join(
        "(" + ", ".join("'" + value.replace("'", "''") + "'" for value in row) + ")" for row in rows
    )


def _insert_dqc_subset_results(store: DuckDBStore, *, validation_run_id: str, run_id: str) -> None:
    """Append the PF-S7 S7-2 SQL-native DQC subset.

    DQC_0015 is limited to the curated non-negative us-gaap concept list above.
    DQC member exclusions and broader sign exceptions require the full official
    rule catalog / Arelle plugin and are deliberately skipped.

    DQC_0053 uses local ``xbrl_dimension_edges`` only when they directly express
    an unusable axis-member relation; otherwise it checks the tiny curated
    disallowed pair list above. It does not claim full transitive
    dimension-domain-member validation.
    """

    non_negative_values = _sql_values(DQC_0015_NON_NEGATIVE_CONCEPTS)
    excluded_pair_values = _sql_values(DQC_0053_EXCLUDED_MEMBER_AXIS_PAIRS)
    store.con.execute(
        f"""
        INSERT INTO xbrl_validation_results (
            validation_id,
            validation_run_id,
            rule_family,
            rule_code,
            severity,
            status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            role_uri,
            parent_taxonomy,
            parent_concept,
            context_ref,
            unit_ref,
            parent_fact_id,
            parent_value,
            child_weighted_sum,
            absolute_difference,
            tolerance,
            child_count,
            child_facts_json,
            message,
            dimensional_evidence_json,
            resolution_status,
            source_url,
            run_id,
            source_loaded_at
        )
        WITH dqc_0015_concepts(taxonomy, concept) AS (
            VALUES
                    {non_negative_values}
        ),
        dqc_0053_curated_pairs(axis_qname, member_qname, reason) AS (
            VALUES
                    {excluded_pair_values}
        ),
        dqc_0015_violations AS (
            SELECT
                sha256(
                    concat_ws(
                        '|',
                        ?,
                        'dqc',
                        'DQC_0015',
                        f.security_id,
                        f.accession_number,
                        f.primary_document,
                        f.context_ref,
                        f.filing_fact_id
                    )
                ) AS validation_id,
                f.*
            FROM xbrl_filing_facts f
            JOIN dqc_0015_concepts c
              ON coalesce(f.taxonomy, '') = c.taxonomy
             AND f.concept = c.concept
            WHERE f.is_numeric
              AND f.numeric_value < 0
        ),
        dqc_0053_edge_violations AS (
            SELECT
                d.*,
                'xbrl_dimension_edges direct usable=false axis-member relation' AS reason
            FROM xbrl_filing_dimensions d
            WHERE d.member_kind = 'explicit'
              AND d.dimension_concept IS NOT NULL
              AND d.member_concept IS NOT NULL
              AND EXISTS (
                    SELECT 1
                    FROM xbrl_dimension_edges e
                    WHERE e.source_concept = d.dimension_concept
                      AND e.target_concept = d.member_concept
                      AND coalesce(e.source_taxonomy, d.dimension_taxonomy, '') = coalesce(d.dimension_taxonomy, '')
                      AND coalesce(e.target_taxonomy, d.member_taxonomy, '') = coalesce(d.member_taxonomy, '')
                      AND e.usable = false
              )
        ),
        dqc_0053_curated_violations AS (
            SELECT
                d.*,
                p.reason
            FROM xbrl_filing_dimensions d
            JOIN dqc_0053_curated_pairs p
              ON d.dimension_qname = p.axis_qname
             AND d.member_qname = p.member_qname
            WHERE d.member_kind = 'explicit'
        ),
        dqc_0053_violations AS (
            SELECT *
            FROM (
                SELECT * FROM dqc_0053_edge_violations
                UNION ALL
                SELECT * FROM dqc_0053_curated_violations
            )
            QUALIFY row_number() OVER (
                PARTITION BY filing_dimension_id
                ORDER BY reason
            ) = 1
        )
        SELECT
            validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0015' AS rule_code,
            'error' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            taxonomy AS parent_taxonomy,
            concept AS parent_concept,
            context_ref,
            unit_ref,
            filing_fact_id AS parent_fact_id,
            numeric_value AS parent_value,
            0.0 AS child_weighted_sum,
            abs(numeric_value) AS absolute_difference,
            0.0 AS tolerance,
            0 AS child_count,
            CAST(
                to_json([
                    struct_pack(
                        filing_fact_id := filing_fact_id,
                        taxonomy := taxonomy,
                        concept := concept,
                        numeric_value := numeric_value
                    )
                ]) AS VARCHAR
            ) AS child_facts_json,
            'DQC_0015 subset: sign-constrained us-gaap concept reported with a negative numeric value. Full DQC member exclusions are not implemented in this SQL subset.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0015',
                        official_rule_url := '{DQC_0015_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'curated non-negative concept list only; no full DQC member exclusions',
                        observed_value := numeric_value
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM dqc_0015_violations
        UNION ALL
        SELECT
            sha256(
                concat_ws(
                    '|',
                    ?,
                    'dqc',
                    'DQC_0053',
                    security_id,
                    accession_number,
                    primary_document,
                    context_id,
                    filing_dimension_id
                )
            ) AS validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0053' AS rule_code,
            'error' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            dimension_taxonomy AS parent_taxonomy,
            dimension_concept AS parent_concept,
            context_id AS context_ref,
            NULL AS unit_ref,
            NULL AS parent_fact_id,
            NULL AS parent_value,
            NULL AS child_weighted_sum,
            NULL AS absolute_difference,
            0.0 AS tolerance,
            1 AS child_count,
            CAST(
                to_json([
                    struct_pack(
                        filing_dimension_id := filing_dimension_id,
                        axis := dimension_qname,
                        member := member_qname
                    )
                ]) AS VARCHAR
            ) AS child_facts_json,
            'DQC_0053 subset: explicit member appears on an excluded axis-member pair or on a direct local dimension edge marked usable=false. Full transitive dimension-domain-member validation is skipped.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0053',
                        official_rule_url := '{DQC_0053_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'direct usable=false xbrl_dimension_edges plus curated excluded pairs only',
                        axis := dimension_qname,
                        member := member_qname,
                        reason := reason
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM dqc_0053_violations
        """,
        [
            run_id,
            validation_run_id,
            run_id,
            run_id,
            validation_run_id,
            run_id,
        ],
    )


def _insert_pf2_s9_dqc_results(
    store: DuckDBStore,
    *,
    validation_run_id: str,
    run_id: str,
    tolerance: float,
) -> None:
    """Append PF2-S9 SQL-native DQC families without changing calc tolerance."""

    ifrs_non_negative_values = _sql_values(DQC_0080_IFRS_NON_NEGATIVE_CONCEPTS)
    deprecated_values = _sql_values(DQC_0018_DEPRECATED_CONCEPTS)
    allowed_pair_values = _sql_values(DQC_0041_ALLOWED_MEMBER_AXIS_PAIRS)
    excluded_pair_values = _sql_values(DQC_0053_EXCLUDED_MEMBER_AXIS_PAIRS)

    store.con.execute(
        f"""
        INSERT INTO xbrl_validation_results (
            validation_id,
            validation_run_id,
            rule_family,
            rule_code,
            severity,
            status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            role_uri,
            parent_taxonomy,
            parent_concept,
            context_ref,
            unit_ref,
            parent_fact_id,
            parent_value,
            child_weighted_sum,
            absolute_difference,
            tolerance,
            child_count,
            child_facts_json,
            message,
            dimensional_evidence_json,
            resolution_status,
            source_url,
            run_id,
            source_loaded_at
        )
        WITH grouped AS (
            SELECT
                security_id,
                cik,
                accession_number,
                form,
                filing_date,
                acceptance_datetime,
                primary_document,
                context_ref,
                unit_ref,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'Assets' THEN filing_fact_id END) AS assets_fact_id,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'Liabilities' THEN filing_fact_id END) AS liabilities_fact_id,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'StockholdersEquity' THEN filing_fact_id END) AS equity_fact_id,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'Assets' THEN numeric_value END) AS assets_value,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'Liabilities' THEN numeric_value END) AS liabilities_value,
                max(CASE WHEN taxonomy = 'us-gaap' AND concept = 'StockholdersEquity' THEN numeric_value END) AS equity_value,
                max(source_url) AS source_url,
                max(source_loaded_at) AS source_loaded_at
            FROM xbrl_filing_facts
            WHERE is_numeric
              AND taxonomy = 'us-gaap'
              AND concept IN ('Assets', 'Liabilities', 'StockholdersEquity')
            GROUP BY
                security_id, cik, accession_number, form, filing_date,
                acceptance_datetime, primary_document, context_ref, unit_ref
        ),
        violations AS (
            SELECT
                *,
                liabilities_value + equity_value AS expected_assets,
                abs(assets_value - (liabilities_value + equity_value)) AS diff
            FROM grouped
            WHERE assets_value IS NOT NULL
              AND liabilities_value IS NOT NULL
              AND equity_value IS NOT NULL
              AND abs(assets_value - (liabilities_value + equity_value)) > ?
        )
        SELECT
            sha256(concat_ws('|', ?, 'dqc', 'DQC_0004', security_id, accession_number, primary_document, context_ref, unit_ref)) AS validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0004' AS rule_code,
            'error' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            'us-gaap' AS parent_taxonomy,
            'Assets' AS parent_concept,
            context_ref,
            unit_ref,
            assets_fact_id AS parent_fact_id,
            assets_value AS parent_value,
            expected_assets AS child_weighted_sum,
            diff AS absolute_difference,
            ? AS tolerance,
            2 AS child_count,
            CAST(
                to_json([
                    struct_pack(filing_fact_id := liabilities_fact_id, taxonomy := 'us-gaap', concept := 'Liabilities', numeric_value := liabilities_value),
                    struct_pack(filing_fact_id := equity_fact_id, taxonomy := 'us-gaap', concept := 'StockholdersEquity', numeric_value := equity_value)
                ]) AS VARCHAR
            ) AS child_facts_json,
            'DQC_0004 subset: same-context Assets does not equal Liabilities plus StockholdersEquity within the unchanged absolute tolerance.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0004',
                        official_rule_url := '{DQC_0004_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'same context_ref and unit_ref only; no full statement table plugin semantics',
                        assets := assets_value,
                        liabilities_plus_equity := expected_assets
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM violations
        """,
        [tolerance, run_id, validation_run_id, tolerance, run_id],
    )

    store.con.execute(
        f"""
        INSERT INTO xbrl_validation_results (
            validation_id,
            validation_run_id,
            rule_family,
            rule_code,
            severity,
            status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            role_uri,
            parent_taxonomy,
            parent_concept,
            context_ref,
            unit_ref,
            parent_fact_id,
            parent_value,
            child_weighted_sum,
            absolute_difference,
            tolerance,
            child_count,
            child_facts_json,
            message,
            dimensional_evidence_json,
            resolution_status,
            source_url,
            run_id,
            source_loaded_at
        )
        WITH dqc_0080_concepts(taxonomy, concept) AS (
            VALUES
                    {ifrs_non_negative_values}
        ),
        dqc_0018_deprecated(taxonomy, concept, replacement_concept, reason) AS (
            VALUES
                    {deprecated_values}
        ),
        ifrs_violations AS (
            SELECT
                sha256(concat_ws('|', ?, 'dqc', 'DQC_0080', f.security_id, f.accession_number, f.primary_document, f.context_ref, f.filing_fact_id)) AS validation_id,
                f.*
            FROM xbrl_filing_facts f
            JOIN dqc_0080_concepts c
              ON coalesce(f.taxonomy, '') = c.taxonomy
             AND f.concept = c.concept
            WHERE f.is_numeric
              AND f.numeric_value < 0
        ),
        deprecated_violations AS (
            SELECT
                sha256(concat_ws('|', ?, 'dqc', 'DQC_0018', f.security_id, f.accession_number, f.primary_document, f.context_ref, f.filing_fact_id)) AS validation_id,
                f.*,
                d.replacement_concept,
                d.reason
            FROM xbrl_filing_facts f
            JOIN dqc_0018_deprecated d
              ON coalesce(f.taxonomy, '') = d.taxonomy
             AND f.concept = d.concept
        )
        SELECT
            validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0080' AS rule_code,
            'error' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            taxonomy AS parent_taxonomy,
            concept AS parent_concept,
            context_ref,
            unit_ref,
            filing_fact_id AS parent_fact_id,
            numeric_value AS parent_value,
            0.0 AS child_weighted_sum,
            abs(numeric_value) AS absolute_difference,
            0.0 AS tolerance,
            0 AS child_count,
            CAST(to_json([struct_pack(filing_fact_id := filing_fact_id, taxonomy := taxonomy, concept := concept, numeric_value := numeric_value)]) AS VARCHAR) AS child_facts_json,
            'DQC_0080 subset: sign-constrained IFRS concept reported with a negative numeric value.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0080',
                        official_rule_url := '{DQC_0080_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'curated IFRS non-negative concept list only',
                        observed_value := numeric_value
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM ifrs_violations
        UNION ALL
        SELECT
            validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0018' AS rule_code,
            'warning' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            taxonomy AS parent_taxonomy,
            concept AS parent_concept,
            context_ref,
            unit_ref,
            filing_fact_id AS parent_fact_id,
            numeric_value AS parent_value,
            NULL AS child_weighted_sum,
            NULL AS absolute_difference,
            0.0 AS tolerance,
            0 AS child_count,
            CAST(to_json([struct_pack(filing_fact_id := filing_fact_id, taxonomy := taxonomy, concept := concept, replacement_concept := replacement_concept)]) AS VARCHAR) AS child_facts_json,
            'DQC_0018 subset: deprecated taxonomy element is present; use the documented replacement concept where applicable.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0018',
                        official_rule_url := '{DQC_0018_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'curated deprecated concept list only; extensible-enumeration checks remain skipped',
                        replacement_concept := replacement_concept,
                        reason := reason
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM deprecated_violations
        """,
        [run_id, run_id, validation_run_id, run_id, validation_run_id, run_id],
    )

    store.con.execute(
        f"""
        INSERT INTO xbrl_validation_results (
            validation_id,
            validation_run_id,
            rule_family,
            rule_code,
            severity,
            status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            role_uri,
            parent_taxonomy,
            parent_concept,
            context_ref,
            unit_ref,
            parent_fact_id,
            parent_value,
            child_weighted_sum,
            absolute_difference,
            tolerance,
            child_count,
            child_facts_json,
            message,
            dimensional_evidence_json,
            resolution_status,
            source_url,
            run_id,
            source_loaded_at
        )
        WITH allowed_pairs(axis_qname, member_qname) AS (
            VALUES
                    {allowed_pair_values}
        ),
        excluded_pairs(axis_qname, member_qname, reason) AS (
            VALUES
                    {excluded_pair_values}
        ),
        violations AS (
            SELECT d.*
            FROM xbrl_filing_dimensions d
            WHERE d.member_kind = 'explicit'
              AND d.dimension_concept IS NOT NULL
              AND d.member_concept IS NOT NULL
              AND NOT EXISTS (
                    SELECT 1
                    FROM xbrl_dimension_edges e
                    WHERE e.source_concept = d.dimension_concept
                      AND e.target_concept = d.member_concept
                      AND coalesce(e.source_taxonomy, d.dimension_taxonomy, '') = coalesce(d.dimension_taxonomy, '')
                      AND coalesce(e.target_taxonomy, d.member_taxonomy, '') = coalesce(d.member_taxonomy, '')
                      AND e.usable = true
              )
              AND NOT EXISTS (
                    SELECT 1
                    FROM xbrl_dimension_edges e
                    WHERE e.source_concept = d.dimension_concept
                      AND e.target_concept = d.member_concept
                      AND coalesce(e.source_taxonomy, d.dimension_taxonomy, '') = coalesce(d.dimension_taxonomy, '')
                      AND coalesce(e.target_taxonomy, d.member_taxonomy, '') = coalesce(d.member_taxonomy, '')
                      AND e.usable = false
              )
              AND NOT EXISTS (
                    SELECT 1
                    FROM allowed_pairs p
                    WHERE p.axis_qname = d.dimension_qname
                      AND p.member_qname = d.member_qname
              )
              AND NOT EXISTS (
                    SELECT 1
                    FROM excluded_pairs p
                    WHERE p.axis_qname = d.dimension_qname
                      AND p.member_qname = d.member_qname
              )
        )
        SELECT
            sha256(concat_ws('|', ?, 'dqc', 'DQC_0041', security_id, accession_number, primary_document, context_id, filing_dimension_id)) AS validation_id,
            ? AS validation_run_id,
            'dqc' AS rule_family,
            'DQC_0041' AS rule_code,
            'error' AS severity,
            'failed' AS status,
            security_id,
            cik,
            accession_number,
            form,
            filing_date,
            acceptance_datetime,
            primary_document,
            NULL AS role_uri,
            dimension_taxonomy AS parent_taxonomy,
            dimension_concept AS parent_concept,
            context_id AS context_ref,
            NULL AS unit_ref,
            NULL AS parent_fact_id,
            NULL AS parent_value,
            NULL AS child_weighted_sum,
            NULL AS absolute_difference,
            0.0 AS tolerance,
            1 AS child_count,
            CAST(
                to_json([
                    struct_pack(
                        filing_dimension_id := filing_dimension_id,
                        axis := dimension_qname,
                        member := member_qname
                    )
                ]) AS VARCHAR
            ) AS child_facts_json,
            'DQC_0041 subset: explicit member is not directly allowed for the reported axis by local dimension edges or the curated allowed-pair list.' AS message,
            CAST(
                to_json(
                    struct_pack(
                        dqc_rule := 'DQC_0041',
                        official_rule_url := '{DQC_0041_URL}',
                        guidance_url := '{DQC_RULES_GUIDANCE_URL}',
                        subset := 'direct usable=true local dimension edge or curated allowed-pair list only; transitive domain-member closure is skipped',
                        axis := dimension_qname,
                        member := member_qname
                    )
                ) AS VARCHAR
            ) AS dimensional_evidence_json,
            'genuine_error' AS resolution_status,
            source_url,
            ? AS run_id,
            coalesce(source_loaded_at, now()) AS source_loaded_at
        FROM violations
        """,
        [run_id, validation_run_id, run_id],
    )


class XbrlValidationDataset(Dataset):
    dataset_id = "xbrl_validation"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: XbrlValidationOptions) -> DatasetLoadResult:
        rows = refresh_xbrl_validation_results(store, options)
        split = store.con.execute(
            """
            SELECT
                count(*) FILTER (WHERE rule_family = 'calculation_linkbase')::INTEGER
                    AS calculation_rows,
                count(*) FILTER (
                    WHERE rule_family = 'calculation_linkbase'
                      AND resolution_status = 'resolved_dimensional_artifact'
                )::INTEGER AS resolved_dimensional_artifacts,
                count(*) FILTER (
                    WHERE rule_family = 'calculation_linkbase'
                      AND resolution_status = 'genuine_error'
                )::INTEGER AS genuine_calculation_errors,
                count(*) FILTER (WHERE rule_family = 'dqc')::INTEGER AS dqc_rows,
                count(*) FILTER (
                    WHERE rule_family = 'dqc'
                      AND status = 'failed'
                )::INTEGER AS dqc_failures
            FROM xbrl_validation_results
            """
        ).fetchone()
        details = {
            "rows_loaded": rows,
            "absolute_tolerance": options.absolute_tolerance,
            "calculation_rows": int(split[0]),
            "resolved_dimensional_artifacts": int(split[1]),
            "genuine_calculation_errors": int(split[2]),
            "dqc_rows": int(split[3]),
            "dqc_failures": int(split[4]),
            "dqc_resolution_status": "failed DQC subset rows are emitted only for violations and use genuine_error",
        }
        failed = details["genuine_calculation_errors"]
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_validation_results",
            check_name="xbrl_calculation_linkbase_failures",
            status="passed" if failed == 0 else "failed",
            observed_value=float(failed),
            threshold_value=0.0,
            details=details,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=SOURCE_NAME,
            details=details,
        )
