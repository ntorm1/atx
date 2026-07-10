"""S1-0: schema tests for the canonical item dimension.

Covers migrations 0061 (schema + table_catalog/field_catalog seed) and 0062
(indexes) for fundamental_item, fundamental_item_alias, and
fundamental_item_vendor_map. This task lays schema only; seed data lands in
S1-1, so these tests assert structure, catalog rows, idempotency, and the
fundamental_item.item_id uniqueness constraint, not row counts.
"""

from __future__ import annotations

import csv
from datetime import date, datetime
import inspect
from pathlib import Path
import re
import tomllib

import duckdb
import pytest


NEW_TABLES = (
    "fundamental_item",
    "fundamental_item_alias",
    "fundamental_item_vendor_map",
)

EXPECTED_COLUMNS = {
    "fundamental_item": (
        "item_id",
        "canonical_code",
        "statement",
        "section",
        "data_type",
        "unit_type",
        "sign_convention",
        "is_derived",
        "definition",
        "citation",
    ),
    "fundamental_item_alias": (
        "item_id",
        "alias_scheme",
        "alias_code",
        "coalesce_priority",
        "valid_from",
        "valid_to",
    ),
    "fundamental_item_vendor_map": (
        "item_id",
        "vendor",
        "vendor_field",
        "sign_note",
    ),
}

SEED_COLUMNS = (
    "item_id",
    "canonical_code",
    "statement",
    "section",
    "data_type",
    "unit_type",
    "sign_convention",
    "is_derived",
    "definition",
    "citation",
    "alias_scheme",
    "alias_code",
    "coalesce_priority",
    "valid_from",
    "valid_to",
    "vendor",
    "vendor_field",
    "sign_note",
)

SEED_PATH = Path(__file__).resolve().parents[1] / "seeds" / "fundamental_items.csv"

# S1-1, PF2-S5, and PF3-S5 source sections define 234 canonical item_ids. The original 460-500
# acceptance count was a plan defect; gap IDs must not be synthesized. 1045-1050 are the PF3-S5
# share-count items (public float, treasury, Class A-D); 1044 remains a reserved gap id.
AUTHORIZED_ITEM_IDS = (
    set(range(1001, 1044))
    | set(range(1045, 1051))
    | set(range(1101, 1120))
    | set(range(1201, 1224))
    | set(range(1301, 1326))
    | set(range(1401, 1428))
    | set(range(1501, 1516))
    | set(range(1601, 1611))
    | set(range(1701, 1713))
    | set(range(1801, 1806))
    | set(range(1901, 1906))
    | set(range(2001, 2045))
)

UNAUTHORIZED_GAP_ITEM_IDS = {1044, 1099, 1224, 1326}

EXPRESSION_VENDOR_FIELDS = {
    (1004, "compustat", "revt - cogs"),
    (1010, "compustat", "cogs+xsga+xrd+dp"),
    (1115, "compustat", "intan - gdwl"),
    (1205, "bloomberg", "BS_ST_BORROW + BS_CUR_PORTION_LT_DEBT"),
    (1208, "compustat", "dlc + dltt"),
    (1325, "compustat", "oancf - capx"),
}

EXPRESSION_MARKERS = (" + ", " - ", " * ", " / ", "(", ")")


def _columns(con: duckdb.DuckDBPyConnection, table_name: str) -> list[str]:
    rows = con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [table_name],
    ).fetchall()
    return [row[0] for row in rows]


def _table_exists(con: duckdb.DuckDBPyConnection, table_name: str) -> bool:
    count = con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ?",
        [table_name],
    ).fetchone()[0]
    return count == 1


def test_item_registry_tables_exist(tmp_store):
    """All three item-registry tables exist after apply_pending_migrations."""
    for table_name in NEW_TABLES:
        assert _table_exists(tmp_store.con, table_name), f"{table_name} missing"


def test_item_registry_table_columns(tmp_store):
    """Each table has exactly the columns specified in the S1-0 brief."""
    for table_name, expected_cols in EXPECTED_COLUMNS.items():
        assert _columns(tmp_store.con, table_name) == list(expected_cols)


