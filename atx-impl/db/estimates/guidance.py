from __future__ import annotations

from ._columns import *
from ._common import *
from .consensus import _source_series

@dataclass(frozen=True)
class EstimateGuidanceOptions:
    """Guidance loader for injectable rows or local SEC 8-K text files.

    Pass `source_file` for local text extraction or `fetch`/`parse` for callables.
    """
    source_file: Path | None = None
    source: str = "est_guidance_injectable"
    fetch: Callable[[], Iterable[Any]] | None = None
    parse: Callable[[Any], Iterable[dict]] | None = None
    replace_source_file: bool = True
    min_confidence: float = 0.70
    run_id: str | None = None

def _normalize_guidance_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = GUIDANCE_COLUMN_ALIASES.get(
            normalized,
            GUIDANCE_COLUMN_ALIASES.get(compact, normalized),
        )
    return frame.rename(columns=renamed)


def _read_guidance_source_file(source_file: Path) -> pd.DataFrame:
    suffix = source_file.suffix.lower()
    if suffix == ".jsonl":
        rows = []
        with source_file.open("r", encoding="utf-8") as handle:
            for line in handle:
                text = line.strip()
                if text:
                    rows.append(json.loads(text))
        return pd.DataFrame(rows)
    if suffix == ".json":
        with source_file.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
        rows = payload.get("rows", payload) if isinstance(payload, dict) else payload
        return pd.DataFrame(rows)
    return pd.read_csv(source_file, dtype=str, keep_default_na=False)


def _clean_guidance_text(value: Any) -> str | None:
    if pd.isna(value):
        return None
    text = re.sub(r"\s+", " ", str(value)).strip()
    return text or None


def _guidance_record_value(record: dict[str, Any], *names: str) -> Any:
    for name in names:
        value = record.get(name)
        if value is not None and not pd.isna(value) and str(value).strip():
            return value
    return None


def _guidance_date_value(value: Any) -> dt.date | None:
    if value is None or pd.isna(value):
        return None
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.date()


