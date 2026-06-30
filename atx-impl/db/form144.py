"""SEC Form 144 intent-to-sell ingestion and Form 4 reconciliation.

Form 144 is the forward-looking affiliate/restricted-stock sale notice. Form 4
is the backward-looking execution report. This module keeps the intent rows and
creates conservative, point-in-time links to subsequent Form 4 sale rows.
"""

from __future__ import annotations

import datetime as dt
import re
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pandas as pd
from lxml import etree

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .insider_ownership import insider_id_for_owner
from .warehouse import (
    cik_security_id,
    file_sha256,
    insert_frame,
    json_dumps,
    now_utc_naive,
    quality_check,
    record_source_file,
    snake_case,
    symbol_key,
)


SOURCE_NAME = "SEC Form 144 intent XML/CSV"
DEFAULT_SOURCE = "injected_sec_form144_v1"

INTENT_COLUMNS = [
    "filing_id",
    "accession_number",
    "seller_name",
    "seller_name_norm",
    "seller_cik",
    "insider_id",
    "security_id",
    "issuer_cik",
    "issuer_name",
    "issuer_trading_symbol",
    "security_title",
    "filing_date",
    "notice_date",
    "approx_sale_date",
    "sale_window_end_date",
    "shares_proposed",
    "aggregate_market_value",
    "approx_price_per_share",
    "brokers_json",
    "acquisition_date",
    "acquisition_nature",
    "past_3mo_sales_json",
    "rule_10b5_1_indicator",
    "plan_adoption_date",
    "is_amendment",
    "remarks",
    "restatement_seq",
    "is_latest",
    "as_of_date",
    "available_at",
    "source_url",
    "source_file",
    "source_file_sha256",
    "raw_payload_json",
    "source",
    "run_id",
]

LINK_COLUMNS = [
    "form144_filing_id",
    "insider_transaction_id",
    "security_id",
    "insider_id",
    "issuer_cik",
    "seller_cik",
    "intent_notice_date",
    "approx_sale_date",
    "transaction_date",
    "days_between",
    "shares_proposed",
    "transaction_shares",
    "execution_ratio",
    "match_confidence",
    "match_method",
    "match_status",
    "shares_matched",
    "value_matched",
    "share_match_ratio",
    "sale_date",
    "as_of_date",
    "available_at",
    "details_json",
    "source",
    "run_id",
]

COLUMN_ALIASES = {
    "accession": "accession_number",
    "accession_number": "accession_number",
    "seller": "seller_name",
    "seller_name": "seller_name",
    "name_of_seller": "seller_name",
    "seller_cik": "seller_cik",
    "issuer_cik": "issuer_cik",
    "issuer_name": "issuer_name",
    "issuer_trading_symbol": "issuer_trading_symbol",
    "symbol": "issuer_trading_symbol",
    "ticker": "issuer_trading_symbol",
    "security_title": "security_title",
    "title_of_class": "security_title",
    "filing_date": "filing_date",
    "notice_date": "notice_date",
    "date_of_notice": "notice_date",
    "approx_sale_date": "approx_sale_date",
    "approximate_date_of_sale": "approx_sale_date",
    "sale_window_end_date": "sale_window_end_date",
    "shares_proposed": "shares_proposed",
    "aggregate_nbr_of_shares": "shares_proposed",
    "aggregate_number_of_shares": "shares_proposed",
    "aggregate_market_value": "aggregate_market_value",
    "approx_price_per_share": "approx_price_per_share",
    "broker": "broker",
    "brokers": "brokers",
    "brokers_json": "brokers_json",
    "acquisition_date": "acquisition_date",
    "acquisition_nature": "acquisition_nature",
    "nature_of_acquisition": "acquisition_nature",
    "past_3mo_sales_json": "past_3mo_sales_json",
    "rule_10b5_1_indicator": "rule_10b5_1_indicator",
    "rule_10b5_one_indicator": "rule_10b5_1_indicator",
    "plan_adoption_date": "plan_adoption_date",
    "plan_10b5_1_adoption_date": "plan_adoption_date",
    "remarks": "remarks",
    "is_amendment": "is_amendment",
    "as_of_date": "as_of_date",
    "available_at": "available_at",
    "acceptance_datetime": "available_at",
    "source_url": "source_url",
}


