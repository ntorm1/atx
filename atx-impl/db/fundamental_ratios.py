"""S9a: derived point-in-time financial ratios (Compustat/FactSet calculated items).

This module materializes ``fundamental_ratios``, a long-format fact table with one
row per (security, period, ratio_code). Ratios are computed from two already-loaded,
already-bitemporal warehouse surfaces:

* ``fundamental_ttm_points``        -- trailing-twelve-month *flow* metrics (revenue,
                                       net income, operating cash flow, capex, ...).
                                       TTM is the quant-standard, fiscal-calendar-robust
                                       flow basis and avoids the Q-vs-YTD ambiguity of
                                       raw statement flows.
* ``fundamental_statement_points``  -- *instant* balance metrics at the period close
                                       (assets, liabilities, equity, shares).

Point-in-time discipline: every ratio carries its **own** ``available_at`` equal to the
maximum availability of the specific inputs it consumes, so an as-of query returns a
ratio only once every input it depends on was knowable. ``as_of_date`` is the period
close (TTM window end). No network is touched — ratios are a pure transform of warehouse
tables, so the dataset is fully deterministic and test-safe.

The ratio math lives in :func:`compute_ratio_rows`, a pure DataFrame->DataFrame transform
that is unit-tested independently of DuckDB. The :class:`FundamentalRatiosDataset` /
:func:`refresh_fundamental_ratios` pair pivots the inputs into the wide frame the transform
expects, then writes the result.

v1 stores only the latest-revision vintage of the inputs (one ratio per period). Storing a
ratio row per restatement vintage is a planned, non-breaking refinement (the bitemporal
columns are already present).
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from functools import lru_cache
from typing import Any, Callable

import pandas as pd

from .asof import fundamental_ratios_asof  # noqa: F401  (re-exported for callers)
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .formula_library import (
    REQUIRE_POSITIVE_DENOMINATOR_RULE,
    eval_operand_term,
    load_ratio_formula_rows,
    parse_operand_expression,
    resolve_composite_evaluator,
)
from .item_registry import input_item_ids_for_ratio, ratio_input_metrics
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Derived point-in-time financial ratios"
DEFAULT_SOURCE = "derived_fundamental_ratios_v1"
DEFAULT_BASIS = "ttm"

# Raw wide-frame input key -> canonical_metric in the source tables.
#
# PF-S1 S1-3: the governed db.item_registry.ratio_input_metrics() map is now the SOLE
# authority for these strings -- no bare "key -> canonical_metric" literal dict is
# hand-typed here anymore. The returned strings are byte-identical to the pre-S1-3
# literals (see db/item_registry.py's _TTM_RATIO_INPUT_SPECS etc. and
# .superpowers/sdd/s1-3-vocab-reconciliation.md); _pivot_case's SQL string-matching
# behavior, and therefore every ratio value, is unchanged.
TTM_INPUTS = ratio_input_metrics("ttm")
BALANCE_INPUTS = ratio_input_metrics("balance")

RATIO_COLUMNS = [
    "ratio_id", "source", "upstream_source", "security_id", "symbol", "cik",
    "ratio_code", "ratio_category", "ratio_kind", "basis", "unit",
    "period_start", "period_end", "fiscal_year", "fiscal_period",
    "value", "numerator_code", "numerator_value", "denominator_code",
    "denominator_value", "is_meaningful", "is_latest_revision",
    "source_accession", "filed_date", "as_of_date", "available_at",
    "input_codes_json", "input_item_ids_json", "run_id",
]


@dataclass(frozen=True)
class RatioDef:
    code: str
    category: str          # profitability | leverage | efficiency | liquidity | cash_flow | payout | per_share | growth | health
    kind: str              # ratio | level | difference | per_share | growth | score
    unit: str              # ratio | currency | currency_per_share | score
    numerator_code: str
    denominator_code: str
    inputs: tuple[str, ...]            # raw input keys that gate availability
    operands: Callable[[dict], tuple]  # (numerator_value, denominator_value)
    require_positive_denominator: bool = False
    # For kind='score': a weighted-composite value function over the wide row
    # (bypasses operands; numerator/denominator are not stored).
    composite: Callable[[dict], float | None] | None = None
    # Optional additive provenance inputs. Does not affect gating, PIT timestamps,
    # ratio_id, or input_codes_json.
    item_inputs: tuple[str, ...] | None = None


# PF-S4 S4-1: RATIO_DEFS is now DERIVED from formula_registry (via
# db/formula_library.py's offline CSV loader) instead of a hand-written
# literal tuple. Each seed row is mechanically translated into a RatioDef:
#
# * ``transform``/``kind`` reproduce the pre-S4-1 kind exactly (the seed's
#   `kind` column IS the RatioDef.kind).
# * ``expression`` ("numerator_term|denominator_term") is interpreted by
#   formula_library.eval_operand_term -- a small closed dispatch over named
#   operand shapes (key/abs/sum/abs_sum/diff/diff_abs/diff_z/avg), never
#   eval/exec -- to reconstruct an operands callable bit-for-bit identical
#   to the original lambda's arithmetic.
# * ``is_meaningful_rule`` ("require_positive_denominator" or blank) drives
#   RatioDef.require_positive_denominator.
# * The 4 distress/quality scores carry an ``expression`` of the form
#   "composite:<code>_v1", a whitelisted dispatch key resolved by
#   formula_library.resolve_composite_evaluator to one of the 4 vetted,
#   unit-tested composite functions (still Python, but registry-driven --
#   not a free-floating lambda in a literal tuple). See the S4-0 review's
#   CRITICAL design flag and formula_library.py's module docstring.
#
# quick_ratio is the one pre-existing `item_inputs` special case (its
# provenance list includes "inventory", an optional NaN-coalesced input not
# in its gating `inputs` tuple); reproduced here from the same seed `inputs`
# column plus a fixed quick_ratio-only item_inputs override, matching the
# pre-S4-1 literal exactly.
_QUICK_RATIO_ITEM_INPUTS = ("current_assets", "inventory", "current_liabilities")
_FUNDAMENTAL_RATIO_EXCLUDED_FAMILIES = frozenset({"valuation"})


def _build_operands(numerator_term: str, denominator_term: str) -> Callable[[dict], tuple]:
    def _operands(r: dict) -> tuple:
        return eval_operand_term(numerator_term, r), eval_operand_term(denominator_term, r)

    return _operands


def _build_ratio_defs() -> tuple[RatioDef, ...]:
    defs: list[RatioDef] = []
    for row in load_ratio_formula_rows():
        # PF-S6 S6-2: valuation formulas are catalogued in formula_registry but
        # materialize through the sibling valuation_multiples engine, not here.
        if row.family in _FUNDAMENTAL_RATIO_EXCLUDED_FAMILIES:
            continue
        inputs = tuple(json.loads(row.inputs))
        require_positive_denominator = row.is_meaningful_rule == REQUIRE_POSITIVE_DENOMINATOR_RULE
        item_inputs = _QUICK_RATIO_ITEM_INPUTS if row.formula_code == "quick_ratio" else None
        # PF-S4 S4-2: route on the `expression` dispatch-key prefix, not `kind == "score"`,
        # so a composite-driven formula can carry ANY kind (e.g. DuPont decomposition is
        # kind="ratio", not "score", but is still a multi-term product not representable
        # by a single numerator/denominator operand-term pair). All 4 pre-S4-2 composites
        # (kind="score") still route here too -- this is a strict superset of the old check.
        if row.expression is not None and row.expression.startswith("composite:"):
            evaluator = resolve_composite_evaluator(row.expression)
            defs.append(RatioDef(
                row.formula_code, row.family, row.kind, row.unit,
                row.numerator_code, row.denominator_code, inputs,
                lambda r: (None, None),
                require_positive_denominator=require_positive_denominator,
                composite=evaluator,
                item_inputs=item_inputs,
            ))
            continue
        numerator_term, denominator_term = parse_operand_expression(row.expression)
        defs.append(RatioDef(
            row.formula_code, row.family, row.kind, row.unit,
            row.numerator_code, row.denominator_code, inputs,
            _build_operands(numerator_term, denominator_term),
            require_positive_denominator=require_positive_denominator,
            item_inputs=item_inputs,
        ))
    return tuple(defs)


RATIO_DEFS: tuple[RatioDef, ...] = _build_ratio_defs()

# Instant (balance) metrics sourced from the consolidated inline-XBRL extraction
# (fundamental_xbrl_metric), pivoted into the wide frame alongside statement-point balances.
# PF-S1 S1-3: sourced from the governed db.item_registry.ratio_input_metrics() map (see
# TTM_INPUTS/BALANCE_INPUTS above); strings are byte-identical to the pre-S1-3 literals.
XBRL_BALANCE_INPUTS = ratio_input_metrics("xbrl_balance")

# Annual (duration) flow metrics from the consolidated inline-XBRL extraction,
# pivoted into the wide frame and joined on the fiscal-year period_end.
# PF-S1 S1-3: sourced from the governed db.item_registry.ratio_input_metrics() map.
XBRL_FLOW_INPUTS = ratio_input_metrics("xbrl_flow")

# Metrics for which a prior-year value is paired in (for YoY growth, average-balance
# ratios, and the Piotroski F-score YoY deltas). The S10e additions (long-term debt,
# current assets/liabilities, shares, gross profit) are paired by the same ~365-day
# window mechanism in _attach_prior_year.
GROWTH_PRIOR_KEYS = (
    "rev", "ni", "oi", "ocf", "assets", "equity",
    "liabilities", "long_term_debt", "current_assets", "current_liabilities",
    "common_shares_outstanding", "gross_profit", "cost_of_revenue",
    "depreciation_amortization", "property_plant_equipment_net",
    "accounts_receivable", "selling_general_and_administrative_expense",
    # PF-S4 S4-2: cash, inventory, and accounts payable priors for the Sloan
    # working-capital-accruals composite (ΔWC needs Δcash; the accruals need
    # Δcurrent-debt proxy) and for the average-payables leg of days_payables_outstanding.
    "cash_and_equivalents", "inventory", "accounts_payable",
)


@dataclass(frozen=True)
class FundamentalRatiosOptions:
    source: str = DEFAULT_SOURCE
    basis: str = DEFAULT_BASIS
    symbols: tuple[str, ...] | None = None
    run_id: str | None = None


def _present(value: Any) -> bool:
    try:
        return not pd.isna(value)
    except (TypeError, ValueError):
        return value is not None


def _ratio_id(source: str, security_id: str, ratio_code: str, basis: str,
              period_end: Any, available_at: Any) -> str:
    payload = "|".join(str(p) for p in (source, security_id, ratio_code, basis, period_end, available_at))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


@lru_cache(maxsize=None)
def _input_item_ids_json_for_inputs(inputs: tuple[str, ...]) -> str | None:
    """Memoized item-linkage JSON for a RatioDef.inputs tuple (pure, in-memory, no I/O).

    PF-S1 S1-3 additive column: the sorted list of governed-registry item_ids the
    ratio consumed, resolved via db.item_registry.input_item_ids_for_ratio. Documented
    S1-3 gaps are simply absent from the list; an empty resolution yields
    NULL rather than "[]" so an all-unmapped ratio's linkage reads as unknown, not empty.
    """
    item_ids = input_item_ids_for_ratio(inputs)
    return json_dumps(item_ids) if item_ids else None


def _optional(value: Any) -> Any | None:
    return value if _present(value) else None


def _availability_sort_value(value: Any) -> int:
    try:
        ts = pd.Timestamp(value)
    except (TypeError, ValueError):
        return 0
    if pd.isna(ts):
        return 0
    return int(ts.value)


def _same_availability(left: Any, right: Any) -> bool:
    try:
        return pd.Timestamp(left) == pd.Timestamp(right)
    except (TypeError, ValueError):
        return left == right


def _ratio_provenance(input_keys: tuple[str, ...], rec: dict, available_at: Any) -> tuple[Any | None, Any | None]:
    candidates = []
    for ordinal, key in enumerate(input_keys):
        av = rec.get(f"{key}_av")
        if not _present(av):
            continue
        accession = _optional(rec.get(f"{key}_accession"))
        filed_date = _optional(rec.get(f"{key}_filed_date"))
        candidates.append({
            "ordinal": ordinal,
            "availability": _availability_sort_value(av),
            "is_driver": _same_availability(av, available_at),
            "has_provenance": accession is not None or filed_date is not None,
            "accession": accession,
            "filed_date": filed_date,
        })

    drivers = [c for c in candidates if c["is_driver"]]
    if drivers:
        chosen = sorted(drivers, key=lambda c: (not c["has_provenance"], c["ordinal"]))[0]
        if chosen["has_provenance"]:
            return chosen["accession"], chosen["filed_date"]

    with_provenance = [c for c in candidates if c["has_provenance"]]
    if with_provenance:
        chosen = sorted(with_provenance, key=lambda c: (-c["availability"], c["ordinal"]))[0]
        return chosen["accession"], chosen["filed_date"]
    return None, None


def _ratio_record(
    d,
    rec,
    source,
    basis,
    run_id,
    value,
    num,
    den,
    is_meaningful,
    available_at,
    source_accession,
    filed_date,
) -> dict:
    period_end = rec.get("period_end")
    return {
        "ratio_id": _ratio_id(source, rec.get("security_id"), d.code, basis, period_end, available_at),
        "source": source,
        "upstream_source": rec.get("upstream_source"),
        "security_id": rec.get("security_id"),
        "symbol": rec.get("symbol"),
        "cik": rec.get("cik"),
        "ratio_code": d.code,
        "ratio_category": d.category,
        "ratio_kind": d.kind,
        "basis": basis,
        "unit": d.unit,
        "period_start": rec.get("period_start"),
        "period_end": period_end,
        "fiscal_year": rec.get("fiscal_year"),
        "fiscal_period": rec.get("fiscal_period"),
        "value": value,
        "numerator_code": d.numerator_code,
        "numerator_value": num,
        "denominator_code": None if d.composite is not None else d.denominator_code,
        "denominator_value": den,
        "is_meaningful": is_meaningful,
        "is_latest_revision": True,
        "source_accession": source_accession,
        "filed_date": filed_date,
        "as_of_date": period_end,
        "available_at": available_at,
        "input_codes_json": json_dumps(list(d.inputs)),
        "input_item_ids_json": _input_item_ids_json_for_inputs(d.item_inputs or d.inputs),
        "run_id": run_id,
    }


def compute_ratio_rows(
    inputs: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    basis: str = DEFAULT_BASIS,
    run_id: str | None = None,
) -> pd.DataFrame:
    """Pure transform: wide per-(security, period) input frame -> long ratio rows.

    Each input column ``<key>`` carries a sibling ``<key>_av`` availability timestamp.
    A ratio is emitted only when every input it depends on (value AND availability) is
    present and the denominator is usable; its ``available_at`` is the max input
    availability and its ``as_of_date`` is the period close. Input-level
    ``<key>_accession`` and ``<key>_filed_date`` siblings identify the filing whose
    availability drives the emitted ratio.
    """
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=RATIO_COLUMNS)

    records: list[dict] = []
    for rec in inputs.to_dict("records"):
        period_end = rec.get("period_end")
        for d in RATIO_DEFS:
            if not all(_present(rec.get(k)) for k in d.inputs):
                continue
            avs = [rec.get(f"{k}_av") for k in d.inputs]
            if not all(_present(a) for a in avs):
                continue
            available_at = max(avs)
            source_accession, filed_date = _ratio_provenance(d.inputs, rec, available_at)
            if d.composite is not None:
                value = d.composite(rec)
                if not _present(value):
                    continue
                records.append(_ratio_record(
                    d, rec, source, basis, run_id, float(value), None, None, True,
                    available_at, source_accession, filed_date
                ))
                continue
            num, den = d.operands(rec)
            if not _present(num) or not _present(den):
                continue
            num = float(num)
            den = float(den)
            if d.kind == "level":
                value = num + den
                is_meaningful = True
            elif d.kind == "difference":
                value = num - den
                is_meaningful = True
            elif d.kind == "growth":
                if den == 0:
                    continue
                value = (num - den) / abs(den)
                is_meaningful = (not d.require_positive_denominator) or den > 0
            else:
                if den == 0:
                    continue
                value = num / den
                is_meaningful = (not d.require_positive_denominator) or den > 0
            records.append(_ratio_record(
                d, rec, source, basis, run_id, value, num, den, is_meaningful,
                available_at, source_accession, filed_date
            ))

    if not records:
        return pd.DataFrame(columns=RATIO_COLUMNS)
    return pd.DataFrame(records, columns=RATIO_COLUMNS)


def _as_date(value: Any) -> dt.date | None:
    if value is None or (not isinstance(value, dt.date) and pd.isna(value)):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _attach_prior_year(wide: pd.DataFrame) -> pd.DataFrame:
    """Pair each (security, period_end) row with its ~1-year-earlier row.

    For each `GROWTH_PRIOR_KEYS` metric, copies the prior period's value,
    availability, and provenance into `<key>_prior*`. The prior row is the one whose
    period_end falls 350-380 days before the current period_end (closest to 365);
    rows with no such match get NA, so dependent growth ratios are skipped.
    """
    out = wide.copy()
    keys = [k for k in GROWTH_PRIOR_KEYS if k in out.columns and f"{k}_av" in out.columns]
    for key in keys:
        out[f"{key}_prior"] = pd.NA
        out[f"{key}_prior_av"] = pd.NaT
        out[f"{key}_prior_accession"] = pd.NA
        out[f"{key}_prior_filed_date"] = pd.NaT
    if out.empty or "period_end" not in out.columns or "security_id" not in out.columns:
        return out

    ends = {idx: _as_date(pe) for idx, pe in out["period_end"].items()}
    for _, grp in out.groupby("security_id", sort=False):
        idxs = list(grp.index)
        for i in idxs:
            cur = ends.get(i)
            if cur is None:
                continue
            best, best_diff = None, None
            for j in idxs:
                prev = ends.get(j)
                if prev is None or prev >= cur:
                    continue
                gap = (cur - prev).days
                if 350 <= gap <= 380:
                    diff = abs(gap - 365)
                    if best_diff is None or diff < best_diff:
                        best, best_diff = j, diff
            if best is not None:
                for key in keys:
                    out.at[i, f"{key}_prior"] = out.at[best, key]
                    out.at[i, f"{key}_prior_av"] = out.at[best, f"{key}_av"]
                    if f"{key}_accession" in out.columns:
                        out.at[i, f"{key}_prior_accession"] = out.at[best, f"{key}_accession"]
                    if f"{key}_filed_date" in out.columns:
                        out.at[i, f"{key}_prior_filed_date"] = out.at[best, f"{key}_filed_date"]
    return out


def _pivot_case(
    prefix: str,
    value_col: str,
    metric_map: dict[str, str],
    accession_expr: str,
    filed_date_expr: str,
) -> str:
    parts = []
    for key, metric in metric_map.items():
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.{value_col} END) AS {key}"
        )
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {prefix}.available_at END) AS {key}_av"
        )
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {accession_expr} END) AS {key}_accession"
        )
        parts.append(
            f"max(CASE WHEN {prefix}.canonical_metric = '{metric}' THEN {filed_date_expr} END) AS {key}_filed_date"
        )
    return ",\n            ".join(parts)


def _wide_select(prefix: str, metric_map: dict[str, str]) -> str:
    parts = []
    for key in metric_map:
        parts.extend([
            f"{prefix}.{key}",
            f"{prefix}.{key}_av",
            f"{prefix}.{key}_accession",
            f"{prefix}.{key}_filed_date",
        ])
    return ",\n            ".join(parts)


def load_ratio_inputs(store: DuckDBStore, options: FundamentalRatiosOptions) -> pd.DataFrame:
    """Pivot the latest-revision TTM flows + instant balances into the wide input frame."""
    symbols = tuple(s for s in (options.symbols or ()) if str(s).strip())
    registered = False
    sym_join_t = ""
    sym_join_b = ""
    if symbols:
        store.con.register(
            "ratio_symbol_filter",
            pd.DataFrame({"symbol": sorted({str(s).strip().upper() for s in symbols})}),
        )
        registered = True
        sym_join_t = "JOIN ratio_symbol_filter rsf ON rsf.symbol = t.symbol"
        sym_join_b = "JOIN ratio_symbol_filter rsf ON rsf.symbol = s.symbol"
        sym_join_x = "JOIN ratio_symbol_filter rsf ON rsf.symbol = x.symbol"
    else:
        sym_join_x = ""

    sql = f"""
        WITH ttm AS (
            SELECT
                t.security_id,
                any_value(t.symbol) AS symbol,
                any_value(t.cik) AS cik,
                any_value(t.source) AS upstream_source,
                t.ttm_end_date AS period_end,
                any_value(t.ttm_start_date) AS period_start,
                any_value(t.fiscal_year) AS fiscal_year,
                any_value(t.fiscal_period) AS fiscal_period,
                {_pivot_case('t', 'ttm_value', TTM_INPUTS, 't.accession_number', 't.as_of_date')}
            FROM fundamental_ttm_points t
            {sym_join_t}
            WHERE t.is_latest_revision
            GROUP BY t.security_id, t.ttm_end_date
        ),
        bal AS (
            SELECT
                s.security_id,
                s.period_end,
                {_pivot_case('s', 'value', BALANCE_INPUTS, 'coalesce(s.source_accession, s.accession_number)', 'coalesce(s.filed_date, s.as_of_date)')}
            FROM fundamental_statement_points s
            {sym_join_b}
            WHERE s.is_latest_revision AND s.period_type = 'instant'
            GROUP BY s.security_id, s.period_end
        ),
        balx AS (
            SELECT
                x.security_id,
                x.period_end,
                {_pivot_case('x', 'value', XBRL_BALANCE_INPUTS, 'x.accession_number', 'x.as_of_date')}
            FROM fundamental_xbrl_metric x
            {sym_join_x}
            WHERE x.is_latest_revision AND x.period_type = 'instant'
            GROUP BY x.security_id, x.period_end
        ),
        flowx AS (
            SELECT
                x.security_id,
                x.period_end,
                {_pivot_case('x', 'value', XBRL_FLOW_INPUTS, 'x.accession_number', 'x.as_of_date')}
            FROM fundamental_xbrl_metric x
            {sym_join_x}
            WHERE x.is_latest_revision AND x.period_type = 'duration'
            GROUP BY x.security_id, x.period_end
        )
        SELECT
            ttm.*,
            {_wide_select('bal', BALANCE_INPUTS)},
            {_wide_select('balx', XBRL_BALANCE_INPUTS)},
            {_wide_select('flowx', XBRL_FLOW_INPUTS)}
        FROM ttm
        LEFT JOIN bal
          ON bal.security_id = ttm.security_id
         AND bal.period_end = ttm.period_end
        LEFT JOIN balx
          ON balx.security_id = ttm.security_id
         AND balx.period_end = ttm.period_end
        LEFT JOIN flowx
          ON flowx.security_id = ttm.security_id
         AND flowx.period_end = ttm.period_end
    """
    try:
        wide = store.con.execute(sql).df()
    finally:
        if registered:
            store.con.unregister("ratio_symbol_filter")
    return _attach_prior_year(wide)


def refresh_fundamental_ratios(store: DuckDBStore, options: FundamentalRatiosOptions) -> int:
    """Recompute and replace the ratio rows for ``options.source``/``options.basis``."""
    store.initialize()
    wide = load_ratio_inputs(store, options)
    rows = compute_ratio_rows(wide, source=options.source, basis=options.basis, run_id=options.run_id)
    with store.transaction():
        store.con.execute(
            "DELETE FROM fundamental_ratios WHERE source = ? AND basis = ?",
            [options.source, options.basis],
        )
        if not rows.empty:
            insert_frame(store, rows, "fundamental_ratios", "fundamental_ratios_insert")
    return int(len(rows))


class FundamentalRatiosDataset(Dataset):
    dataset_id = "fundamental_ratios"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FundamentalRatiosOptions) -> DatasetLoadResult:
        rows = refresh_fundamental_ratios(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fundamental_ratios",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "basis": options.basis},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"basis": options.basis, "ratio_codes": [d.code for d in RATIO_DEFS]},
        )