def _guidance_ts_value(value: Any) -> dt.datetime | None:
    if value is None or pd.isna(value):
        return None
    parsed = pd.to_datetime(value, errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.to_pydatetime().replace(tzinfo=None)


def _guidance_source_item(record: dict[str, Any], text: str) -> str:
    explicit = _guidance_record_value(record, "source_item")
    if explicit is not None:
        return str(explicit).strip()
    combined = f"{record.get('items', '')} {text[:1000]}".upper()
    has_202 = "2.02" in combined or "ITEM 2.02" in combined
    has_701 = "7.01" in combined or "ITEM 7.01" in combined
    if has_202 and has_701:
        return "8-K_2.02_7.01"
    if has_202:
        return "8-K_2.02"
    if has_701:
        return "8-K_7.01"
    return "8-K"


def _quarter_end(year: int, quarter: int) -> dt.date:
    month = quarter * 3
    if month == 12:
        return dt.date(year, 12, 31)
    return dt.date(year, month + 1, 1) - dt.timedelta(days=1)


def _guidance_period_from_text(sentence: str, record: dict[str, Any]) -> tuple[int | None, str | None, dt.date | None]:
    fiscal_year = _guidance_record_value(record, "fiscal_year")
    fiscal_period = _guidance_record_value(record, "fiscal_period")
    period_end = _guidance_date_value(_guidance_record_value(record, "period_end"))
    if fiscal_year is not None and fiscal_period is not None and period_end is not None:
        return int(float(fiscal_year)), str(fiscal_period).strip().upper(), period_end
    fy_match = re.search(r"\b(?:full\s+year|fiscal\s+year|fiscal|fy)\s*(20\d{2})\b", sentence, re.IGNORECASE)
    if fy_match:
        year = int(fy_match.group(1))
        return year, "FY", dt.date(year, 12, 31)
    q_match = re.search(
        r"\b(?:(first|second|third|fourth|1st|2nd|3rd|4th)\s+quarter|q([1-4]))\s*(?:of\s*)?(20\d{2})\b",
        sentence,
        re.IGNORECASE,
    )
    if q_match:
        quarter_text = (q_match.group(1) or q_match.group(2) or "").lower()
        year = int(q_match.group(3))
        quarter_map = {
            "first": 1, "1st": 1, "1": 1,
            "second": 2, "2nd": 2, "2": 2,
            "third": 3, "3rd": 3, "3": 3,
            "fourth": 4, "4th": 4, "4": 4,
        }
        quarter = quarter_map.get(quarter_text, int(quarter_text) if quarter_text.isdigit() else None)
        if quarter is not None:
            return year, f"Q{quarter}", _quarter_end(year, quarter)
    if period_end is not None:
        year = int(fiscal_year) if fiscal_year is not None else period_end.year
        period = str(fiscal_period).strip().upper() if fiscal_period is not None else _quarter_label(period_end)
        return year, period, period_end
    return None, None, None


def _guidance_basis(sentence: str) -> str | None:
    text = sentence.lower()
    if "non-gaap" in text or "adjusted" in text:
        return "NON_GAAP"
    if "gaap" in text:
        return "GAAP"
    return None


def _guidance_scale(unit: str | None) -> int:
    if not unit:
        return 1
    normalized = unit.strip().lower()
    if normalized in {"billion", "bn"}:
        return 1_000_000_000
    if normalized in {"million", "mm", "m"}:
        return 1_000_000
    if normalized in {"thousand", "k"}:
        return 1_000
    return 1


def _guidance_values_after(sentence: str, start: int, *, measure_code: str) -> tuple[float | None, float | None, float | None, int, str, str | None]:
    segment = sentence[start : start + 180]
    values: list[tuple[float, str | None, bool]] = []
    for match in GUIDANCE_VALUE_RE.finditer(segment):
        number = float(match.group("number").replace(",", ""))
        unit = match.group("unit")
        has_currency = bool(match.group("currency"))
        if not has_currency and unit is None and 1900 <= number <= 2100 and number.is_integer():
            continue
        values.append((number, unit, has_currency))
        if len(values) >= 2:
            break
    if not values:
        return None, None, None, 1, "PER_SHARE" if measure_code.startswith("EPS") else "VALUE", None
    first, second = values[0], values[1] if len(values) > 1 else None
    unit = first[1] or (second[1] if second else None)
    if measure_code.startswith("EPS"):
        scale = 1
        value_unit = "USD_PER_SHARE" if first[2] else "PER_SHARE"
        currency = "USD" if first[2] else None
    else:
        scale = _guidance_scale(unit)
        value_unit = "USD" if first[2] else "VALUE"
        currency = "USD" if first[2] else None
    if second is not None:
        low = min(first[0], second[0])
        high = max(first[0], second[0])
        return low, high, (low + high) / 2.0, scale, value_unit, currency
    return None, None, first[0], scale, value_unit, currency


def _guidance_confidence(*, has_range: bool, has_period: bool, source_item: str) -> float:
    score = 0.82 if has_range else 0.76
    if has_period:
        score += 0.04
    if "2.02" in source_item or "7.01" in source_item:
        score += 0.06
    return min(score, 0.96)


def _extract_guidance_rows_from_record(record: dict[str, Any], *, source: str, run_id: str | None) -> list[dict[str, Any]]:
    text = None
    for column in GUIDANCE_TEXT_COLUMNS:
        text = _clean_guidance_text(record.get(column))
        if text:
            break
    if not text:
        return []
    sentences = re.split(r"(?<=[.!?])\s+", text)
    guidance_date = (
        _guidance_date_value(_guidance_record_value(record, "guidance_date"))
        or _guidance_date_value(_guidance_record_value(record, "filing_date"))
        or _guidance_date_value(_guidance_record_value(record, "report_date"))
        or _guidance_date_value(_guidance_record_value(record, "acceptance_datetime"))
    )
    available_at = (
        _guidance_ts_value(_guidance_record_value(record, "available_at"))
        or _guidance_ts_value(_guidance_record_value(record, "acceptance_datetime"))
        or (dt.datetime.combine(guidance_date, dt.time(23, 59, 59)) if guidance_date is not None else None)
    )
    security_id = _guidance_record_value(record, "security_id")
    if security_id is None:
        symbol = _guidance_record_value(record, "symbol")
        security_id = security_id_for_symbol(symbol_key(symbol)) if symbol is not None else None
    rows: list[dict[str, Any]] = []
    for sentence in sentences:
        if not GUIDANCE_SOURCE_CUE_RE.search(sentence):
            continue
        source_item = _guidance_source_item(record, sentence)
        fiscal_year, fiscal_period, period_end = _guidance_period_from_text(sentence, record)
        for measure_code, measure_re in GUIDANCE_MEASURE_PATTERNS:
            for match in measure_re.finditer(sentence):
                low, high, mid, units_scale, value_unit, currency = _guidance_values_after(sentence, match.end(), measure_code=measure_code)
                if low is None and high is None and mid is None:
                    continue
                has_range = low is not None and high is not None
                rows.append(
                    {
                        "security_id": security_id,
                        "measure_code": measure_code,
                        "fiscal_year": fiscal_year,
                        "fiscal_period": fiscal_period,
                        "period_end": period_end,
                        "low": low,
                        "high": high,
                        "mid": mid,
                        "guidance_type": "RANGE" if has_range else "POINT",
                        "basis": _guidance_basis(sentence),
                        "currency": currency,
                        "unit": value_unit,
                        "units_scale": units_scale,
                        "source_item": source_item,
                        "guidance_date": guidance_date,
                        "form": _guidance_record_value(record, "form") or "8-K",
                        "accession_number": _guidance_record_value(record, "accession_number"),
                        "as_of_date": guidance_date or period_end,
                        "available_at": available_at,
                        "extraction_confidence": _guidance_confidence(has_range=has_range, has_period=period_end is not None, source_item=source_item),
                        "evidence_text": sentence.strip(),
                        "source_url": _guidance_record_value(record, "source_url"),
                        "run_id": run_id,
                        "source": source,
                    }
                )
    return rows


def _period_end_from_fiscal_fields(fiscal_year: Any, fiscal_period: Any) -> dt.date | None:
    if pd.isna(fiscal_year) or pd.isna(fiscal_period):
        return None
    try:
        year = int(fiscal_year)
    except (TypeError, ValueError):
        return None
    period = str(fiscal_period).strip().upper()
    if period == "FY":
        return dt.date(year, 12, 31)
    if period in {"Q1", "Q2", "Q3", "Q4"}:
        return _quarter_end(year, int(period[1]))
    return None


def _guidance_type(row: pd.Series) -> str | None:
    explicit = row.get("guidance_type")
    if explicit is not None and not pd.isna(explicit) and str(explicit).strip():
        return str(explicit).strip().upper()
    has_low = row.get("low") is not None and not pd.isna(row.get("low"))
    has_high = row.get("high") is not None and not pd.isna(row.get("high"))
    has_mid = row.get("mid") is not None and not pd.isna(row.get("mid"))
    if has_low and has_high:
        return "RANGE"
    if has_mid:
        return "POINT"
    if has_low:
        return "OPEN_LOW"
    if has_high:
        return "OPEN_HIGH"
    return None


def _empty_estimate_guidance_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=ESTIMATE_GUIDANCE_COLUMNS)


