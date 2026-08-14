"""PF2-S3: deterministic fundamental standardization engine.

The engine turns as-reported SEC/XBRL fundamentals into one comparable long fact:
``fundamental_standardized``. Rules live in ``db/seeds/standardization_rules.csv`` and
are evaluated by a closed dispatch table. No eval, no network, no DuckDB in the pure
transform.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any, cast

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import json_dumps, quality_check

RULE_PATH = Path(__file__).resolve().parent / "seeds" / "standardization_rules.csv"
DEFAULT_SOURCE = "fundamental_standardization_v1"
SOURCE_NAME = "Standardized fundamental statement items"

COMBINATION_RULES = frozenset(
    {"coalesce_priority", "first_non_null", "identity", "sum", "difference"}
)
SIGN_RULES = frozenset({"statement_normalized", "as_reported", "absolute", "invert"})
SCALE_RULES = frozenset({"identity", "thousands", "millions"})
MISSING_POLICIES = frozenset({"skip", "zero_fill"})

RULE_COLUMNS = (
    "rule_id",
    "item_id",
    "canonical_code",
    "basis",
    "source_aliases_json",
    "source_item_ids_json",
    "combination_rule",
    "sign_rule",
    "scale_rule",
    "missing_policy",
    "is_active",
    "valid_from",
    "valid_to",
)

STANDARDIZED_COLUMNS = [
    "standardized_id",
    "source",
    "upstream_source",
    "security_id",
    "symbol",
    "cik",
    "item_id",
    "canonical_code",
    "basis",
    "period_start",
    "period_end",
    "fiscal_year",
    "fiscal_period",
    "value",
    "unit_type",
    "source_accession",
    "filed_date",
    "as_of_date",
    "available_at",
    "input_codes_json",
    "input_item_ids_json",
    "rule_id",
    "combination_rule",
    "is_latest_revision",
    "run_id",
]

EXCEPTION_COLUMNS = [
    "exception_id",
    "source",
    "upstream_source",
    "security_id",
    "symbol",
    "cik",
    "basis",
    "period_start",
    "period_end",
    "accession_number",
    "concept",
    "taxonomy",
    "unit",
    "value",
    "reason",
    "as_of_date",
    "available_at",
    "run_id",
]


@dataclass(frozen=True)
class SourceAlias:
    alias_scheme: str
    alias_code: str
    priority: int


@dataclass(frozen=True)
class StandardizationRule:
    rule_id: str
    item_id: int
    canonical_code: str
    basis: str
    source_aliases: tuple[SourceAlias, ...]
    source_item_ids: tuple[int, ...]
    combination_rule: str
    sign_rule: str
    scale_rule: str
    missing_policy: str
    is_active: bool
    valid_from: dt.date | None
    valid_to: dt.date | None


@dataclass(frozen=True)
class StandardizationResult:
    standardized: pd.DataFrame
    exceptions: pd.DataFrame
    standardized_row_count: int = 0
    exception_row_count: int = 0
    input_row_count: int = 0
    build_id: str | None = None
    run_id: str | None = None
    rule_set_sha256: str | None = None
    basis_counts: Mapping[str, int] | None = None


@dataclass(frozen=True)
class FundamentalStandardizationOptions:
    source: str = DEFAULT_SOURCE
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None
    materialize_result_limit: int = 10_000


def _none_if_blank(value: str | None) -> str | None:
    if value is None:
        return None
    value = str(value).strip()
    return value or None


def _parse_bool(value: str) -> bool:
    token = str(value).strip().lower()
    if token in {"1", "true", "t", "yes", "y"}:
        return True
    if token in {"0", "false", "f", "no", "n"}:
        return False
    raise ValueError(f"invalid boolean token {value!r}")


def _parse_date(value: str | None) -> dt.date | None:
    value = _none_if_blank(value)
    if value is None:
        return None
    return dt.date.fromisoformat(value)


def _parse_aliases(raw_json: str) -> tuple[SourceAlias, ...]:
    payload = json.loads(raw_json or "[]")
    if not isinstance(payload, list):
        raise ValueError("source_aliases_json must be a JSON list")
    aliases: list[SourceAlias] = []
    for row in payload:
        if not isinstance(row, Mapping):
            raise ValueError("source_aliases_json entries must be objects")
        aliases.append(
            SourceAlias(
                alias_scheme=str(row["alias_scheme"]),
                alias_code=str(row["alias_code"]),
                priority=int(row.get("priority", 100)),
            )
        )
    return tuple(sorted(aliases, key=lambda alias: (alias.priority, alias.alias_scheme, alias.alias_code)))


def _parse_item_ids(raw_json: str) -> tuple[int, ...]:
    payload = json.loads(raw_json or "[]")
    if not isinstance(payload, list):
        raise ValueError("source_item_ids_json must be a JSON list")
    return tuple(int(value) for value in payload)


def _read_rule(row: Mapping[str, str], *, row_number: int, seed_path: Path) -> StandardizationRule:
    missing = [column for column in RULE_COLUMNS if column not in row]
    if missing:
        raise ValueError(f"{seed_path} row {row_number}: missing columns {missing!r}")
    rule = StandardizationRule(
        rule_id=str(row["rule_id"]).strip(),
        item_id=int(row["item_id"]),
        canonical_code=str(row["canonical_code"]).strip(),
        basis=str(row["basis"]).strip(),
        source_aliases=_parse_aliases(row["source_aliases_json"]),
        source_item_ids=_parse_item_ids(row["source_item_ids_json"]),
        combination_rule=str(row["combination_rule"]).strip(),
        sign_rule=str(row["sign_rule"]).strip(),
        scale_rule=str(row["scale_rule"]).strip(),
        missing_policy=str(row["missing_policy"]).strip(),
        is_active=_parse_bool(row["is_active"]),
        valid_from=_parse_date(row["valid_from"]),
        valid_to=_parse_date(row["valid_to"]),
    )
    if not rule.rule_id:
        raise ValueError(f"{seed_path} row {row_number}: blank rule_id")
    if rule.combination_rule not in COMBINATION_RULES:
        raise ValueError(f"{seed_path} row {row_number}: unknown combination_rule {rule.combination_rule!r}")
    if rule.sign_rule not in SIGN_RULES:
        raise ValueError(f"{seed_path} row {row_number}: unknown sign_rule {rule.sign_rule!r}")
    if rule.scale_rule not in SCALE_RULES:
        raise ValueError(f"{seed_path} row {row_number}: unknown scale_rule {rule.scale_rule!r}")
    if rule.missing_policy not in MISSING_POLICIES:
        raise ValueError(f"{seed_path} row {row_number}: unknown missing_policy {rule.missing_policy!r}")
    if rule.combination_rule in {"sum", "difference"} and not rule.source_item_ids:
        raise ValueError(f"{seed_path} row {row_number}: {rule.combination_rule} requires source_item_ids_json")
    if rule.combination_rule == "difference" and len(rule.source_item_ids) != 2:
        raise ValueError(f"{seed_path} row {row_number}: difference requires exactly two source item ids")
    return rule


def read_standardization_rules(path: Path | str = RULE_PATH) -> tuple[StandardizationRule, ...]:
    seed_path = Path(path)
    with seed_path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        if tuple(reader.fieldnames or ()) != RULE_COLUMNS:
            raise ValueError(f"{seed_path} has unexpected columns: {reader.fieldnames}")
        rules = tuple(
            _read_rule(row, row_number=row_number, seed_path=seed_path)
            for row_number, row in enumerate(reader, start=2)
        )
    seen: set[tuple[int, str]] = set()
    for rule in rules:
        key = (rule.item_id, rule.basis)
        if key in seen:
            raise ValueError(f"{seed_path}: duplicate active rule key {key!r}")
        if rule.is_active:
            seen.add(key)
    return rules


@lru_cache(maxsize=1)
def default_standardization_rules() -> tuple[StandardizationRule, ...]:
    return read_standardization_rules(RULE_PATH)


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _stable_id(*parts: Any) -> str:
    payload = "|".join("" if part is None else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _date_key(value: Any) -> dt.date | None:
    if not _present(value):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _rule_is_live(rule: StandardizationRule, period_end: Any) -> bool:
    if not rule.is_active:
        return False
    as_of = _date_key(period_end)
    if as_of is None:
        return True
    valid_from = rule.valid_from or dt.date.min
    valid_to = rule.valid_to or dt.date.max
    return valid_from <= as_of < valid_to


def _apply_sign(value: float, rule: StandardizationRule) -> float:
    if rule.sign_rule in {"statement_normalized", "as_reported"}:
        return value
    if rule.sign_rule == "absolute":
        return abs(value)
    if rule.sign_rule == "invert":
        return -value
    raise ValueError(f"unknown sign_rule {rule.sign_rule!r}")


def _apply_scale(value: float, rule: StandardizationRule) -> float:
    if rule.scale_rule == "identity":
        return value
    if rule.scale_rule == "thousands":
        return value * 1_000.0
    if rule.scale_rule == "millions":
        return value * 1_000_000.0
    raise ValueError(f"unknown scale_rule {rule.scale_rule!r}")


def _normalize_value(value: Any, rule: StandardizationRule) -> float | None:
    if not _present(value):
        return None
    return _apply_scale(_apply_sign(float(value), rule), rule)


def _candidate_code(row: Mapping[str, Any]) -> str:
    taxonomy = row.get("taxonomy")
    concept = row.get("concept")
    if _present(taxonomy) and _present(concept):
        return f"{taxonomy}:{concept}"
    metric = row.get("canonical_metric")
    return "" if not _present(metric) else str(metric)


def _row_sort_key(row: Mapping[str, Any]) -> tuple[int, int, str]:
    rank_value = row.get("input_rank")
    rank = 100 if not _present(rank_value) else int(str(rank_value))
    av = row.get("available_at")
    av_value = 0 if not _present(av) else int(pd.Timestamp(str(av)).value)
    return (rank, -av_value, _candidate_code(row))


def _best_direct_rows(candidates: Sequence[Mapping[str, Any]], rule: StandardizationRule) -> list[Mapping[str, Any]]:
    rows = [
        row
        for row in candidates
        if _present(row.get("item_id"))
        and int(row["item_id"]) == rule.item_id
        and _present(row.get("value"))
    ]
    return sorted(rows, key=_row_sort_key)


def _best_for_item(candidates: Sequence[Mapping[str, Any]], item_id: int) -> Mapping[str, Any] | None:
    rows = [
        row
        for row in candidates
        if _present(row.get("item_id"))
        and int(row["item_id"]) == item_id
        and _present(row.get("value"))
    ]
    if not rows:
        return None
    return sorted(rows, key=_row_sort_key)[0]


def _select_inputs(
    candidates: Sequence[Mapping[str, Any]],
    rule: StandardizationRule,
) -> tuple[float, list[Mapping[str, Any]]] | None:
    if rule.combination_rule in {"identity", "coalesce_priority", "first_non_null"}:
        direct = _best_direct_rows(candidates, rule)
        if not direct:
            return None
        value = _normalize_value(direct[0].get("value"), rule)
        if value is None:
            return None
        return value, [direct[0]]

    selected: list[Mapping[str, Any]] = []
    values: list[float] = []
    for item_id in rule.source_item_ids:
        row = _best_for_item(candidates, item_id)
        if row is None:
            if rule.missing_policy == "zero_fill":
                values.append(0.0)
                continue
            return None
        value = _normalize_value(row.get("value"), rule)
        if value is None:
            if rule.missing_policy == "zero_fill":
                values.append(0.0)
                continue
            return None
        selected.append(row)
        values.append(value)

    if rule.combination_rule == "sum":
        return sum(values), selected
    if rule.combination_rule == "difference":
        return values[0] - values[1], selected
    raise ValueError(f"unknown combination_rule {rule.combination_rule!r}")


def _max_present(values: Iterable[Any]) -> Any | None:
    present = [value for value in values if _present(value)]
    return max(present) if present else None


def _first_present(values: Iterable[Any]) -> Any | None:
    for value in values:
        if _present(value):
            return value
    return None


def _source_accession(selected: Sequence[Mapping[str, Any]], available_at: Any) -> Any | None:
    if not selected:
        return None
    matching = [row for row in selected if _present(row.get("available_at")) and row.get("available_at") == available_at]
    rows = matching or list(selected)
    return _first_present(row.get("source_accession") or row.get("accession_number") for row in rows)


def _filed_date(selected: Sequence[Mapping[str, Any]], available_at: Any) -> Any | None:
    if not selected:
        return None
    matching = [row for row in selected if _present(row.get("available_at")) and row.get("available_at") == available_at]
    rows = matching or list(selected)
    return _first_present(row.get("filed_date") for row in rows)


def _standardized_record(
    *,
    source: str,
    run_id: str | None,
    rule: StandardizationRule,
    value: float,
    selected: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    anchor = selected[0]
    available_at = _max_present(row.get("available_at") for row in selected)
    period_end = anchor.get("period_end")
    input_codes = [_candidate_code(row) for row in selected]
    input_item_ids = sorted(
        {
            int(row["item_id"])
            for row in selected
            if _present(row.get("item_id"))
        }
    )
    return {
        "standardized_id": _stable_id(
            source,
            anchor.get("security_id"),
            rule.item_id,
            rule.basis,
            period_end,
            available_at,
            rule.rule_id,
        ),
        "source": source,
        "upstream_source": _first_present(row.get("upstream_source") for row in selected),
        "security_id": anchor.get("security_id"),
        "symbol": _first_present(row.get("symbol") for row in selected),
        "cik": _first_present(row.get("cik") for row in selected),
        "item_id": rule.item_id,
        "canonical_code": rule.canonical_code,
        "basis": rule.basis,
        "period_start": _first_present(row.get("period_start") for row in selected),
        "period_end": period_end,
        "fiscal_year": _first_present(row.get("fiscal_year") for row in selected),
        "fiscal_period": _first_present(row.get("fiscal_period") for row in selected),
        "value": value,
        "unit_type": _first_present(row.get("unit_type") for row in selected),
        "source_accession": _source_accession(selected, available_at),
        "filed_date": _filed_date(selected, available_at),
        "as_of_date": period_end,
        "available_at": available_at,
        "input_codes_json": json_dumps(input_codes),
        "input_item_ids_json": json_dumps(input_item_ids),
        "rule_id": rule.rule_id,
        "combination_rule": rule.combination_rule,
        "is_latest_revision": True,
        "run_id": run_id,
    }


def _active_rules_by_group(
    rules: Iterable[StandardizationRule],
    period_end: Any,
) -> dict[tuple[str, int], StandardizationRule]:
    out: dict[tuple[str, int], StandardizationRule] = {}
    for rule in rules:
        if _rule_is_live(rule, period_end):
            out[(rule.basis, rule.item_id)] = rule
    return out


def compute_standardized_rows(
    inputs: pd.DataFrame,
    *,
    rules: Iterable[StandardizationRule] | None = None,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: long candidate facts -> standardized long facts."""

    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=STANDARDIZED_COLUMNS)

    resolved_rules = tuple(rules or default_standardization_rules())
    records: list[dict[str, Any]] = []
    key_columns = ["security_id", "period_end", "basis"]
    sortable = inputs.copy()
    sortable["available_at"] = pd.to_datetime(sortable["available_at"], errors="coerce")
    for (_, period_end, _), group in sortable.groupby(key_columns, dropna=False, sort=False):
        group_rows = cast(list[Mapping[str, Any]], group.to_dict("records"))
        rule_map = _active_rules_by_group(resolved_rules, period_end)
        for rule in sorted(rule_map.values(), key=lambda r: (r.item_id, r.rule_id)):
            if not any(row.get("basis") == rule.basis for row in group_rows):
                continue
            basis_rows = [row for row in group_rows if row.get("basis") == rule.basis]
            selected = _select_inputs(basis_rows, rule)
            if selected is None:
                continue
            value, selected_rows = selected
            records.append(
                _standardized_record(
                    source=source,
                    run_id=run_id,
                    rule=rule,
                    value=value,
                    selected=selected_rows,
                )
            )
    if not records:
        return pd.DataFrame(columns=STANDARDIZED_COLUMNS)
    return pd.DataFrame(records, columns=STANDARDIZED_COLUMNS)


