"""PF2-S7: dimensional inline-XBRL footnote sub-ledgers.

The module normalizes selected dimensional pension/OPEB, deferred-tax, lease, and
stock-compensation facts into separate PIT-safe sub-ledgers. It intentionally captures
aggregate and award-type SBC only; executive-compensation grant detail is a different
product surface.
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


SOURCE_NAME = "sec_inline_xbrl_footnotes_v1"


COMMON_COLUMNS = [
    "source",
    "security_id",
    "symbol",
    "cik",
    "accession_number",
    "filing_context_id",
    "filing_fact_id",
    "axis_qname",
    "member_qname",
    "member_label",
    "period_type",
    "period_start",
    "period_end",
    "instant_date",
    "taxonomy",
    "concept",
    "unit",
    "value",
    "raw_value",
    "as_of_date",
    "available_at",
    "revision_sequence",
    "revision_count",
    "is_latest_revision",
    "input_codes_json",
    "run_id",
    "source_loaded_at",
]


FOOTNOTE_PENSION_COLUMNS = ["pension_id", "plan_type", "line_item", *COMMON_COLUMNS]
FOOTNOTE_DEFERRED_TAX_COLUMNS = ["deferred_tax_id", "tax_component", "line_item", *COMMON_COLUMNS]
FOOTNOTE_LEASE_COLUMNS = ["lease_id", "lease_standard", "lease_class", "maturity_bucket", "line_item", *COMMON_COLUMNS]
FOOTNOTE_SBC_COLUMNS = ["sbc_id", "award_type", "line_item", *COMMON_COLUMNS]


PENSION_CONCEPTS = {
    "DefinedBenefitPlanBenefitObligation": "benefit_obligation",
    "DefinedBenefitPlanFairValueOfPlanAssets": "fair_value_plan_assets",
    "DefinedBenefitPlanFundedStatusOfPlan": "funded_status",
    "DefinedBenefitPlanServiceCost": "service_cost",
    "DefinedBenefitPlanInterestCost": "interest_cost",
    "DefinedBenefitPlanAssumptionsUsedCalculatingBenefitObligationDiscountRate": "discount_rate",
    "DefinedBenefitPlanExpectedLongTermRateOfReturnOnAssets": "expected_return_rate",
}

DEFERRED_TAX_CONCEPTS = {
    "DeferredTaxAssetsNet": "deferred_tax_assets_net",
    "DeferredTaxLiabilities": "deferred_tax_liabilities",
    "DeferredIncomeTaxExpenseBenefit": "deferred_income_tax_expense_benefit",
    "CurrentIncomeTaxExpenseBenefit": "current_income_tax_expense_benefit",
    "DeferredTaxAssetsTaxCreditCarryforwards": "tax_credit_carryforwards",
    "DeferredTaxAssetsOperatingLossCarryforwards": "operating_loss_carryforwards",
    "DeferredTaxAssetsValuationAllowance": "valuation_allowance",
}

LEASE_CONCEPTS = {
    "OperatingLeaseLiability": "operating_lease_liability",
    "OperatingLeaseRightOfUseAsset": "operating_lease_rou_asset",
    "FinanceLeaseLiability": "finance_lease_liability",
    "FinanceLeaseRightOfUseAsset": "finance_lease_rou_asset",
    "OperatingLeasesFutureMinimumPaymentsDue": "operating_lease_future_minimum_payments_due",
    "OperatingLeasesFutureMinimumPaymentsDueCurrent": "operating_lease_future_minimum_payments_due_current",
    "OperatingLeasesFutureMinimumPaymentsDueThereafter": "operating_lease_future_minimum_payments_due_thereafter",
    "CapitalLeasesFutureMinimumPaymentsDue": "capital_lease_future_minimum_payments_due",
    "FinanceLeaseLiabilityPaymentsDue": "finance_lease_liability_payments_due",
}

SBC_CONCEPTS = {
    "ShareBasedCompensation": "share_based_compensation_expense",
    "AllocatedShareBasedCompensationExpense": "allocated_share_based_compensation_expense",
    "EmployeeServiceShareBasedCompensationTaxBenefitFromCompensationExpense": "share_based_compensation_tax_benefit",
}

ALL_FOOTNOTE_CONCEPTS = {
    *PENSION_CONCEPTS,
    *DEFERRED_TAX_CONCEPTS,
    *LEASE_CONCEPTS,
    *SBC_CONCEPTS,
}


@dataclass(frozen=True)
class FootnoteOptions:
    source: str = SOURCE_NAME
    run_id: str | None = None


def _is_missing(value: object) -> bool:
    if value is None or value is pd.NA:
        return True
    try:
        return bool(pd.isna(value))
    except (TypeError, ValueError):
        return False


def _clean(value: object) -> object | None:
    return None if _is_missing(value) else value


def _stable_id(*parts: object) -> str:
    payload = "|".join("" if _is_missing(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


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


def _plan_type(member: object) -> str:
    local = _local_name(member)
    if "OtherPostretirementBenefit" in local or "Postretirement" in local or "OPEB" in local.upper():
        return "opeb"
    if "Pension" in local or "Retirement" in local:
        return "pension"
    return "other"


def _award_type(member: object) -> str:
    local = _local_name(member)
    if not local:
        return "aggregate"
    return local.replace("Member", "")


def _lease_standard(concept: str) -> str:
    if concept.startswith(("OperatingLeaseLiability", "OperatingLeaseRightOfUseAsset", "FinanceLease")):
        return "asc_842"
    return "pre_842"


def _lease_class(concept: str, member: object) -> str:
    local_member = _local_name(member)
    if local_member:
        return local_member.replace("Member", "")
    if concept.startswith("FinanceLease"):
        return "finance"
    if concept.startswith("CapitalLease"):
        return "capital"
    return "operating"


def _lease_bucket(concept: str) -> str | None:
    if concept.endswith("Current"):
        return "current"
    if concept.endswith("Thereafter"):
        return "thereafter"
    if "PaymentsDue" in concept or "FutureMinimumPaymentsDue" in concept:
        return "total"
    return None


def _prepare_candidates(candidates: pd.DataFrame, *, source: str, run_id: str | None) -> pd.DataFrame:
    if candidates is None or candidates.empty:
        return pd.DataFrame()
    out = candidates.copy()
    out = out[out["concept"].isin(ALL_FOOTNOTE_CONCEPTS)].copy()
    if out.empty:
        return out
    out["source"] = source
    out["run_id"] = run_id
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
    out["member_qname"] = out.get("member_qname")
    out["member_label"] = [
        _clean(label) or _clean(member_concept) or _clean(member_qname) or _clean(typed)
        for label, member_concept, member_qname, typed in zip(
            out.get("member_text"),
            out.get("member_concept"),
            out.get("member_qname"),
            out.get("typed_member_value"),
        )
    ]
    out["unit"] = out.get("unit")
    out["input_codes_json"] = [
        _json_list(axis, member, concept, fact_id)
        for axis, member, concept, fact_id in zip(
            out["axis_qname"], out["member_qname"], out["concept"], out["filing_fact_id"]
        )
    ]
    return out


def _revision_columns(frame: pd.DataFrame, key_columns: list[str]) -> pd.DataFrame:
    if frame.empty:
        return frame
    frame = frame.sort_values(key_columns + ["available_at", "accession_number", "filing_fact_id"]).reset_index(drop=True)
    grouped = frame.groupby(key_columns, dropna=False)
    frame["revision_sequence"] = grouped.cumcount() + 1
    frame["revision_count"] = grouped["revision_sequence"].transform("max")
    frame["is_latest_revision"] = frame["revision_sequence"] == frame["revision_count"]
    return frame


def _family_frame(
    rows: pd.DataFrame,
    *,
    id_column: str,
    id_parts: list[str],
    extra_key_columns: list[str],
    columns: list[str],
) -> pd.DataFrame:
    if rows.empty:
        return pd.DataFrame(columns=columns)
    rows[id_column] = [
        _stable_id(*[row.get(part) for part in id_parts])
        for row in rows.to_dict("records")
    ]
    rows = _revision_columns(
        rows,
        ["source", "security_id", *extra_key_columns, "period_start", "as_of_date"],
    )
    return rows[columns]


def compute_footnote_rows(
    candidates: pd.DataFrame,
    *,
    source: str = SOURCE_NAME,
    run_id: str | None = None,
) -> dict[str, pd.DataFrame]:
    """Return deterministic footnote sub-ledger rows keyed by table name."""

    prepared = _prepare_candidates(candidates, source=source, run_id=run_id)
    empty = {
        "footnote_pension": pd.DataFrame(columns=FOOTNOTE_PENSION_COLUMNS),
        "footnote_deferred_tax": pd.DataFrame(columns=FOOTNOTE_DEFERRED_TAX_COLUMNS),
        "footnote_lease": pd.DataFrame(columns=FOOTNOTE_LEASE_COLUMNS),
        "footnote_sbc": pd.DataFrame(columns=FOOTNOTE_SBC_COLUMNS),
    }
    if prepared.empty:
        return empty

    pension = prepared[prepared["concept"].isin(PENSION_CONCEPTS)].copy()
    if not pension.empty:
        pension["plan_type"] = pension["member_qname"].map(_plan_type)
        pension["line_item"] = pension["concept"].map(PENSION_CONCEPTS)
    deferred_tax = prepared[prepared["concept"].isin(DEFERRED_TAX_CONCEPTS)].copy()
    if not deferred_tax.empty:
        deferred_tax["tax_component"] = [
            _clean(member_label) or _clean(member_qname) or "aggregate"
            for member_label, member_qname in zip(deferred_tax["member_label"], deferred_tax["member_qname"])
        ]
        deferred_tax["line_item"] = deferred_tax["concept"].map(DEFERRED_TAX_CONCEPTS)
    lease = prepared[prepared["concept"].isin(LEASE_CONCEPTS)].copy()
    if not lease.empty:
        lease["lease_standard"] = lease["concept"].map(_lease_standard)
        lease["lease_class"] = [
            _lease_class(concept, member)
            for concept, member in zip(lease["concept"], lease["member_qname"])
        ]
        lease["maturity_bucket"] = lease["concept"].map(_lease_bucket)
        lease["line_item"] = lease["concept"].map(LEASE_CONCEPTS)
    sbc = prepared[prepared["concept"].isin(SBC_CONCEPTS)].copy()
    if not sbc.empty:
        sbc["award_type"] = sbc["member_qname"].map(_award_type)
        sbc["line_item"] = sbc["concept"].map(SBC_CONCEPTS)

    return {
        "footnote_pension": _family_frame(
            pension,
            id_column="pension_id",
            id_parts=["source", "security_id", "plan_type", "line_item", "as_of_date", "accession_number", "filing_fact_id"],
            extra_key_columns=["plan_type", "line_item"],
            columns=FOOTNOTE_PENSION_COLUMNS,
        ),
        "footnote_deferred_tax": _family_frame(
            deferred_tax,
            id_column="deferred_tax_id",
            id_parts=["source", "security_id", "tax_component", "line_item", "as_of_date", "accession_number", "filing_fact_id"],
            extra_key_columns=["tax_component", "line_item"],
            columns=FOOTNOTE_DEFERRED_TAX_COLUMNS,
        ),
        "footnote_lease": _family_frame(
            lease,
            id_column="lease_id",
            id_parts=["source", "security_id", "lease_class", "maturity_bucket", "line_item", "as_of_date", "accession_number", "filing_fact_id"],
            extra_key_columns=["lease_class", "maturity_bucket", "line_item"],
            columns=FOOTNOTE_LEASE_COLUMNS,
        ),
        "footnote_sbc": _family_frame(
            sbc,
            id_column="sbc_id",
            id_parts=["source", "security_id", "award_type", "line_item", "as_of_date", "accession_number", "filing_fact_id"],
            extra_key_columns=["award_type", "line_item"],
            columns=FOOTNOTE_SBC_COLUMNS,
        ),
    }


def _fetch_footnote_candidates(store: DuckDBStore) -> pd.DataFrame:
    concepts = sorted(ALL_FOOTNOTE_CONCEPTS)
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
            f.concept,
            f.taxonomy,
            f.unit_ref AS unit,
            f.numeric_value AS value,
            f.raw_value,
            f.source_url,
            coalesce(ctx.acceptance_datetime, f.acceptance_datetime, ctx.filing_date::TIMESTAMP) AS available_at,
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
        concepts,
    ).df()


def refresh_footnotes(
    store: DuckDBStore,
    options: FootnoteOptions | None = None,
) -> dict[str, int]:
    """Refresh all S7 footnote sub-ledgers from dimensional inline-XBRL facts."""

    options = options or FootnoteOptions()
    store.initialize()
    rows_by_table = compute_footnote_rows(
        _fetch_footnote_candidates(store),
        source=options.source,
        run_id=options.run_id,
    )
    counts: dict[str, int] = {}
    with store.transaction():
        for table_name, frame in rows_by_table.items():
            store.con.execute(f"DELETE FROM {table_name} WHERE source = ?", [options.source])
            insert_frame(store, frame, table_name, f"_{table_name}_rows")
            counts[f"{table_name}_rows"] = int(len(frame))
    return counts


def run_footnote_refresh(
    store: DuckDBStore,
    options: FootnoteOptions | None = None,
) -> dict[str, Any]:
    """Refresh footnotes and record a simple materialization check."""

    options = options or FootnoteOptions()
    counts = refresh_footnotes(store, options)
    total_rows = sum(counts.values())
    quality_check(
        store,
        dataset_id="footnotes",
        table_name="footnote_pension",
        check_name="footnote_rows_materialized",
        status="passed" if total_rows > 0 else "warning",
        observed_value=float(total_rows),
        threshold_value=1.0,
        details={**counts, "source": options.source},
    )
    return {**counts, "total_rows": total_rows}


class FootnoteDataset(Dataset):
    dataset_id = "footnotes"
    source_name = SOURCE_NAME
    depends_on = ("xbrl_filing_contexts",)

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FootnoteOptions) -> DatasetLoadResult:
        details = run_footnote_refresh(store, options)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(details["total_rows"]),
            source=options.source,
            details=details,
            run_id=options.run_id,
        )