@dataclass(frozen=True)
class Form144Options:
    source_files: tuple[Path, ...] | None = None
    source: str = DEFAULT_SOURCE
    replace_source_files: bool = True
    reconcile: bool = True
    match_window_days: int = 92
    run_id: str | None = None
    metadata_by_source: dict[str, dict[str, Any]] = field(default_factory=dict)


def _empty_intents() -> pd.DataFrame:
    return pd.DataFrame(columns=INTENT_COLUMNS)


def _empty_links() -> pd.DataFrame:
    return pd.DataFrame(columns=LINK_COLUMNS)


def _uuid5_id(prefix: str, *parts: object) -> str:
    payload = "|".join([prefix, *(str(part) if part is not None else "" for part in parts)])
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


def _normalize_name(value: str | None) -> str:
    text = re.sub(r"[^0-9A-Za-z]+", " ", value or "").strip().upper()
    text = re.sub(r"\b(JR|SR|II|III|IV|V)\b\.?", "", text)
    return " ".join(text.split())


def _normalize_cik(value: Any) -> str | None:
    if value is None or pd.isna(value):
        return None
    digits = re.sub(r"\D+", "", str(value))
    if not digits:
        return None
    return f"{int(digits):010d}"


def _date(value: Any) -> dt.date | None:
    if value is None or value == "" or pd.isna(value):
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    parsed = pd.to_datetime(value, errors="coerce", utc=True)
    return None if pd.isna(parsed) else parsed.date()


def _timestamp(value: Any) -> dt.datetime | None:
    if value is None or value == "" or pd.isna(value):
        return None
    if isinstance(value, dt.datetime):
        return value.replace(tzinfo=None)
    parsed = pd.to_datetime(value, errors="coerce", utc=True)
    return None if pd.isna(parsed) else parsed.tz_convert(None).to_pydatetime()


def _float(value: Any) -> float | None:
    if value is None or value == "" or pd.isna(value):
        return None
    cleaned = str(value).replace(",", "").replace("$", "").strip()
    if cleaned.startswith("(") and cleaned.endswith(")"):
        cleaned = "-" + cleaned[1:-1]
    try:
        return float(cleaned)
    except ValueError:
        return None


def _bool(value: Any) -> bool:
    if value is None or value == "" or pd.isna(value):
        return False
    text = str(value).strip().lower()
    return text in {"1", "true", "t", "yes", "y", "amendment", "144/a"}


def _local_name(element: etree._Element | None) -> str | None:
    if element is None:
        return None
    return etree.QName(element).localname


def _element_text(element: etree._Element | None) -> str | None:
    if element is None:
        return None
    text = " ".join("".join(element.itertext()).split())
    return text or None


def _first_desc(root: etree._Element, *names: str) -> str | None:
    wanted = {name.lower() for name in names}
    for element in root.iter():
        local = (_local_name(element) or "").lower()
        if local in wanted:
            value = _element_text(element)
            if value:
                return value
    return None


def _desc_contains(root: etree._Element, token: str) -> list[str]:
    token = token.lower()
    values: list[str] = []
    for element in root.iter():
        local = (_local_name(element) or "").lower()
        if token in local:
            value = _element_text(element)
            if value and value not in values:
                values.append(value)
    return values


def _children_payload(element: etree._Element) -> dict[str, str]:
    payload: dict[str, str] = {}
    for child in element:
        local = _local_name(child)
        value = _element_text(child)
        if local and value:
            payload[snake_case(local)] = value
    return payload


def _past_sales(root: etree._Element) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for element in root.iter():
        local = (_local_name(element) or "").lower()
        if "past3" in local or "past_three" in local or "soldpast" in local:
            payload = _children_payload(element)
            if payload:
                rows.append(payload)
    return rows


def _accession_from_path(path: Path) -> str:
    stem = path.stem
    match = re.search(r"\d{10}-\d{2}-\d{6}", stem)
    return match.group(0) if match else stem


