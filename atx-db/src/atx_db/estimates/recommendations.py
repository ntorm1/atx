from __future__ import annotations

from ._columns import *
from ._common import *
from .consensus import _provider_name_series, _source_series

@dataclass(frozen=True)
class EstimateRecommendationOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    source_file: Path | None = None
    source: str = "est_recommendation_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    source_vendor_table: str | None = None
    replace_source_file: bool = True
    run_id: str | None = None


@dataclass(frozen=True)
class EstimateRecommendationSummaryOptions:
    provider: Callable[[], Iterable[dict]] | None = None
    source_file: Path | None = None
    source: str = "est_recommendation_summary_injected_v1"
    provider_name: str = "INJECTED"
    vendor_security_id_type: str = "IBES_TICKER"
    source_vendor_table: str | None = None
    rating_scale: str = "IBES_1_STRONG_BUY_5_SELL"
    scale_direction: str = "LOWER_IS_BULLISH"
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_estimate_recommendation_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_RECOMMENDATION_COLUMNS)


def _empty_estimate_recommendation_summary_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS)


def _normalize_recommendation_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = RECOMMENDATION_COLUMN_ALIASES.get(
            normalized,
            RECOMMENDATION_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _recommendation_code(value: Any, rating_text: Any = None) -> int | None:
    if not pd.isna(value) and str(value).strip():
        try:
            parsed = int(float(str(value).strip()))
        except ValueError:
            parsed = RECOMMENDATION_TEXT_MAP.get(str(value).strip().upper())
        if parsed in RECOMMENDATION_LABELS:
            return parsed
    if not pd.isna(rating_text) and str(rating_text).strip():
        text = re.sub(r"\s+", " ", str(rating_text).strip().upper())
        if text in RECOMMENDATION_TEXT_MAP:
            return RECOMMENDATION_TEXT_MAP[text]
        compact = text.replace("_", " ").replace("-", " ")
        return RECOMMENDATION_TEXT_MAP.get(compact)
    return None


def _recommendation_label(code: Any, rating_text: Any = None) -> str | None:
    if not pd.isna(code):
        try:
            parsed = int(code)
            if parsed in RECOMMENDATION_LABELS:
                return RECOMMENDATION_LABELS[parsed]
        except (TypeError, ValueError):
            pass
    if not pd.isna(rating_text) and str(rating_text).strip():
        return str(rating_text).strip()
    return None


def _standardized_rating(label: Any) -> str | None:
    if pd.isna(label) or not str(label).strip():
        return None
    return re.sub(r"[^0-9A-Z]+", "_", str(label).strip().upper()).strip("_") or None


def _recommendation_action(new_code: Any, prior_code: Any, explicit_action: Any = None) -> str | None:
    if not pd.isna(explicit_action) and str(explicit_action).strip():
        return str(explicit_action).strip().upper()
    if pd.isna(new_code) or pd.isna(prior_code):
        return None
    try:
        new_int = int(new_code)
        prior_int = int(prior_code)
    except (TypeError, ValueError):
        return None
    if new_int < prior_int:
        return "UPGRADE"
    if new_int > prior_int:
        return "DOWNGRADE"
    return "REITERATE"


def normalize_estimate_recommendation_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateRecommendationOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_recommendation_frame()

    raw = _normalize_recommendation_columns(frame.copy())
    if "rating" not in raw.columns and "recommendation_code" not in raw.columns and "price_target" not in raw.columns:
        raise ValueError("Estimate recommendation rows require rating/recommendation_code or price_target")

    now = now_utc_naive()
    provider = _provider_name_series(raw, options.provider_name)
    source = _source_series(raw, options.source)
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

    rating_text = _string_series(raw, "rating")
    recommendation_code = pd.Series(
        [
            _recommendation_code(code, text)
            for code, text in zip(_string_series(raw, "recommendation_code"), rating_text)
        ],
        index=raw.index,
        dtype="Int64",
    )
    prior_rating = _string_series(raw, "prior_rating")
    prior_recommendation_code = pd.Series(
        [
            _recommendation_code(code, text)
            for code, text in zip(_string_series(raw, "prior_recommendation_code"), prior_rating)
        ],
        index=raw.index,
        dtype="Int64",
    )
    recommendation_label = pd.Series(
        [_recommendation_label(code, text) for code, text in zip(recommendation_code, rating_text)],
        index=raw.index,
        dtype="string",
    )
    prior_recommendation_label = pd.Series(
        [_recommendation_label(code, text) for code, text in zip(prior_recommendation_code, prior_rating)],
        index=raw.index,
        dtype="string",
    )
    rating_standardized_raw = _string_series(raw, "rating_standardized")
    rating_standardized = rating_standardized_raw.where(
        rating_standardized_raw.notna(),
        recommendation_label.map(_standardized_rating).astype("string"),
    )

    announce_date = _date_series(raw, "announce_date", fallback=_date_series(raw, "rating_date"))
    rating_date = _date_series(raw, "rating_date", fallback=announce_date)
    activation_date = _date_series(raw, "activation_date", fallback=announce_date)
    as_of_date = _date_series(raw, "as_of_date", fallback=rating_date)
    available_at = _timestamp_series(raw, "available_at")
    activation_ts = _timestamp_from_date_time(activation_date, _time_series(raw, "activation_time"), now)
    announce_ts = _timestamp_from_date_time(announce_date, _time_series(raw, "announce_time"), now)
    available_at = available_at.where(available_at.notna(), activation_ts.where(pd.notna(activation_date), pd.NaT))
    available_at = available_at.where(available_at.notna(), announce_ts.where(pd.notna(announce_date), pd.NaT))
    available_at = available_at.where(available_at.notna(), _timestamp_from_dates_end_of_day(rating_date, now))

    price_target = _numeric_series(raw, "price_target")
    target_horizon_months = _integer_series(raw, "target_horizon_months")
    target_horizon_months = target_horizon_months.where(
        target_horizon_months.notna() | price_target.isna(),
        12,
    )
    source_vendor_table = _string_series(raw, "source_vendor_table")
    if options.source_vendor_table:
        source_vendor_table = source_vendor_table.where(source_vendor_table.notna(), options.source_vendor_table)
    event_type = pd.Series(
        [
            "RECOMMENDATION_PRICE_TARGET"
            if not pd.isna(code) and not pd.isna(target)
            else ("PRICE_TARGET" if not pd.isna(target) else "RECOMMENDATION")
            for code, target in zip(recommendation_code, price_target)
        ],
        index=raw.index,
        dtype="string",
    )
    rating_scale_raw = _string_series(raw, "rating_scale")
    rating_scale = rating_scale_raw.where(rating_scale_raw.notna(), "IBES_1_STRONG_BUY_5_SELL")
    industry_code = _string_series(raw, "industry_code")
    is_industry = _bool_series(raw, "is_industry_recommendation")
    is_industry = is_industry.where(is_industry.notna(), industry_code.notna())

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["cusip"] = _string_series(raw, "cusip").str.upper()
    normalized["provider"] = provider
    normalized["source_vendor_table"] = source_vendor_table
    normalized["vendor_broker_id"] = _string_series(raw, "vendor_broker_id")
    normalized["vendor_analyst_id"] = _string_series(raw, "vendor_analyst_id")
    normalized["broker_mask_code"] = _string_series(raw, "broker_mask_code")
    normalized["analyst_mask_code"] = _string_series(raw, "analyst_mask_code")
    normalized["broker_name"] = _string_series(raw, "broker_name")
    normalized["analyst_name"] = _string_series(raw, "analyst_name")
    normalized["broker_id"] = [
        _derive_party_id("EST-BROKER", canonical, prov, vendor_id, mask_code, name, valid_from)
        for canonical, prov, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "broker_id"),
            provider,
            normalized["vendor_broker_id"],
            normalized["broker_mask_code"],
            normalized["broker_name"],
            rating_date,
        )
    ]
    normalized["analyst_id"] = [
        _derive_party_id("EST-ANALYST", canonical, prov, vendor_id, mask_code, name, valid_from)
        for canonical, prov, vendor_id, mask_code, name, valid_from in zip(
            _string_series(raw, "analyst_id"),
            provider,
            normalized["vendor_analyst_id"],
            normalized["analyst_mask_code"],
            normalized["analyst_name"],
            rating_date,
        )
    ]
    normalized["rating"] = rating_text.where(rating_text.notna(), recommendation_label)
    normalized["rating_standardized"] = rating_standardized
    normalized["recommendation_code"] = recommendation_code
    normalized["recommendation_label"] = recommendation_label
    normalized["prior_rating"] = prior_rating.where(prior_rating.notna(), prior_recommendation_label)
    normalized["prior_recommendation_code"] = prior_recommendation_code
    normalized["prior_recommendation_label"] = prior_recommendation_label
    normalized["rating_scale"] = rating_scale
    normalized["action"] = [
        _recommendation_action(new, prior, action)
        for new, prior, action in zip(recommendation_code, prior_recommendation_code, _string_series(raw, "action"))
    ]
    normalized["event_type"] = event_type
    normalized["price_target"] = price_target
    normalized["target_currency"] = _string_series(raw, "target_currency").str.upper()
    normalized["target_horizon_months"] = target_horizon_months
    normalized["industry_code"] = industry_code
    normalized["is_industry_recommendation"] = is_industry
    normalized["usfirm"] = _string_series(raw, "usfirm")
    normalized["rating_date"] = rating_date
    normalized["announce_date"] = announce_date
    normalized["announce_time"] = _time_series(raw, "announce_time")
    normalized["activation_date"] = activation_date
    normalized["activation_time"] = _time_series(raw, "activation_time")
    normalized["revision_date"] = _date_series(raw, "revision_date")
    normalized["revision_time"] = _time_series(raw, "revision_time")
    normalized["stop_date"] = _date_series(raw, "stop_date")
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    normalized = normalized[
        normalized["rating_date"].notna()
        & normalized["available_at"].notna()
        & (
            normalized["recommendation_code"].notna()
            | normalized["rating"].notna()
            | normalized["price_target"].notna()
        )
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
            | normalized["cusip"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_recommendation_frame()

    normalized["est_recommendation_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-REC",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("broker_id"),
            row.get("analyst_id"),
            row.get("event_type"),
            row.get("rating_date"),
            row.get("recommendation_code"),
            row.get("price_target"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_recommendation_id"])
    return normalized[ESTIMATE_RECOMMENDATION_COLUMNS]


def _estimate_recommendation_dimensions(
    recommendation: pd.DataFrame,
) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    valid_from = recommendation["rating_date"].where(recommendation["rating_date"].notna(), recommendation["as_of_date"])
    valid_to = recommendation["stop_date"].where(recommendation["stop_date"].notna(), recommendation["revision_date"])
    brokers = pd.DataFrame(
        {
            "broker_id": recommendation["broker_id"],
            "broker_name": recommendation["broker_name"],
            "source": recommendation["source"],
            "provider": recommendation["provider"],
            "vendor_broker_id": recommendation["vendor_broker_id"],
            "broker_mask_code": recommendation["broker_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": recommendation["available_at"],
            "run_id": recommendation["run_id"],
            "source_file_sha256": recommendation["source_file_sha256"],
        }
    ).dropna(subset=["broker_id"]).drop_duplicates(subset=["broker_id"])
    analysts = pd.DataFrame(
        {
            "analyst_id": recommendation["analyst_id"],
            "analyst_name": recommendation["analyst_name"],
            "broker_id": recommendation["broker_id"],
            "source": recommendation["source"],
            "provider": recommendation["provider"],
            "vendor_analyst_id": recommendation["vendor_analyst_id"],
            "analyst_mask_code": recommendation["analyst_mask_code"],
            "valid_from": valid_from,
            "valid_to": valid_to,
            "available_at": recommendation["available_at"],
            "run_id": recommendation["run_id"],
            "source_file_sha256": recommendation["source_file_sha256"],
        }
    ).dropna(subset=["analyst_id"]).drop_duplicates(subset=["analyst_id"])
    broker_alias_rows = []
    for _, row in recommendation.dropna(subset=["broker_id"]).iterrows():
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
                        row.get("rating_date"),
                    ),
                    "broker_id": row.get("broker_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("rating_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    analyst_alias_rows = []
    for _, row in recommendation.dropna(subset=["analyst_id"]).iterrows():
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
                        row.get("rating_date"),
                    ),
                    "analyst_id": row.get("analyst_id"),
                    "provider": row.get("provider"),
                    "alias_type": alias_type,
                    "alias_value": str(alias_value).strip(),
                    "valid_from": row.get("rating_date"),
                    "valid_to": row.get("stop_date") if pd.notna(row.get("stop_date")) else row.get("revision_date"),
                    "available_at": row.get("available_at"),
                    "source": row.get("source"),
                    "run_id": row.get("run_id"),
                }
            )
    return (
        brokers,
        analysts,
        pd.DataFrame(broker_alias_rows, columns=ESTIMATE_BROKER_ALIAS_COLUMNS),
        pd.DataFrame(analyst_alias_rows, columns=ESTIMATE_ANALYST_ALIAS_COLUMNS),
    )


def _write_estimate_recommendation_frame(
    store: DuckDBStore,
    recommendation: pd.DataFrame,
    *,
    options: EstimateRecommendationOptions,
    source_file_sha256: str | None = None,
) -> int:
    if recommendation.empty:
        return 0
    brokers, analysts, broker_aliases, analyst_aliases = _estimate_recommendation_dimensions(recommendation)
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_recommendation
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, recommendation, "est_recommendation", "est_recommendation_insert")
        _insert_or_replace_frame(store, brokers, "est_broker", "est_rec_broker_insert")
        _insert_or_replace_frame(store, analysts, "est_analyst", "est_rec_analyst_insert")
        _insert_or_replace_frame(store, broker_aliases, "est_broker_alias", "est_rec_broker_alias_insert")
        _insert_or_replace_frame(store, analyst_aliases, "est_analyst_alias", "est_rec_analyst_alias_insert")
    return int(len(recommendation))


def load_estimate_recommendation_rows(
    store: DuckDBStore,
    options: EstimateRecommendationOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    recommendation = normalize_estimate_recommendation_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_recommendation",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_recommendation_frame(
        store,
        recommendation,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateRecommendationDataset(Dataset):
    """Broker recommendation and price-target loader.

    Default-empty (licensed vendor data — IBES, FactSet, etc.).
    Pass a source CSV or injectable `provider: Callable[[], Iterable[dict]]` to populate.

    CSV loads are idempotent per source-file hash and normalize IBES-style
    recddet/ptgdet rows into deterministic event ids. Callable providers remain
    supported for tests or licensed adapters and are normalized before insert.
    """
    dataset_id = "est_recommendation"
    source_name = "est_recommendation_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self, store: DuckDBStore, options: EstimateRecommendationOptions
    ) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_recommendation_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_recommendation",
                    check_name="est_recommendation_loaded",
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
        source_from_rows = "source" in df.columns
        if "available_at" not in df.columns:
            df["available_at"] = now_utc_naive()
        else:
            df["available_at"] = df["available_at"].fillna(now_utc_naive())
        if not source_from_rows:
            df["source"] = self.source_name
        recommendation = normalize_estimate_recommendation_rows(df, options=options)
        rows_loaded = _write_estimate_recommendation_frame(store, recommendation, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
        )


def _normalize_recommendation_summary_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = RECOMMENDATION_SUMMARY_COLUMN_ALIASES.get(
            normalized,
            RECOMMENDATION_SUMMARY_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _scale_direction_values(
    raw: pd.DataFrame,
    rating_scale: pd.Series,
    options: EstimateRecommendationSummaryOptions,
) -> pd.Series:
    explicit = _string_series(raw, "scale_direction").str.upper()
    values: list[str] = []
    for direction, scale, provider in zip(explicit, rating_scale, _provider_name_series(raw, options.provider_name)):
        if not pd.isna(direction) and str(direction).strip():
            cleaned = str(direction).strip().upper()
            values.append("HIGHER_IS_BULLISH" if "HIGHER" in cleaned else "LOWER_IS_BULLISH")
            continue
        scale_text = "" if pd.isna(scale) else str(scale).upper()
        provider_text = "" if pd.isna(provider) else str(provider).upper()
        if "BLOOMBERG" in scale_text or "BEST" in scale_text or "BLOOMBERG" in provider_text:
            values.append("HIGHER_IS_BULLISH")
        else:
            default = str(options.scale_direction or "LOWER_IS_BULLISH").strip().upper()
            values.append("HIGHER_IS_BULLISH" if "HIGHER" in default else "LOWER_IS_BULLISH")
    return pd.Series(values, index=raw.index, dtype="string")


def _canonical_recommendation_mean(values: pd.Series, scale_direction: pd.Series) -> pd.Series:
    canonical = values.astype("Float64")
    higher_is_bullish = scale_direction.astype("string").str.upper().eq("HIGHER_IS_BULLISH")
    canonical = canonical.where(~higher_is_bullish | canonical.isna(), 6.0 - canonical)
    return canonical


def _count_series(frame: pd.DataFrame, column: str) -> pd.Series:
    values = _integer_series(frame, column)
    return values.where(values.notna(), pd.NA)


def _sum_count_series(*series: pd.Series) -> pd.Series:
    if not series:
        return pd.Series(dtype="Int64")
    counts = pd.concat(series, axis=1)
    has_any = counts.notna().any(axis=1)
    summed = counts.fillna(0).sum(axis=1).round().astype("Int64")
    return summed.where(has_any, pd.NA)


def _infer_recommendation_summary_source_table(
    raw_table: pd.Series,
    *,
    options: EstimateRecommendationSummaryOptions,
    has_recommendation: pd.Series,
    has_price_target: pd.Series,
) -> pd.Series:
    if options.source_vendor_table:
        fallback = pd.Series([options.source_vendor_table] * len(raw_table), index=raw_table.index, dtype="string")
    else:
        inferred = [
            "RECOMMENDATION_PRICE_TARGET_SUMMARY"
            if rec and target
            else ("PTGSUM" if target else ("RECDSUM" if rec else "RECOMMENDATION_SUMMARY"))
            for rec, target in zip(has_recommendation, has_price_target)
        ]
        fallback = pd.Series(inferred, index=raw_table.index, dtype="string")
    return raw_table.where(raw_table.notna(), fallback).str.upper()


def normalize_estimate_recommendation_summary_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateRecommendationSummaryOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_recommendation_summary_frame()

    raw = _normalize_recommendation_summary_columns(frame.copy())
    metric_columns = {
        "mean_recommendation",
        "median_recommendation",
        "strong_buy_count",
        "buy_count",
        "hold_count",
        "underperform_count",
        "sell_count",
        "buy_equivalent_count",
        "sell_equivalent_count",
        "total_recommendations",
        "mean_price_target",
        "median_price_target",
        "high_price_target",
        "low_price_target",
        "price_target_count",
    }
    if not any(column in raw.columns for column in metric_columns):
        raise ValueError("Recommendation summary rows require recommendation counts/ratings or price-target stats")

    now = now_utc_naive()
    provider = _provider_name_series(raw, options.provider_name)
    source = _source_series(raw, options.source)
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

    snapshot_date = _date_series(raw, "snapshot_date", fallback=_date_series(raw, "as_of_date"))
    as_of_date = _date_series(raw, "as_of_date", fallback=snapshot_date)
    available_at = _timestamp_series(raw, "available_at")
    available_date = _date_series(raw, "available_date")
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(available_date, now).where(pd.notna(available_date), pd.NaT),
    )
    available_at = available_at.where(
        available_at.notna(),
        _timestamp_from_dates_end_of_day(snapshot_date, now).where(pd.notna(snapshot_date), pd.NaT),
    )

    rating_scale = _string_series(raw, "rating_scale").where(
        _string_series(raw, "rating_scale").notna(),
        options.rating_scale,
    )
    scale_direction = _scale_direction_values(raw, rating_scale, options)
    mean_recommendation = _canonical_recommendation_mean(_numeric_series(raw, "mean_recommendation"), scale_direction)
    median_recommendation = _canonical_recommendation_mean(_numeric_series(raw, "median_recommendation"), scale_direction)

    strong_buy_count = _count_series(raw, "strong_buy_count")
    buy_count = _count_series(raw, "buy_count")
    hold_count = _count_series(raw, "hold_count")
    underperform_count = _count_series(raw, "underperform_count")
    sell_count = _count_series(raw, "sell_count")
    buy_equivalent_count = _count_series(raw, "buy_equivalent_count")
    sell_equivalent_count = _count_series(raw, "sell_equivalent_count")
    buy_equivalent_count = buy_equivalent_count.where(
        buy_equivalent_count.notna(),
        _sum_count_series(strong_buy_count, buy_count),
    )
    sell_equivalent_count = sell_equivalent_count.where(
        sell_equivalent_count.notna(),
        _sum_count_series(underperform_count, sell_count),
    )
    total_recommendations = _count_series(raw, "total_recommendations")
    total_recommendations = total_recommendations.where(
        total_recommendations.notna(),
        _sum_count_series(strong_buy_count, buy_count, hold_count, underperform_count, sell_count),
    )
    total_recommendations = total_recommendations.where(
        total_recommendations.notna(),
        _sum_count_series(buy_equivalent_count, hold_count, sell_equivalent_count),
    )

    mean_price_target = _numeric_series(raw, "mean_price_target")
    median_price_target = _numeric_series(raw, "median_price_target")
    high_price_target = _numeric_series(raw, "high_price_target")
    low_price_target = _numeric_series(raw, "low_price_target")
    price_target_count = _count_series(raw, "price_target_count")
    target_horizon_months = _integer_series(raw, "target_horizon_months")
    has_price_target = (
        mean_price_target.notna()
        | median_price_target.notna()
        | high_price_target.notna()
        | low_price_target.notna()
        | price_target_count.notna()
    )
    target_horizon_months = target_horizon_months.where(
        target_horizon_months.notna() | ~has_price_target,
        12,
    )
    has_recommendation = (
        mean_recommendation.notna()
        | median_recommendation.notna()
        | total_recommendations.notna()
        | buy_equivalent_count.notna()
        | hold_count.notna()
        | sell_equivalent_count.notna()
    )
    source_vendor_table = _infer_recommendation_summary_source_table(
        _string_series(raw, "source_vendor_table"),
        options=options,
        has_recommendation=has_recommendation,
        has_price_target=has_price_target,
    )

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["symbol"] = symbol
    normalized["vendor_security_id"] = vendor_security_id
    normalized["vendor_security_id_type"] = vendor_security_id_type
    normalized["cusip"] = _string_series(raw, "cusip").str.upper()
    normalized["provider"] = provider
    normalized["source_vendor_table"] = source_vendor_table
    normalized["snapshot_date"] = snapshot_date
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["mean_recommendation"] = mean_recommendation
    normalized["median_recommendation"] = median_recommendation
    normalized["rating_scale"] = rating_scale
    normalized["scale_direction"] = scale_direction
    normalized["strong_buy_count"] = strong_buy_count
    normalized["buy_count"] = buy_count
    normalized["hold_count"] = hold_count
    normalized["underperform_count"] = underperform_count
    normalized["sell_count"] = sell_count
    normalized["buy_equivalent_count"] = buy_equivalent_count
    normalized["sell_equivalent_count"] = sell_equivalent_count
    normalized["total_recommendations"] = total_recommendations
    normalized["mean_price_target"] = mean_price_target
    normalized["median_price_target"] = median_price_target
    normalized["high_price_target"] = high_price_target
    normalized["low_price_target"] = low_price_target
    normalized["price_target_count"] = price_target_count
    normalized["target_currency"] = _string_series(raw, "target_currency").str.upper()
    normalized["target_horizon_months"] = target_horizon_months
    normalized["provider_scale_notes"] = _string_series(raw, "provider_scale_notes")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    normalized = normalized[
        normalized["snapshot_date"].notna()
        & normalized["available_at"].notna()
        & (
            normalized["security_id"].notna()
            | normalized["symbol"].notna()
            | normalized["vendor_security_id"].notna()
            | normalized["cusip"].notna()
        )
        & (
            normalized["mean_recommendation"].notna()
            | normalized["median_recommendation"].notna()
            | normalized["total_recommendations"].notna()
            | normalized["mean_price_target"].notna()
            | normalized["median_price_target"].notna()
            | normalized["price_target_count"].notna()
        )
    ].copy()
    if normalized.empty:
        return _empty_estimate_recommendation_summary_frame()

    normalized["est_recommendation_summary_id"] = normalized.apply(
        lambda row: _hash_id(
            "EST-REC-SUMMARY",
            row.get("source"),
            row.get("provider"),
            row.get("security_id"),
            row.get("symbol"),
            row.get("vendor_security_id_type"),
            row.get("vendor_security_id"),
            row.get("source_vendor_table"),
            row.get("snapshot_date"),
            row.get("rating_scale"),
            row.get("source_file_sha256"),
        ),
        axis=1,
    )
    normalized = normalized.drop_duplicates(subset=["est_recommendation_summary_id"])
    return normalized[ESTIMATE_RECOMMENDATION_SUMMARY_COLUMNS]


def _write_estimate_recommendation_summary_frame(
    store: DuckDBStore,
    summary: pd.DataFrame,
    *,
    options: EstimateRecommendationSummaryOptions,
    source_file_sha256: str | None = None,
) -> int:
    if summary.empty:
        return 0
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_recommendation_summary
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        insert_frame(store, summary, "est_recommendation_summary", "est_recommendation_summary_insert")
    return int(len(summary))


def load_estimate_recommendation_summary_rows(
    store: DuckDBStore,
    options: EstimateRecommendationSummaryOptions,
) -> int:
    store.initialize()
    if options.source_file is None:
        return 0
    source_file = Path(options.source_file)
    frame = pd.read_csv(source_file, dtype=str, keep_default_na=False)
    source_hash = file_sha256(source_file)
    summary = normalize_estimate_recommendation_summary_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="est_recommendation_summary",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"provider": options.provider_name, "rows": int(len(frame))},
    )
    return _write_estimate_recommendation_summary_frame(
        store,
        summary,
        options=options,
        source_file_sha256=source_hash,
    )


