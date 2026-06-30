from __future__ import annotations

import datetime as dt
import hashlib
import re
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

import pandas as pd
from lxml import etree

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .security_master import SEC_USER_AGENT, dedupe_open_identifier_intervals, sec_session
from .warehouse import cik_security_id, insert_frame, json_dumps, now_utc_naive, quality_check
from .xbrl_filing_contexts import archive_primary_document_url


SOURCE_NAME = "SEC ownership XML"
DEFAULT_FORMS = ("3", "3/A", "4", "4/A", "5", "5/A")
BLOCKHOLDER_SOURCE_NAME = "SEC Schedule 13D/G XML"
DEFAULT_BLOCKHOLDER_FORMS = ("SC 13D", "SC 13D/A", "SC 13G", "SC 13G/A")
SEC_ARCHIVE_BASE_URL = "https://www.sec.gov/Archives/edgar/data"
ISSUER_SEED_SOURCE = "SEC ownership XML issuer seed"

TRANSACTION_CODES = frozenset(
    {
        "P",
        "S",
        "A",
        "D",
        "F",
        "I",
        "M",
        "G",
        "L",
        "W",
        "Z",
        "C",
        "E",
        "H",
        "O",
        "X",
        "U",
        "J",
        "K",
        "V",
        "Q",
        "R",
        "B",
        "T",
        "N",
        "Y",
        "1",
        "2",
    }
)


@dataclass(frozen=True)
class InsiderOwnershipOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    forms: tuple[str, ...] = DEFAULT_FORMS
    accession_numbers: tuple[str, ...] | None = None
    source_urls: tuple[str, ...] | None = None
    source_files: tuple[Path, ...] | None = None
    max_filings: int = 25
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    source: str = SOURCE_NAME
    run_id: str | None = None
    metadata_by_source: dict[str, dict[str, Any]] = field(default_factory=dict)


@dataclass(frozen=True)
class BlockholderOwnershipOptions:
    symbols: tuple[str, ...] = ("AAPL",)
    forms: tuple[str, ...] = DEFAULT_BLOCKHOLDER_FORMS
    accession_numbers: tuple[str, ...] | None = None
    source_urls: tuple[str, ...] | None = None
    source_files: tuple[Path, ...] | None = None
    max_filings: int = 25
    request_timeout: int = 120
    user_agent: str = SEC_USER_AGENT
    source: str = BLOCKHOLDER_SOURCE_NAME
    run_id: str | None = None
    metadata_by_source: dict[str, dict[str, Any]] = field(default_factory=dict)


def _uuid5_id(prefix: str, *parts: object) -> str:
    payload = "|".join([prefix, *(str(part) if part is not None else "" for part in parts)])
    return str(uuid.uuid5(uuid.NAMESPACE_URL, payload))


def _local_name(element: etree._Element | None) -> str | None:
    if element is None:
        return None
    return etree.QName(element).localname


def _first_child(element: etree._Element | None, name: str) -> etree._Element | None:
    if element is None:
        return None
    for child in element:
        if _local_name(child) == name:
            return child
    return None


def _value_from_element(element: etree._Element | None) -> str | None:
    if element is None:
        return None
    value_child = _first_child(element, "value")
    target = value_child if value_child is not None else element
    text = " ".join("".join(target.itertext()).split())
    return text or None


def _first_value(element: etree._Element | None, *path: str) -> str | None:
    current = element
    for name in path:
        current = _first_child(current, name)
        if current is None:
            return None
    return _value_from_element(current)


def _desc_value(element: etree._Element | None, *names: str) -> str | None:
    if element is None:
        return None
    predicate = " or ".join(f"local-name()='{name}'" for name in names)
    for candidate in element.xpath(f".//*[{predicate}]"):
        value = _value_from_element(candidate)
        if value:
            return value
    return None


def _footnote_refs(element: etree._Element | None) -> list[str]:
    if element is None:
        return []
    refs: list[str] = []
    for footnote in element.xpath('.//*[local-name()="footnoteId"]'):
        value = footnote.get("id") or _value_from_element(footnote)
        if value and value not in refs:
            refs.append(value)
    return refs


def _text_or_none(value: Any) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    return text or None


def _normalize_name(value: str | None) -> str:
    text = re.sub(r"[^0-9A-Za-z]+", " ", value or "").strip().upper()
    text = re.sub(r"\b(JR|SR|II|III|IV|V)\b\.?", "", text)
    return " ".join(text.split())


def _normalize_cik(value: Any) -> str | None:
    text = _text_or_none(value)
    if text is None:
        return None
    digits = re.sub(r"\D+", "", text)
    if not digits:
        return None
    return f"{int(digits):010d}"


def insider_id_for_owner(cik: str | None, full_name: str | None) -> str:
    normalized_cik = _normalize_cik(cik)
    if normalized_cik is not None:
        return f"SEC-INSIDER-CIK-{normalized_cik}"
    return "SEC-INSIDER-NAME-" + _uuid5_id("sec-insider-name", _normalize_name(full_name))


def ownership_primary_document_url(cik: str, accession_number: str, primary_document: str) -> str:
    document = str(primary_document).strip().lstrip("/")
    if document.lower().startswith("xslf345"):
        document = Path(document).name
    return archive_primary_document_url(cik, accession_number, document)


def _date(value: Any) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    text = str(value).strip()
    try:
        return dt.date.fromisoformat(text[:10])
    except ValueError:
        parsed = pd.to_datetime(text, errors="coerce", utc=True)
        return None if pd.isna(parsed) else parsed.date()


def _timestamp(value: Any) -> dt.datetime | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.datetime):
        return value.replace(tzinfo=None)
    parsed = pd.to_datetime(value, errors="coerce", utc=True)
    return None if pd.isna(parsed) else parsed.tz_convert(None).to_pydatetime()


def _float(value: Any) -> float | None:
    text = _text_or_none(value)
    if text is None:
        return None
    cleaned = text.replace(",", "").replace("$", "")
    cleaned = cleaned.replace("\u2212", "-").replace("\u2013", "-").replace("\u2014", "-")
    if cleaned.startswith("(") and cleaned.endswith(")"):
        cleaned = "-" + cleaned[1:-1]
    try:
        return float(cleaned)
    except ValueError:
        return None


def _bool(value: Any) -> bool | None:
    text = _text_or_none(value)
    if text is None:
        return None
    normalized = text.lower()
    if normalized in {"1", "true", "t", "yes", "y"}:
        return True
    if normalized in {"0", "false", "f", "no", "n"}:
        return False
    return None


def _safe_code(value: str | None) -> str | None:
    text = _text_or_none(value)
    return None if text is None else text.upper()


