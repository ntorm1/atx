"""PF2-S7: dimensional inline-XBRL segment surfaces.

The consolidated extractor intentionally keeps only entity-level facts. This module
mines the complementary dimensional facts and writes a separate ASC 280 segment
surface. Segment sums are noisy in practice, so reconciliation to consolidated
totals is tolerance-banded and informational: divergences are flagged, not rejected.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, quality_check


SOURCE_NAME = "sec_inline_xbrl_segments_v1"
COVERAGE_SOURCE_NAME = "segment_footnote_coverage_v1"
DEFAULT_RECONCILIATION_TOLERANCE = 0.02


SEGMENT_DIM_COLUMNS = [
    "segment_dim_id",
    "source",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "filing_context_id",
    "filing_dimension_id",
    "segment_type",
    "axis_qname",
    "axis_taxonomy",
    "axis_concept",
    "member_qname",
    "member_taxonomy",
    "member_concept",
    "member_label",
    "member_kind",
    "typed_member_value",
    "context_element",
    "segment_sic",
    "segment_naics",
    "major_customer_name",
    "major_customer_type",
    "axis_legal_status",
    "input_codes_json",
    "as_of_date",
    "available_at",
    "revision_sequence",
    "revision_count",
    "is_latest_revision",
    "run_id",
    "source_loaded_at",
]


SEGMENT_FACT_COLUMNS = [
    "segment_fact_id",
    "segment_dim_id",
    "source",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "filing_context_id",
    "filing_fact_id",
    "segment_type",
    "axis_qname",
    "member_qname",
    "member_label",
    "canonical_item",
    "consolidated_code",
    "taxonomy",
    "concept",
    "unit",
    "period_type",
    "period_start",
    "period_end",
    "instant_date",
    "value",
    "raw_value",
    "segment_sum_value",
    "consolidated_value",
    "reconciliation_difference",
    "reconciliation_relative_difference",
    "reconciliation_tolerance",
    "reconciliation_status",
    "as_of_date",
    "available_at",
    "revision_sequence",
    "revision_count",
    "is_latest_revision",
    "input_codes_json",
    "run_id",
    "source_loaded_at",
]


SEGMENT_FOOTNOTE_COVERAGE_COLUMNS = [
    "coverage_id",
    "source",
    "segment_dim_count",
    "segment_fact_count",
    "segment_reconciled_count",
    "segment_flagged_divergent_count",
    "segment_no_consolidated_count",
    "footnote_pension_count",
    "footnote_deferred_tax_count",
    "footnote_lease_count",
    "footnote_sbc_count",
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "run_id",
    "source_loaded_at",
]


SEGMENT_AXIS_TYPES = {
    "StatementBusinessSegmentsAxis": "business",
    "BusinessSegmentsAxis": "business",
    "StatementGeographicalAxis": "geographic",
    "GeographicalAreasAxis": "geographic",
    "ProductOrServiceAxis": "product",
    "ProductsAndServicesAxis": "product",
    "MajorCustomersAxis": "customer",
}


SEGMENT_CONCEPT_MAP = {
    "RevenueFromContractWithCustomerExcludingAssessedTax": ("segment_sales", "revenue"),
    "Revenues": ("segment_sales", "revenue"),
    "SalesRevenueNet": ("segment_sales", "revenue"),
    "OperatingIncomeLoss": ("segment_operating_income", "operating_income"),
    "Assets": ("segment_assets", "assets"),
    "DepreciationDepletionAndAmortization": ("segment_dep_amort", "depreciation_amortization"),
    "PaymentsToAcquirePropertyPlantAndEquipment": ("segment_capex", "capital_expenditures"),
}


@dataclass(frozen=True)
class SegmentOptions:
    source: str = SOURCE_NAME
    coverage_source: str = COVERAGE_SOURCE_NAME
    reconciliation_tolerance: float = DEFAULT_RECONCILIATION_TOLERANCE
    run_id: str | None = None


def _stable_id(*parts: object) -> str:
    payload = "|".join("" if _is_missing(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _is_missing(value: object) -> bool:
    if value is None or value is pd.NA:
        return True
    try:
        return bool(pd.isna(value))
    except (TypeError, ValueError):
        return False


def _clean(value: object) -> object | None:
    return None if _is_missing(value) else value


def _local_name(value: object) -> str:
    if _is_missing(value):
        return ""
    return str(value).split(":")[-1]


def _as_date(value: object) -> dt.date | None:
    if _is_missing(value):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    return pd.Timestamp(value).date()


def _as_ts(value: object) -> pd.Timestamp | None:
    if _is_missing(value):
        return None
    return pd.Timestamp(value)


def _json_list(*values: object) -> str:
    return json.dumps([str(value) for value in values if not _is_missing(value)], sort_keys=True)


def infer_segment_type(axis_qname: object, axis_concept: object) -> str:
    """Infer the broad ASC 280 segment family from a decoded axis."""

    for value in (axis_concept, axis_qname):
        local = _local_name(value)
        if local in SEGMENT_AXIS_TYPES:
            return SEGMENT_AXIS_TYPES[local]
    return "unclassified"


def _major_customer_name(segment_type: str, member_label: object, typed_value: object) -> str | None:
    if segment_type != "customer":
        return None
    return _clean(typed_value) or _clean(member_label)


def _prepare_segment_candidates(candidates: pd.DataFrame) -> pd.DataFrame:
    out = candidates.copy()
    out["canonical_item"] = out["concept"].map(lambda concept: SEGMENT_CONCEPT_MAP.get(str(concept), (None, None))[0])
    out["consolidated_code"] = out["concept"].map(lambda concept: SEGMENT_CONCEPT_MAP.get(str(concept), (None, None))[1])
    out = out[out["canonical_item"].notna()].copy()
    if out.empty:
        return out

    out["period_start"] = out.get("period_start").map(_as_date) if "period_start" in out else None
    out["period_end"] = out.get("period_end").map(_as_date) if "period_end" in out else None
    out["instant_date"] = out.get("instant_date").map(_as_date) if "instant_date" in out else None
    out["as_of_date"] = [
        instant if str(period_type) == "instant" and instant is not None else period_end
        for period_type, instant, period_end in zip(out["period_type"], out["instant_date"], out["period_end"])
    ]
    out["available_at"] = out["available_at"].map(_as_ts)
    out["source_loaded_at"] = out.get("source_loaded_at", out["available_at"]).map(_as_ts)
    out["value"] = pd.to_numeric(out["value"], errors="coerce")
    out = out[out["value"].notna() & pd.notna(out["as_of_date"]) & pd.notna(out["available_at"])].copy()
    out["axis_qname"] = out.get("dimension_qname")
    out["axis_taxonomy"] = out.get("dimension_taxonomy")
    out["axis_concept"] = out.get("dimension_concept")
    out["member_qname"] = out.get("member_qname")
    out["member_taxonomy"] = out.get("member_taxonomy")
    out["member_concept"] = out.get("member_concept")
    out["member_label"] = [
        _clean(label) or _clean(member_concept) or _clean(member_qname) or _clean(typed)
        for label, member_concept, member_qname, typed in zip(
            out.get("member_text"),
            out.get("member_concept"),
            out.get("member_qname"),
            out.get("typed_member_value"),
        )
    ]
    out["segment_type"] = [
        infer_segment_type(axis_qname, axis_concept)
        for axis_qname, axis_concept in zip(out["axis_qname"], out["axis_concept"])
    ]
    out["major_customer_name"] = [
        _major_customer_name(segment_type, member_label, typed)
        for segment_type, member_label, typed in zip(
            out["segment_type"], out["member_label"], out.get("typed_member_value")
        )
    ]
    out["major_customer_type"] = [
        _clean(member_concept) if segment_type == "customer" else None
        for segment_type, member_concept in zip(out["segment_type"], out.get("member_concept"))
    ]
    return out


def _revision_columns(
    frame: pd.DataFrame,
    key_columns: list[str],
    *,
    sequence_column: str = "revision_sequence",
) -> pd.DataFrame:
    if frame.empty:
        return frame
    sort_columns = key_columns + ["available_at", "accession_number"]
    if "filing_fact_id" in frame.columns:
        sort_columns.append("filing_fact_id")
    frame = frame.sort_values(sort_columns).reset_index(drop=True)
    grouped = frame.groupby(key_columns, dropna=False)
    frame[sequence_column] = grouped.cumcount() + 1
    frame["revision_count"] = grouped[sequence_column].transform("max")
    frame["is_latest_revision"] = frame[sequence_column] == frame["revision_count"]
    return frame


def _consolidated_lookup(consolidated: pd.DataFrame | None) -> dict[tuple[object, ...], float]:
    if consolidated is None or consolidated.empty:
        return {}
    cons = consolidated.copy()
    code_col = "canonical_code" if "canonical_code" in cons.columns else "canonical_metric"
    if code_col not in cons.columns:
        return {}
    cons["consolidated_code"] = cons[code_col]
    cons["period_start_key"] = cons.get("period_start").map(lambda value: "" if _is_missing(value) else str(_as_date(value)))
    cons["as_of_date"] = cons.get("period_end", cons.get("as_of_date")).map(_as_date)
    cons["available_at"] = cons.get("available_at", pd.Timestamp("1970-01-01")).map(_as_ts)
    if "is_latest_revision" in cons.columns:
        cons = cons[cons["is_latest_revision"].fillna(True).astype(bool)].copy()
    cons = cons[cons["value"].notna() & cons["as_of_date"].notna()].copy()
    if cons.empty:
        return {}
    cons = cons.sort_values(
        ["security_id", "consolidated_code", "period_start_key", "as_of_date", "available_at"]
    )
    cons = cons.drop_duplicates(
        ["security_id", "consolidated_code", "period_start_key", "as_of_date"],
        keep="last",
    )
    return {
        (row.security_id, row.consolidated_code, row.period_start_key, row.as_of_date): float(row.value)
        for row in cons.itertuples(index=False)
    }


def _reconciliation_frame(
    facts: pd.DataFrame,
    consolidated: pd.DataFrame | None,
    *,
    tolerance: float,
) -> pd.DataFrame:
    if facts.empty:
        return facts
    out = facts.copy()
    out["period_start_key"] = out["period_start"].map(lambda value: "" if _is_missing(value) else str(value))
    segment_sums = (
        out.groupby(
            [
                "security_id",
                "canonical_item",
                "consolidated_code",
                "period_start_key",
                "as_of_date",
                "accession_number",
            ],
            dropna=False,
        )["value"]
        .sum()
        .reset_index()
        .rename(columns={"value": "segment_sum_value"})
    )
    out = out.merge(
        segment_sums,
        on=[
            "security_id",
            "canonical_item",
            "consolidated_code",
            "period_start_key",
            "as_of_date",
            "accession_number",
        ],
        how="left",
    )
    lookup = _consolidated_lookup(consolidated)
    consolidated_values: list[float | None] = []
    diffs: list[float | None] = []
    rel_diffs: list[float | None] = []
    statuses: list[str] = []
    for row in out.itertuples(index=False):
        cons_value = lookup.get((row.security_id, row.consolidated_code, row.period_start_key, row.as_of_date))
        consolidated_values.append(cons_value)
        if cons_value is None:
            diffs.append(None)
            rel_diffs.append(None)
            statuses.append("no_consolidated")
            continue
        diff = float(row.segment_sum_value) - float(cons_value)
        rel = abs(diff) / max(abs(float(cons_value)), 1.0)
        diffs.append(diff)
        rel_diffs.append(rel)
        statuses.append("reconciled" if abs(diff) <= tolerance * max(abs(float(cons_value)), 1.0) else "flagged_divergent")
    out["consolidated_value"] = consolidated_values
    out["reconciliation_difference"] = diffs
    out["reconciliation_relative_difference"] = rel_diffs
    out["reconciliation_tolerance"] = float(tolerance)
    out["reconciliation_status"] = statuses
    return out.drop(columns=["period_start_key"])


def compute_segment_rows(
    candidates: pd.DataFrame,
    consolidated: pd.DataFrame | None = None,
    *,
    source: str = SOURCE_NAME,
    reconciliation_tolerance: float = DEFAULT_RECONCILIATION_TOLERANCE,
    run_id: str | None = None,
) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Return deterministic segment_dim and segment_fact rows from dimensional facts."""

    prepared = _prepare_segment_candidates(candidates)
    if prepared.empty:
        return (
            pd.DataFrame(columns=SEGMENT_DIM_COLUMNS),
            pd.DataFrame(columns=SEGMENT_FACT_COLUMNS),
    )

    prepared["segment_dim_id"] = [
        _stable_id(source, security_id, context_id, axis, member, typed)
        for security_id, context_id, axis, member, typed in zip(
            prepared["security_id"],
            prepared["filing_context_id"],
            prepared["axis_qname"],
            prepared["member_qname"],
            prepared.get("typed_member_value"),
        )
    ]
    dim = prepared.drop_duplicates(["segment_dim_id"]).copy()
    dim["source"] = source
    dim["input_codes_json"] = [
        _json_list(axis, member, typed, dimension_id)
        for axis, member, typed, dimension_id in zip(
            dim["axis_qname"],
            dim["member_qname"],
            dim.get("typed_member_value"),
            dim["filing_dimension_id"],
        )
    ]
    dim["segment_sic"] = dim.get("segment_sic", None)
    dim["segment_naics"] = dim.get("segment_naics", None)
    dim["axis_legal_status"] = dim.get("axis_legal_status", "unchecked")
    dim["run_id"] = run_id
    dim = _revision_columns(
        dim,
        ["source", "security_id", "axis_qname", "member_qname", "typed_member_value", "as_of_date"],
    )
    dim = dim[SEGMENT_DIM_COLUMNS]

    facts = prepared.copy()
    facts["source"] = source
    facts["unit"] = facts.get("unit")
    facts["input_codes_json"] = [
        _json_list(axis, member, concept, fact_id)
        for axis, member, concept, fact_id in zip(
            facts["axis_qname"],
            facts["member_qname"],
            facts["concept"],
            facts["filing_fact_id"],
        )
    ]
    facts["run_id"] = run_id
    facts = _reconciliation_frame(
        facts,
        consolidated,
        tolerance=reconciliation_tolerance,
    )
    facts["segment_fact_id"] = [
        _stable_id(source, segment_dim_id, canonical_item, as_of_date, accession, fact_id)
        for segment_dim_id, canonical_item, as_of_date, accession, fact_id in zip(
            facts["segment_dim_id"],
            facts["canonical_item"],
            facts["as_of_date"],
            facts["accession_number"],
            facts["filing_fact_id"],
        )
    ]
    facts = _revision_columns(
        facts,
        ["source", "security_id", "segment_dim_id", "canonical_item", "period_start", "as_of_date"],
    )
    facts = facts[SEGMENT_FACT_COLUMNS]
    return dim, facts