def normalize_estimate_guidance_rows(
    frame: pd.DataFrame,
    *,
    options: EstimateGuidanceOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_estimate_guidance_frame()
    raw = _normalize_guidance_columns(frame.copy())
    extracted_rows: list[dict[str, Any]] = []
    has_text = pd.Series(False, index=raw.index)
    for column in GUIDANCE_TEXT_COLUMNS:
        if column in raw.columns:
            has_text = has_text | _string_series(raw, column).notna()
    for record in raw[has_text].to_dict("records"):
        extracted_rows.extend(_extract_guidance_rows_from_record(record, source=options.source, run_id=options.run_id))
    candidate_measure = raw.get("measure_code", pd.Series([pd.NA] * len(raw), index=raw.index))
    normalized_candidates = raw[candidate_measure.replace("", pd.NA).notna()].copy()
    raw = pd.concat([normalized_candidates, pd.DataFrame(extracted_rows)], ignore_index=True) if extracted_rows else normalized_candidates
    if raw.empty:
        return _empty_estimate_guidance_frame()

    now = now_utc_naive()
    measure_code = pd.Series([_canonical_measure(measure, None) for measure in _string_series(raw, "measure_code")], index=raw.index, dtype="string")
    symbol = _string_series(raw, "symbol").map(lambda value: symbol_key(value) if not pd.isna(value) and str(value).strip() else pd.NA).astype("string")
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
    fiscal_year = _integer_series(raw, "fiscal_year")
    fiscal_period = _string_series(raw, "fiscal_period").str.upper()
    period_end = _date_series(raw, "period_end")
    derived_period_end = pd.Series([_period_end_from_fiscal_fields(fy, fp) for fy, fp in zip(fiscal_year, fiscal_period)], index=raw.index, dtype="object")
    period_end = period_end.where(pd.notna(period_end), derived_period_end)
    fiscal_year = fiscal_year.where(fiscal_year.notna(), period_end.map(lambda value: pd.NA if pd.isna(value) else value.year).astype("Int64"))
    fiscal_period = fiscal_period.where(fiscal_period.notna(), period_end.map(lambda value: pd.NA if pd.isna(value) else _quarter_label(value)).astype("string"))
    guidance_date = _date_series(raw, "guidance_date")
    filing_date = _date_series(raw, "filing_date")
    report_date = _date_series(raw, "report_date")
    guidance_date = guidance_date.where(pd.notna(guidance_date), filing_date)
    guidance_date = guidance_date.where(pd.notna(guidance_date), report_date)
    guidance_date = guidance_date.where(pd.notna(guidance_date), period_end)
    as_of_date = _date_series(raw, "as_of_date", fallback=guidance_date)
    as_of_date = as_of_date.where(pd.notna(as_of_date), period_end)
    available_at = _timestamp_series(raw, "available_at")
    acceptance_at = _timestamp_series(raw, "acceptance_datetime")
    available_at = available_at.where(available_at.notna(), acceptance_at)
    fallback_available_at = _timestamp_from_dates_end_of_day(guidance_date, now)
    if source_file is None:
        fallback_available_at = pd.Series([now] * len(raw), index=raw.index, dtype="datetime64[ns]")
    available_at = available_at.where(available_at.notna(), fallback_available_at)
    low = _numeric_series(raw, "low")
    high = _numeric_series(raw, "high")
    mid = _numeric_series(raw, "mid")
    both_bounds = low.notna() & high.notna() & mid.isna()
    mid = mid.where(~both_bounds, (low + high) / 2.0)
    source = _source_series(raw, options.source)

    normalized = pd.DataFrame(index=raw.index)
    normalized["security_id"] = security_id
    normalized["measure_code"] = measure_code
    normalized["fiscal_year"] = fiscal_year
    normalized["fiscal_period"] = fiscal_period
    normalized["period_end"] = period_end
    normalized["low"] = low
    normalized["high"] = high
    normalized["mid"] = mid
    normalized["guidance_type"] = raw.apply(_guidance_type, axis=1)
    normalized["basis"] = _string_series(raw, "basis").str.upper()
    normalized["currency"] = _string_series(raw, "currency").str.upper()
    normalized["unit"] = _string_series(raw, "unit").str.upper()
    normalized["units_scale"] = _integer_series(raw, "units_scale").fillna(1)
    normalized["source_item"] = _string_series(raw, "source_item")
    normalized["guidance_date"] = guidance_date
    normalized["form"] = _string_series(raw, "form").str.upper()
    normalized["accession_number"] = _string_series(raw, "accession_number")
    normalized["as_of_date"] = as_of_date
    normalized["available_at"] = available_at
    normalized["extraction_confidence"] = _numeric_series(raw, "extraction_confidence")
    normalized["evidence_text"] = _string_series(raw, "evidence_text")
    normalized["source_file"] = str(source_file) if source_file else pd.NA
    normalized["source_file_sha256"] = source_file_sha256
    normalized["raw_payload_json"] = _raw_payloads(raw)
    normalized["run_id"] = _string_series(raw, "run_id").where(_string_series(raw, "run_id").notna(), options.run_id)
    normalized["source"] = source

    has_value = normalized[["low", "high", "mid"]].notna().any(axis=1)
    confidence_ok = normalized["extraction_confidence"].isna() | (normalized["extraction_confidence"] >= float(options.min_confidence))
    normalized = normalized[
        normalized["security_id"].notna()
        & normalized["measure_code"].notna()
        & normalized["period_end"].notna()
        & has_value
        & confidence_ok
    ].copy()
    if normalized.empty:
        return _empty_estimate_guidance_frame()
    normalized["est_guidance_id"] = [
        _hash_id(
            "EST-GUIDANCE",
            row["source"],
            row["security_id"],
            row["measure_code"],
            row["period_end"],
            row["guidance_date"],
            row["accession_number"],
            row["low"],
            row["high"],
            row["mid"],
            str(row["evidence_text"])[:160] if not pd.isna(row["evidence_text"]) else "",
        )
        for _, row in normalized.iterrows()
    ]
    return normalized[ESTIMATE_GUIDANCE_COLUMNS].drop_duplicates(subset=["est_guidance_id"])


def _write_estimate_guidance_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    *,
    options: EstimateGuidanceOptions,
    source_file_sha256: str | None = None,
) -> int:
    with store.transaction():
        if source_file_sha256 and options.replace_source_file:
            store.con.execute(
                """
                DELETE FROM est_guidance
                WHERE source = ?
                  AND source_file_sha256 = ?
                """,
                [options.source, source_file_sha256],
            )
        if frame.empty:
            return 0
        insert_frame(store, frame, "est_guidance", "est_guidance_insert")
    return int(len(frame))


