from __future__ import annotations

import uuid
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check


SOURCE_NAME = "XBRL validation"


@dataclass(frozen=True)
class XbrlValidationOptions:
    absolute_tolerance: float = 1.0
    run_id: str | None = None


def refresh_xbrl_validation_results(
    store: DuckDBStore,
    options: XbrlValidationOptions | None = None,
) -> int:
    """Validate filing facts against public calculation-linkbase relationships.

    This is the SQL-native v1 validator. It compares same-filing, same-context,
    same-unit numeric parent facts to weighted child sums from the latest loaded
    calculation linkbase arc for each role/parent/child combination. A future
    Arelle/DQC sidecar can append additional rule families to the same table.
    """

    options = options or XbrlValidationOptions()
    validation_run_id = str(uuid.uuid4())
    run_id = options.run_id or validation_run_id
    tolerance = float(options.absolute_tolerance)

    with store.transaction():
        store.con.execute("DELETE FROM xbrl_validation_results WHERE rule_family = 'calculation_linkbase'")
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
            calc_groups AS (
                SELECT
                    parent.filing_fact_id AS parent_fact_id,
                    parent.security_id,
                    parent.cik,
                    parent.accession_number,
                    parent.form,
                    parent.filing_date,
                    parent.acceptance_datetime,
                    parent.primary_document,
                    edge.role_uri,
                    parent.taxonomy AS parent_taxonomy,
                    parent.concept AS parent_concept,
                    parent.context_ref,
                    parent.unit_ref,
                    parent.numeric_value AS parent_value,
                    sum(child.numeric_value * edge.weight) AS child_weighted_sum,
                    abs(parent.numeric_value - sum(child.numeric_value * edge.weight)) AS absolute_difference,
                    count(*)::INTEGER AS child_count,
                    CAST(
                        to_json(
                            list(
                                struct_pack(
                                    filing_fact_id := child.filing_fact_id,
                                    taxonomy := child.taxonomy,
                                    concept := child.concept,
                                    weight := edge.weight,
                                    numeric_value := child.numeric_value
                                )
                                ORDER BY edge.order_value NULLS LAST, child.concept, child.filing_fact_id
                            )
                        ) AS VARCHAR
                    ) AS child_facts_json,
                    parent.source_url,
                    max(coalesce(child.source_loaded_at, parent.source_loaded_at)) AS source_loaded_at
                FROM xbrl_filing_facts parent
                JOIN latest_edges edge
                  ON edge.parent_concept = parent.concept
                 AND (
                        edge.parent_taxonomy IS NULL
                     OR parent.taxonomy IS NULL
                     OR edge.parent_taxonomy = parent.taxonomy
                 )
                JOIN xbrl_filing_facts child
                  ON child.security_id = parent.security_id
                 AND child.accession_number = parent.accession_number
                 AND child.primary_document = parent.primary_document
                 AND child.context_ref = parent.context_ref
                 AND coalesce(child.unit_ref, '') = coalesce(parent.unit_ref, '')
                 AND child.concept = edge.child_concept
                 AND (
                        edge.child_taxonomy IS NULL
                     OR child.taxonomy IS NULL
                     OR edge.child_taxonomy = child.taxonomy
                 )
                 AND child.is_numeric
                 AND child.numeric_value IS NOT NULL
                WHERE parent.is_numeric
                  AND parent.numeric_value IS NOT NULL
                GROUP BY
                    parent.filing_fact_id,
                    parent.security_id,
                    parent.cik,
                    parent.accession_number,
                    parent.form,
                    parent.filing_date,
                    parent.acceptance_datetime,
                    parent.primary_document,
                    edge.role_uri,
                    parent.taxonomy,
                    parent.concept,
                    parent.context_ref,
                    parent.unit_ref,
                    parent.numeric_value,
                    parent.source_url
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
                    WHEN absolute_difference <= ? THEN 'passed'
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
                child_count,
                child_facts_json,
                CASE
                    WHEN absolute_difference <= ? THEN 'Calculation linkbase sum matched within tolerance.'
                    ELSE 'Calculation linkbase sum did not match weighted child facts.'
                END AS message,
                source_url,
                ? AS run_id,
                coalesce(source_loaded_at, now()) AS source_loaded_at
            FROM calc_groups
            """,
            [validation_run_id, validation_run_id, tolerance, tolerance, tolerance, run_id],
        )

    return int(
        store.con.execute(
            "SELECT count(*) FROM xbrl_validation_results WHERE validation_run_id = ?",
            [validation_run_id],
        ).fetchone()[0]
    )


class XbrlValidationDataset(Dataset):
    dataset_id = "xbrl_validation"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: XbrlValidationOptions) -> DatasetLoadResult:
        rows = refresh_xbrl_validation_results(store, options)
        failed = int(
            store.con.execute(
                """
                SELECT count(*)
                FROM xbrl_validation_results
                WHERE rule_family = 'calculation_linkbase'
                  AND status = 'failed'
                """
            ).fetchone()[0]
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="xbrl_validation_results",
            check_name="xbrl_calculation_linkbase_failures",
            status="passed" if failed == 0 else "failed",
            observed_value=float(failed),
            threshold_value=0.0,
            details={"rows_loaded": rows, "absolute_tolerance": options.absolute_tolerance},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=SOURCE_NAME,
            details={"failed_calculation_rows": failed, "absolute_tolerance": options.absolute_tolerance},
        )