def test_table_catalog_has_three_new_rows(tmp_store):
    """table_catalog gains exactly one row per new table."""
    rows = tmp_store.con.execute(
        "SELECT table_name FROM table_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    catalogued = {row[0] for row in rows}
    assert catalogued == set(NEW_TABLES), (
        f"Expected table_catalog rows for {sorted(NEW_TABLES)}, found {sorted(catalogued)}"
    )


def test_field_catalog_has_one_row_per_column(tmp_store):
    """field_catalog carries one row per column for each new table."""
    for table_name, expected_cols in EXPECTED_COLUMNS.items():
        rows = tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = ?",
            [table_name],
        ).fetchall()
        catalogued = {row[0] for row in rows}
        expected = set(expected_cols)
        assert catalogued == expected, (
            f"{table_name} field_catalog mismatch; "
            f"missing={sorted(expected - catalogued)}, "
            f"extra={sorted(catalogued - expected)}"
        )


def test_fundamental_item_item_id_unique(tmp_store):
    """fundamental_item.item_id rejects duplicate inserts (PK/unique constraint)."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item (item_id, canonical_code)
        VALUES (999001, 'test_metric_a')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item (item_id, canonical_code)
            VALUES (999001, 'test_metric_b')
            """
        )


def test_fundamental_item_canonical_code_unique(tmp_store):
    """fundamental_item.canonical_code rejects duplicate canonical items."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item (item_id, canonical_code)
        VALUES (999101, 'test_unique_metric')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item (item_id, canonical_code)
            VALUES (999102, 'test_unique_metric')
            """
        )


def test_fundamental_item_alias_rejects_exact_duplicate_null_window(tmp_store):
    """Alias uniqueness treats NULL validity windows as equal for duplicate rows."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item_alias (
            item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
        )
        VALUES (999201, 'us-gaap', 'TestAliasConcept', 10, NULL, NULL)
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item_alias (
                item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
            )
            VALUES (999201, 'us-gaap', 'TestAliasConcept', 10, NULL, NULL)
            """
        )