def _normalize_schedule_type(value: str | None) -> str:
    text = _safe_code(value) or ""
    text = text.replace("SCHEDULE", "").replace("SC ", "").strip()
    text = re.sub(r"\s+", " ", text)
    if "13D" in text and "/A" in text:
        return "13D/A"
    if "13G" in text and "/A" in text:
        return "13G/A"
    if "13D" in text:
        return "13D"
    if "13G" in text:
        return "13G"
    return text or "UNKNOWN"


def _role_norm(*, is_director: bool, is_officer: bool, title: str | None) -> str | None:
    text = (title or "").upper()
    if "CHIEF EXECUTIVE" in text or re.search(r"\bCEO\b", text):
        return "CEO"
    if "CHIEF FINANCIAL" in text or re.search(r"\bCFO\b", text):
        return "CFO"
    if "CHIEF OPERATING" in text or re.search(r"\bCOO\b", text):
        return "COO"
    if "CHIEF TECHNOLOGY" in text or re.search(r"\bCTO\b", text):
        return "CTO"
    if "GENERAL COUNSEL" in text or re.search(r"\bGC\b", text):
        return "GC"
    if "PRESIDENT" in text:
        return "PRESIDENT"
    if is_director and not is_officer:
        return "DIRECTOR"
    if is_officer:
        return "OTHER_OFFICER"
    return None


def _accession_from_source(source_url: str, content: bytes) -> str:
    match = re.search(r"/Archives/edgar/data/\d+/(\d{18})/", source_url)
    if match:
        raw = match.group(1)
        return f"{raw[:10]}-{raw[10:12]}-{raw[12:]}"
    stem = Path(source_url).stem if not source_url.startswith("http") else ""
    if stem:
        return stem
    return "LOCAL-" + hashlib.sha256(content).hexdigest()[:16].upper()


def _footnotes(root: etree._Element) -> dict[str, str]:
    payload: dict[str, str] = {}
    for footnote in root.xpath('.//*[local-name()="footnote"]'):
        footnote_id = footnote.get("id")
        text = " ".join("".join(footnote.itertext()).split())
        if footnote_id and text:
            payload[footnote_id] = text
    return payload


def _owner_rows(root: etree._Element) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for ordinal, owner in enumerate(root.xpath('./*[local-name()="reportingOwner"]'), start=1):
        owner_id = _first_child(owner, "reportingOwnerId")
        relationship = _first_child(owner, "reportingOwnerRelationship")
        owner_cik = _normalize_cik(_first_value(owner_id, "rptOwnerCik"))
        owner_name = _first_value(owner_id, "rptOwnerName")
        is_director = bool(_bool(_first_value(relationship, "isDirector")))
        is_officer = bool(_bool(_first_value(relationship, "isOfficer")))
        title = _first_value(relationship, "officerTitle")
        rows.append(
            {
                "owner_ordinal": ordinal,
                "insider_id": insider_id_for_owner(owner_cik, owner_name),
                "reporting_owner_cik": owner_cik,
                "full_name": owner_name or "UNKNOWN REPORTING OWNER",
                "full_name_norm": _normalize_name(owner_name),
                "is_director": is_director,
                "is_officer": is_officer,
                "is_ten_percent_owner": bool(_bool(_first_value(relationship, "isTenPercentOwner"))),
                "is_other": bool(_bool(_first_value(relationship, "isOther"))),
                "officer_title_raw": title,
                "officer_title_norm": _role_norm(is_director=is_director, is_officer=is_officer, title=title),
                "other_text": _first_value(relationship, "otherText"),
            }
        )
    return rows


