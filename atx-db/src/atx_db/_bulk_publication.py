"""Short transactional swaps for validated physical publication shadows.

DuckDB ART indexes make a table non-renamable.  Callers therefore use this only
for targets with no secondary ART indexes (or only their required primary-key
constraint), after validating a complete shadow table outside the transaction.
"""

from __future__ import annotations

from collections.abc import Callable

from .connection import DuckDBStore


def _identifier(value: str) -> str:
    """Return a deliberately narrow SQL identifier for warehouse relations."""
    if not value or not value.replace("_", "").isalnum() or not (value[0].isalpha() or value[0] == "_"):
        raise ValueError(f"invalid relation identifier: {value!r}")
    return value


def _contract(store: DuckDBStore, relation: str) -> tuple[tuple[object, ...], tuple[object, ...]]:
    """Return the physical column and logical-key contract of one relation."""
    name = _identifier(relation)
    columns = tuple(
        store.con.execute(f"DESCRIBE {name}").fetchall()
    )
    constraints = tuple(
        store.con.execute(
            """
            SELECT constraint_type, constraint_column_names
            FROM duckdb_constraints()
            WHERE table_name = ? AND constraint_type IN ('PRIMARY KEY', 'UNIQUE')
            ORDER BY constraint_type, constraint_column_names
            """,
            [name],
        ).fetchall()
    )
    return columns, constraints


def _validate_shadow_contract(
    store: DuckDBStore, *, live_table: str, shadow_table: str
) -> None:
    """Reject a shadow that weakens the public relation's physical contract."""
    live_contract = _contract(store, live_table)
    shadow_contract = _contract(store, shadow_table)
    if shadow_contract != live_contract:
        raise RuntimeError(
            "bulk publication shadow contract differs from live table: "
            f"live={live_table!r}, shadow={shadow_table!r}"
        )


def publish_validated_shadow(
    store: DuckDBStore,
    *,
    live_table: str,
    shadow_table: str,
    before_swap: Callable[[], None] | None = None,
) -> None:
    """Atomically replace a physical table with a previously validated shadow.

    ``before_swap`` executes inside the same transaction so related rows become
    visible only with the new table.  If it or any catalog operation fails, both
    relations retain their pre-publication names and contents.
    """
    live = _identifier(live_table)
    shadow = _identifier(shadow_table)
    previous = _identifier(f"{live}_bulk_previous")
    if live == shadow:
        raise ValueError("live_table and shadow_table must differ")

    with store.transaction():
        _validate_shadow_contract(store, live_table=live, shadow_table=shadow)
        if before_swap is not None:
            before_swap()
        store.con.execute(f"ALTER TABLE {live} RENAME TO {previous}")
        store.con.execute(f"ALTER TABLE {shadow} RENAME TO {live}")
        store.con.execute(f"DROP TABLE {previous}")