def _fetch_segment_candidates(store: DuckDBStore) -> pd.DataFrame:
    concepts = tuple(SEGMENT_CONCEPT_MAP)
    if not concepts:
        return pd.DataFrame()
    placeholders = ", ".join(["?"] * len(concepts))
    return store.con.execute(
        f"""
        SELECT
            f.filing_fact_id,
            f.filing_context_id,
            d.filing_dimension_id,
            f.security_id,
            s.primary_symbol AS symbol,
            f.cik,
            f.accession_number,
            f.form,
            f.filing_date,
            f.acceptance_datetime,
            f.primary_document,
            ctx.context_id,
            ctx.period_type,
            ctx.period_start,
            ctx.period_end,
            ctx.instant_date,
            ctx.dimension_count,
            d.context_element,
            d.member_kind,
            d.dimension_qname,
            d.dimension_taxonomy,
            d.dimension_concept,
            d.member_qname,
            d.member_taxonomy,
            d.member_concept,
            d.typed_member_value,
            d.member_text,
            d.ordinal,
            f.concept,
            f.taxonomy,
            f.unit_ref AS unit,
            f.numeric_value AS value,
            f.raw_value,
            f.source_url,
            coalesce(ctx.acceptance_datetime, f.acceptance_datetime, ctx.filing_date::TIMESTAMP) AS available_at,
            f.run_id AS upstream_run_id,
            greatest(
                coalesce(f.source_loaded_at, TIMESTAMP '1970-01-01'),
                coalesce(ctx.source_loaded_at, TIMESTAMP '1970-01-01'),
                coalesce(d.source_loaded_at, TIMESTAMP '1970-01-01')
            ) AS source_loaded_at
        FROM xbrl_filing_facts f
        JOIN xbrl_filing_contexts ctx ON ctx.filing_context_id = f.filing_context_id
        JOIN xbrl_filing_dimensions d ON d.filing_context_id = ctx.filing_context_id
        LEFT JOIN securities s ON s.security_id = f.security_id
        WHERE f.taxonomy = 'us-gaap'
          AND f.is_numeric
          AND f.numeric_value IS NOT NULL
          AND coalesce(ctx.dimension_count, 0) > 0
          AND f.concept IN ({placeholders})
        """,
        list(concepts),
    ).df()