def _transaction_rows(
    *,
    root: etree._Element,
    owners: list[dict[str, Any]],
    filing: dict[str, Any],
    source: str,
    run_id: str | None,
    source_loaded_at: dt.datetime,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    transactions: list[dict[str, Any]] = []
    holdings: list[dict[str, Any]] = []

    specs = (
        ("nonDerivativeTable", "nonDerivativeTransaction", False),
        ("derivativeTable", "derivativeTransaction", True),
    )
    ordinal = 0
    for table_name, row_name, is_derivative in specs:
        table = _first_child(root, table_name)
        if table is None:
            continue
        for row in table.xpath(f'./*[local-name()="{row_name}"]'):
            ordinal += 1
            coding = _first_child(row, "transactionCoding")
            amounts = _first_child(row, "transactionAmounts")
            post_amounts = _first_child(row, "postTransactionAmounts")
            ownership = _first_child(row, "ownershipNature")
            underlying = _first_child(row, "underlyingSecurity")
            transaction_code = _safe_code(_first_value(coding, "transactionCode"))
            transaction_form_type = _first_value(coding, "transactionFormType")
            acquired_disposed = _safe_code(_first_value(amounts, "transactionAcquiredDisposedCode"))
            transaction_date = _date(_first_value(row, "transactionDate"))
            plan_indicator = _bool(
                _desc_value(
                    coding,
                    "rule10b5-1Indicator",
                    "rule10b5OneIndicator",
                    "rule10b5One",
                    "tenbFiveOneIndicator",
                )
            )
            plan_adoption = _date(
                _desc_value(
                    coding,
                    "plan10b5-1AdoptionDate",
                    "plan10b5OneAdoptionDate",
                    "tenbFiveOneAdoptionDate",
                    "adoptionDate",
                )
            )
            for owner in owners:
                plan_id = (
                    _uuid5_id(
                        "sec-10b5-1-plan",
                        owner["insider_id"],
                        filing["security_id"],
                        plan_adoption,
                    )
                    if plan_indicator and plan_adoption is not None
                    else None
                )
                transaction_id = _uuid5_id(
                    "sec-insider-transaction",
                    filing["accession_number"],
                    owner["insider_id"],
                    ordinal,
                    is_derivative,
                    transaction_code,
                )
                transactions.append(
                    {
                        "transaction_id": transaction_id,
                        "filing_id": filing["filing_id"],
                        "accession_number": filing["accession_number"],
                        "transaction_ordinal": ordinal,
                        "transaction_form_type": transaction_form_type,
                        "insider_id": owner["insider_id"],
                        "security_id": filing["security_id"],
                        "issuer_cik": filing["issuer_cik"],
                        "issuer_name": filing["issuer_name"],
                        "issuer_trading_symbol": filing["issuer_trading_symbol"],
                        "security_title": _first_value(row, "securityTitle"),
                        "transaction_date": transaction_date,
                        "deemed_execution_date": _date(_first_value(row, "deemedExecutionDate")),
                        "is_derivative": is_derivative,
                        "transaction_code": transaction_code,
                        "acquired_disposed": acquired_disposed,
                        "transaction_shares": _float(_first_value(amounts, "transactionShares")),
                        "transaction_price": _float(_first_value(amounts, "transactionPricePerShare")),
                        "shares_owned_following": _float(_first_value(post_amounts, "sharesOwnedFollowingTransaction")),
                        "direct_indirect": _safe_code(_first_value(ownership, "directOrIndirectOwnership")),
                        "nature_of_ownership": _first_value(ownership, "natureOfOwnership"),
                        "rule_10b5_1_indicator": plan_indicator,
                        "plan_10b5_1_adoption_date": plan_adoption,
                        "plan_10b5_1_id": plan_id,
                        "equity_swap_involved": bool(_bool(_first_value(coding, "equitySwapInvolved"))),
                        "underlying_security_title": _first_value(underlying, "underlyingSecurityTitle"),
                        "underlying_shares": _float(_first_value(underlying, "underlyingSecurityShares")),
                        "conversion_or_exercise_price": _float(_first_value(row, "conversionOrExercisePrice")),
                        "exercise_date": _date(_first_value(row, "exerciseDate")),
                        "expiration_date": _date(_first_value(row, "expirationDate")),
                        "footnote_ids_json": json_dumps(_footnote_refs(row)),
                        "reported_early": transaction_form_type == "V",
                        "as_of_date": filing["period_of_report"] or transaction_date or filing["filing_date"],
                        "available_at": filing["available_at"],
                        "source": source,
                        "run_id": run_id,
                        "source_loaded_at": source_loaded_at,
                    }
                )

    holding_specs = (
        ("nonDerivativeTable", "nonDerivativeHolding", False),
        ("derivativeTable", "derivativeHolding", True),
    )
    holding_ordinal = 0
    for table_name, row_name, is_derivative in holding_specs:
        table = _first_child(root, table_name)
        if table is None:
            continue
        for row in table.xpath(f'./*[local-name()="{row_name}"]'):
            holding_ordinal += 1
            post_amounts = _first_child(row, "postTransactionAmounts")
            ownership = _first_child(row, "ownershipNature")
            underlying = _first_child(row, "underlyingSecurity")
            for owner in owners:
                holdings.append(
                    {
                        "holding_id": _uuid5_id(
                            "sec-insider-holding",
                            filing["accession_number"],
                            owner["insider_id"],
                            holding_ordinal,
                            is_derivative,
                        ),
                        "filing_id": filing["filing_id"],
                        "accession_number": filing["accession_number"],
                        "holding_ordinal": holding_ordinal,
                        "insider_id": owner["insider_id"],
                        "security_id": filing["security_id"],
                        "issuer_cik": filing["issuer_cik"],
                        "issuer_name": filing["issuer_name"],
                        "issuer_trading_symbol": filing["issuer_trading_symbol"],
                        "security_title": _first_value(row, "securityTitle"),
                        "is_derivative": is_derivative,
                        "shares_owned_following": _float(_first_value(post_amounts, "sharesOwnedFollowingTransaction")),
                        "direct_indirect": _safe_code(_first_value(ownership, "directOrIndirectOwnership")),
                        "nature_of_ownership": _first_value(ownership, "natureOfOwnership"),
                        "underlying_security_title": _first_value(underlying, "underlyingSecurityTitle"),
                        "underlying_shares": _float(_first_value(underlying, "underlyingSecurityShares")),
                        "conversion_or_exercise_price": _float(_first_value(row, "conversionOrExercisePrice")),
                        "exercise_date": _date(_first_value(row, "exerciseDate")),
                        "expiration_date": _date(_first_value(row, "expirationDate")),
                        "as_of_date": filing["period_of_report"] or filing["filing_date"],
                        "available_at": filing["available_at"],
                        "source": source,
                        "run_id": run_id,
                        "source_loaded_at": source_loaded_at,
                    }
                )
    return transactions, holdings


def parse_ownership_xml(
    content: bytes,
    *,
    source_url: str,
    metadata: dict[str, Any] | None = None,
    source: str = SOURCE_NAME,
    run_id: str | None = None,
    source_loaded_at: dt.datetime | None = None,
) -> dict[str, pd.DataFrame]:
    source_loaded_at = source_loaded_at or now_utc_naive()
    metadata = metadata or {}
    parser = etree.XMLParser(recover=True, huge_tree=True)
    root = etree.fromstring(content, parser=parser)

    issuer = _first_child(root, "issuer")
    issuer_cik = _normalize_cik(_first_value(issuer, "issuerCik") or metadata.get("issuer_cik") or metadata.get("cik"))
    issuer_name = _first_value(issuer, "issuerName") or metadata.get("issuer_name")
    issuer_symbol = _safe_code(_first_value(issuer, "issuerTradingSymbol") or metadata.get("issuer_trading_symbol"))
    accession_number = str(metadata.get("accession_number") or _accession_from_source(source_url, content))
    form_type = str(_first_value(root, "documentType") or metadata.get("form") or "").upper()
    filing_date = _date(metadata.get("filing_date")) or _date(_first_value(root, "dateOfOriginalSubmission"))
    period_of_report = _date(_first_value(root, "periodOfReport")) or _date(metadata.get("report_date")) or filing_date
    acceptance_datetime = _timestamp(metadata.get("acceptance_datetime"))
    available_at = acceptance_datetime
    if available_at is None and filing_date is not None:
        available_at = dt.datetime.combine(filing_date, dt.time(22, 0))
    if available_at is None:
        available_at = source_loaded_at

    security_id = str(metadata.get("security_id") or (cik_security_id(issuer_cik) if issuer_cik else "UNKNOWN-ISSUER"))
    filing = {
        "filing_id": _uuid5_id("sec-ownership-filing", accession_number),
        "accession_number": accession_number,
        "security_id": security_id,
        "issuer_cik": issuer_cik,
        "issuer_name": issuer_name,
        "issuer_trading_symbol": issuer_symbol,
        "form_type": form_type,
        "schema_version": _first_value(root, "schemaVersion"),
        "period_of_report": period_of_report,
        "filing_date": filing_date or period_of_report,
        "acceptance_datetime": acceptance_datetime,
        "available_at": available_at,
        "remarks": _first_value(root, "remarks"),
        "footnotes_json": json_dumps(_footnotes(root)),
        "source_url": source_url,
        "raw_document_sha256": hashlib.sha256(content).hexdigest(),
        "source": source,
        "run_id": run_id,
        "source_loaded_at": source_loaded_at,
    }

    owners = _owner_rows(root)
    relationships = []
    for owner in owners:
        valid_from = filing["period_of_report"] or filing["filing_date"] or source_loaded_at.date()
        relationships.append(
            {
                "relationship_id": _uuid5_id(
                    "sec-insider-relationship",
                    filing["accession_number"],
                    owner["insider_id"],
                    security_id,
                    valid_from,
                ),
                "accession_number": filing["accession_number"],
                "insider_id": owner["insider_id"],
                "reporting_owner_cik": owner["reporting_owner_cik"],
                "full_name": owner["full_name"],
                "full_name_norm": owner["full_name_norm"],
                "security_id": security_id,
                "issuer_cik": issuer_cik,
                "issuer_name": issuer_name,
                "issuer_trading_symbol": issuer_symbol,
                "is_director": owner["is_director"],
                "is_officer": owner["is_officer"],
                "is_ten_percent_owner": owner["is_ten_percent_owner"],
                "is_other": owner["is_other"],
                "officer_title_raw": owner["officer_title_raw"],
                "officer_title_norm": owner["officer_title_norm"],
                "other_text": owner["other_text"],
                "valid_from": valid_from,
                "valid_to": None,
                "as_of_date": filing["period_of_report"] or filing["filing_date"],
                "available_at": filing["available_at"],
                "source": source,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )

    transactions, holdings = _transaction_rows(
        root=root,
        owners=owners,
        filing=filing,
        source=source,
        run_id=run_id,
        source_loaded_at=source_loaded_at,
    )
    plans = _trading_plan_rows(transactions, relationships, source=source, run_id=run_id, source_loaded_at=source_loaded_at)
    issuers = _issuer_seed_rows([filing])

    return {
        "filing_form4": pd.DataFrame([filing]),
        "insider_relationship": pd.DataFrame(relationships),
        "insider_transaction": pd.DataFrame(transactions),
        "insider_holding": pd.DataFrame(holdings),
        "tradingplan_10b5_1": pd.DataFrame(plans),
        "issuer_seed": pd.DataFrame(issuers),
    }


def _trading_plan_rows(
    transactions: list[dict[str, Any]],
    relationships: list[dict[str, Any]],
    *,
    source: str,
    run_id: str | None,
    source_loaded_at: dt.datetime,
) -> list[dict[str, Any]]:
    by_owner = {row["insider_id"]: row for row in relationships}
    groups: dict[str, list[dict[str, Any]]] = {}
    for row in transactions:
        plan_id = row.get("plan_10b5_1_id")
        adoption = row.get("plan_10b5_1_adoption_date")
        if not plan_id or adoption is None:
            continue
        groups.setdefault(str(plan_id), []).append(row)

    plans = []
    for plan_id, rows in sorted(groups.items()):
        rows = sorted(rows, key=lambda row: (row.get("transaction_date") or dt.date.min, row["transaction_id"]))
        first_date = rows[0].get("transaction_date")
        last_date = rows[-1].get("transaction_date")
        adoption = rows[0]["plan_10b5_1_adoption_date"]
        cooling_off_days = (first_date - adoption).days if first_date is not None and adoption is not None else None
        owner = by_owner.get(rows[0]["insider_id"], {})
        threshold = 90 if owner.get("is_director") or owner.get("is_officer") else 30
        plans.append(
            {
                "plan_id": plan_id,
                "insider_id": rows[0]["insider_id"],
                "security_id": rows[0]["security_id"],
                "issuer_cik": rows[0]["issuer_cik"],
                "adoption_date": adoption,
                "first_transaction_date": first_date,
                "last_transaction_date": last_date,
                "transaction_count": len(rows),
                "cooling_off_days": cooling_off_days,
                "cooling_off_compliant": None if cooling_off_days is None else cooling_off_days >= threshold,
                "source_filing_ids_json": json_dumps(sorted({row["filing_id"] for row in rows})),
                "source_transaction_ids_json": json_dumps([row["transaction_id"] for row in rows]),
                "as_of_date": first_date,
                "available_at": max(row["available_at"] for row in rows if row.get("available_at") is not None),
                "source": source,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )
    return plans


def _issuer_seed_rows(filings: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for filing in filings:
        issuer_cik = filing.get("issuer_cik")
        security_id = filing.get("security_id")
        if not issuer_cik or not security_id:
            continue
        rows.append(
            {
                "security_id": security_id,
                "issuer_id": f"CIK-{issuer_cik}",
                "primary_symbol": filing.get("issuer_trading_symbol"),
                "name": filing.get("issuer_name") or security_id,
                "asset_class": "EQUITY",
                "country": "US",
                "currency": "USD",
                "active": True,
                "first_seen_date": filing.get("period_of_report") or filing.get("filing_date"),
                "last_seen_date": filing.get("period_of_report") or filing.get("filing_date"),
                "source": ISSUER_SEED_SOURCE,
                "cik": issuer_cik,
                "ticker": filing.get("issuer_trading_symbol"),
                "as_of_date": filing.get("period_of_report") or filing.get("filing_date"),
                "available_at": filing.get("available_at"),
            }
        )
    return rows


def _blockholder_reporting_person_elements(root: etree._Element) -> list[etree._Element]:
    candidates = []
    for name in (
        "reportingPerson",
        "reportingOwner",
        "reportingPersonInfo",
        "coverPageHeaderReportingPersonDetails",
    ):
        candidates.extend(root.xpath(f'.//*[local-name()="{name}"]'))
    unique: list[etree._Element] = []
    seen: set[int] = set()
    for element in candidates:
        marker = id(element)
        if marker not in seen:
            seen.add(marker)
            unique.append(element)
    return unique


def parse_blockholder_xml(
    content: bytes,
    *,
    source_url: str,
    metadata: dict[str, Any] | None = None,
    source: str = BLOCKHOLDER_SOURCE_NAME,
    run_id: str | None = None,
    source_loaded_at: dt.datetime | None = None,
) -> dict[str, pd.DataFrame]:
    """Parse post-2024 structured Schedule 13D/G-like XML into warehouse rows.

    SEC's structured 13D/G XML has seen naming uncertainty in the research notes,
    so this parser intentionally accepts multiple local-name variants and stores a
    conservative normalized row rather than depending on a single exact XSD path.
    """
    source_loaded_at = source_loaded_at or now_utc_naive()
    metadata = metadata or {}
    parser = etree.XMLParser(recover=True, huge_tree=True)
    root = etree.fromstring(content, parser=parser)

    accession_number = str(metadata.get("accession_number") or _accession_from_source(source_url, content))
    schedule_type = _normalize_schedule_type(
        _desc_value(root, "documentType", "formType", "submissionType", "scheduleType")
        or metadata.get("form")
    )
    issuer_cik = _normalize_cik(
        _desc_value(root, "issuerCik", "subjectCompanyCik", "companyCik", "cikOfIssuer")
        or metadata.get("issuer_cik")
        or metadata.get("cik")
    )
    issuer_name = (
        _desc_value(root, "issuerName", "subjectCompanyName", "companyName", "nameOfIssuer")
        or metadata.get("issuer_name")
    )
    cusip = _safe_code(_desc_value(root, "cusip", "cusipNumber", "titleCusip", "issuerCusip") or metadata.get("cusip"))
    security_id = str(metadata.get("security_id") or (cik_security_id(issuer_cik) if issuer_cik else ""))
    filing_date = _date(
        _desc_value(root, "filingDate", "dateFiled", "signatureDate")
        or metadata.get("filing_date")
    )
    event_date = _date(
        _desc_value(root, "eventDate", "dateOfEvent", "dateOfEventWhichRequiresFiling", "dateOfEventRequiringStatement")
        or metadata.get("report_date")
    )
    acceptance_datetime = _timestamp(metadata.get("acceptance_datetime"))
    available_at = acceptance_datetime
    if available_at is None and filing_date is not None:
        available_at = dt.datetime.combine(filing_date, dt.time(22, 0))
    if available_at is None:
        available_at = source_loaded_at
    filing_lag = (filing_date - event_date).days if filing_date is not None and event_date is not None else None
    source_hash = hashlib.sha256(content).hexdigest()
    filing_id = _uuid5_id("sec-blockholder-filing", accession_number)
    filing_row = {
        "filing_id": filing_id,
        "accession_number": accession_number,
        "schedule_type": schedule_type,
        "amendment_seq": int(_float(_desc_value(root, "amendmentNo", "amendmentNumber")) or 0) or None,
        "amends_filing_id": None,
        "is_group_filing": bool(_bool(_desc_value(root, "groupFiling", "isGroupFiling"))),
        "security_id": security_id or None,
        "issuer_cik": issuer_cik,
        "issuer_name": issuer_name,
        "cusip": cusip,
        "event_date": event_date,
        "filing_date": filing_date,
        "filing_lag_business_days": filing_lag,
        "is_xml_filing": True,
        "purpose_text": _desc_value(root, "purposeOfTransaction", "purposeText", "item4Text", "itemFourText"),
        "available_at": available_at,
        "source_url": source_url,
        "source": source,
        "run_id": run_id,
        "source_loaded_at": source_loaded_at,
    }

    people = _blockholder_reporting_person_elements(root)
    if not people:
        people = [root]
    person_rows: list[dict[str, Any]] = []
    for seq, person in enumerate(people, start=1):
        name = (
            _desc_value(person, "reportingPersonName", "nameOfReportingPerson", "reportingOwnerName", "personName")
            or _desc_value(root, "reportingPersonName", "nameOfReportingPerson")
            or "UNKNOWN REPORTING PERSON"
        )
        person_cik = _normalize_cik(_desc_value(person, "reportingPersonCik", "rptOwnerCik", "cik"))
        is_individual = (_safe_code(_desc_value(person, "typeOfReportingPerson", "personType")) == "IN")
        insider_id = insider_id_for_owner(person_cik, name) if is_individual or person_cik else None
        person_rows.append(
            {
                "reporting_person_id": _uuid5_id("sec-blockholder-reporting-person", accession_number, seq, name),
                "filing_id": filing_id,
                "reporting_person_seq": seq,
                "insider_id": insider_id,
                "entity_id": None if insider_id else _uuid5_id("sec-blockholder-entity", name),
                "reporting_person_name": name,
                "type_of_reporting_person": _safe_code(_desc_value(person, "typeOfReportingPerson", "reportingPersonType")),
                "citizenship_or_place_of_org": _desc_value(
                    person,
                    "citizenshipOrPlaceOfOrganization",
                    "citizenshipOrPlaceOfOrg",
                    "placeOfOrganization",
                    "citizenship",
                ),
                "source_of_funds": _safe_code(_desc_value(person, "sourceOfFunds")),
                "sole_voting_power": _float(_desc_value(person, "soleVotingPower", "votingSole")),
                "shared_voting_power": _float(_desc_value(person, "sharedVotingPower", "votingShared")),
                "sole_dispositive_power": _float(_desc_value(person, "soleDispositivePower", "dispositiveSole")),
                "shared_dispositive_power": _float(_desc_value(person, "sharedDispositivePower", "dispositiveShared")),
                "aggregate_beneficially_owned": _float(
                    _desc_value(person, "aggregateBeneficiallyOwned", "amountBeneficiallyOwned", "aggregateAmountOwned")
                ),
                "percent_of_class": _float(_desc_value(person, "percentOfClass", "classPercent", "percentClass")),
                "excludes_certain_shares": bool(_bool(_desc_value(person, "excludesCertainShares"))),
                "legal_proceedings_flag": bool(_bool(_desc_value(person, "legalProceedingsFlag"))),
                "source": source,
                "run_id": run_id,
                "source_loaded_at": source_loaded_at,
            }
        )

    issuer_seed = _issuer_seed_rows(
        [
            {
                "issuer_cik": issuer_cik,
                "security_id": security_id,
                "issuer_trading_symbol": metadata.get("issuer_trading_symbol"),
                "issuer_name": issuer_name,
                "period_of_report": event_date,
                "filing_date": filing_date,
                "available_at": available_at,
            }
        ]
    )
    # Keep the raw hash reachable through the source-file record; the table has no
    # dedicated raw hash column to avoid widening the sparse 13D/G landing shape.
    _ = source_hash
    return {
        "blockholder_filing": pd.DataFrame([filing_row]),
        "blockholder_reporting_person": pd.DataFrame(person_rows),
        "issuer_seed": pd.DataFrame(issuer_seed),
    }


def _concat_frames(frames: list[pd.DataFrame], columns: list[str] | None = None) -> pd.DataFrame:
    non_empty = [frame for frame in frames if not frame.empty]
    if non_empty:
        return pd.concat(non_empty, ignore_index=True)
    return pd.DataFrame(columns=columns or [])


class InsiderOwnershipDataset(Dataset):
    dataset_id = "sec_insider_ownership"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: InsiderOwnershipOptions) -> DatasetLoadResult:
        if options.max_filings < 1:
            raise ValueError("max_filings must be positive")

        source_loaded_at = now_utc_naive()
        parsed_frames: dict[str, list[pd.DataFrame]] = {
            "filing_form4": [],
            "insider_relationship": [],
            "insider_transaction": [],
            "insider_holding": [],
            "tradingplan_10b5_1": [],
            "issuer_seed": [],
        }
        source_count = 0

        for source_url, content, metadata in self._iter_sources(store, options):
            parsed = parse_ownership_xml(
                content,
                source_url=source_url,
                metadata=metadata,
                source=options.source,
                run_id=options.run_id,
                source_loaded_at=source_loaded_at,
            )
            for key, frame in parsed.items():
                parsed_frames[key].append(frame)
            source_count += 1

        frames = {key: _concat_frames(value) for key, value in parsed_frames.items()}
        rows_loaded = self._replace_rows(store, frames)
        self._record_checks(store, frames)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=options.source,
            details={
                "source_count": source_count,
                "filing_rows": int(len(frames["filing_form4"])),
                "relationship_rows": int(len(frames["insider_relationship"])),
                "transaction_rows": int(len(frames["insider_transaction"])),
                "holding_rows": int(len(frames["insider_holding"])),
                "trading_plan_rows": int(len(frames["tradingplan_10b5_1"])),
            },
        )

    def _iter_sources(
        self,
        store: DuckDBStore,
        options: InsiderOwnershipOptions,
    ) -> Iterable[tuple[str, bytes, dict[str, Any]]]:
        if options.source_files:
            for path in options.source_files[: options.max_filings]:
                content = path.read_bytes()
                metadata = dict(options.metadata_by_source.get(str(path), {}))
                metadata.setdefault("accession_number", path.stem)
                from .warehouse import record_source_file

                record_source_file(
                    store,
                    dataset_id=self.dataset_id,
                    source_url=str(path),
                    cache_path=path,
                    status="available",
                    metadata=metadata,
                    sha256=hashlib.sha256(content).hexdigest(),
                )
                yield str(path), content, metadata
            return

        session = sec_session(options.user_agent)
        if options.source_urls:
            for url in options.source_urls[: options.max_filings]:
                response = session.get(url, timeout=options.request_timeout)
                response.raise_for_status()
                content = response.content
                metadata = dict(options.metadata_by_source.get(url, {}))
                yield url, content, metadata
            return

        targets = self._target_filings(store, options)
        for row in targets.to_dict("records"):
            url = ownership_primary_document_url(
                str(row["cik"]),
                str(row["accession_number"]),
                str(row["primary_document"]),
            )
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            content = response.content
            store.con.execute(
                """
                DELETE FROM raw_source_files
                WHERE dataset_id = ? AND source_url = ?
                """,
                [self.dataset_id, url],
            )
            from .warehouse import record_source_file

            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={
                    "security_id": row["security_id"],
                    "cik": row["cik"],
                    "accession_number": row["accession_number"],
                    "form": row["form"],
                },
                sha256=hashlib.sha256(content).hexdigest(),
            )
            yield url, content, row

    def _target_filings(self, store: DuckDBStore, options: InsiderOwnershipOptions) -> pd.DataFrame:
        filters = [
            "s.primary_document IS NOT NULL",
            "s.primary_document <> ''",
        ]
        params: list[Any] = [options.max_filings]
        registered: list[str] = []
        if options.forms:
            forms = pd.DataFrame({"form": [str(form).upper() for form in options.forms]})
            store.con.register("insider_form_filter", forms)
            registered.append("insider_form_filter")
            filters.append("upper(s.form) IN (SELECT form FROM insider_form_filter)")
        if options.accession_numbers:
            accession_numbers = pd.DataFrame({"accession_number": [str(value).strip() for value in options.accession_numbers]})
            store.con.register("insider_accession_filter", accession_numbers)
            registered.append("insider_accession_filter")
            filters.append("s.accession_number IN (SELECT accession_number FROM insider_accession_filter)")
        if options.symbols:
            symbols = pd.DataFrame({"ticker": [str(symbol).strip().upper() for symbol in options.symbols if str(symbol).strip()]})
            store.con.register("insider_symbol_input", symbols)
            registered.append("insider_symbol_input")
            store.con.execute(
                """
                CREATE TEMPORARY TABLE insider_symbol_filter AS
                SELECT DISTINCT security_id
                FROM sec_company_tickers t
                JOIN insider_symbol_input i ON i.ticker = t.ticker
                """
            )
            registered.append("insider_symbol_filter")
            filters.append("s.security_id IN (SELECT security_id FROM insider_symbol_filter)")
        try:
            return store.con.execute(
                f"""
                SELECT
                    s.security_id,
                    s.cik,
                    s.accession_number,
                    s.form,
                    s.filing_date,
                    s.report_date,
                    s.acceptance_datetime,
                    s.primary_document
                FROM sec_submissions s
                WHERE {" AND ".join(filters)}
                ORDER BY
                    s.filing_date DESC NULLS LAST,
                    s.acceptance_datetime DESC NULLS LAST,
                    s.accession_number DESC
                LIMIT ?
                """,
                params,
            ).df()
        finally:
            for relation in registered:
                try:
                    store.con.unregister(relation)
                except Exception:
                    store.con.execute(f"DROP TABLE IF EXISTS {relation}")

    def _replace_rows(self, store: DuckDBStore, frames: dict[str, pd.DataFrame]) -> int:
        filings = frames["filing_form4"]
        if filings.empty:
            quality_check(
                store,
                dataset_id=self.dataset_id,
                table_name="filing_form4",
                check_name="rows_loaded",
                status="warning",
                observed_value=0.0,
                threshold_value=1.0,
                details={"reason": "no ownership XML sources matched"},
            )
            return 0

        with store.transaction():
            store.con.register("insider_filing_keys", filings[["accession_number"]].drop_duplicates())
            try:
                store.con.execute(
                    """
                    CREATE TEMPORARY TABLE insider_existing_plan_keys AS
                    SELECT DISTINCT plan_10b5_1_id AS plan_id
                    FROM insider_transaction
                    WHERE accession_number IN (SELECT accession_number FROM insider_filing_keys)
                      AND plan_10b5_1_id IS NOT NULL
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM tradingplan_10b5_1
                    WHERE plan_id IN (SELECT plan_id FROM insider_existing_plan_keys)
                    """
                )
                for table in (
                    "tradingplan_10b5_1",
                    "insider_holding",
                    "insider_transaction",
                    "insider_relationship",
                    "filing_form4",
                ):
                    if table == "tradingplan_10b5_1":
                        continue
                    store.con.execute(
                        f"""
                        DELETE FROM {table}
                        WHERE accession_number IN (SELECT accession_number FROM insider_filing_keys)
                        """
                    )
                for table in (
                    "filing_form4",
                    "insider_relationship",
                    "insider_transaction",
                    "insider_holding",
                    "tradingplan_10b5_1",
                ):
                    frame = frames[table]
                    if frame.empty:
                        continue
                    if table == "tradingplan_10b5_1":
                        store.con.register("insider_plan_keys", frame[["plan_id"]].drop_duplicates())
                        store.con.execute(
                            """
                            DELETE FROM tradingplan_10b5_1
                            WHERE plan_id IN (SELECT plan_id FROM insider_plan_keys)
                            """
                        )
                        store.con.unregister("insider_plan_keys")
                    insert_frame(store, frame, table, f"{table}_insert")

                self._upsert_issuer_seeds(store, frames["issuer_seed"])
                self._refresh_insiders(store, frames["insider_relationship"])
            finally:
                try:
                    store.con.unregister("insider_filing_keys")
                except Exception:
                    pass
                store.con.execute("DROP TABLE IF EXISTS insider_existing_plan_keys")

        return int(len(frames["insider_transaction"]) + len(frames["insider_holding"]))

    def _upsert_issuer_seeds(self, store: DuckDBStore, frame: pd.DataFrame) -> None:
        if frame.empty:
            return
        securities = frame[
            [
                "security_id",
                "issuer_id",
                "primary_symbol",
                "name",
                "asset_class",
                "country",
                "currency",
                "active",
                "first_seen_date",
                "last_seen_date",
                "source",
            ]
        ].drop_duplicates(subset=["security_id"])
        identifiers = []
        for row in frame.to_dict("records"):
            if row.get("cik"):
                identifiers.append(
                    {
                        "security_id": row["security_id"],
                        "id_type": "CIK",
                        "id_value": row["cik"],
                        "valid_from": row["first_seen_date"],
                        "valid_to": None,
                        "as_of_date": row["as_of_date"],
                        "available_at": row["available_at"],
                        "source": ISSUER_SEED_SOURCE,
                        "run_id": None,
                    }
                )
            if row.get("ticker"):
                identifiers.append(
                    {
                        "security_id": row["security_id"],
                        "id_type": "TICKER",
                        "id_value": row["ticker"],
                        "valid_from": row["first_seen_date"],
                        "valid_to": None,
                        "as_of_date": row["as_of_date"],
                        "available_at": row["available_at"],
                        "source": ISSUER_SEED_SOURCE,
                        "run_id": None,
                    }
                )
        identifier_frame = pd.DataFrame(identifiers)

        store.con.register("insider_issuer_securities", securities)
        try:
            store.con.execute(
                """
                INSERT INTO securities (
                    security_id, issuer_id, primary_symbol, name, asset_class,
                    country, currency, active, first_seen_date, last_seen_date, source
                )
                SELECT
                    security_id, issuer_id, primary_symbol, name, asset_class,
                    country, currency, active, first_seen_date, last_seen_date, source
                FROM insider_issuer_securities src
                WHERE NOT EXISTS (
                    SELECT 1 FROM securities dst WHERE dst.security_id = src.security_id
                )
                """
            )
        finally:
            store.con.unregister("insider_issuer_securities")

        if not identifier_frame.empty:
            # Collapse this batch's repeated open-ended sightings to one
            # canonical interval per identifier, then insert only identifiers
            # without an existing open-ended interval. Re-observing an issuer
            # therefore never starts a second overlapping interval and never
            # moves the earliest valid_from later.
            identifier_frame = dedupe_open_identifier_intervals(identifier_frame)
            store.con.register("insider_issuer_identifiers", identifier_frame)
            try:
                store.con.execute(
                    """
                    INSERT INTO security_identifier_history (
                        security_id, id_type, id_value, valid_from, valid_to,
                        as_of_date, available_at, source, run_id
                    )
                    SELECT
                        src.security_id, src.id_type, src.id_value, src.valid_from, src.valid_to,
                        src.as_of_date, src.available_at, src.source, src.run_id
                    FROM insider_issuer_identifiers src
                    WHERE NOT EXISTS (
                        SELECT 1 FROM security_identifier_history h
                        WHERE h.security_id = src.security_id
                          AND h.id_type = src.id_type
                          AND h.id_value = src.id_value
                          AND h.source = src.source
                          AND h.valid_to IS NULL
                    )
                    """
                )
            finally:
                store.con.unregister("insider_issuer_identifiers")

    def _refresh_insiders(self, store: DuckDBStore, relationships: pd.DataFrame) -> None:
        if relationships.empty:
            return
        affected = relationships[["insider_id"]].drop_duplicates()
        store.con.register("insider_refresh_ids", affected)
        try:
            store.con.execute(
                """
                DELETE FROM insider
                WHERE insider_id IN (SELECT insider_id FROM insider_refresh_ids)
                """
            )
            store.con.execute(
                """
                INSERT INTO insider (
                    insider_id,
                    reporting_owner_cik,
                    full_name,
                    full_name_norm,
                    resolution_source,
                    first_filing_date,
                    last_filing_date,
                    filing_count,
                    notes_json,
                    source,
                    run_id,
                    source_loaded_at
                )
                WITH scoped AS (
                    SELECT r.*
                    FROM insider_relationship r
                    JOIN insider_refresh_ids i ON i.insider_id = r.insider_id
                ),
                ranked AS (
                    SELECT
                        *,
                        row_number() OVER (
                            PARTITION BY insider_id
                            ORDER BY available_at DESC NULLS LAST, source_loaded_at DESC, accession_number DESC
                        ) AS rn
                    FROM scoped
                ),
                agg AS (
                    SELECT
                        insider_id,
                        min(valid_from) AS first_filing_date,
                        max(valid_from) AS last_filing_date,
                        count(DISTINCT accession_number) AS filing_count
                    FROM scoped
                    GROUP BY insider_id
                )
                SELECT
                    ranked.insider_id,
                    ranked.reporting_owner_cik,
                    ranked.full_name,
                    ranked.full_name_norm,
                    CASE WHEN ranked.reporting_owner_cik IS NOT NULL THEN 'EDGAR_CIK' ELSE 'NAME_MATCH' END AS resolution_source,
                    agg.first_filing_date,
                    agg.last_filing_date,
                    agg.filing_count,
                    '{}',
                    ranked.source,
                    ranked.run_id,
                    ranked.source_loaded_at
                FROM ranked
                JOIN agg USING (insider_id)
                WHERE ranked.rn = 1
                """
            )
        finally:
            store.con.unregister("insider_refresh_ids")

    def _record_checks(self, store: DuckDBStore, frames: dict[str, pd.DataFrame]) -> None:
        filing_rows = len(frames["filing_form4"])
        transaction_rows = len(frames["insider_transaction"])
        relationship_rows = len(frames["insider_relationship"])
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="filing_form4",
            check_name="rows_loaded",
            status="passed" if filing_rows > 0 else "warning",
            observed_value=float(filing_rows),
            threshold_value=1.0,
            details={
                "transaction_rows": transaction_rows,
                "relationship_rows": relationship_rows,
            },
        )
        bad_codes = 0
        if not frames["insider_transaction"].empty:
            codes = frames["insider_transaction"]["transaction_code"].dropna().astype(str).str.upper()
            bad_codes = int((~codes.isin(TRANSACTION_CODES)).sum())
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="insider_transaction",
            check_name="valid_transaction_codes",
            status="passed" if bad_codes == 0 else "failed",
            observed_value=float(bad_codes),
            threshold_value=0.0,
            details={"allowed_codes": sorted(TRANSACTION_CODES)},
        )