def compute_standardization_exceptions(
    inputs: pd.DataFrame,
    *,
    rules: Iterable[StandardizationRule] | None = None,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: candidate facts that cannot route to an active rule."""

    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=EXCEPTION_COLUMNS)

    resolved_rules = tuple(rules or default_standardization_rules())
    active_keys = {
        (rule.basis, rule.item_id)
        for rule in resolved_rules
        if rule.is_active
    }
    records: list[dict[str, Any]] = []
    for row in inputs.to_dict("records"):
        item_id = row.get("item_id")
        if not _present(item_id):
            reason = "unmapped_concept"
        elif (str(row.get("basis")), int(cast(Any, item_id))) not in active_keys:
            reason = "no_active_standardization_rule"
        else:
            continue
        period_end = row.get("period_end")
        concept = row.get("concept") if _present(row.get("concept")) else row.get("canonical_metric")
        taxonomy = row.get("taxonomy")
        records.append(
            {
                "exception_id": _stable_id(
                    source,
                    row.get("security_id"),
                    row.get("basis"),
                    period_end,
                    row.get("accession_number"),
                    taxonomy,
                    concept,
                    reason,
                ),
                "source": source,
                "upstream_source": row.get("upstream_source"),
                "security_id": row.get("security_id"),
                "symbol": row.get("symbol"),
                "cik": row.get("cik"),
                "basis": row.get("basis"),
                "period_start": row.get("period_start"),
                "period_end": period_end,
                "accession_number": row.get("accession_number"),
                "concept": concept,
                "taxonomy": taxonomy,
                "unit": row.get("unit"),
                "value": row.get("value"),
                "reason": reason,
                "as_of_date": period_end,
                "available_at": row.get("available_at"),
                "run_id": run_id,
            }
        )
    if not records:
        return pd.DataFrame(columns=EXCEPTION_COLUMNS)
    return pd.DataFrame(records, columns=EXCEPTION_COLUMNS)


def compute_standardization_result(
    inputs: pd.DataFrame,
    *,
    rules: Iterable[StandardizationRule] | None = None,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> StandardizationResult:
    resolved_rules = tuple(rules or default_standardization_rules())
    standardized = compute_standardized_rows(inputs, rules=resolved_rules, source=source, run_id=run_id)
    exceptions = compute_standardization_exceptions(inputs, rules=resolved_rules, source=source, run_id=run_id)
    return StandardizationResult(
        standardized=standardized,
        exceptions=exceptions,
        standardized_row_count=len(standardized),
        exception_row_count=len(exceptions),
        input_row_count=len(inputs),
        run_id=run_id,
    )


def _table_exists(store: DuckDBStore, table_name: str) -> bool:
    row = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name = 'main' AND table_name = ?",
        [table_name],
    ).fetchone()
    return bool(row and row[0])


def load_standardization_inputs(
    store: DuckDBStore,
    options: FundamentalStandardizationOptions,
) -> pd.DataFrame:
    """Load latest-revision candidate facts from warehouse fundamentals surfaces."""

    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    symbol_join = ""
    if symbols:
        store.con.register(
            "standardization_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        symbol_join = "JOIN standardization_symbol_filter ssf ON ssf.symbol = src.symbol"

    lookup_ctes = """
        WITH metric_map AS (
            SELECT canonical_metric, min(item_id) AS item_id
            FROM fundamental_statement_map
            WHERE item_id IS NOT NULL
            GROUP BY canonical_metric
        ),
        alias_map AS (
            SELECT alias_scheme, alias_code, min(item_id) AS item_id, min(coalesce_priority) AS input_rank
            FROM fundamental_item_alias
            GROUP BY alias_scheme, alias_code
        ),
        vendor_map AS (
            SELECT lower(vendor) AS vendor, vendor_field, min(item_id) AS item_id
            FROM fundamental_item_vendor_map
            GROUP BY lower(vendor), vendor_field
        )
    """
    queries: list[str] = []
    queries.append(
        f"""
        SELECT
            'fundamental_ttm_points' AS upstream_source,
            src.source,
            src.security_id,
            src.symbol,
            src.cik,
            coalesce(m.item_id, i.item_id) AS item_id,
            src.canonical_metric,
            src.canonical_metric AS concept,
            'warehouse' AS taxonomy,
            src.unit,
            coalesce(src.unit_type, mi.unit_type, i.unit_type) AS unit_type,
            'ttm' AS basis,
            src.ttm_start_date AS period_start,
            src.ttm_end_date AS period_end,
            src.fiscal_year,
            src.fiscal_period,
            src.accession_number,
            src.accession_number AS source_accession,
            CAST(NULL AS DATE) AS filed_date,
            src.ttm_value AS value,
            src.available_at,
            0 AS input_rank
        FROM fundamental_ttm_points src
        LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
        LEFT JOIN fundamental_item mi ON mi.item_id = m.item_id
        LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
        {symbol_join}
        WHERE src.is_latest_revision
          AND src.ttm_value IS NOT NULL
          AND src.available_at IS NOT NULL
        """
    )
    queries.append(
        f"""
        SELECT
            'fundamental_statement_points' AS upstream_source,
            src.source,
            src.security_id,
            src.symbol,
            src.cik,
            coalesce(src.item_id, m.item_id, i.item_id) AS item_id,
            src.canonical_metric,
            src.concept,
            src.taxonomy,
            src.unit,
            coalesce(src.unit_type, mi.unit_type, i.unit_type) AS unit_type,
            CASE WHEN src.period_type = 'instant' THEN 'instant' ELSE 'annual' END AS basis,
            src.period_start,
            src.period_end,
            src.fiscal_year,
            src.fiscal_period,
            src.accession_number,
            coalesce(src.source_accession, src.accession_number) AS source_accession,
            src.filed_date,
            src.value,
            src.available_at,
            0 AS input_rank
        FROM fundamental_statement_points src
        LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
        LEFT JOIN fundamental_item mi ON mi.item_id = m.item_id
        LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
        {symbol_join}
        WHERE src.is_latest_revision
          AND src.value IS NOT NULL
          AND src.available_at IS NOT NULL
        """
    )
    queries.append(
        f"""
        SELECT
            'fundamental_xbrl_metric' AS upstream_source,
            src.source,
            src.security_id,
            src.symbol,
            src.cik,
            coalesce(m.item_id, i.item_id, a.item_id, v.item_id) AS item_id,
            src.canonical_metric,
            src.concept,
            src.taxonomy,
            src.unit,
            coalesce(mi.unit_type, i.unit_type, ai.unit_type, vi.unit_type) AS unit_type,
            CASE WHEN src.period_type = 'instant' THEN 'instant' ELSE 'annual' END AS basis,
            src.period_start,
            src.period_end,
            src.fiscal_year,
            src.fiscal_period,
            src.accession_number,
            src.accession_number AS source_accession,
            CAST(NULL AS DATE) AS filed_date,
            src.value,
            src.available_at,
            coalesce(a.input_rank, 50) AS input_rank
        FROM fundamental_xbrl_metric src
        LEFT JOIN metric_map m ON m.canonical_metric = src.canonical_metric
        LEFT JOIN fundamental_item mi ON mi.item_id = m.item_id
        LEFT JOIN fundamental_item i ON i.canonical_code = src.canonical_metric
        LEFT JOIN alias_map a ON a.alias_scheme = src.taxonomy AND a.alias_code = src.concept
        LEFT JOIN fundamental_item ai ON ai.item_id = a.item_id
        LEFT JOIN vendor_map v ON v.vendor = lower(src.taxonomy) AND v.vendor_field = src.concept
        LEFT JOIN fundamental_item vi ON vi.item_id = v.item_id
        {symbol_join}
        WHERE src.is_latest_revision
          AND src.value IS NOT NULL
          AND src.available_at IS NOT NULL
        """
    )
    sql = lookup_ctes + "\n" + "\nUNION ALL\n".join(queries)
    try:
        return store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("standardization_symbol_filter")


def _delete_scope(store: DuckDBStore, table_name: str, options: FundamentalStandardizationOptions) -> None:
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    if not symbols:
        store.con.execute(f"DELETE FROM {table_name} WHERE source = ?", [options.source])
        return
    store.con.register(
        "standardization_delete_symbol_filter",
        pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
    )
    try:
        store.con.execute(
            f"""
            DELETE FROM {table_name}
            WHERE source = ?
              AND symbol IN (SELECT symbol FROM standardization_delete_symbol_filter)
            """,
            [options.source],
        )
    finally:
        store.con.unregister("standardization_delete_symbol_filter")


def refresh_fundamental_standardized(
    store: DuckDBStore,
    options: FundamentalStandardizationOptions | None = None,
) -> StandardizationResult:
    """Recompute revision-complete standardized facts for the requested scope."""

    options = options or FundamentalStandardizationOptions()
    store.initialize()
    from ._standardization_set_based import refresh_standardized_set_based

    outcome = refresh_standardized_set_based(
        store,
        options,
        default_standardization_rules(),
    )
    return StandardizationResult(
        standardized=outcome.standardized,
        exceptions=outcome.exceptions,
        standardized_row_count=outcome.standardized_row_count,
        exception_row_count=outcome.exception_row_count,
        input_row_count=outcome.input_row_count,
        build_id=outcome.build_id,
        run_id=outcome.run_id,
        rule_set_sha256=outcome.rule_set_sha256,
        basis_counts=outcome.basis_counts,
    )


class FundamentalStandardizationDataset(Dataset):
    dataset_id = "fundamental_standardized"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalStandardizationOptions) -> DatasetLoadResult:
        result = refresh_fundamental_standardized(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_standardized",
            check_name="rows_materialized",
            status="passed" if result.standardized_row_count > 0 else "warning",
            observed_value=float(result.standardized_row_count),
            threshold_value=1.0,
            details={
                "source": options.source,
                "exceptions": result.exception_row_count,
                "build_id": result.build_id,
                "rule_set_sha256": result.rule_set_sha256,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=result.standardized_row_count,
            source=options.source,
            details={
                "exceptions": result.exception_row_count,
                "build_id": result.build_id,
                "rule_set_sha256": result.rule_set_sha256,
                "basis_counts": dict(result.basis_counts or {}),
            },
        )
