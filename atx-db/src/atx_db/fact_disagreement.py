"""PF2-S9: cross-vendor reconciliation for standardized fundamentals."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import security_ids_for_symbols, symbol_key
from .warehouse import insert_frame, json_dumps, quality_check


SOURCE_NAME = "Cross-vendor fundamental fact reconciliation"
DEFAULT_VENDOR_BASELINE_SOURCE = "vendor_baseline_facts_v1"
DEFAULT_FACT_DISAGREEMENT_SOURCE = "fact_disagreement_v1"
DEFAULT_TOLERANCE_ABS = 1e-6
DEFAULT_TOLERANCE_REL = 1e-6
AGREEMENT_GATE = 0.99

VENDOR_BASELINE_COLUMNS = [
    "baseline_fact_id",
    "source",
    "vendor",
    "vendor_fact_id",
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
    "as_of_date",
    "available_at",
    "is_latest_revision",
    "input_lineage_json",
    "run_id",
]

FACT_DISAGREEMENT_COLUMNS = [
    "disagreement_id",
    "source",
    "baseline_source",
    "vendor",
    "baseline_fact_id",
    "standardized_id",
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
    "warehouse_value",
    "vendor_value",
    "absolute_difference",
    "relative_difference",
    "tolerance_abs",
    "tolerance_rel",
    "agreement_status",
    "vintage_status",
    "warehouse_available_at",
    "vendor_available_at",
    "is_latest_revision",
    "as_of_date",
    "available_at",
    "input_lineage_json",
    "run_id",
]


@dataclass(frozen=True)
class FactDisagreementOptions:
    source: str = DEFAULT_FACT_DISAGREEMENT_SOURCE
    baseline_source: str = DEFAULT_VENDOR_BASELINE_SOURCE
    vendor: str | None = None
    baseline_rows: Sequence[Mapping[str, Any]] | None = None
    baseline_frame: pd.DataFrame | None = None
    baseline_path: str | Path | None = None
    baseline_loader: Callable[[], pd.DataFrame | Sequence[Mapping[str, Any]]] | None = None
    as_of_ts: dt.datetime | None = None
    tolerance_abs: float = DEFAULT_TOLERANCE_ABS
    tolerance_rel: float = DEFAULT_TOLERANCE_REL
    run_id: str | None = None


def _baseline_fact_id(
    source: str,
    vendor: str,
    security_id: str,
    item_id: int,
    period_end: Any,
    basis: str,
) -> str:
    payload = "|".join(str(part) for part in (source, vendor, security_id, item_id, period_end, basis))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _disagreement_id(
    source: str,
    baseline_source: str,
    vendor: str,
    security_id: str,
    item_id: int,
    period_end: Any,
    basis: str,
) -> str:
    payload = "|".join(str(part) for part in (source, baseline_source, vendor, security_id, item_id, period_end, basis))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _read_baseline_input(options: FactDisagreementOptions) -> pd.DataFrame:
    if options.baseline_frame is not None:
        return options.baseline_frame.copy()
    if options.baseline_rows is not None:
        return pd.DataFrame(list(options.baseline_rows))
    if options.baseline_loader is not None:
        loaded = options.baseline_loader()
        return loaded.copy() if isinstance(loaded, pd.DataFrame) else pd.DataFrame(list(loaded))
    if options.baseline_path is None:
        return pd.DataFrame()

    path = Path(options.baseline_path)
    suffix = path.suffix.lower()
    if suffix == ".csv":
        return pd.read_csv(path)
    if suffix in {".json", ".jsonl", ".ndjson"}:
        if suffix == ".json":
            with path.open("r", encoding="utf-8") as handle:
                payload = json.load(handle)
            return pd.DataFrame(payload)
        return pd.read_json(path, lines=True)
    raise ValueError(f"unsupported baseline_path format: {path}")


def _map_missing_security_ids(store: DuckDBStore, frame: pd.DataFrame) -> pd.DataFrame:
    if "security_id" not in frame.columns:
        frame["security_id"] = pd.NA
    if "symbol" not in frame.columns and "ticker" in frame.columns:
        frame["symbol"] = frame["ticker"]
    if "symbol" not in frame.columns:
        return frame

    symbols = sorted({symbol_key(value) for value in frame["symbol"].dropna() if symbol_key(value)})
    resolved = security_ids_for_symbols(store, symbols)
    if not resolved:
        return frame

    out = frame.copy()
    missing = out["security_id"].isna() | (out["security_id"].astype("string").str.strip() == "")
    out.loc[missing, "security_id"] = out.loc[missing, "symbol"].map(lambda value: resolved.get(symbol_key(value)))
    return out


def _item_maps(store: DuckDBStore) -> tuple[dict[str, int], dict[int, str]]:
    rows = store.con.execute(
        """
        SELECT item_id, canonical_code
        FROM fundamental_item
        UNION
        SELECT item_id, canonical_code
        FROM fundamental_standardized
        """
    ).fetchall()
    by_code = {str(code): int(item_id) for item_id, code in rows}
    by_id = {int(item_id): str(code) for item_id, code in rows}
    return by_code, by_id


def normalize_vendor_baseline_rows(
    store: DuckDBStore,
    frame: pd.DataFrame,
    options: FactDisagreementOptions | None = None,
) -> pd.DataFrame:
    """Normalize injectable Sharadar/SimFin-style facts into baseline rows."""

    options = options or FactDisagreementOptions()
    if frame is None or frame.empty:
        return pd.DataFrame(columns=VENDOR_BASELINE_COLUMNS)

    out = frame.copy()
    out = out.rename(columns={"ticker": "symbol", "reportperiod": "period_end", "calendardate": "period_end"})
    if "value" not in out.columns and "fact_value" in out.columns:
        out["value"] = out["fact_value"]
    if "basis" not in out.columns:
        out["basis"] = "annual"
    if "vendor" not in out.columns:
        out["vendor"] = options.vendor or "vendor"
    out["source"] = options.baseline_source

    out = _map_missing_security_ids(store, out)
    by_code, by_id = _item_maps(store)
    if "item_id" not in out.columns:
        out["item_id"] = pd.NA
    if "canonical_code" not in out.columns:
        out["canonical_code"] = pd.NA
    missing_item = out["item_id"].isna() & out["canonical_code"].notna()
    out.loc[missing_item, "item_id"] = out.loc[missing_item, "canonical_code"].map(by_code)
    missing_code = out["canonical_code"].isna() & out["item_id"].notna()
    out.loc[missing_code, "canonical_code"] = out.loc[missing_code, "item_id"].map(lambda value: by_id.get(int(value)))

    out["vendor"] = out["vendor"].astype("string").str.strip().str.upper()
    if options.vendor:
        out = out[out["vendor"] == options.vendor.strip().upper()].copy()
    out["basis"] = out["basis"].astype("string").str.strip().str.lower()
    if "symbol" not in out.columns:
        out["symbol"] = pd.NA
    out["symbol"] = out["symbol"].where(out["symbol"].isna(), out["symbol"].astype("string").str.strip().str.upper())
    out["security_id"] = out["security_id"].where(
        out["security_id"].isna(),
        out["security_id"].astype("string").str.strip(),
    )
    out["security_id"] = out["security_id"].replace("", pd.NA)
    out["period_end"] = pd.to_datetime(out["period_end"], errors="coerce").dt.date
    if "period_start" in out.columns:
        out["period_start"] = pd.to_datetime(out["period_start"], errors="coerce").dt.date
    else:
        out["period_start"] = pd.NaT
    if "as_of_date" in out.columns:
        out["as_of_date"] = pd.to_datetime(out["as_of_date"], errors="coerce").dt.date
    else:
        out["as_of_date"] = out["period_end"]
    if "available_at" in out.columns:
        out["available_at"] = pd.to_datetime(out["available_at"], errors="coerce")
    else:
        out["available_at"] = pd.to_datetime(out["as_of_date"], errors="coerce")
    out["value"] = pd.to_numeric(out["value"], errors="coerce")
    out["item_id"] = pd.to_numeric(out["item_id"], errors="coerce").astype("Int64")
    if "vendor_fact_id" not in out.columns:
        out["vendor_fact_id"] = pd.NA
    if "cik" not in out.columns:
        out["cik"] = pd.NA
    if "fiscal_year" not in out.columns:
        out["fiscal_year"] = pd.NA
    if "fiscal_period" not in out.columns:
        out["fiscal_period"] = pd.NA
    if "unit_type" not in out.columns:
        out["unit_type"] = pd.NA
    if "source_accession" not in out.columns:
        out["source_accession"] = pd.NA
    if "is_latest_revision" not in out.columns:
        out["is_latest_revision"] = True

    out = out.dropna(subset=["source", "vendor", "security_id", "item_id", "canonical_code", "basis", "period_end", "value", "as_of_date", "available_at"])
    if out.empty:
        return pd.DataFrame(columns=VENDOR_BASELINE_COLUMNS)

    out["item_id"] = out["item_id"].astype(int)
    out["baseline_fact_id"] = [
        _baseline_fact_id(source, vendor, security_id, item_id, period_end, basis)
        for source, vendor, security_id, item_id, period_end, basis in zip(
            out["source"],
            out["vendor"],
            out["security_id"],
            out["item_id"],
            out["period_end"],
            out["basis"],
        )
    ]
    out["input_lineage_json"] = out.apply(
        lambda row: json_dumps(
            {
                "source": row["source"],
                "vendor": row["vendor"],
                "vendor_fact_id": None if pd.isna(row.get("vendor_fact_id")) else row.get("vendor_fact_id"),
                "canonical_code": row["canonical_code"],
            }
        ),
        axis=1,
    )
    out["run_id"] = options.run_id
    return out[VENDOR_BASELINE_COLUMNS].drop_duplicates(
        subset=["source", "vendor", "security_id", "item_id", "period_end", "basis"],
        keep="last",
    )


def refresh_vendor_baseline_facts(store: DuckDBStore, options: FactDisagreementOptions | None = None) -> int:
    options = options or FactDisagreementOptions()
    store.initialize()
    rows = normalize_vendor_baseline_rows(store, _read_baseline_input(options), options)
    if rows.empty:
        return 0

    vendors = sorted(rows["vendor"].dropna().unique().tolist())
    with store.transaction():
        placeholders = ", ".join("?" for _ in vendors)
        store.con.execute(
            f"DELETE FROM vendor_baseline_facts WHERE source = ? AND vendor IN ({placeholders})",
            [options.baseline_source, *vendors],
        )
        insert_frame(store, rows, "vendor_baseline_facts", "vendor_baseline_facts_insert")
    return int(len(rows))


def _comparison_inputs(store: DuckDBStore, options: FactDisagreementOptions) -> pd.DataFrame:
    predicates = ["b.source = ?"]
    params: list[object] = [options.baseline_source]
    if options.vendor:
        predicates.append("b.vendor = ?")
        params.append(options.vendor.strip().upper())
    as_of_filter = ""
    if options.as_of_ts is not None:
        as_of_filter = "AND b.available_at <= ?"
        params.append(options.as_of_ts)
    std_as_of_filter = ""
    std_params: list[object] = []
    if options.as_of_ts is not None:
        std_as_of_filter = "AND s.available_at <= ?"
        std_params.append(options.as_of_ts)

    sql = f"""
        WITH baseline_ranked AS (
            SELECT
                b.*,
                row_number() OVER (
                    PARTITION BY b.source, b.vendor, b.security_id, b.item_id, b.period_end, b.basis
                    ORDER BY b.available_at DESC, b.source_loaded_at DESC, b.baseline_fact_id DESC
                ) AS rn
            FROM vendor_baseline_facts b
            WHERE {' AND '.join(predicates)}
              AND b.is_latest_revision
              {as_of_filter}
        ),
        baseline AS (
            SELECT * EXCLUDE (rn)
            FROM baseline_ranked
            WHERE rn = 1
        ),
        standardized_ranked AS (
            SELECT
                s.*,
                lower(s.basis) AS basis_key,
                row_number() OVER (
                    PARTITION BY s.security_id, s.item_id, s.period_end, lower(s.basis)
                    ORDER BY s.available_at DESC, s.source_loaded_at DESC, s.standardized_id DESC
                ) AS rn
            FROM fundamental_standardized s
            WHERE s.is_latest_revision
              {std_as_of_filter}
        ),
        standardized AS (
            SELECT * EXCLUDE (rn)
            FROM standardized_ranked
            WHERE rn = 1
        )
        SELECT
            b.baseline_fact_id,
            b.source AS baseline_source,
            b.vendor,
            s.standardized_id,
            b.security_id,
            coalesce(s.symbol, b.symbol) AS symbol,
            coalesce(s.cik, b.cik) AS cik,
            b.item_id,
            b.canonical_code,
            b.basis,
            coalesce(s.period_start, b.period_start) AS period_start,
            b.period_end,
            coalesce(s.fiscal_year, b.fiscal_year) AS fiscal_year,
            coalesce(s.fiscal_period, b.fiscal_period) AS fiscal_period,
            s.value AS warehouse_value,
            b.value AS vendor_value,
            s.available_at AS warehouse_available_at,
            b.available_at AS vendor_available_at,
            s.input_codes_json AS warehouse_lineage_json,
            b.input_lineage_json AS baseline_lineage_json
        FROM baseline b
        LEFT JOIN standardized s
          ON s.security_id = b.security_id
         AND s.item_id = b.item_id
         AND s.period_end = b.period_end
         AND s.basis_key = b.basis
        ORDER BY b.vendor, b.security_id, b.item_id, b.period_end, b.basis
    """
    return store.con.execute(sql, [*params, *std_params]).df()


def compute_fact_disagreement_rows(
    inputs: pd.DataFrame,
    options: FactDisagreementOptions | None = None,
) -> pd.DataFrame:
    options = options or FactDisagreementOptions()
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=FACT_DISAGREEMENT_COLUMNS)

    out = inputs.copy()
    out["absolute_difference"] = (out["warehouse_value"] - out["vendor_value"]).abs()
    denominator = out["vendor_value"].abs().where(out["vendor_value"].abs() > 0)
    out["relative_difference"] = out["absolute_difference"] / denominator
    out["relative_difference"] = out["relative_difference"].fillna(out["absolute_difference"])
    out["tolerance_abs"] = float(options.tolerance_abs)
    out["tolerance_rel"] = float(options.tolerance_rel)
    missing = out["warehouse_value"].isna()
    agrees = (
        ~missing
        & (
            (out["absolute_difference"] <= out["tolerance_abs"])
            | (out["relative_difference"] <= out["tolerance_rel"])
        )
    )
    out["agreement_status"] = "disagrees"
    out.loc[agrees, "agreement_status"] = "agrees"
    out.loc[missing, "agreement_status"] = "missing_warehouse"
    out["vintage_status"] = "like_for_like_latest_visible"
    out.loc[missing, "vintage_status"] = "missing_warehouse_vintage"
    out["is_latest_revision"] = True
    out["warehouse_available_at"] = pd.to_datetime(out["warehouse_available_at"], errors="coerce")
    out["vendor_available_at"] = pd.to_datetime(out["vendor_available_at"], errors="coerce")
    out["available_at"] = out[["warehouse_available_at", "vendor_available_at"]].max(axis=1)
    out["as_of_date"] = pd.to_datetime(out["period_end"], errors="coerce").dt.date
    out["input_lineage_json"] = out.apply(
        lambda row: json_dumps(
            {
                "warehouse": {
                    "table": "fundamental_standardized",
                    "standardized_id": row.get("standardized_id"),
                    "available_at": row.get("warehouse_available_at"),
                    "input_codes_json": row.get("warehouse_lineage_json"),
                },
                "baseline": {
                    "table": "vendor_baseline_facts",
                    "baseline_fact_id": row.get("baseline_fact_id"),
                    "available_at": row.get("vendor_available_at"),
                    "input_lineage_json": row.get("baseline_lineage_json"),
                },
            }
        ),
        axis=1,
    )
    out["source"] = options.source
    out["run_id"] = options.run_id
    out["disagreement_id"] = [
        _disagreement_id(options.source, baseline_source, vendor, security_id, item_id, period_end, basis)
        for baseline_source, vendor, security_id, item_id, period_end, basis in zip(
            out["baseline_source"],
            out["vendor"],
            out["security_id"],
            out["item_id"],
            out["period_end"],
            out["basis"],
        )
    ]
    return out[FACT_DISAGREEMENT_COLUMNS]


def _delete_fact_disagreement_scope(store: DuckDBStore, options: FactDisagreementOptions) -> None:
    predicates = ["source = ?", "baseline_source = ?"]
    params: list[object] = [options.source, options.baseline_source]
    if options.vendor:
        predicates.append("vendor = ?")
        params.append(options.vendor.strip().upper())
    store.con.execute(f"DELETE FROM fact_disagreement WHERE {' AND '.join(predicates)}", params)


def refresh_fact_disagreement(store: DuckDBStore, options: FactDisagreementOptions | None = None) -> int:
    options = options or FactDisagreementOptions()
    store.initialize()
    refresh_vendor_baseline_facts(store, options)
    inputs = _comparison_inputs(store, options)
    rows = compute_fact_disagreement_rows(inputs, options)
    with store.transaction():
        _delete_fact_disagreement_scope(store, options)
        if not rows.empty:
            insert_frame(store, rows, "fact_disagreement", "fact_disagreement_insert")
    return int(len(rows))


def fact_disagreement_summary(
    store: DuckDBStore,
    options: FactDisagreementOptions | None = None,
) -> dict[str, object]:
    options = options or FactDisagreementOptions()
    predicates = ["source = ?", "baseline_source = ?"]
    params: list[object] = [options.source, options.baseline_source]
    if options.vendor:
        predicates.append("vendor = ?")
        params.append(options.vendor.strip().upper())
    row = store.con.execute(
        f"""
        SELECT
            count(*)::INTEGER,
            count(*) FILTER (WHERE agreement_status = 'agrees')::INTEGER,
            count(*) FILTER (WHERE agreement_status = 'disagrees')::INTEGER,
            count(*) FILTER (WHERE agreement_status = 'missing_warehouse')::INTEGER,
            count(DISTINCT vendor)::INTEGER,
            count(DISTINCT security_id)::INTEGER
        FROM fact_disagreement
        WHERE {' AND '.join(predicates)}
        """,
        params,
    ).fetchone()
    total = int(row[0] or 0)
    agrees = int(row[1] or 0)
    agreement_ratio = (agrees / total) if total else None
    return {
        "source": options.source,
        "baseline_source": options.baseline_source,
        "vendor": options.vendor.strip().upper() if options.vendor else None,
        "row_count": total,
        "agrees": agrees,
        "disagrees": int(row[2] or 0),
        "missing_warehouse": int(row[3] or 0),
        "vendor_count": int(row[4] or 0),
        "security_count": int(row[5] or 0),
        "agreement_ratio": agreement_ratio,
        "threshold": AGREEMENT_GATE,
    }


class FactDisagreementDataset(Dataset):
    dataset_id = "fact_disagreement"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FactDisagreementOptions) -> DatasetLoadResult:
        rows = refresh_fact_disagreement(store, options)
        summary = fact_disagreement_summary(store, options)
        ratio = summary["agreement_ratio"]
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fact_disagreement",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "baseline_source": options.baseline_source},
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="fact_disagreement",
            check_name="agreement_ratio",
            status="passed" if ratio is not None and ratio >= AGREEMENT_GATE else "failed",
            observed_value=None if ratio is None else float(ratio),
            threshold_value=AGREEMENT_GATE,
            details=summary,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=SOURCE_NAME,
            run_id=options.run_id,
            details=summary,
        )