def _fetch_consolidated_totals(store: DuckDBStore) -> pd.DataFrame:
    codes = sorted({code for _item, code in SEGMENT_CONCEPT_MAP.values()})
    placeholders = ", ".join(["?"] * len(codes))
    frames: list[pd.DataFrame] = []
    frames.append(
        store.con.execute(
            f"""
            SELECT
                security_id,
                symbol,
                cik,
                canonical_code,
                period_start,
                period_end,
                basis,
                value,
                available_at,
                is_latest_revision
            FROM fundamental_standardized
            WHERE canonical_code IN ({placeholders})
            """,
            codes,
        ).df()
    )
    frames.append(
        store.con.execute(
            f"""
            SELECT
                security_id,
                symbol,
                cik,
                canonical_metric AS canonical_code,
                period_start,
                period_end,
                period_type AS basis,
                value,
                available_at,
                is_latest_revision
            FROM fundamental_xbrl_metric
            WHERE canonical_metric IN ({placeholders})
            """,
            codes,
        ).df()
    )
    return pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()


def refresh_segments(
    store: DuckDBStore,
    options: SegmentOptions | None = None,
) -> dict[str, int]:
    """Refresh segment_dim and segment_fact from dimensional inline-XBRL facts."""

    options = options or SegmentOptions()
    store.initialize()
    candidates = _fetch_segment_candidates(store)
    consolidated = _fetch_consolidated_totals(store)
    segment_dim, segment_fact = compute_segment_rows(
        candidates,
        consolidated,
        source=options.source,
        reconciliation_tolerance=options.reconciliation_tolerance,
        run_id=options.run_id,
    )
    with store.transaction():
        store.con.execute("DELETE FROM segment_fact WHERE source = ?", [options.source])
        store.con.execute("DELETE FROM segment_dim WHERE source = ?", [options.source])
        insert_frame(store, segment_dim, "segment_dim", "_segment_dim_rows")
        insert_frame(store, segment_fact, "segment_fact", "_segment_fact_rows")
    return {"segment_dim_rows": int(len(segment_dim)), "segment_fact_rows": int(len(segment_fact))}


