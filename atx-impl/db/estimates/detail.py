from __future__ import annotations

from ._columns import *
from ._common import *

@dataclass(frozen=True)
class EstimateDetailOptions:
    source_file: Path | None = None
    source: str = "est_detail_injected_v1"
    provider: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_estimate_detail_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_DETAIL_COLUMNS)


def _normalize_detail_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = DETAIL_COLUMN_ALIASES.get(
            normalized,
            DETAIL_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)

def normalize_estimate_detail_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateDetailOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_detail_frame()

    raw = _normalize_detail_columns(frame.copy())
    if "measure_code" not in raw.columns:
        raise ValueError("Estimate detail rows require measure/measure_code")
    if "value" not in raw.columns:
        raise ValueError("Estimate detail rows require value/estimate")
    if "period_end" not in raw.columns:
        raise ValueError("Estimate detail rows require period_end/FPEDATS")

    now = now_utc_naive()
    provider = _safe_provider(options.provider)
    period_end = _date_series(raw, "period_end")
    announce_date = _date_series(raw, "announce_date")
    activation_date = _date_series(raw, "activation_date")
    revision_date = _date_series(raw, "revision_date")
    stop_date = _date_series(raw, "stop_date")
    estimate_date = _date_series(raw, "estimate_date", announce_date)
    estimate_date = estimate_date.where(pd.notna(estimate_date), activation_date)
    as_of_date = _date_series(raw, "as_of_date", estimate_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    activation_ts = _timestamp_from_date_time(
        activation_date,
        _time_series(raw, "activation_time"),
        now,
    )
    available_at = available_at.fillna(activation_ts).fillna(pd.Timestamp(now))

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = _string_series(raw, "security_id")
    normalized["symbol"] = _string_series(raw, "symbol").map(
        lambda value: symbol_key(None if pd.isna(value) else str(value))
    )
    normalized["vendor_security_id"] = _string_series(raw, "vendor_security_id")
    non_ibes_symbol = (
        normalized["symbol"].isna()
        & normalized["vendor_security_id"].notna()
        & (provider not in {"IBES", "LSEG-IBES", "I-B-E-S"})
    )
    normalized.loc[non_ibes_symbol, "symbol"] = normalized.loc[non_ibes_symbol, "vendor_security_id"].map(symbol_key)
    missing_security = normalized["security_id"].isna() & normalized["symbol"].notna()
    normalized.loc[missing_security, "security_id"] = normalized.loc[missing_security, "symbol"].map(security_id_for_symbol)
    normalized["vendor_security_id_type"] = _string_series(raw, "vendor_security_id_type").fillna(
        options.vendor_security_id_type
    ).str.upper()
    pdf = _string_series(raw, "pdf").str.upper()
    normalized["measure_code"] = [
        _canonical_measure(measure, pdf_value)
        for measure, pdf_value in zip(_string_series(raw, "measure_code"), pdf)
    ]
    normalized["fiscal_year"] = pd.to_numeric(
        _string_series(raw, "fiscal_year").replace(pd.NA, None),
        errors="coerce",
    ).astype("Int64")
    missing_fy = normalized["fiscal_year"].isna() & pd.notna(period_end)
    normalized.loc[missing_fy, "fiscal_year"] = [
        int(value.year) if pd.notna(value) else pd.NA
        for value in period_end[missing_fy]
    ]
    normalized["fiscal_period"] = _string_series(raw, "fiscal_period").str.upper()
    fpi = _string_series(raw, "fpi").str.upper()
    period_type = fpi.map(_period_type_from_fpi).astype("string")
    missing_fp = normalized["fiscal_period"].isna() & pd.notna(period_end)
    derived_fp = []
    for pt, pe in zip(period_type[missing_fp], period_end[missing_fp]):
        if pt == "FY":
            derived_fp.append("FY")
        elif pt == "FQ":
            derived_fp.append(_quarter_label(pe))
        else:
            derived_fp.append(_quarter_label(pe))
    if derived_fp:
        normalized.loc[missing_fp, "fiscal_period"] = derived_fp
    normalized["period_end"] = period_end
    normalized["value"] = _numeric_series(raw, "value")
    normalized["estimate_date"] = estimate_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["provider"] = provider
    normalized["source_vendor_table"] = _string_series(raw, "source_vendor_table")
    normalized["vendor_broker_id"] = _string_series(raw, "vendor_broker_id")
    normalized["vendor_analyst_id"] = _string_series(raw, "vendor_analyst_id")
    normalized["broker_mask_code"] = _string_series(raw, "broker_mask_code")
    normalized["analyst_mask_code"] = _string_series(raw, "analyst_mask_code")
    normalized["broker_name"] = _string_series(raw, "broker_name")
    normalized["analyst_name"] = _string_series(raw, "analyst_name")
    normalized["broker_id"] = [
        _derive_party_id(
            "EST-BROKER",
            canonical,
            provider,
            vendor_id,
            mask_code,
            name,
            valid_from,
        )
        for canonical, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "broker_id"),
            normalized["vendor_broker_id"],
            normalized["broker_mask_code"],
            normalized["broker_name"],
            estimate_date,
        )
    ]
    normalized["analyst_id"] = [
        _derive_party_id(
            "EST-ANALYST",
            canonical,
            provider,
            vendor_id,
            mask_code,
            name,
            valid_from,
        )
        for canonical, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "analyst_id"),
            normalized["vendor_analyst_id"],
            normalized["analyst_mask_code"],
            normalized["analyst_name"],
            estimate_date,
        )
    ]
    normalized["fpi"] = fpi
    normalized["period_type"] = period_type
    normalized["expected_report_date"] = _date_series(raw, "expected_report_date")
    normalized["announce_date"] = announce_date
    normalized["announce_time"] = _time_series(raw, "announce_time")
    normalized["activation_date"] = activation_date
    normalized["activation_time"] = _time_series(raw, "activation_time")
    normalized["revision_date"] = revision_date
    normalized["revision_time"] = _time_series(raw, "revision_time")
    normalized["stop_date"] = stop_date
    normalized["pdf"] = pdf
    normalized["basis"] = _string_series(raw, "basis")
    normalized["is_gaap"] = _bool_series(raw, "is_gaap")
    normalized["estimate_type"] = _string_series(raw, "estimate_type").str.upper()
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["unit"] = _string_series(raw, "unit")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = options.run_id
    normalized["source"] = options.source

    normalized = normalized[
        normalized["period_end"].notna()
        & normalized["as_of_date"].notna()
        & normalized["available_at"].notna()
        & normalized["measure_code"].notna()
        & normalized["value"].notna()
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_detail_frame()
    normalized["est_detail_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-DETAIL",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("measure_code"),
            row.get("period_end"),
            row.get("broker_id"),
            row.get("analyst_id"),
            row.get("announce_date"),
            row.get("activation_date"),
            row.get("revision_date"),
            row.get("value"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_detail_id"])
    return normalized[ESTIMATE_DETAIL_COLUMNS]

def _estimate_detail_dimensions(detail: pd.DataFrame) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    valid_from = detail["estimate_date"].where(detail["estimate_date"].notna(), detail["as_of_date"])
    valid_to = detail["stop_date"].where(detail["stop_date"].notna(), detail["revision_date"])
    brokers = pd.DataFrame(
        {
            "broker_id": detail["broker_id"],
            "broker_name": detail["broker_name"],
            "source": detail["source"],
            "provider": detail["provider"],
            "vendor_broker_id": detail["vendor_broker_id"],
            "broker_mask_code": detail["broker_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": detail["available_at"],
            "run_id": detail["run_id"],
            "source_file_sha256": detail["source_file_sha256"],
        }
    ).dropna(subset=["broker_id"]).drop_duplicates(subset=["broker_id"])
    analysts = pd.DataFrame(
        {
            "analyst_id": detail["analyst_id"],
            "analyst_name": detail["analyst_name"],
            "broker_id": detail["broker_id"],
            "source": detail["source"],
            "provider": detail["provider"],
            "vendor_analyst_id": detail["vendor_analyst_id"],
            "analyst_mask_code": detail["analyst_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": detail["available_at"],
            "run_id": detail["run_id"],
            "source_file_sha256": detail["source_file_sha256"],
        }
    ).dropna(subset=["analyst_id"]).drop_duplicates(subset=["analyst_id"])
    periods = pd.DataFrame(
        {
            "provider": detail["provider"],
            "measure_code": detail["measure_code"],
            "fiscal_year": detail["fiscal_year"],
            "fiscal_period": detail["fiscal_period"],
            "period_end": detail["period_end"],
            "fpi": detail["fpi"],
            "period_type": detail["period_type"],
            "expected_report_date": detail["expected_report_date"],
            "valid_from": valid_from,
            "valid_to": pd.Series([pd.NA] * len(detail), index=detail.index, dtype="object"),
            "as_of_date": detail["as_of_date"],
            "available_at": detail["available_at"],
            "source": detail["source"],
            "run_id": detail["run_id"],
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
    periods = periods[ESTIMATE_PERIOD_COLUMNS].drop_duplicates(subset=["est_period_id"])

    broker_alias_rows = []
    for _, row in detail.dropna(subset=["broker_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_BROKER_ID", row.get("vendor_broker_id")),
            ("BROKER_MASK_CODE", row.get("broker_mask_code")),
            ("BROKER_NAME", row.get("broker_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            broker_alias_rows.append(
                {
                    "broker_alias_id": _hash_id(
                        "EST-BROKER-ALIAS",
                        row.get("provider"),
                        row.get("broker_id"),
                        alias_type,
                        alias_value,
                        row.get("estimate_date"),
                    ),
                    "broker_id": row.get("broker_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("estimate_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    analyst_alias_rows = []
    for _, row in detail.dropna(subset=["analyst_id"]).iterrows():
        for alias_type, alias_value in (
            ("VENDOR_ANALYST_ID", row.get("vendor_analyst_id")),
            ("ANALYST_MASK_CODE", row.get("analyst_mask_code")),
            ("ANALYST_NAME", row.get("analyst_name")),
        ):
            if pd.isna(alias_value) or not str(alias_value).strip():
                continue
            analyst_alias_rows.append(
                {
                    "analyst_alias_id": _hash_id(
                        "EST-ANALYST-ALIAS",
                        row.get("provider"),
                        row.get("analyst_id"),
                        alias_type,
                        alias_value,
                        row.get("estimate_date"),
                    ),
                    "analyst_id": row.get("analyst_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("estimate_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    broker_aliases = pd.DataFrame(broker_alias_rows, columns=ESTIMATE_BROKER_ALIAS_COLUMNS)
    analyst_aliases = pd.DataFrame(analyst_alias_rows, columns=ESTIMATE_ANALYST_ALIAS_COLUMNS)
    return brokers, analysts, periods, broker_aliases, analyst_aliases


def load_estimate_detail_rows(
    store: DuckDBStore,
    options: EstimateDetailOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    detail = normalize_estimate_detail_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_detail",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider, "rows": int(len(frame))},
    )
    if detail.empty:
        return 0
    brokers, analysts, periods, broker_aliases, analyst_aliases = _estimate_detail_dimensions(detail)
    with store.transaction():
        if options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_detail
                WHERE source = ?
                  AND provider = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, _safe_provider(options.provider), source_hash],
            )
        insert_frame(store, detail, "est_detail", "est_detail_insert")
        _insert_or_replace_frame(store, brokers, "est_broker", "est_broker_insert")
        _insert_or_replace_frame(store, analysts, "est_analyst", "est_analyst_insert")
        _insert_or_replace_frame(store, periods, "est_period_dim", "est_period_insert")
        _insert_or_replace_frame(store, broker_aliases, "est_broker_alias", "est_broker_alias_insert")
        _insert_or_replace_frame(store, analyst_aliases, "est_analyst_alias", "est_analyst_alias_insert")
    return int(len(detail))


class EstimateDetailDataset(Dataset):
    dataset_id = "est_detail"
    source_name = "est_detail_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateDetailOptions) -> DatasetLoadResult:
        rows_loaded = load_estimate_detail_rows(store, options)
        if rows_loaded:
            quality_check(
                store,
                dataset_id=self.dataset_id,
                table_name="est_detail",
                check_name="est_detail_loaded",
                status="passed",
                observed_value=float(rows_loaded),
                threshold_value=0.0,
                details={"provider": options.provider, "source_file": str(options.source_file)},
            )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={
                "provider": options.provider,
                "source_file": None if options.source_file is None else str(options.source_file),
            },
        )