def _intent_id(accession_number: str) -> str:
    return _uuid5_id("sec-form144-intent", accession_number)


def _sale_window_end(start: dt.date | None, window_days: int) -> dt.date | None:
    if start is None:
        return None
    return start + dt.timedelta(days=window_days)


def parse_form144_xml(
    content: bytes | str,
    *,
    source_url: str,
    metadata: dict[str, Any] | None = None,
    options: Form144Options | None = None,
    source_file: Path | None = None,
    source_file_sha256: str | None = None,
) -> pd.DataFrame:
    """Parse one Form 144 XML document into a normalized intent row."""

    options = options or Form144Options()
    metadata = metadata or {}
    root = etree.fromstring(content.encode("utf-8") if isinstance(content, str) else content)

    accession_number = str(metadata.get("accession_number") or _accession_from_path(Path(source_url))).strip()
    document_type = (metadata.get("form") or _first_desc(root, "documentType", "submissionType") or "").upper()
    seller_name = _first_desc(root, "sellerName", "nameOfSeller", "nameOfPersonForWhoseAccountTheSecuritiesAreToBeSold")
    seller_cik = _normalize_cik(_first_desc(root, "sellerCik", "reportingOwnerCik", "rptOwnerCik"))
    issuer_cik = _normalize_cik(_first_desc(root, "issuerCik", "cikOfIssuer"))
    issuer_name = _first_desc(root, "issuerName", "nameOfIssuer")
    ticker = symbol_key(_first_desc(root, "issuerTradingSymbol", "tickerSymbol", "tradingSymbol"))
    notice_date = _date(metadata.get("notice_date") or _first_desc(root, "noticeDate", "dateOfNotice"))
    approx_sale_date = _date(_first_desc(root, "approxDateOfSale", "approximateDateOfSale"))
    filing_date = _date(metadata.get("filing_date") or _first_desc(root, "filingDate", "dateOfNotice") or notice_date)
    available_at = _timestamp(metadata.get("acceptance_datetime") or metadata.get("available_at"))
    if available_at is None and (filing_date is not None or notice_date is not None):
        available_at = dt.datetime.combine(filing_date or notice_date, dt.time(22, 0))
    if available_at is None:
        available_at = now_utc_naive()

    shares = _float(_first_desc(root, "aggregateNbrOfShares", "aggregateNumberOfShares", "numberOfSharesToBeSold"))
    market_value = _float(_first_desc(root, "aggregateMarketValue", "aggregateSalePrice"))
    price = _float(_first_desc(root, "approxPricePerShare"))
    if price is None and shares and market_value is not None and shares != 0:
        price = market_value / shares

    seller_norm = _normalize_name(seller_name)
    security_id = str(metadata.get("security_id") or "").strip() or None
    if security_id is None and issuer_cik is not None:
        security_id = cik_security_id(issuer_cik)
    insider_id = str(metadata.get("insider_id") or "").strip() or None
    if insider_id is None and seller_cik is not None:
        insider_id = insider_id_for_owner(seller_cik, seller_name)

    brokers = _desc_contains(root, "broker")
    past_sales = _past_sales(root)
    raw_payload = {
        "document_type": document_type,
        "seller_cik": seller_cik,
        "source_url": source_url,
        "metadata": metadata,
    }
    row = {
        "filing_id": _intent_id(accession_number),
        "accession_number": accession_number,
        "seller_name": seller_name or "UNKNOWN",
        "seller_name_norm": seller_norm,
        "seller_cik": seller_cik,
        "insider_id": insider_id,
        "security_id": security_id,
        "issuer_cik": issuer_cik,
        "issuer_name": issuer_name,
        "issuer_trading_symbol": ticker or None,
        "security_title": _first_desc(root, "titleOfClass", "securityTitle"),
        "filing_date": filing_date,
        "notice_date": notice_date,
        "approx_sale_date": approx_sale_date,
        "sale_window_end_date": _sale_window_end(approx_sale_date, options.match_window_days),
        "shares_proposed": shares,
        "aggregate_market_value": market_value,
        "approx_price_per_share": price,
        "brokers_json": json_dumps(brokers),
        "acquisition_date": _date(_first_desc(root, "acquisitionDate", "approximateDateOfAcquisition")),
        "acquisition_nature": _first_desc(root, "natureOfAcquisition", "acquisitionNature"),
        "past_3mo_sales_json": json_dumps(past_sales),
        "rule_10b5_1_indicator": _bool(_first_desc(root, "rule10b5-1Indicator", "rule10b5OneIndicator")),
        "plan_adoption_date": _date(_first_desc(root, "plan10b5-1AdoptionDate", "plan10b5OneAdoptionDate")),
        "is_amendment": document_type in {"144/A", "FORM 144/A"} or accession_number.upper().endswith("/A"),
        "remarks": _first_desc(root, "remarks", "explanation"),
        "restatement_seq": 0,
        "is_latest": True,
        "as_of_date": notice_date or filing_date or approx_sale_date,
        "available_at": available_at,
        "source_url": source_url,
        "source_file": str(source_file) if source_file else None,
        "source_file_sha256": source_file_sha256,
        "raw_payload_json": json_dumps(raw_payload),
        "source": options.source,
        "run_id": options.run_id,
    }
    return pd.DataFrame([row], columns=INTENT_COLUMNS)