class EstimateGuidanceDataset(Dataset):
    """Management guidance loader.

    Default-empty unless `source_file` or both `fetch` and `parse` are supplied.

    Append/snapshot semantics: est_guidance has no primary key and rows are INSERTed
    (not INSERT OR REPLACE). Re-running the same fetch/parse appends duplicate rows;
    the fetch/parse pair is responsible for not re-emitting already-loaded guidance
    (or the caller should truncate before a full reload).
    """
    dataset_id = "est_guidance"
    source_name = "est_guidance_injectable"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: EstimateGuidanceOptions) -> DatasetLoadResult:
        if options.min_confidence < 0 or options.min_confidence > 1:
            raise ValueError("min_confidence must be in [0, 1]")

        if options.source_file is not None:
            source_file = Path(options.source_file)
            frame = _read_guidance_source_file(source_file)
            source_hash = file_sha256(source_file)
            guidance = normalize_estimate_guidance_rows(
                frame,
                options=options,
                source_file_sha256=source_hash,
                source_file=source_file,
            )
            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=str(source_file),
                cache_path=source_file,
                sha256=source_hash,
                status="loaded",
                metadata={"source": options.source, "rows": len(frame), "parsed_rows": len(guidance)},
            )
            rows_loaded = _write_estimate_guidance_frame(
                store,
                guidance,
                options=options,
                source_file_sha256=source_hash,
            )
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=rows_loaded,
                source=options.source,
                details={
                    "source_file": str(source_file),
                    "source_file_sha256": source_hash,
                    "parsed_rows": len(guidance),
                    "min_confidence": options.min_confidence,
                },
            )

        if options.fetch is None or options.parse is None:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "fetch/parse not supplied; table remains empty"},
            )

        parsed_rows: list[dict] = []
        for raw in options.fetch():
            parsed_rows.extend(options.parse(raw))

        if not parsed_rows:
            return DatasetLoadResult(
                dataset_id=self.dataset_id,
                rows_loaded=0,
                source=self.source_name,
                details={"reason": "provider returned no rows"},
            )

        guidance = normalize_estimate_guidance_rows(
            pd.DataFrame(parsed_rows),
            options=options,
        )
        rows_loaded = _write_estimate_guidance_frame(store, guidance, options=options)

        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=self.source_name,
            details={"parsed_rows": len(guidance)},
        )
