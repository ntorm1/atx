"""Legacy XBRL-XML filing-instance lineage."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

_INSTANCE_TABLES = (
    "xbrl_filing_contexts",
    "xbrl_filing_dimensions",
    "xbrl_filing_facts",
)


def _legacy_xbrl_instance_lineage(conn: duckdb.DuckDBPyConnection) -> None:
    for table_name in _INSTANCE_TABLES:
        conn.execute(
            f"""
            ALTER TABLE {table_name}
                ADD COLUMN IF NOT EXISTS filing_primary_document VARCHAR;
            ALTER TABLE {table_name}
                ADD COLUMN IF NOT EXISTS instance_document VARCHAR;
            ALTER TABLE {table_name}
                ADD COLUMN IF NOT EXISTS instance_format VARCHAR;
            """
        )
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_contexts_instance
            ON xbrl_filing_contexts(security_id,accession_number,instance_document);
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_facts_instance
            ON xbrl_filing_facts(security_id,accession_number,instance_document);
        """
    )
    for table_name in _INSTANCE_TABLES:
        conn.execute(
            f"""
            UPDATE {table_name}
            SET filing_primary_document=coalesce(filing_primary_document,primary_document),
                instance_document=coalesce(instance_document,primary_document),
                instance_format=coalesce(instance_format,'inline_xbrl')
            WHERE filing_primary_document IS NULL
               OR instance_document IS NULL
               OR instance_format IS NULL
            """
        )
    conn.execute(
        """
        UPDATE table_catalog
        SET description='XBRL contexts parsed from SEC inline XBRL or legacy EX-101.INS XML instances.',
            pit_notes='filing_primary_document preserves the submission document; instance_document and instance_format identify the exact fact-bearing source.',
            updated_at=now()
        WHERE table_name='xbrl_filing_contexts';

        UPDATE table_catalog
        SET description='Explicit and typed dimensions parsed from SEC inline XBRL or legacy XBRL-XML instances.',
            pit_notes='Instance lineage distinguishes the filing primary document from a legacy EX-101.INS attachment.',
            updated_at=now()
        WHERE table_name='xbrl_filing_dimensions';

        UPDATE table_catalog
        SET description='Facts parsed from SEC inline XBRL or legacy XBRL-XML filing instances.',
            pit_notes='Instance lineage and filing acceptance timestamps preserve auditable point-in-time provenance.',
            updated_at=now()
        WHERE table_name='xbrl_filing_facts';
        """
    )
    _catalog_fields_for_tables(conn, _INSTANCE_TABLES)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=279,
        name="legacy_xbrl_instance_lineage",
        up=_legacy_xbrl_instance_lineage,
    )
]
