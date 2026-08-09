from __future__ import annotations

from ..lake import DEFAULT_EXPORT_OBJECTS


# security_identifier_history.internal_cusip is internal-only matching support
# (see the field_catalog description seeded in migration 0079) and must never
# appear in a lake-exported / public / catalogued-public object. This is the
# single column the boundary protects today; the export-scan check below
# fails if any DEFAULT_EXPORT_OBJECTS member ever carries a column by this
# name.
INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN = "internal_cusip"
DEFAULT_VALUATION_STALE_GAP_DAYS = 5


def _export_scan_internal_cusip_sql(export_objects: tuple[str, ...]) -> str:
    """Build the export-scan SQL: count DEFAULT_EXPORT_OBJECTS columns named
    ``internal_cusip``. Metadata-only (``duckdb_columns()``), so it is safe to
    run even against a warehouse where some listed export objects do not
    exist yet (an empty/partial test DB) -- the count is simply 0 for those.
    """
    if not export_objects:
        return "SELECT 0.0"
    placeholders = ", ".join(f"'{name}'" for name in export_objects)
    return f"""
        SELECT count(*)::DOUBLE
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name IN ({placeholders})
          AND column_name = '{INTERNAL_ONLY_EXPORT_FORBIDDEN_COLUMN}'
    """
