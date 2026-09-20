"""Render docs/DATA_DICTIONARY.md from the committed registries.

Pure function of the repository: the item seed, the derived-metric seed, the public
record schemas, and the universe/delisting/release vocabularies. It never opens a
warehouse, so CI can run it in --check mode and fail the build when the committed file
is stale.

Two sections (*Delistings*, *Release datasets*) read their vocabulary through a small
source function -- ``_delisting_vocabulary`` / ``_release_datasets`` -- that prefers the
real Task 3 / Task 8 module (``atx_db.delisting_evidence`` / ``atx_db.publication``) and
falls back to the nearest already-committed equivalent when that module has not landed
yet. Once it lands, the next regeneration picks it up automatically with no code change;
the doc's own "Vocabulary source" / "Dataset source" line always says which one rendered.
"""

from __future__ import annotations

import argparse
import difflib
import sys
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

from atx_db.api.catalog import DATASETS  # noqa: E402
from atx_db.derived_registry import DerivedMetricDefinition, default_derived_definitions  # noqa: E402
from atx_db.item_registry import read_fundamental_item_seed  # noqa: E402
from atx_db.universe_us_listed import (  # noqa: E402
    ELIGIBLE_SECURITY_TYPES,
    EXCHANGE_LABELS,
    SECURITY_TYPE_PATTERNS,
)

DATA_DICTIONARY_PATH = ROOT / "docs" / "DATA_DICTIONARY.md"

# The fixed spine of market_daily_metrics (migration 0302); the metric columns come from
# the derived seed's window='daily' rows, so the two can never drift apart here.
MARKET_DAILY_SPINE_COLUMNS: tuple[tuple[str, str], ...] = (
    ("market_daily_id", "Content hash of (source, security_id, trade_date)."),
    ("source", "Producing engine identity."),
    ("security_id", "Stable ATX security identifier."),
    ("symbol", "Ticker on the bar."),
    ("trade_date", "Trading session date."),
    ("close", "Session close price, unadjusted."),
    ("adj_close", "Split/dividend adjusted close; the only return input."),
    ("volume", "Reported share volume."),
    ("shares_outstanding", "Point-in-time shares, dei preferred over archive."),
    ("shares_source", "'dei' or 'archive'."),
    ("shares_reconciliation_ratio", "dei shares / archive shares on the same session."),
    ("fundamental_available_at", "Availability of the newest fundamental input."),
    ("available_at", "Earliest timestamp a consumer may use the row."),
    ("inputs_hash", "Content hash of the inputs that produced the row."),
    ("as_of_date", "Economic observation date."),
    ("is_latest_revision", "Chain-head flag."),
    ("run_id", "Producing run."),
    ("source_loaded_at", "Warehouse load stamp; never a signal input."),
)

_ESCAPES = str.maketrans({"|": "\\|", "\n": " ", "\r": " "})


def _cell(value: object) -> str:
    if value is None:
        return ""
    return str(value).strip().translate(_ESCAPES)