class BlockholderOwnershipDataset(Dataset):
    dataset_id = "sec_blockholder_ownership"
    source_name = BLOCKHOLDER_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: BlockholderOwnershipOptions) -> DatasetLoadResult:
        if options.max_filings < 1:
            raise ValueError("max_filings must be positive")

        source_loaded_at = now_utc_naive()
        parsed_frames: dict[str, list[pd.DataFrame]] = {
            "blockholder_filing": [],
            "blockholder_reporting_person": [],
            "issuer_seed": [],
        }
        source_count = 0
        for source_url, content, metadata in self._iter_sources(store, options):
            parsed = parse_blockholder_xml(
                content,
                source_url=source_url,
                metadata=metadata,
                source=options.source,
                run_id=options.run_id,
                source_loaded_at=source_loaded_at,
            )
            for key, frame in parsed.items():
                parsed_frames[key].append(frame)
            source_count += 1

        frames = {key: _concat_frames(value) for key, value in parsed_frames.items()}
        rows_loaded = self._replace_rows(store, frames)
        self._record_checks(store, frames)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows_loaded,
            source=options.source,
            details={
                "source_count": source_count,
                "filing_rows": int(len(frames["blockholder_filing"])),
                "reporting_person_rows": int(len(frames["blockholder_reporting_person"])),
            },
        )

    def _iter_sources(
        self,
        store: DuckDBStore,
        options: BlockholderOwnershipOptions,
    ) -> Iterable[tuple[str, bytes, dict[str, Any]]]:
        if options.source_files:
            for path in options.source_files[: options.max_filings]:
                content = path.read_bytes()
                metadata = dict(options.metadata_by_source.get(str(path), {}))
                metadata.setdefault("accession_number", path.stem)
                from .warehouse import record_source_file

                record_source_file(
                    store,
                    dataset_id=self.dataset_id,
                    source_url=str(path),
                    cache_path=path,
                    status="available",
                    metadata=metadata,
                    sha256=hashlib.sha256(content).hexdigest(),
                )
                yield str(path), content, metadata
            return

        session = sec_session(options.user_agent)
        if options.source_urls:
            for url in options.source_urls[: options.max_filings]:
                response = session.get(url, timeout=options.request_timeout)
                response.raise_for_status()
                content = response.content
                metadata = dict(options.metadata_by_source.get(url, {}))
                yield url, content, metadata
            return

        targets = self._target_filings(store, options)
        for row in targets.to_dict("records"):
            url = ownership_primary_document_url(
                str(row["cik"]),
                str(row["accession_number"]),
                str(row["primary_document"]),
            )
            response = session.get(url, timeout=options.request_timeout)
            response.raise_for_status()
            content = response.content
            from .warehouse import record_source_file

            record_source_file(
                store,
                dataset_id=self.dataset_id,
                source_url=url,
                status="fetched",
                metadata={
                    "security_id": row["security_id"],
                    "cik": row["cik"],
                    "accession_number": row["accession_number"],
                    "form": row["form"],
                },
                sha256=hashlib.sha256(content).hexdigest(),
            )
            yield url, content, row

    def _target_filings(self, store: DuckDBStore, options: BlockholderOwnershipOptions) -> pd.DataFrame:
        filters = [
            "s.primary_document IS NOT NULL",
            "s.primary_document <> ''",
        ]
        params: list[Any] = [options.max_filings]
        registered: list[str] = []
        if options.forms:
            forms = pd.DataFrame({"form": [str(form).upper() for form in options.forms]})
            store.con.register("blockholder_form_filter", forms)
            registered.append("blockholder_form_filter")
            filters.append("upper(s.form) IN (SELECT form FROM blockholder_form_filter)")
        if options.accession_numbers:
            accession_numbers = pd.DataFrame({"accession_number": [str(value).strip() for value in options.accession_numbers]})
            store.con.register("blockholder_accession_filter", accession_numbers)
            registered.append("blockholder_accession_filter")
            filters.append("s.accession_number IN (SELECT accession_number FROM blockholder_accession_filter)")
        if options.symbols:
            symbols = pd.DataFrame({"ticker": [str(symbol).strip().upper() for symbol in options.symbols if str(symbol).strip()]})
            store.con.register("blockholder_symbol_input", symbols)
            registered.append("blockholder_symbol_input")
            store.con.execute(
                """
                CREATE TEMPORARY TABLE blockholder_symbol_filter AS
                SELECT DISTINCT security_id
                FROM sec_company_tickers t
                JOIN blockholder_symbol_input i ON i.ticker = t.ticker
                """
            )
            registered.append("blockholder_symbol_filter")
            filters.append("s.security_id IN (SELECT security_id FROM blockholder_symbol_filter)")
        try:
            return store.con.execute(
                f"""
                SELECT
                    s.security_id,
                    s.cik,
                    s.accession_number,
                    s.form,
                    s.filing_date,
                    s.report_date,
                    s.acceptance_datetime,
                    s.primary_document
                FROM sec_submissions s
                WHERE {" AND ".join(filters)}
                ORDER BY
                    s.filing_date DESC NULLS LAST,
                    s.acceptance_datetime DESC NULLS LAST,
                    s.accession_number DESC
                LIMIT ?
                """,
                params,
            ).df()
        finally:
            for relation in registered:
                try:
                    store.con.unregister(relation)
                except Exception:
                    store.con.execute(f"DROP TABLE IF EXISTS {relation}")

    def _replace_rows(self, store: DuckDBStore, frames: dict[str, pd.DataFrame]) -> int:
        filings = frames["blockholder_filing"]
        if filings.empty:
            quality_check(
                store,
                dataset_id=self.dataset_id,
                table_name="blockholder_filing",
                check_name="rows_loaded",
                status="warning",
                observed_value=0.0,
                threshold_value=1.0,
                details={"reason": "no Schedule 13D/G XML sources matched"},
            )
            return 0

        with store.transaction():
            store.con.register("blockholder_filing_keys", filings[["accession_number"]].drop_duplicates())
            try:
                store.con.execute(
                    """
                    DELETE FROM blockholder_reporting_person
                    WHERE filing_id IN (
                        SELECT filing_id
                        FROM blockholder_filing
                        WHERE accession_number IN (SELECT accession_number FROM blockholder_filing_keys)
                    )
                    """
                )
                store.con.execute(
                    """
                    DELETE FROM blockholder_filing
                    WHERE accession_number IN (SELECT accession_number FROM blockholder_filing_keys)
                    """
                )
                insert_frame(store, filings, "blockholder_filing", "blockholder_filing_insert")
                insert_frame(
                    store,
                    frames["blockholder_reporting_person"],
                    "blockholder_reporting_person",
                    "blockholder_reporting_person_insert",
                )
                InsiderOwnershipDataset()._upsert_issuer_seeds(store, frames["issuer_seed"])
            finally:
                try:
                    store.con.unregister("blockholder_filing_keys")
                except Exception:
                    pass

        return int(len(frames["blockholder_filing"]) + len(frames["blockholder_reporting_person"]))

    def _record_checks(self, store: DuckDBStore, frames: dict[str, pd.DataFrame]) -> None:
        filing_rows = len(frames["blockholder_filing"])
        person_rows = len(frames["blockholder_reporting_person"])
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="blockholder_filing",
            check_name="rows_loaded",
            status="passed" if filing_rows > 0 else "warning",
            observed_value=float(filing_rows),
            threshold_value=1.0,
            details={"reporting_person_rows": person_rows},
        )