def refresh_segment_footnote_coverage(
    store: DuckDBStore,
    options: SegmentOptions | None = None,
) -> int:
    """Refresh the segment/footnote coverage report row."""

    options = options or SegmentOptions()
    source = options.coverage_source
    with store.transaction():
        store.con.execute("DELETE FROM segment_footnote_coverage WHERE source = ?", [source])
        store.con.execute(
            """
            INSERT INTO segment_footnote_coverage (
                coverage_id,
                source,
                segment_dim_count,
                segment_fact_count,
                segment_reconciled_count,
                segment_flagged_divergent_count,
                segment_no_consolidated_count,
                footnote_pension_count,
                footnote_deferred_tax_count,
                footnote_lease_count,
                footnote_sbc_count,
                as_of_date,
                available_at,
                is_latest_revision,
                run_id,
                source_loaded_at
            )
            SELECT
                sha256(concat_ws('|', ?, CAST(current_date AS VARCHAR))) AS coverage_id,
                ? AS source,
                (SELECT count(*) FROM segment_dim WHERE is_latest_revision) AS segment_dim_count,
                (SELECT count(*) FROM segment_fact WHERE is_latest_revision) AS segment_fact_count,
                (SELECT count(*) FROM segment_fact WHERE is_latest_revision AND reconciliation_status = 'reconciled') AS segment_reconciled_count,
                (SELECT count(*) FROM segment_fact WHERE is_latest_revision AND reconciliation_status = 'flagged_divergent') AS segment_flagged_divergent_count,
                (SELECT count(*) FROM segment_fact WHERE is_latest_revision AND reconciliation_status = 'no_consolidated') AS segment_no_consolidated_count,
                (SELECT count(*) FROM footnote_pension WHERE is_latest_revision) AS footnote_pension_count,
                (SELECT count(*) FROM footnote_deferred_tax WHERE is_latest_revision) AS footnote_deferred_tax_count,
                (SELECT count(*) FROM footnote_lease WHERE is_latest_revision) AS footnote_lease_count,
                (SELECT count(*) FROM footnote_sbc WHERE is_latest_revision) AS footnote_sbc_count,
                current_date AS as_of_date,
                now() AS available_at,
                true AS is_latest_revision,
                ? AS run_id,
                now() AS source_loaded_at
            """,
            [source, source, options.run_id],
        )
    return int(
        store.con.execute(
            "SELECT count(*) FROM segment_footnote_coverage WHERE source = ?",
            [source],
        ).fetchone()[0]
    )


