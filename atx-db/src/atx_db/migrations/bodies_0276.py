"""Governed issuer-extension mappings for filing-context reconciliation."""

from __future__ import annotations

import hashlib
import json

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _extension_mapping_id(cik: str, taxonomy: str, concept: str, item_id: int) -> str:
    return hashlib.sha256(
        f"fundamental-extension-map|{cik}|{taxonomy}|{concept}|{item_id}".encode()
    ).hexdigest()


def _fundamental_extension_mapping_release(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_extension_concept_map (
            extension_mapping_id VARCHAR PRIMARY KEY,
            cik VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            item_id INTEGER NOT NULL,
            mapping_status VARCHAR NOT NULL,
            confidence DOUBLE NOT NULL,
            sign_multiplier DOUBLE NOT NULL DEFAULT 1.0,
            evidence_type VARCHAR NOT NULL,
            evidence_json VARCHAR NOT NULL,
            citation VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            valid_from DATE NOT NULL DEFAULT DATE '1900-01-01',
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            reviewed_by VARCHAR,
            reviewed_at TIMESTAMP,
            is_active BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            UNIQUE (cik,taxonomy,concept,item_id)
        )
        """
    )
    cik = "0000004904"
    taxonomy = "aep"
    concept = "RedeemableNoncontrollingInterestEquityPerformanceSharesCarryingAmount"
    citation = (
        "https://www.sec.gov/Archives/edgar/data/4904/"
        "000000490426000034/aep-20260331.htm"
    )
    evidence = json.dumps(
        {
            "accession_number": "0000004904-26-000034",
            "contexts": ["c-47", "c-58"],
            "canonical_role": "temporary_equity",
            "equation": "LiabilitiesAndStockholdersEquity = Liabilities + EquityIncludingNCI + TemporaryEquity",
            "observed_values": [38_000_000.0, 51_000_000.0],
        },
        separators=(",", ":"),
        sort_keys=True,
    )
    conn.execute(
        """
        DELETE FROM fundamental_extension_concept_map
        WHERE cik=? AND taxonomy=? AND concept=? AND item_id=1224
        """,
        [cik, taxonomy, concept],
    )
    conn.execute(
        """
        INSERT INTO fundamental_extension_concept_map (
            extension_mapping_id,cik,taxonomy,concept,item_id,mapping_status,
            confidence,sign_multiplier,evidence_type,evidence_json,citation,
            description,valid_from,valid_to,as_of_date,available_at,
            reviewed_by,reviewed_at,is_active,run_id,source_loaded_at,updated_at
        ) VALUES (
            ?,?,?,?,1224,'approved',1.0,1.0,'same_context_equation',?,?,?,
            DATE '1900-01-01',NULL,DATE '2026-05-05',
            TIMESTAMP '2026-05-05 14:23:25','atx-systematic-review',
            TIMESTAMP '2026-08-14 00:00:00',true,'migration-0276',now(),now()
        )
        """,
        [
            _extension_mapping_id(cik, taxonomy, concept, 1224),
            cik,
            taxonomy,
            concept,
            evidence,
            citation,
            (
                "CIK-scoped AEP extension representing redeemable noncontrolling "
                "performance-share equity; approved only where the fact shares the "
                "verified balance-sheet context and closes the optional DQC 0004 residual."
            ),
        ],
    )
    conn.execute(
        """
        DELETE FROM fundamental_statement_map
        WHERE taxonomy='us-gaap'
          AND concept='RedeemableNoncontrollingInterestEquityPerformanceSharesCarryingAmount'
        """
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_reconciliation_contextual AS
        WITH extension_candidates AS (
            SELECT
                base.reconciliation_id,
                mapping.extension_mapping_id,
                mapping.item_id,
                mapping.confidence,
                mapping.evidence_type,
                mapping.evidence_json AS mapping_evidence_json,
                mapping.citation AS mapping_citation,
                mapping.description AS mapping_description,
                term.term_position,
                term.term_role,
                term.weight,
                fact.filing_fact_id,
                fact.accession_number,
                fact.primary_document,
                fact.taxonomy,
                fact.concept,
                fact.qname,
                fact.numeric_value * mapping.sign_multiplier AS mapped_value,
                fact.unit_ref,
                fact.decimals,
                fact.acceptance_datetime,
                CASE
                    WHEN term.term_role='lhs'
                        THEN fact.numeric_value * mapping.sign_multiplier * term.weight
                    ELSE -fact.numeric_value * mapping.sign_multiplier * term.weight
                END AS equation_effect,
                row_number() OVER (
                    PARTITION BY base.reconciliation_id
                    ORDER BY mapping.confidence DESC,mapping.available_at,
                             mapping.extension_mapping_id,fact.filing_fact_id
                ) AS candidate_rank
            FROM v_fundamental_reconciliation base
            JOIN fundamental_reconciliation_rule_term term
              ON term.rule_id=base.rule_id
             AND NOT term.is_required
            JOIN fundamental_extension_concept_map mapping
              ON mapping.cik=base.cik
             AND mapping.item_id=term.item_id
             AND mapping.mapping_status='approved'
             AND mapping.is_active
             AND mapping.valid_from <= base.period_end
             AND coalesce(mapping.valid_to,DATE '9999-12-31') > base.period_end
             AND mapping.available_at <= base.available_at
            JOIN xbrl_filing_facts fact
              ON fact.filing_context_id=base.verified_filing_context_id
             AND fact.taxonomy=mapping.taxonomy
             AND fact.concept=mapping.concept
             AND fact.is_numeric
             AND fact.numeric_value IS NOT NULL
             AND fact.acceptance_datetime <= base.available_at
            WHERE base.status='mismatch'
              AND base.context_verification_status='verified_same_context'
              AND abs(
                    base.residual
                    + CASE
                        WHEN term.term_role='lhs'
                            THEN fact.numeric_value * mapping.sign_multiplier * term.weight
                        ELSE -fact.numeric_value * mapping.sign_multiplier * term.weight
                      END
                  ) <= base.tolerance
        ),
        picked_extension AS (
            SELECT * EXCLUDE (candidate_rank)
            FROM extension_candidates
            WHERE candidate_rank=1
        )
        SELECT
            base.* EXCLUDE (
                lhs_value,rhs_value,residual,absolute_difference,residual_percent,
                status,context_verification_status,mismatch_reason,is_hard_failure
            ),
            base.lhs_value
                + CASE WHEN extension.term_role='lhs' THEN extension.mapped_value * extension.weight ELSE 0 END
                AS lhs_value,
            base.rhs_value
                + CASE WHEN extension.term_role='rhs' THEN extension.mapped_value * extension.weight ELSE 0 END
                AS rhs_value,
            base.residual + coalesce(extension.equation_effect,0) AS residual,
            abs(base.residual + coalesce(extension.equation_effect,0)) AS absolute_difference,
            CASE
                WHEN greatest(
                    abs(base.lhs_value + CASE WHEN extension.term_role='lhs'
                        THEN extension.mapped_value * extension.weight ELSE 0 END),
                    abs(base.rhs_value + CASE WHEN extension.term_role='rhs'
                        THEN extension.mapped_value * extension.weight ELSE 0 END)
                )=0 THEN NULL
                ELSE (base.residual + coalesce(extension.equation_effect,0)) / greatest(
                    abs(base.lhs_value + CASE WHEN extension.term_role='lhs'
                        THEN extension.mapped_value * extension.weight ELSE 0 END),
                    abs(base.rhs_value + CASE WHEN extension.term_role='rhs'
                        THEN extension.mapped_value * extension.weight ELSE 0 END)
                )
            END AS residual_percent,
            CASE WHEN extension.extension_mapping_id IS NOT NULL
                THEN 'reconciled' ELSE base.status END AS status,
            CASE WHEN extension.extension_mapping_id IS NOT NULL
                THEN 'verified_same_context_with_extension_map'
                ELSE base.context_verification_status END AS context_verification_status,
            CASE WHEN extension.extension_mapping_id IS NOT NULL
                THEN 'within_tolerance_extension_mapped'
                ELSE base.mismatch_reason END AS mismatch_reason,
            CASE WHEN extension.extension_mapping_id IS NOT NULL
                THEN false ELSE base.is_hard_failure END AS is_hard_failure,
            extension.extension_mapping_id IS NOT NULL AS extension_mapping_applied,
            CASE
                WHEN extension.extension_mapping_id IS NULL THEN '[]'
                ELSE CAST(to_json(list_value(struct_pack(
                    extension_mapping_id := extension.extension_mapping_id,
                    item_id := extension.item_id,
                    term_position := extension.term_position,
                    term_role := extension.term_role,
                    weight := extension.weight,
                    filing_fact_id := extension.filing_fact_id,
                    accession_number := extension.accession_number,
                    primary_document := extension.primary_document,
                    taxonomy := extension.taxonomy,
                    concept := extension.concept,
                    qname := extension.qname,
                    mapped_value := extension.mapped_value,
                    unit_ref := extension.unit_ref,
                    decimals := extension.decimals,
                    acceptance_datetime := extension.acceptance_datetime,
                    confidence := extension.confidence,
                    evidence_type := extension.evidence_type,
                    mapping_evidence_json := extension.mapping_evidence_json,
                    citation := extension.mapping_citation,
                    description := extension.mapping_description
                ))) AS VARCHAR)
            END AS extension_inputs_json
        FROM v_fundamental_reconciliation base
        LEFT JOIN picked_extension extension
          ON extension.reconciliation_id=base.reconciliation_id
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_extension_map_lookup ON fundamental_extension_concept_map(cik,item_id,mapping_status,is_active)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_extension_map_concept ON fundamental_extension_concept_map(taxonomy,concept,cik)",
    ):
        conn.execute(statement)

    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "fundamental_extension_concept_map",
                "reference",
                "fundamental_extension_concept_mapping",
                "cik,taxonomy,concept,item_id",
                "Governed issuer-scoped mappings from XBRL extension concepts to canonical items.",
                '["extension_mapping_id"]',
                "available_at gates when an approved mapping may affect a PIT result; valid_from/valid_to scope economic periods.",
            ),
            (
                "v_fundamental_reconciliation_contextual",
                "api",
                "fundamental_reconciliation",
                "security_id,rule_id,basis,period_end,available_at",
                "Customer reconciliation results after governed same-context issuer-extension completion.",
                '["reconciliation_id"]',
                "The numerical base remains v_fundamental_reconciliation; approved mappings apply only when visible and exact-context residual-closing.",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        (
            "fundamental_extension_concept_map",
            "v_fundamental_reconciliation_contextual",
        ),
    )
    _seed_public_contract(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=276,
        name="governed_fundamental_extension_mapping",
        up=_fundamental_extension_mapping_release,
    )
]