def test_fundamental_item_vendor_map_rejects_duplicate_key(tmp_store):
    """Vendor map uniqueness rejects duplicate item/vendor/field rows."""
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO fundamental_item_vendor_map (
            item_id, vendor, vendor_field, sign_note
        )
        VALUES (999301, 'compustat', 'test_field', 'same sign')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO fundamental_item_vendor_map (
                item_id, vendor, vendor_field, sign_note
            )
            VALUES (999301, 'compustat', 'test_field', 'same sign')
            """
        )


def test_item_registry_indexes_exist(tmp_store):
    """0062 creates lookup and uniqueness indexes for the registry."""
    rows = tmp_store.con.execute(
        "SELECT index_name, table_name FROM duckdb_indexes() WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    index_names = {row[0] for row in rows}
    assert "idx_fundamental_item_alias_lookup" in index_names
    assert "idx_fundamental_item_canonical" in index_names
    assert "idx_fundamental_item_alias_unique" in index_names
    assert "idx_fundamental_item_vendor_map_unique" in index_names


def test_migrations_0061_and_0062_recorded(tmp_store):
    """schema_migrations records versions 61 and 62 after bootstrap."""
    rows = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    versions = {row[0] for row in rows}
    assert 61 in versions, f"Migration 0061 not recorded; found: {sorted(versions)}"
    assert 62 in versions, f"Migration 0062 not recorded; found: {sorted(versions)}"


def test_apply_pending_migrations_is_idempotent(tmp_store):
    """Re-running apply_pending_migrations after bootstrap is a no-op."""
    from db.migrations import apply_pending_migrations

    result = apply_pending_migrations(tmp_store.con)
    assert result == [], f"Expected [] (no-op) but got {result}"


def test_item_registry_migration_bodies_are_idempotent(tmp_store):
    """Directly re-running 0061/0062 helper bodies is safe and does not duplicate catalog."""
    from db.migrations import (
        _fundamental_item_registry_indexes,
        _fundamental_item_registry_schema,
    )

    con = tmp_store.con
    _fundamental_item_registry_schema(con)
    _fundamental_item_registry_indexes(con)

    table_count = con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchone()[0]
    field_count = con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchone()[0]
    assert table_count == len(NEW_TABLES)
    assert field_count == sum(len(cols) for cols in EXPECTED_COLUMNS.values())


def test_fundamental_item_seed_csv_is_stdlib_parseable():
    """The committed seed is a deterministic CSV parsed with stdlib csv."""
    with SEED_PATH.open(newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))

    assert rows, "fundamental_items.csv is empty or missing data rows"
    assert tuple(rows[0].keys()) == SEED_COLUMNS
    seeded_item_ids = {int(row["item_id"]) for row in rows}
    assert seeded_item_ids == AUTHORIZED_ITEM_IDS
    assert len(seeded_item_ids) == 234
    assert seeded_item_ids.isdisjoint(UNAUTHORIZED_GAP_ITEM_IDS)
    assert all("ifrs-full:" not in row["alias_code"] for row in rows)
    assert all("ifrs-full:" not in row["vendor_field"] for row in rows)
    assert all(row["alias_code"] != "[unverified]" for row in rows)
    assert all(row["vendor_field"] != "[unverified]" for row in rows)
    assert all("[" not in row["alias_code"] and "]" not in row["alias_code"] for row in rows)
    assert all("[" not in row["vendor_field"] and "]" not in row["vendor_field"] for row in rows)
    assert all(
        not any(marker in row["vendor_field"] for marker in EXPRESSION_MARKERS)
        for row in rows
    )
    assert all(
        re.search(r"[A-Za-z0-9_][+\-*/][A-Za-z0-9_]", row["vendor_field"]) is None
        for row in rows
    )
    vendor_fields = {
        (int(row["item_id"]), row["vendor"], row["vendor_field"])
        for row in rows
        if row["vendor_field"]
    }
    assert vendor_fields.isdisjoint(EXPRESSION_VENDOR_FIELDS)


def _write_seed_csv(tmp_path: Path, rows: list[list[str]], *, header: list[str] | None = None) -> Path:
    seed_path = tmp_path / "fundamental_items.csv"
    with seed_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(header or SEED_COLUMNS)
        writer.writerows(rows)
    return seed_path


def _valid_seed_values(**overrides: str) -> list[str]:
    values = {
        "item_id": "1001",
        "canonical_code": "revenue",
        "statement": "income",
        "section": "income_statement",
        "data_type": "duration",
        "unit_type": "monetary",
        "sign_convention": "positive",
        "is_derived": "false",
        "definition": "Revenue",
        "citation": "test source",
        "alias_scheme": "",
        "alias_code": "",
        "coalesce_priority": "",
        "valid_from": "1900-01-01",
        "valid_to": "",
        "vendor": "compustat",
        "vendor_field": "revt",
        "sign_note": "",
    }
    values.update(overrides)
    return [values[column] for column in SEED_COLUMNS]


def _seed_row_for_registry(**overrides):
    from db.item_registry import FundamentalItemSeedRow

    values = {
        "item_id": 1001,
        "canonical_code": "revenue",
        "statement": "income",
        "section": "income_statement",
        "data_type": "duration",
        "unit_type": "monetary",
        "sign_convention": "positive",
        "is_derived": False,
        "definition": "Revenue",
        "citation": "test source",
        "alias_scheme": "us-gaap",
        "alias_code": "RevenueFromContractWithCustomerExcludingAssessedTax",
        "coalesce_priority": 10,
        "valid_from": "1900-01-01",
        "valid_to": None,
        "vendor": None,
        "vendor_field": None,
        "sign_note": None,
    }
    values.update(overrides)
    return FundamentalItemSeedRow(**values)


@pytest.mark.parametrize(
    ("row", "message"),
    [
        (_valid_seed_values() + ["extra"], "extra CSV fields"),
        (_valid_seed_values(is_derived="maybe"), "invalid is_derived"),
        (_valid_seed_values(alias_scheme="us-gaap", alias_code=""), "partial alias"),
        (_valid_seed_values(vendor="", vendor_field="revt"), "partial vendor mapping"),
        (_valid_seed_values(valid_from="19000101"), "invalid valid_from"),
        (_valid_seed_values(item_id=""), "blank required field item_id"),
        (_valid_seed_values(vendor_field="[unverified bank template]"), "placeholder vendor_field"),
        (_valid_seed_values(vendor_field="revt - cogs"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="BS_ST_BORROW + BS_CUR_PORTION_LT_DEBT"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="revt/cogs"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="field (derived)"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="cogs+xsga+xrd+dp"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="a-b"), "expression vendor_field"),
        (_valid_seed_values(vendor_field="x*y"), "expression vendor_field"),
    ],
)
def test_read_fundamental_item_seed_rejects_malformed_rows(tmp_path, row, message):
    """Malformed seed CSV rows fail closed with row-context errors."""
    from db.item_registry import read_fundamental_item_seed

    seed_path = _write_seed_csv(tmp_path, [row])

    with pytest.raises(ValueError, match=message):
        read_fundamental_item_seed(seed_path)


def test_default_fundamental_item_seed_path_exists():
    """The default package-data seed path resolves in the source tree."""
    from db.item_registry import SEED_PATH

    assert SEED_PATH.exists()


def test_pyproject_includes_fundamental_item_seed_package_data():
    """Installed packages include the offline CSV seed file."""
    pyproject_path = Path(__file__).resolve().parents[2] / "pyproject.toml"
    pyproject = tomllib.loads(pyproject_path.read_text(encoding="utf-8"))

    package_data = pyproject["tool"]["setuptools"]["package-data"]
    assert "seeds/*.csv" in package_data["db"]


def test_seed_fundamental_item_registry_loads_acceptance_count(tmp_store):
    """S1-1 loader seeds the canonical item registry from the offline CSV."""
    from db.item_registry import seed_fundamental_item_registry

    inserted = seed_fundamental_item_registry(tmp_store)
    item_count = tmp_store.con.execute("SELECT count(*) FROM fundamental_item").fetchone()[0]

    assert inserted == item_count
    assert item_count == 234


def test_seed_fundamental_item_registry_rejects_unauthorized_gap_ids(tmp_store):
    """Loader must not create pseudo-items for vendor-field bridge rows."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    unauthorized_count = tmp_store.con.execute(
        "SELECT count(*) FROM fundamental_item WHERE item_id = ANY(?)",
        [list(UNAUTHORIZED_GAP_ITEM_IDS)],
    ).fetchone()[0]

    assert unauthorized_count == 0