def run_segment_refresh(
    store: DuckDBStore,
    options: SegmentOptions | None = None,
) -> dict[str, Any]:
    """Refresh segments, coverage, and record the divergence warning summary."""

    options = options or SegmentOptions()
    details = refresh_segments(store, options)
    coverage_rows = refresh_segment_footnote_coverage(store, options)
    flagged = store.con.execute(
        """
        SELECT count(*)
        FROM segment_fact
        WHERE source = ?
          AND is_latest_revision
          AND reconciliation_status = 'flagged_divergent'
        """,
        [options.source],
    ).fetchone()[0]
    status = "passed" if int(flagged) == 0 else "warning"
    quality_check(
        store,
        dataset_id="segments",
        table_name="segment_fact",
        check_name="segment_reconciliation_divergence_warning",
        status=status,
        observed_value=float(flagged),
        threshold_value=0.0,
        details={**details, "coverage_rows": coverage_rows, "source": options.source},
    )
    return {**details, "coverage_rows": coverage_rows, "flagged_divergent_rows": int(flagged)}


class SegmentDataset(Dataset):
    dataset_id = "segments"
    source_name = SOURCE_NAME
    depends_on = ("xbrl_filing_contexts", "fundamental_xbrl_metric")

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: SegmentOptions) -> DatasetLoadResult:
        details = run_segment_refresh(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(details["segment_dim_rows"]) + int(details["segment_fact_rows"]),
            source=options.source,
            details=details,
            run_id=options.run_id,
        )
