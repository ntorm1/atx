from __future__ import annotations

from ._columns import *
from ._common import *

@dataclass(frozen=True)
class EstimateConsensusOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    source_file: Path | None = None
    source: str = "est_consensus_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    replace_source_file: bool = True
    stale_after_days: int = 105
    run_id: str | None = None


def _empty_estimate_consensus_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_CONSENSUS_COLUMNS)


def _normalize_consensus_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = CONSENSUS_COLUMN_ALIASES.get(
            normalized,
            CONSENSUS_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _provider_name_series(raw: pd.DataFrame, default_provider: str) -> pd.Series:
    base = _string_series(raw, "provider")
    fallback = _safe_provider(default_provider)
    values = [
        fallback if pd.isna(value) or not str(value).strip() else _safe_provider(str(value))
        for value in base
    ]
    return pd.Series(values, index=raw.index, dtype="string")


def _source_series(raw: pd.DataFrame, default_source: str) -> pd.Series:
    base = _string_series(raw, "source")
    return base.where(base.notna(), default_source)


def _derive_consensus_fiscal_period(existing: Any, period_type: Any, period_end: Any) -> str | None:
    if not pd.isna(existing) and str(existing).strip():
        return str(existing).strip().upper()
    if not pd.isna(period_type):
        period_type_text = str(period_type).strip().upper()
        if period_type_text == "FY":
            return "FY"
        if period_type_text == "FQ":
            return _quarter_label(period_end)
    return None


def _stale_after_series(consensus_date: pd.Series, stale_after_days: int) -> pd.Series:
    delta = dt.timedelta(days=max(int(stale_after_days), 0))
    return consensus_date.map(lambda value: pd.NA if pd.isna(value) else value + delta)


def normalize_estimate_consensus_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateConsensusOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_consensus_frame()

    raw = _normalize_consensus_columns(frame.copy())
    if "measure_code" not in raw.columns:
        raise ValueError("Estimate consensus rows require measure/measure_code")
    if "period_end" not in raw.columns:
        raise ValueError("Estimate consensus rows require period_end/FPEDATS")

    now = now_utc_naive()
    provider = _provider_name_series(raw, options.provider_name)
    source = _source_series(raw, options.source)
    pdf = _string_series(raw, "pdf").str.upper()
    measure_code = pd.Series(
        [_canonical_measure(measure, pdf_value) for measure, pdf_value in zip(_string_series(raw, "measure_code"), pdf)],
        index=raw.index,
        dtype="string",
    )
    symbol = _string_series(raw, "symbol").map(
        lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA
    ).astype("string")
    vendor_security_id = _string_series(raw, "vendor_security_id").str.upper()
    vendor_security_id_type = _string_series(raw, "vendor_security_id_type").where(
        _string_series(raw, "vendor_security_id_type").notna(),
        options.vendor_security_id_type,
    ).str.upper()
    security_raw = _string_series(raw, "security_id")
    security_id = pd.Series(
        [
            str(existing).strip()
            if not pd.isna(existing) and str(existing).strip()
            else (security_id_for_symbol(sym) if not pd.isna(sym) and str(sym).strip() else pd.NA)
            for existing, sym in zip(security_raw, symbol)
        ],
        index=raw.index,
        dtype="string",
    )

    period_end = _date_series(raw, "period_end")
    consensus_date = _date_series(raw, "consensus_date", fallback=_date_series(raw, "as_of_date"))
    consensus_date = consensus_date.where(pd.notna(consensus_date), period_end)
    as_of_date = _date_series(raw, "as_of_date", fallback=consensus_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    available_date = _date_series(raw, "available_date")
    available_time = _time_series(raw, "available_time")
    available_from_parts = _timestamp_from_date_time(available_date, available_time, now)
    available_at = available_at.where(available_at.notna(), available_from_parts.where(pd.notna(available_date), pd.NaT))
    availability_anchor = consensus_date.where(pd.notna(consensus_date), as_of_date)
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(availability_anchor, now),
    )

    fiscal_year = _integer_series(raw, "fiscal_year")
    fiscal_year = fiscal_year.where(
        fiscal_year.notna(),
        period_end.map(lambda value: pd.NA if pd.isna(value) else value.year).astype("Int64"),
    )
    fpi = _string_series(raw, "fpi").str.upper()
    period_type_existing = _string_series(raw, "period_type").str.upper()
    period_type = pd.Series(
        [
            str(existing).strip().upper()
            if not pd.isna(existing) and str(existing).strip()
            else _period_type_from_fpi(code)
            for existing, code in zip(period_type_existing, fpi)
        ],
        index=raw.index,
        dtype="string",
    )
    fiscal_period_raw = _string_series(raw, "fiscal_period")
    fiscal_period = pd.Series(
        [
            _derive_consensus_fiscal_period(existing, ptype, pend)
            for existing, ptype, pend in zip(fiscal_period_raw, period_type, period_end)
        ],
        index=raw.index,
        dtype="string",
    )

    stale_after_date = _date_series(raw, "stale_after_date")
    stale_after_date = stale_after_date.where(
        pd.notna(stale_after_date),
        _stale_after_series(consensus_date, options.stale_after_days),
    )

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["provider"] = provider
    normalized["source_vendor_table"] = _string_series(raw, "source_vendor_table")
    normalized["measure_code"] = measure_code
    normalized["fiscal_year"] = fiscal_year
    normalized["fiscal_period"] = fiscal_period
    normalized["period_end"] = period_end
    normalized["fpi"] = fpi
    normalized["period_type"] = period_type
    normalized["expected_report_date"] = _date_series(raw, "expected_report_date")
    normalized["consensus_date"] = consensus_date
    normalized["mean"] = _numeric_series(raw, "mean")
    normalized["median"] = _numeric_series(raw, "median")
    normalized["high"] = _numeric_series(raw, "high")
    normalized["low"] = _numeric_series(raw, "low")
    normalized["stdev"] = _numeric_series(raw, "stdev")
    normalized["num_estimates"] = _integer_series(raw, "num_estimates")
    normalized["num_up"] = _integer_series(raw, "num_up")
    normalized["num_down"] = _integer_series(raw, "num_down")
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["pdf"] = pdf
    normalized["basis"] = _string_series(raw, "basis")
    normalized["is_gaap"] = _bool_series(raw, "is_gaap")
    normalized["unit"] = _string_series(raw, "unit")
    normalized["stale_after_date"] = stale_after_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    statistic_columns = ["mean", "median", "high", "low", "stdev", "num_estimates"]
    has_statistic = normalized[statistic_columns].notna().any(axis=1)
    normalized = normalized[
        normalized["period_end"].notna()
        & normalized["consensus_date"].notna()
        & normalized["available_at"].notna()
        & normalized["measure_code"].notna()
        & has_statistic
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_consensus_frame()

    normalized["est_consensus_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-CONSENSUS",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("consensus_date"),
            row.get("fpi"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_consensus_id"])
    return normalized[ESTIMATE_CONSENSUS_COLUMNS]


def _estimate_consensus_periods(consensus: pd.DataFrame) -> pd.DataFrame:
    if consensus.empty:
        return pd.DataFrame(columns=ESTIMATE_PERIOD_COLUMNS)
    periods = pd.DataFrame(
        {
            "provider": consensus["provider"],
            "measure_code": consensus["measure_code"],
            "fiscal_year": consensus["fiscal_year"],
            "fiscal_period": consensus["fiscal_period"],
            "period_end": consensus["period_end"],
            "fpi": consensus["fpi"],
            "period_type": consensus["period_type"],
            "expected_report_date": consensus["expected_report_date"],
            "valid_from": consensus["consensus_date"],
            "valid_to": consensus["stale_after_date"],
            "as_of_date": consensus["as_of_date"],
            "available_at": consensus["available_at"],
            "source": consensus["source"],
            "run_id": consensus["run_id"],
        }
    ).drop_duplicates()
    periods["est_period_id"] = periods.apply(
        lambda row: _hash_id(
            "EST-PERIOD",
            row.get("provider"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("fpi"),
            row.get("fiscal_year"),
            row.get("fiscal_period"),
        ),
        axis=1,
    )
    return periods[ESTIMATE_PERIOD_COLUMNS].drop_duplicates(subset=["est_period_id"])


def _write_estimate_consensus_frame(
    store: DuckDBStore,
    consensus: pd.DataFrame,
    *,
    options: EstimateConsensusOptions,
    source_file_sha256: str | None = None,
) -> int:
    if consensus.empty:
        return 0
    periods = _estimate_consensus_periods(consensus)
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_consensus
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, consensus, "est_consensus", "est_consensus_insert")
        _insert_or_replace_frame(store, periods, "est_period_dim", "est_consensus_period_insert")
    return int(len(consensus))


def load_estimate_consensus_rows(
    store: DuckDBStore,
    options: EstimateConsensusOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    consensus = normalize_estimate_consensus_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_consensus",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_consensus_frame(
        store,
        consensus,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateConsensusDataset(Dataset):
    """Consensus estimates loader.

    Default-empty (licensed: IBES, FactSet Estimates, Zacks).
    Pass `source_file` for CSV/IBES-style summary snapshots or an injectable
    `provider: Callable[[], Iterable[dict]]` for test/adapter rows.

    File loads are idempotent per `(source, source_file_sha256)` and keep a stable
    `est_consensus_id`. Callable providers preserve the old append semantics.
    """
    dataset_id = "est_consensus"
    source_name = "est_consensus_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateConsensusOptions) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_consensus_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_consensus",
                    check_name="est_consensus_loaded",
                    status="passed",
                    observed_value=float(rows_loaded),
                    threshold_value=0.0,
                    details={"provider": options.provider_name, "source_file": str(options.source_file)},
                )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=self.source_name,
                details={
                    "provider": options.provider_name,
                    "source_file": str(options.source_file),
                },
            )

        if options.provider is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "no provider/source_file supplied; table remains empty"},
            )

        rows = list(options.provider())
        if not rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "provider returned no rows"},
            )

        df = pd.DataFrame(rows)
        # Preserve historical behavior for callables: omit available_at -> now.
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        consensus = normalize_estimate_consensus_rows(df, options=options)
        rows_loaded = _write_estimate_consensus_frame(store, consensus, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
        )