def test_seed_fundamental_item_registry_excludes_placeholder_vendor_fields(tmp_store):
    """Placeholder prose like [unverified bank template] is not a vendor field."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    rows = tmp_store.con.execute(
        """
        SELECT vendor, vendor_field
        FROM fundamental_item_vendor_map
        WHERE item_id = 1501 AND vendor = 'compustat'
        """
    ).fetchall()
    bracketed_count = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fundamental_item_vendor_map
        WHERE vendor_field LIKE '%[%' OR vendor_field LIKE '%]%'
        """
    ).fetchone()[0]

    assert rows == []
    assert bracketed_count == 0


def test_seed_fundamental_item_registry_excludes_expression_vendor_fields(tmp_store):
    """Formula expressions are not real standalone vendor field identifiers."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    expression_rows = tmp_store.con.execute(
        """
        SELECT item_id, vendor, vendor_field
        FROM fundamental_item_vendor_map
        WHERE vendor_field LIKE '% + %'
           OR vendor_field LIKE '% - %'
           OR vendor_field LIKE '% * %'
           OR vendor_field LIKE '% / %'
           OR vendor_field LIKE '%(%'
           OR vendor_field LIKE '%)%'
           OR regexp_matches(vendor_field, '[A-Za-z0-9_][+\\-*/][A-Za-z0-9_]')
        ORDER BY item_id, vendor, vendor_field
        """
    ).fetchall()
    named_examples = set(
        tmp_store.con.execute(
            """
            SELECT item_id, vendor, vendor_field
            FROM fundamental_item_vendor_map
            WHERE (item_id = 1004 AND vendor = 'compustat' AND vendor_field = 'revt - cogs')
               OR (item_id = 1010 AND vendor = 'compustat' AND vendor_field = 'cogs+xsga+xrd+dp')
               OR (item_id = 1115 AND vendor = 'compustat' AND vendor_field = 'intan - gdwl')
               OR (item_id = 1205 AND vendor = 'bloomberg' AND vendor_field = 'BS_ST_BORROW + BS_CUR_PORTION_LT_DEBT')
               OR (item_id = 1208 AND vendor = 'compustat' AND vendor_field = 'dlc + dltt')
               OR (item_id = 1325 AND vendor = 'compustat' AND vendor_field = 'oancf - capx')
            """
        ).fetchall()
    )

    assert expression_rows == []
    assert named_examples == set()


def test_seed_fundamental_item_registry_aliases_reference_live_items(tmp_store):
    """Every alias edge resolves to an item row after loading the registry seed."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    dangling = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM fundamental_item_alias alias
        LEFT JOIN fundamental_item item ON item.item_id = alias.item_id
        WHERE item.item_id IS NULL
        """
    ).fetchone()[0]

    assert dangling == 0


def test_registry_resolves_taxonomy_alias_to_item_id_without_duckdb():
    """Pure registry lookup maps taxonomy aliases to canonical item ids."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(
                item_id=1031,
                canonical_code="net_income",
                alias_code="NetIncomeLoss",
                coalesce_priority=10,
            ),
        )
    )

    assert registry.resolve_item("us-gaap", "NetIncomeLoss") == 1031
    assert registry.resolve_item("us-gaap", "DoesNotExist") is None
    assert registry.resolve_item("ifrs-full", "NetIncomeLoss") is None