def _table(headers: Sequence[str], rows: Sequence[tuple[object, ...]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |"]
    lines.append("| " + " | ".join("---" for _ in headers) + " |")
    lines.extend("| " + " | ".join(_cell(value) for value in row) + " |" for row in rows)
    return lines


def _item_section() -> list[str]:
    seen: dict[int, tuple[object, ...]] = {}
    for row in read_fundamental_item_seed():
        if row.item_id in seen:
            continue
        seen[row.item_id] = (
            row.item_id,
            row.canonical_code,
            row.statement,
            row.section,
            row.unit_type,
            "yes" if row.is_derived else "no",
            row.definition,
        )
    lines = [
        "## Canonical statement items",
        "",
        f"{len(seen)} items, from `src/atx_db/seeds/fundamental_items.csv`. One row per "
        "`item_id`; alias rows are collapsed.",
        "",
    ]
    lines.extend(
        _table(
            ("item_id", "canonical_code", "statement", "section", "unit_type", "derived", "definition"),
            [seen[item_id] for item_id in sorted(seen)],
        )
    )
    return lines


def _derived_section() -> list[str]:
    definitions = sorted(default_derived_definitions(), key=lambda d: (d.family, d.metric_code))
    lines = [
        "## Derived metrics",
        "",
        f"{len(definitions)} metrics, from `src/atx_db/seeds/derived_metric_definitions.csv`. "
        "Every value carries `available_at = max(input available_at)` and an `inputs_hash`.",
        "",
    ]
    lines.extend(
        _table(
            ("family", "metric", "window", "expression", "description"),
            [(d.family, d.metric_code, d.window, d.expression, d.description) for d in definitions],
        )
    )
    return lines


def _market_daily_section() -> list[str]:
    daily: list[DerivedMetricDefinition] = sorted(
        (d for d in default_derived_definitions() if d.window == "daily"),
        key=lambda d: d.metric_code,
    )
    lines = [
        "## Daily market panel",
        "",
        "`market_daily_metrics` -- one row per (security_id, trade_date). The spine is fixed; "
        "the metric columns are exactly the `window='daily'` rows of the derived seed.",
        "",
        "### Spine columns",
        "",
    ]
    lines.extend(_table(("column", "meaning"), list(MARKET_DAILY_SPINE_COLUMNS)))
    lines.extend(["", "### Metric columns", ""])
    lines.extend(
        _table(
            ("column", "family", "expression"),
            [(d.metric_code, d.family, d.expression) for d in daily],
        )
    )
    return lines


def _universe_section() -> list[str]:
    lines = [
        "## Universe",
        "",
        "`universe_us_listed_membership` -- interval-keyed point-in-time membership. "
        "`valid_to` extends `lookback_days` trading sessions past the security's last bar; "
        "an interval that reaches the archive end stays open. `market_cap_decile` is the "
        "decile AT `valid_from` only.",
        "",
        "### Eligible exchanges",
        "",
    ]
    lines.extend(
        _table(
            ("exchange_code", "venue"),
            [(code, EXCHANGE_LABELS[code]) for code in sorted(EXCHANGE_LABELS)],
        )
    )
    lines.extend(
        [
            "",
            "### Security types",
            "",
            "Eligible: "
            + ", ".join(f"`{value}`" for value in ELIGIBLE_SECURITY_TYPES)
            + ". Every other label is excluded from the universe and counted in the "
            "build's quality-check details.",
            "",
        ]
    )
    lines.extend(
        _table(
            ("security_type", "matched by"),
            [
                *[(label, f"`{pattern.pattern}`") for label, pattern in SECURITY_TYPE_PATTERNS],
                ("ETF", "the directory `etf` boolean"),
                ("test", "the directory `test_issue` boolean"),
                ("common", "no pattern matched"),
                ("unknown", "no security name available"),
            ],
        )
    )
    lines.extend(
        [
            "",
            "### Membership reasons",
            "",
            "- `member` -- in the universe with a resolved CIK (the fundamentals universe).",
            "- `member_no_cik` -- in the market universe, CIK unresolved. Retained and counted, never dropped.",
        ]
    )
    return lines


def _delisting_vocabulary() -> tuple[str, tuple[str, ...], list[tuple[object, ...]], list[str]]:
    """Return ``(source_label, table_headers, table_rows, reason_categories)``.

    Prefers Task 3's ``atx_db.delisting_evidence`` (``EVIDENCE_PRECEDENCE``,
    ``REASON_CATEGORIES``). Falls back to the real, already-committed
    ``atx_db.delisting`` vocabulary (``DELIST_CODE_ROWS``, ``TERMINAL_RETURN_POLICY_ROWS``)
    when ``delisting_evidence.py`` has not landed yet, so this section self-upgrades on
    the next regeneration with no code change.
    """
    try:
        from atx_db.delisting_evidence import EVIDENCE_PRECEDENCE, REASON_CATEGORIES
    except ImportError:
        pass
    else:
        precedence_rows: list[tuple[object, ...]] = [
            (rank, kind, code, reason, confidence)
            for kind, rank, code, reason, confidence in sorted(EVIDENCE_PRECEDENCE, key=lambda row: row[1])
        ]
        return (
            "atx_db.delisting_evidence.EVIDENCE_PRECEDENCE / REASON_CATEGORIES",
            ("rank", "evidence_kind", "delist_code", "default reason", "confidence"),
            precedence_rows,
            list(REASON_CATEGORIES),
        )

    from atx_db.delisting import DELIST_CODE_ROWS, TERMINAL_RETURN_POLICY_ROWS

    fallback_rows: list[tuple[object, ...]] = [
        (index + 1, row[0], row[5], row[6], row[11]) for index, row in enumerate(DELIST_CODE_ROWS)
    ]
    fallback_reasons = sorted({str(row[1]) for row in TERMINAL_RETURN_POLICY_ROWS})
    return (
        "atx_db.delisting.DELIST_CODE_ROWS / TERMINAL_RETURN_POLICY_ROWS "
        "(atx_db.delisting_evidence -- Task 3 -- not yet landed)",
        ("rank", "delist_code", "reason_category", "description", "source"),
        fallback_rows,
        fallback_reasons,
    )


def _delisting_section() -> list[str]:
    source_label, headers, rows, reasons = _delisting_vocabulary()
    lines = [
        "## Delistings",
        "",
        "`delisting_evidence` holds one row per independent piece of public evidence; "
        "`delisting_events` keeps the minimum `evidence_rank` per "
        "(security_id, delist_date).",
        "",
        f"Vocabulary source: `{source_label}`.",
        "",
        "### Evidence precedence",
        "",
    ]
    lines.extend(_table(headers, rows))
    lines.extend(
        [
            "",
            "### Reason categories",
            "",
            ", ".join(f"`{value}`" for value in reasons) + ".",
            "",
            "A Nasdaq `financial_status` bankruptcy flag (`Q`) on or before the delist date "
            "upgrades any row to `bankruptcy` with `high` confidence.",
        ]
    )
    return lines


def _api_section() -> list[str]:
    lines = ["## Public API schemas", ""]
    for dataset in DATASETS:
        for schema in dataset.schemas:
            lines.extend(
                [
                    f"### {dataset.code}/{schema.code}",
                    "",
                    f"{schema.title} (v{schema.version}) over `{schema.source_table}`; "
                    f"time column `{schema.time_column}`; natural key "
                    f"`{', '.join(schema.natural_key)}`.",
                    "",
                ]
            )
            lines.extend(
                _table(
                    ("field", "type", "unit", "nullable", "filterable", "description"),
                    [
                        (
                            field.name,
                            field.data_type,
                            field.unit,
                            "yes" if field.nullable else "no",
                            "yes" if field.filterable else "no",
                            field.description,
                        )
                        for field in schema.fields
                    ],
                )
            )
            lines.append("")
    return lines


def _release_datasets() -> tuple[str, tuple[str, ...], list[tuple[object, ...]]]:
    """Return ``(source_label, table_headers, table_rows)``.

    Prefers Task 8's ``atx_db.publication.RELEASE_DATASETS``. Falls back to the real,
    already-committed lakehouse export registry ``atx_db.lake.DEFAULT_EXPORT_OBJECTS``
    when ``publication.py`` has not landed yet -- it carries no key-column or
    public-schema shape, so the fallback table lists only the relation name.
    """
    try:
        # atx_db.publication (Task 8) does not exist on disk yet, so mypy cannot resolve
        # it statically; the runtime ImportError fallback below is what actually matters.
        from atx_db.publication import RELEASE_DATASETS  # type: ignore[import-not-found]
    except ImportError:
        pass
    else:
        return (
            "atx_db.publication.RELEASE_DATASETS",
            ("dataset", "relation", "key columns", "public schema"),
            [(d.name, d.object_name, ", ".join(d.key_columns), d.schema_ref or "") for d in RELEASE_DATASETS],
        )

    from atx_db.lake import DEFAULT_EXPORT_OBJECTS

    return (
        "atx_db.lake.DEFAULT_EXPORT_OBJECTS (atx_db.publication -- Task 8 -- not yet landed)",
        ("relation",),
        [(name,) for name in DEFAULT_EXPORT_OBJECTS],
    )


def _release_section() -> list[str]:
    source_label, headers, rows = _release_datasets()
    lines = [
        "## Release datasets",
        "",
        "`atx-db publish-release` writes one Parquet file per dataset plus a single "
        "`manifest.json` pinning the exported schema hash, the public record-contract hash, "
        "the exact query text, the file bytes, and the diff against the previous release.",
        "",
        f"Dataset source: `{source_label}`.",
        "",
    ]
    lines.extend(_table(headers, rows))
    return lines


def render_data_dictionary() -> str:
    """Render the full dictionary. Deterministic: no clock, no warehouse, sorted throughout."""

    lines: list[str] = [
        "# atx-db data dictionary",
        "",
        "Generated by `scripts/generate_data_dictionary.py`. **Do not edit by hand** -- run",
        "`python scripts/generate_data_dictionary.py` after changing any registry; CI fails",
        "when this file is stale.",
        "",
        "Sources of truth: `src/atx_db/seeds/fundamental_items.csv`,",
        "`src/atx_db/seeds/derived_metric_definitions.csv`, `src/atx_db/api/catalog.py`,",
        "`src/atx_db/universe_us_listed.py`. The Delistings and Release datasets sections each",
        "prefer their Task 3 / Task 8 module (`atx_db.delisting_evidence`,",
        "`atx_db.publication`) and fall back to an already-committed equivalent otherwise; see",
        'each section\'s "source" line for which one rendered this revision.',
        "",
    ]
    for section in (
        _item_section(),
        _derived_section(),
        _market_daily_section(),
        _universe_section(),
        _delisting_section(),
        _api_section(),
        _release_section(),
    ):
        lines.extend(section)
        lines.append("")
    while lines and lines[-1] == "":
        lines.pop()
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="generate-data-dictionary")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit 1 and print a diff when the committed file is stale; write nothing.",
    )
    args = parser.parse_args(argv)

    rendered = render_data_dictionary()
    if args.check:
        current = DATA_DICTIONARY_PATH.read_bytes().decode("utf-8") if DATA_DICTIONARY_PATH.exists() else ""
        if current == rendered:
            return 0
        print(f"{DATA_DICTIONARY_PATH.name} is stale; run scripts/generate_data_dictionary.py")
        sys.stdout.writelines(
            difflib.unified_diff(
                current.splitlines(keepends=True),
                rendered.splitlines(keepends=True),
                fromfile="committed",
                tofile="generated",
                n=2,
            )
        )
        return 1
    DATA_DICTIONARY_PATH.parent.mkdir(parents=True, exist_ok=True)
    # write_bytes (not write_text) so LF newlines survive untranslated on Windows --
    # byte-deterministic output is a hard requirement (no platform-dependent CRLF).
    DATA_DICTIONARY_PATH.write_bytes(rendered.encode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