class EstimateRecommendationSummaryDataset(Dataset):
    """Aggregate recommendation and price-target summary loader."""

    dataset_id = "est_recommendation_summary"
    source_name = "est_recommendation_summary_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(
        self, store: DuckDBStore, options: EstimateRecommendationSummaryOptions
    ) -> DatasetLoadResult:
        if options.source_file is not None:
            rows_loaded = load_estimate_recommendation_summary_rows(store, options)
            if rows_loaded:
                quality_check(
                    store,
                    dataset_id=self.dataset_id,
                    table_name="est_recommendation_summary",
                    check_name="est_recommendation_summary_loaded",
                    status="passed",
                    observed_value=float(rows_loaded),
                    threshold_value=0.0,
                    details={"provider": options.provider_name, "source_file": str(options.source_file)},
                )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=self.source_name,
                details={"provider": options.provider_name, "source_file": str(options.source_file)},
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

        frame = pd.DataFrame(rows)
        if "available_at" not in frame.columns:
            frame["available_at"] = now_utc_naive()
        else:
            frame["available_at"] = frame["available_at"].fillna(now_utc_naive())
        if "source" not in frame.columns:
            frame["source"] = self.source_name
        summary = normalize_estimate_recommendation_summary_rows(frame, options=options)
        rows_loaded = _write_estimate_recommendation_summary_frame(store, summary, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"provider": options.provider_name},
        )