def test_registry_returns_inputs_in_priority_then_alias_order_without_duckdb():
    """Input aliases are deterministic and match COALESCE fallback order."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),
            _seed_row_for_registry(
                alias_code="RevenueFromContractWithCustomerExcludingAssessedTax",
                coalesce_priority=10,
            ),
            _seed_row_for_registry(alias_code="SalesRevenueNet", coalesce_priority=30),
            _seed_row_for_registry(alias_code="RevenueFallbackB", coalesce_priority=40),
            _seed_row_for_registry(alias_code="RevenueFallbackA", coalesce_priority=40),
        )
    )

    assert registry.resolve_inputs("revenue") == [
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "Revenues",
        "SalesRevenueNet",
        "RevenueFallbackA",
        "RevenueFallbackB",
    ]
    assert registry.resolve_inputs("unknown_metric") == []


def test_registry_filters_aliases_by_as_of_validity_without_duckdb():
    """Alias validity is inclusive on valid_from and exclusive on valid_to."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),
            _seed_row_for_registry(
                alias_code="SalesRevenueNet",
                coalesce_priority=30,
                valid_to="2018-01-01",
            ),
        )
    )

    assert registry.resolve_inputs("revenue", as_of=date(2017, 12, 31)) == [
        "Revenues",
        "SalesRevenueNet",
    ]
    assert registry.resolve_inputs("revenue", as_of=date(2018, 1, 1)) == ["Revenues"]
    assert registry.resolve_item("us-gaap", "SalesRevenueNet", as_of=date(2018, 1, 1)) is None


def test_registry_accepts_string_as_of_without_duckdb():
    """String as_of values use the same alias validity rules as date objects."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),
            _seed_row_for_registry(
                alias_code="SalesRevenueNet",
                coalesce_priority=30,
                valid_to="2018-01-01",
            ),
        )
    )

    assert registry.resolve_inputs("revenue", as_of="2017-12-31") == [
        "Revenues",
        "SalesRevenueNet",
    ]
    assert registry.resolve_inputs("revenue", as_of="2018-01-01") == ["Revenues"]


def test_registry_normalizes_datetime_as_of_without_duckdb():
    """Datetime values are normalized to calendar dates before comparison."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),
            _seed_row_for_registry(
                alias_code="SalesRevenueNet",
                coalesce_priority=30,
                valid_to="2018-01-01",
            ),
        )
    )

    assert registry.resolve_inputs("revenue", as_of=datetime(2017, 12, 31, 23, 59)) == [
        "Revenues",
        "SalesRevenueNet",
    ]
    assert registry.resolve_item("us-gaap", "SalesRevenueNet", as_of=datetime(2018, 1, 1)) is None