def _normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = COLUMN_ALIASES.get(normalized, COLUMN_ALIASES.get(compact, normalized))
    return frame.rename(columns=renamed)


def normalize_form144_rows(
    frame: pd.DataFrame,
    *,
    options: Form144Options,
    source_file: Path | None = None,
    source_file_sha256: str | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_intents()

    raw = _normalize_columns(frame.copy())
    if "accession_number" not in raw.columns:
        raise ValueError("Form 144 rows require accession_number")
    if "seller_name" not in raw.columns:
        raise ValueError("Form 144 rows require seller_name")

    out = pd.DataFrame(index=raw.index)
    out["accession_number"] = raw["accession_number"].astype(str).str.strip()
    out["filing_id"] = out["accession_number"].map(_intent_id)
    out["seller_name"] = raw["seller_name"].replace("", pd.NA).astype("string")
    out["seller_name_norm"] = out["seller_name"].map(lambda value: _normalize_name(None if pd.isna(value) else str(value)))
    seller_cik = raw["seller_cik"].map(_normalize_cik) if "seller_cik" in raw.columns else pd.Series([None] * len(raw), index=raw.index)
    out["seller_cik"] = seller_cik
    out["insider_id"] = raw["insider_id"] if "insider_id" in raw.columns else pd.NA
    out["insider_id"] = out["insider_id"].replace("", pd.NA)
    out.loc[out["insider_id"].isna() & seller_cik.notna(), "insider_id"] = [
        insider_id_for_owner(cik, name)
        for cik, name in zip(seller_cik[out["insider_id"].isna() & seller_cik.notna()], out.loc[out["insider_id"].isna() & seller_cik.notna(), "seller_name"])
    ]
    issuer_cik = raw["issuer_cik"].map(_normalize_cik) if "issuer_cik" in raw.columns else pd.Series([None] * len(raw), index=raw.index)
    out["issuer_cik"] = issuer_cik
    out["issuer_name"] = raw["issuer_name"] if "issuer_name" in raw.columns else pd.NA
    out["issuer_trading_symbol"] = (
        raw["issuer_trading_symbol"].map(lambda value: symbol_key(None if pd.isna(value) else str(value)))
        if "issuer_trading_symbol" in raw.columns
        else pd.NA
    )
    out["security_id"] = raw["security_id"] if "security_id" in raw.columns else pd.NA
    out["security_id"] = out["security_id"].replace("", pd.NA)
    out.loc[out["security_id"].isna() & issuer_cik.notna(), "security_id"] = issuer_cik[out["security_id"].isna() & issuer_cik.notna()].map(cik_security_id)
    out["security_title"] = raw["security_title"] if "security_title" in raw.columns else pd.NA
    out["filing_date"] = pd.to_datetime(raw["filing_date"], errors="coerce").dt.date if "filing_date" in raw.columns else pd.NaT
    out["notice_date"] = pd.to_datetime(raw["notice_date"], errors="coerce").dt.date if "notice_date" in raw.columns else pd.NaT
    out["approx_sale_date"] = pd.to_datetime(raw["approx_sale_date"], errors="coerce").dt.date if "approx_sale_date" in raw.columns else pd.NaT
    out["sale_window_end_date"] = (
        pd.to_datetime(raw["sale_window_end_date"], errors="coerce").dt.date
        if "sale_window_end_date" in raw.columns
        else out["approx_sale_date"].map(lambda value: _sale_window_end(value, options.match_window_days))
    )
    out["shares_proposed"] = raw["shares_proposed"].map(_float) if "shares_proposed" in raw.columns else pd.NA
    out["aggregate_market_value"] = raw["aggregate_market_value"].map(_float) if "aggregate_market_value" in raw.columns else pd.NA
    out["approx_price_per_share"] = raw["approx_price_per_share"].map(_float) if "approx_price_per_share" in raw.columns else pd.NA
    missing_price = out["approx_price_per_share"].isna() & out["shares_proposed"].notna() & out["aggregate_market_value"].notna() & out["shares_proposed"].ne(0)
    out.loc[missing_price, "approx_price_per_share"] = out.loc[missing_price, "aggregate_market_value"] / out.loc[missing_price, "shares_proposed"]
    if "brokers_json" in raw.columns:
        out["brokers_json"] = raw["brokers_json"]
    elif "brokers" in raw.columns:
        out["brokers_json"] = raw["brokers"].map(lambda value: json_dumps([part.strip() for part in str(value).split(";") if part.strip()]))
    elif "broker" in raw.columns:
        out["brokers_json"] = raw["broker"].map(lambda value: json_dumps([str(value).strip()] if str(value).strip() else []))
    else:
        out["brokers_json"] = json_dumps([])
    out["acquisition_date"] = pd.to_datetime(raw["acquisition_date"], errors="coerce").dt.date if "acquisition_date" in raw.columns else pd.NaT
    out["acquisition_nature"] = raw["acquisition_nature"] if "acquisition_nature" in raw.columns else pd.NA
    out["past_3mo_sales_json"] = raw["past_3mo_sales_json"] if "past_3mo_sales_json" in raw.columns else json_dumps([])
    out["rule_10b5_1_indicator"] = raw["rule_10b5_1_indicator"].map(_bool) if "rule_10b5_1_indicator" in raw.columns else False
    out["plan_adoption_date"] = pd.to_datetime(raw["plan_adoption_date"], errors="coerce").dt.date if "plan_adoption_date" in raw.columns else pd.NaT
    out["is_amendment"] = raw["is_amendment"].map(_bool) if "is_amendment" in raw.columns else False
    out["remarks"] = raw["remarks"] if "remarks" in raw.columns else pd.NA
    out["restatement_seq"] = 0
    out["is_latest"] = True
    out["as_of_date"] = out["notice_date"].where(pd.notna(out["notice_date"]), out["filing_date"])
    out["as_of_date"] = out["as_of_date"].where(pd.notna(out["as_of_date"]), out["approx_sale_date"])
    if "available_at" in raw.columns:
        out["available_at"] = pd.to_datetime(raw["available_at"], errors="coerce", utc=True).dt.tz_convert(None)
    else:
        availability_base = out["filing_date"].where(pd.notna(out["filing_date"]), out["notice_date"])
        availability_base = availability_base.where(pd.notna(availability_base), out["approx_sale_date"])
        out["available_at"] = pd.to_datetime(availability_base, errors="coerce") + pd.Timedelta(hours=22)
    out["available_at"] = out["available_at"].fillna(now_utc_naive())
    out["source_url"] = raw["source_url"] if "source_url" in raw.columns else str(source_file) if source_file else "injected-form144"
    out["source_file"] = str(source_file) if source_file else pd.NA
    out["source_file_sha256"] = source_file_sha256
    out["raw_payload_json"] = raw.apply(lambda row: json_dumps(row.dropna().to_dict()), axis=1)
    out["source"] = options.source
    out["run_id"] = options.run_id
    out = out[out["accession_number"].ne("") & out["seller_name"].notna()].copy()
    return out[INTENT_COLUMNS] if not out.empty else _empty_intents()


def _read_source(path: Path, options: Form144Options, metadata: dict[str, Any], sha256: str) -> pd.DataFrame:
    if path.suffix.lower() in {".xml", ".txt"}:
        content = path.read_bytes()
        if b"<" in content[:256]:
            return parse_form144_xml(
                content,
                source_url=str(path),
                metadata=metadata,
                options=options,
                source_file=path,
                source_file_sha256=sha256,
            )
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        first_line = handle.readline()
    sep = "|" if "|" in first_line else ","
    frame = pd.read_csv(path, dtype=str, keep_default_na=False, sep=sep)
    return normalize_form144_rows(frame, options=options, source_file=path, source_file_sha256=sha256)


def _delete_intents(store: DuckDBStore, filings: pd.DataFrame) -> None:
    keys = filings[["filing_id"]].drop_duplicates()
    store.con.register("form144_intent_delete_keys", keys)
    try:
        store.con.execute(
            """
            DELETE FROM form144_to_form4_link
            WHERE form144_filing_id IN (SELECT filing_id FROM form144_intent_delete_keys)
            """
        )
        store.con.execute(
            """
            DELETE FROM form144_intent
            WHERE filing_id IN (SELECT filing_id FROM form144_intent_delete_keys)
            """
        )
    finally:
        store.con.unregister("form144_intent_delete_keys")


def _resolve_security_ids(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        UPDATE form144_intent f
        SET security_id = s.security_id
        FROM securities s
        WHERE f.source = ?
          AND f.security_id IS NULL
          AND f.issuer_trading_symbol IS NOT NULL
          AND s.primary_symbol = f.issuer_trading_symbol
        """,
        [source],
    )


def _resolve_insider_ids(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH candidates AS (
            SELECT
                i.full_name_norm,
                r.security_id,
                min(i.insider_id) AS insider_id,
                count(DISTINCT i.insider_id) AS insider_count
            FROM insider i
            JOIN insider_relationship r ON r.insider_id = i.insider_id
            GROUP BY i.full_name_norm, r.security_id
            HAVING count(DISTINCT i.insider_id) = 1
        )
        UPDATE form144_intent f
        SET insider_id = c.insider_id
        FROM candidates c
        WHERE f.source = ?
          AND f.insider_id IS NULL
          AND f.security_id = c.security_id
          AND f.seller_name_norm = c.full_name_norm
        """,
        [source],
    )


def _recompute_latest_intents(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                filing_id,
                row_number() OVER (
                    PARTITION BY source, coalesce(seller_cik, seller_name_norm), security_id,
                                 coalesce(approx_sale_date, notice_date, filing_date), coalesce(security_title, '')
                    ORDER BY available_at DESC, filing_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, coalesce(seller_cik, seller_name_norm), security_id,
                                 coalesce(approx_sale_date, notice_date, filing_date), coalesce(security_title, '')
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM form144_intent
            WHERE source = ?
        )
        UPDATE form144_intent f
        SET is_latest = (ranked.rn = 1),
            restatement_seq = ranked.seq
        FROM ranked
        WHERE ranked.filing_id = f.filing_id
        """,
        [source],
    )


def load_form144_intents(store: DuckDBStore, options: Form144Options) -> int:
    store.initialize()
    if not options.source_files:
        return 0

    frames: list[pd.DataFrame] = []
    for path in options.source_files:
        source_path = Path(path)
        source_hash = file_sha256(source_path)
        metadata = options.metadata_by_source.get(str(source_path), {})
        frame = _read_source(source_path, options, metadata, source_hash)
        frames.append(frame)
        record_source_file(
            store,
            dataset_id="form144_intent",
            source_url=str(source_path),
            cache_path=source_path,
            sha256=source_hash,
            metadata={"rows": int(len(frame))},
        )

    intents = pd.concat(frames, ignore_index=True) if frames else _empty_intents()
    if intents.empty:
        return 0

    with store.transaction():
        if options.replace_source_files:
            _delete_intents(store, intents)
        else:
            _delete_intents(store, intents[["filing_id"]])
        insert_frame(store, intents, "form144_intent", "form144_intent_insert")
        _resolve_security_ids(store, options.source)
        _resolve_insider_ids(store, options.source)
        _recompute_latest_intents(store, options.source)
    if options.reconcile:
        refresh_form144_reconciliation(store, source=options.source, run_id=options.run_id, match_window_days=options.match_window_days)
    return int(len(intents))


def _load_link_candidates(store: DuckDBStore, source: str, match_window_days: int) -> pd.DataFrame:
    return store.con.execute(
        """
        SELECT
            f.filing_id AS form144_filing_id,
            f.accession_number AS form144_accession_number,
            f.seller_name,
            f.seller_name_norm,
            f.seller_cik,
            f.notice_date,
            f.security_id,
            f.issuer_cik,
            f.insider_id AS form144_insider_id,
            f.approx_sale_date,
            coalesce(
                f.sale_window_end_date,
                coalesce(f.approx_sale_date, f.filing_date) + (CAST(? AS INTEGER) * INTERVAL 1 DAY)
            ) AS sale_window_end_date,
            f.shares_proposed,
            f.aggregate_market_value,
            f.available_at AS form144_available_at,
            t.transaction_id AS insider_transaction_id,
            t.insider_id,
            t.issuer_cik AS transaction_issuer_cik,
            t.transaction_date,
            t.transaction_shares,
            t.transaction_price,
            t.available_at AS transaction_available_at,
            i.full_name_norm AS transaction_seller_name_norm,
            CASE
                WHEN f.insider_id IS NOT NULL AND f.insider_id = t.insider_id THEN 'insider_id_security_window'
                ELSE 'seller_name_security_window'
            END AS match_method
        FROM form144_intent f
        JOIN insider_transaction t
          ON t.security_id = f.security_id
         AND upper(t.transaction_code) = 'S'
         AND upper(coalesce(t.acquired_disposed, '')) = 'D'
         AND t.transaction_date BETWEEN coalesce(f.approx_sale_date, f.filing_date)
                                    AND coalesce(
                                        f.sale_window_end_date,
                                        coalesce(f.approx_sale_date, f.filing_date) + (CAST(? AS INTEGER) * INTERVAL 1 DAY)
                                    )
        JOIN insider i ON i.insider_id = t.insider_id
        WHERE f.source = ?
          AND coalesce(f.is_latest, true)
          AND f.security_id IS NOT NULL
          AND coalesce(f.approx_sale_date, f.filing_date) IS NOT NULL
          AND (
                (f.insider_id IS NOT NULL AND f.insider_id = t.insider_id)
                OR (f.insider_id IS NULL AND f.seller_name_norm = i.full_name_norm)
              )
        """,
        [match_window_days, match_window_days, source],
    ).df()


def _confidence(method: str, intended_date: Any, transaction_date: Any) -> float:
    base = 0.95 if method == "insider_id_security_window" else 0.85
    intended = _date(intended_date)
    executed = _date(transaction_date)
    if intended is not None and executed is not None:
        base -= min(0.20, abs((executed - intended).days) * 0.01)
    return round(max(0.50, min(1.0, base)), 6)


def _match_status(ratio: float | None) -> str:
    if ratio is None or pd.isna(ratio):
        return "UNKNOWN"
    if ratio >= 1.05:
        return "EXCESS"
    if ratio >= 0.95:
        return "FULL"
    return "PARTIAL"


def compute_form144_links(
    candidates: pd.DataFrame,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> pd.DataFrame:
    if candidates is None or candidates.empty:
        return _empty_links()

    rows: list[dict[str, Any]] = []
    for _, row in candidates.iterrows():
        shares = _float(row.get("transaction_shares"))
        price = _float(row.get("transaction_price"))
        proposed = _float(row.get("shares_proposed"))
        ratio = shares / proposed if shares is not None and proposed not in (None, 0) else None
        value = shares * price if shares is not None and price is not None else None
        sale_date = _date(row.get("transaction_date"))
        intent_date = _date(row.get("approx_sale_date")) or _date(row.get("notice_date"))
        days_between = abs((sale_date - intent_date).days) if sale_date is not None and intent_date is not None else None
        available_values = [value for value in [row.get("form144_available_at"), row.get("transaction_available_at")] if pd.notna(value)]
        available_at = max(pd.to_datetime(available_values, errors="coerce")) if available_values else pd.NaT
        as_of_values = [value for value in [sale_date, _date(row.get("notice_date")), _date(row.get("approx_sale_date"))] if value is not None]
        as_of_date = max(as_of_values) if as_of_values else sale_date
        details = {
            "form144_accession_number": row.get("form144_accession_number"),
            "seller_name": row.get("seller_name"),
            "seller_cik": row.get("seller_cik"),
            "approx_sale_date": str(row.get("approx_sale_date")) if pd.notna(row.get("approx_sale_date")) else None,
            "transaction_date": str(row.get("transaction_date")) if pd.notna(row.get("transaction_date")) else None,
            "shares_proposed": proposed,
            "transaction_shares": shares,
            "transaction_price": price,
        }
        rows.append(
            {
                "form144_filing_id": row.get("form144_filing_id"),
                "insider_transaction_id": row.get("insider_transaction_id"),
                "security_id": row.get("security_id"),
                "insider_id": row.get("insider_id"),
                "issuer_cik": row.get("issuer_cik") or row.get("transaction_issuer_cik"),
                "seller_cik": row.get("seller_cik"),
                "intent_notice_date": _date(row.get("notice_date")),
                "approx_sale_date": _date(row.get("approx_sale_date")),
                "transaction_date": sale_date,
                "days_between": days_between,
                "shares_proposed": proposed,
                "transaction_shares": shares,
                "execution_ratio": ratio,
                "match_confidence": _confidence(str(row.get("match_method")), row.get("approx_sale_date"), row.get("transaction_date")),
                "match_method": row.get("match_method"),
                "match_status": _match_status(ratio),
                "shares_matched": shares,
                "value_matched": value,
                "share_match_ratio": ratio,
                "sale_date": sale_date,
                "available_at": available_at,
                "details_json": json_dumps(details),
                "as_of_date": as_of_date,
                "source": source,
                "run_id": run_id,
            }
        )
    out = pd.DataFrame(rows)
    out = out.drop_duplicates(["form144_filing_id", "insider_transaction_id"], keep="first")
    return out[LINK_COLUMNS]


def refresh_form144_reconciliation(
    store: DuckDBStore,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
    match_window_days: int = 92,
) -> int:
    store.initialize()
    candidates = _load_link_candidates(store, source, match_window_days)
    links = compute_form144_links(candidates, source=source, run_id=run_id)
    with store.transaction():
        store.con.execute("DELETE FROM form144_to_form4_link WHERE source = ?", [source])
        if not links.empty:
            insert_frame(store, links, "form144_to_form4_link", "form144_link_insert")
    return int(len(links))


class Form144IntentDataset(Dataset):
    dataset_id = "form144_intent"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Form144Options) -> DatasetLoadResult:
        rows = load_form144_intents(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="form144_intent",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "source_files": [str(path) for path in options.source_files or ()]},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"source_files": [str(path) for path in options.source_files or ()]},
        )


class Form144ReconciliationDataset(Dataset):
    dataset_id = "form144_to_form4_link"
    source_name = "Derived Form 144 to Form 4 reconciliation"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Form144Options) -> DatasetLoadResult:
        rows = refresh_form144_reconciliation(
            store,
            source=options.source,
            run_id=options.run_id,
            match_window_days=options.match_window_days,
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="form144_to_form4_link",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source, "match_window_days": options.match_window_days},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "form144_filing_id,insider_transaction_id"},
        )