def test_registry_rejects_malformed_as_of_on_hit_and_miss_paths_without_duckdb():
    """Bad as_of values fail before alias lookup can short-circuit."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (_seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),)
    )

    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_item("us-gaap", "Revenues", as_of="not-a-date")
    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_item("us-gaap", "DoesNotExist", as_of="not-a-date")
    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_inputs("revenue", as_of="not-a-date")
    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_inputs("unknown_metric", as_of="not-a-date")


def test_empty_registry_rejects_malformed_as_of_without_duckdb():
    """Empty registries still validate as_of at method entry."""
    from db.item_registry import Registry

    registry = Registry.from_table_rows((), ())

    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_item("us-gaap", "Revenues", as_of="not-a-date")
    with pytest.raises(ValueError, match="invalid as_of date"):
        registry.resolve_inputs("revenue", as_of="not-a-date")


def test_registry_from_table_rows_resolves_without_duckdb():
    """Table-row constructor accepts plain tuples and mappings without opening DuckDB."""
    from db.item_registry import Registry

    registry = Registry.from_table_rows(
        (
            {"item_id": 1001, "canonical_code": "revenue"},
            (1031, "net_income"),
        ),
        (
            {
                "item_id": 1001,
                "alias_scheme": "us-gaap",
                "alias_code": "Revenues",
                "coalesce_priority": 20,
                "valid_from": "1900-01-01",
                "valid_to": None,
            },
            (1031, "us-gaap", "NetIncomeLoss", 10, "1900-01-01", None),
        ),
    )

    assert registry.resolve_item("us-gaap", "NetIncomeLoss") == 1031
    assert registry.resolve_inputs("revenue") == ["Revenues"]


def test_registry_rejects_duplicate_item_id_with_conflicting_identity_without_duckdb():
    """Constructor rejects duplicate item ids with conflicting item records."""
    from db.item_registry import Registry

    rows = (
        _seed_row_for_registry(item_id=1001, canonical_code="revenue"),
        _seed_row_for_registry(item_id=1001, canonical_code="total_revenue"),
    )

    with pytest.raises(ValueError, match="Conflicting fundamental_item"):
        Registry.from_seed_rows(rows)


def test_registry_rejects_duplicate_canonical_code_for_different_item_ids_without_duckdb():
    """Canonical item codes must identify a single item id."""
    from db.item_registry import Registry

    rows = (
        _seed_row_for_registry(item_id=1001, canonical_code="revenue", alias_code="Revenues"),
        _seed_row_for_registry(item_id=1002, canonical_code="revenue", alias_code="SalesRevenueNet"),
    )

    with pytest.raises(ValueError, match="Conflicting canonical_code"):
        Registry.from_seed_rows(rows)


def test_registry_rejects_exact_duplicate_alias_rows_without_duckdb():
    """Exact duplicate aliases fail before resolution can pick a duplicate row."""
    from db.item_registry import Registry

    row = _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20)

    with pytest.raises(ValueError, match="Duplicate fundamental_item_alias"):
        Registry.from_seed_rows((row, row))


def test_registry_rejects_overlapping_alias_windows_for_different_items_without_duckdb():
    """Same alias may not resolve to two item ids over the same as-of window."""
    from db.item_registry import Registry

    rows = (
        _seed_row_for_registry(
            item_id=1001,
            canonical_code="revenue",
            alias_code="SharedConcept",
            valid_from="2010-01-01",
            valid_to="2020-01-01",
        ),
        _seed_row_for_registry(
            item_id=1002,
            canonical_code="sales_legacy",
            alias_code="SharedConcept",
            valid_from="2015-01-01",
            valid_to=None,
        ),
    )

    with pytest.raises(ValueError, match="Overlapping alias validity"):
        Registry.from_seed_rows(rows)


def test_registry_allows_non_overlapping_alias_windows_for_different_items_without_duckdb():
    """A reused alias can move item ids when validity windows do not overlap."""
    from db.item_registry import Registry

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(
                item_id=1001,
                canonical_code="revenue",
                alias_code="MovedConcept",
                valid_from="2010-01-01",
                valid_to="2020-01-01",
            ),
            _seed_row_for_registry(
                item_id=1002,
                canonical_code="sales_legacy",
                alias_code="MovedConcept",
                valid_from="2020-01-01",
                valid_to=None,
            ),
        )
    )

    assert registry.resolve_item("us-gaap", "MovedConcept", as_of="2019-12-31") == 1001
    assert registry.resolve_item("us-gaap", "MovedConcept", as_of="2020-01-01") == 1002


def test_registry_from_table_rows_rejects_duplicate_item_rows_without_duckdb():
    """Table-row constructor fails clearly instead of overwriting item identities."""
    from db.item_registry import Registry

    with pytest.raises(ValueError, match="Duplicate fundamental_item row"):
        Registry.from_table_rows(
            (
                {"item_id": 1001, "canonical_code": "revenue"},
                {"item_id": 1001, "canonical_code": "revenue"},
            ),
            (),
        )


def test_registry_from_table_rows_rejects_conflicting_item_rows_without_duckdb():
    """Table-row constructor rejects duplicate item ids with different canonical codes."""
    from db.item_registry import Registry

    with pytest.raises(ValueError, match="Conflicting fundamental_item"):
        Registry.from_table_rows(
            (
                {"item_id": 1001, "canonical_code": "revenue"},
                {"item_id": 1001, "canonical_code": "total_revenue"},
            ),
            (),
        )


def test_registry_from_table_rows_rejects_duplicate_canonical_codes_without_duckdb():
    """Table-row constructor enforces canonical_code uniqueness."""
    from db.item_registry import Registry

    with pytest.raises(ValueError, match="Conflicting canonical_code"):
        Registry.from_table_rows(
            (
                {"item_id": 1001, "canonical_code": "revenue"},
                {"item_id": 1002, "canonical_code": "revenue"},
            ),
            (),
        )


def test_module_level_resolvers_accept_explicit_registry_without_duckdb():
    """Public shim functions can resolve against caller-provided in-memory rows."""
    from db.item_registry import Registry, resolve_inputs, resolve_item

    registry = Registry.from_seed_rows(
        (
            _seed_row_for_registry(alias_code="Revenues", coalesce_priority=20),
            _seed_row_for_registry(
                alias_code="RevenueFromContractWithCustomerExcludingAssessedTax",
                coalesce_priority=10,
            ),
        )
    )

    assert resolve_item("us-gaap", "Revenues", registry=registry) == 1001
    assert resolve_inputs("revenue", registry=registry) == [
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "Revenues",
    ]


def test_committed_registry_seed_resolves_acceptance_examples_without_duckdb():
    """The committed offline seed can back the pure resolution shim."""
    from db.item_registry import Registry, read_fundamental_item_seed

    registry = Registry.from_seed_rows(read_fundamental_item_seed())

    assert registry.resolve_item("us-gaap", "NetIncomeLoss") == 1031
    assert registry.resolve_inputs("revenue") == [
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "Revenues",
        "SalesRevenueNet",
    ]
    assert "SalesRevenueNet" in registry.resolve_inputs("revenue", as_of=date(2017, 12, 31))
    assert "SalesRevenueNet" not in registry.resolve_inputs("revenue", as_of=date(2018, 1, 1))


def test_revenue_alias_priorities_match_statement_map_order(tmp_store):
    """Item 1001 uses the existing revenue COALESCE order: ASC-606, legacy, sales."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    rows = tmp_store.con.execute(
        """
        SELECT alias_code, coalesce_priority
        FROM fundamental_item_alias
        WHERE item_id = 1001 AND alias_scheme = 'us-gaap'
        ORDER BY coalesce_priority, alias_code
        """
    ).fetchall()

    assert rows == [
        ("RevenueFromContractWithCustomerExcludingAssessedTax", 10),
        ("Revenues", 20),
        ("SalesRevenueNet", 30),
    ]


def test_seed_fundamental_item_registry_reload_is_noop(tmp_store):
    """Reloading the same committed CSV leaves table counts and contents stable."""
    from db.item_registry import seed_fundamental_item_registry

    seed_fundamental_item_registry(tmp_store)
    before_counts = {
        table_name: tmp_store.con.execute(f"SELECT count(*) FROM {table_name}").fetchone()[0]
        for table_name in NEW_TABLES
    }
    before_snapshots = {
        "fundamental_item": tmp_store.con.execute(
            "SELECT * FROM fundamental_item ORDER BY item_id"
        ).fetchall(),
        "fundamental_item_alias": tmp_store.con.execute(
            """
            SELECT * FROM fundamental_item_alias
            ORDER BY item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
            """
        ).fetchall(),
        "fundamental_item_vendor_map": tmp_store.con.execute(
            """
            SELECT * FROM fundamental_item_vendor_map
            ORDER BY item_id, vendor, vendor_field
            """
        ).fetchall(),
    }

    seed_fundamental_item_registry(tmp_store)

    after_counts = {
        table_name: tmp_store.con.execute(f"SELECT count(*) FROM {table_name}").fetchone()[0]
        for table_name in NEW_TABLES
    }
    after_snapshots = {
        "fundamental_item": tmp_store.con.execute(
            "SELECT * FROM fundamental_item ORDER BY item_id"
        ).fetchall(),
        "fundamental_item_alias": tmp_store.con.execute(
            """
            SELECT * FROM fundamental_item_alias
            ORDER BY item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
            """
        ).fetchall(),
        "fundamental_item_vendor_map": tmp_store.con.execute(
            """
            SELECT * FROM fundamental_item_vendor_map
            ORDER BY item_id, vendor, vendor_field
            """
        ).fetchall(),
    }

    assert after_counts == before_counts
    assert after_snapshots == before_snapshots


def test_item_registry_loader_uses_stdlib_csv_without_pandas():
    """Loading the seed stays lightweight and offline; pandas is not in the load path."""
    import db.item_registry as item_registry

    source = inspect.getsource(item_registry)
    assert "import csv" in source
    assert "pandas" not in source


def test_ratio_input_metrics_are_byte_identical_to_pre_s1_3_literals():
    """S1-3 registry map preserves the exact source-table canonical_metric strings."""
    from db.item_registry import ratio_input_metrics

    assert ratio_input_metrics("ttm") == {
        "rev": "revenue",
        "ni": "net_income",
        "oi": "operating_income",
        "ocf": "operating_cash_flow",
        "capex": "capital_expenditures",
        "div": "dividends_paid",
        "repurch": "share_repurchases",
    }
    assert ratio_input_metrics("balance") == {
        "assets": "assets",
        "liabilities": "liabilities",
        "equity": "stockholders_equity",
        "shares": "shares_outstanding",
    }
    assert ratio_input_metrics("xbrl_balance") == {
        "current_assets": "current_assets",
        "current_liabilities": "current_liabilities",
        "cash_and_equivalents": "cash_and_equivalents",
        "inventory": "inventory",
        "long_term_debt": "long_term_debt",
        "retained_earnings": "retained_earnings",
        "common_shares_outstanding": "common_shares_outstanding",
        "property_plant_equipment_net": "property_plant_equipment_net",
        "accounts_receivable": "accounts_receivable",
        "accounts_payable": "ap",
        "goodwill": "goodwill",
        "intangibles_other": "intangibles_other",
    }
    assert ratio_input_metrics("xbrl_flow") == {
        "gross_profit": "gross_profit",
        "cost_of_revenue": "cost_of_revenue",
        "interest_expense": "interest_expense",
        "depreciation_amortization": "depreciation_amortization",
        "selling_general_and_administrative_expense": "selling_general_and_administrative_expense",
        "pretax_income": "pretax_income",
        "income_tax": "income_tax",
        "shares_basic_avg": "shares_basic_avg",
        "shares_diluted_avg": "shares_diluted_avg",
    }


def test_ratio_input_item_ids_encode_controller_semantic_picks_and_gaps():
    """Ratio input item ids use the S1-3 governed bridge, not seed string matching."""
    from db.item_registry import input_item_ids_for_ratio, ratio_input_item_ids

    assert ratio_input_item_ids("ttm")["ni"] == 1031  # actual net income, not estimate item 2009
    assert ratio_input_item_ids("xbrl_balance")["cash_and_equivalents"] == 1104
    assert ratio_input_item_ids("xbrl_flow")["depreciation_amortization"] == 1307
    assert ratio_input_item_ids("xbrl_balance")["common_shares_outstanding"] is None
    assert ratio_input_item_ids("xbrl_flow")["selling_general_and_administrative_expense"] is None
    assert ratio_input_item_ids("xbrl_balance")["accounts_payable"] == 1203
    assert ratio_input_item_ids("xbrl_flow")["pretax_income"] == 1023
    assert ratio_input_item_ids("xbrl_flow")["income_tax"] == 1024
    assert ratio_input_item_ids("xbrl_balance")["goodwill"] == 1114
    assert ratio_input_item_ids("xbrl_balance")["intangibles_other"] == 1115
    assert ratio_input_item_ids("xbrl_flow")["shares_basic_avg"] == 1040
    assert ratio_input_item_ids("xbrl_flow")["shares_diluted_avg"] == 1041

    assert input_item_ids_for_ratio(("rev", "ni", "rev_prior", "ni_prior")) == [1001, 1031]
    assert input_item_ids_for_ratio(("current_assets", "inventory", "current_liabilities")) == [1102, 1107, 1202]
    assert input_item_ids_for_ratio(
        (
            "common_shares_outstanding",
            "common_shares_outstanding_prior",
            "selling_general_and_administrative_expense",
        )
    ) == []


def test_ratio_input_registry_returns_copies_and_rejects_unknown_groups():
    """Callers cannot mutate the governed map; unknown groups fail clearly."""
    from db.item_registry import ratio_input_metrics

    metrics = ratio_input_metrics("ttm")
    metrics["rev"] = "wrong"

    assert ratio_input_metrics("ttm")["rev"] == "revenue"
    with pytest.raises(ValueError, match="unknown ratio input group"):
        ratio_input_metrics("not_a_group")
