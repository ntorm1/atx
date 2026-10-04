"""Original raw vendor close plus independently pinned dated economic evidence.

Supported vendor columns are exactly securityID/tradingDate/ticker_tk/close.
Currency, class, units and raw/split/FX semantics never come from absent vendor
fields or issuer primary flags. An independently reviewed external contract
must pin original authoritative economic evidence, exact quotations and dated
security/ticker bindings. No supplied boolean/admission ID is a certificate.
Only raw USD/common-share basis with explicitly unchanged conversion terms is
supported initially; other FX, ADS and split conversions remain unsupported.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import re
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path

from . import terminal_mna_v2 as T


@dataclass(frozen=True)
class EconomicPin:
    source_id: str
    sha256: str
    issuer_name: str
    ticker: str
    share_class: str
    currency: str
    unit: str
    basis: str
    valid_from: dt.date
    valid_to: dt.date
    available_at: dt.datetime
    class_quote: str
    currency_unit_quote: str
    raw_basis_quote: str
    conversion_basis_quote: str
    # Original authoritative evidence must bind these date strings exactly.
    validity_quote: str
    clock_basis: str
    fetched_at: dt.datetime


@dataclass(frozen=True)
class VendorPricePin:
    source_id: str
    sha256: str
    expected_schema: str
    security_id: int
    session_date: dt.date
    ticker: str
    available_at: dt.datetime
    economic_source_id: str


def _sha(source: bytes | Path) -> str:
    if isinstance(source, bytes):
        return hashlib.sha256(source).hexdigest()
    digest = hashlib.sha256()
    with source.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def read_economic(blob: bytes, pin: EconomicPin) -> EconomicPin:
    if hashlib.sha256(blob).hexdigest() != pin.sha256 or not pin.source_id:
        raise ValueError("original authoritative economic source identity mismatch")
    text = re.sub(r"\s+", " ", blob.decode("utf-8", errors="strict"))
    quotes = (pin.class_quote, pin.currency_unit_quote, pin.raw_basis_quote,
              pin.conversion_basis_quote, pin.validity_quote)
    if any(not quote or re.sub(r"\s+", " ", quote) not in text for quote in quotes):
        raise ValueError("complete original economic quotations required")
    # Supported evidence is a complete five-statement native block. Reject
    # unparsed context instead of selecting an affirmative substring from a
    # negated, conditional, quoted, or contradicted statement. In particular,
    # caller-selected quotations cannot discard scope in the original bytes.
    statements = (
        f"{pin.issuer_name} common stock trades as {pin.ticker}.",
        "Prices are USD per share.",
        "close is the unadjusted raw traded close.",
        "conversion terms are unchanged with no split or FX conversion.",
        f"Effective from {pin.valid_from.isoformat()} through {pin.valid_to.isoformat()}.",
    )
    normalized = lambda value: re.sub(r"\s+", " ", value).strip().casefold()
    if (tuple(normalized(quote) for quote in quotes) != tuple(normalized(value) for value in statements)
            or normalized(text) != normalized(" ".join(statements))):
        raise ValueError("complete affirmative native economic statement context unsupported")
    # The reviewed contract binds literal native identities; matching the words
    # alone is insufficient if any typed economic field or time interval differs.
    if (pin.share_class != "common stock" or pin.unit != "share" or pin.currency != "USD"
        or pin.basis != "raw-USD-common-share" or not pin.issuer_name or not pin.ticker
        or pin.issuer_name not in pin.class_quote or pin.ticker not in pin.class_quote
        or not re.search(r"\bcommon stock\b", pin.class_quote, re.I)
        or re.search(r"\bADSs?\b|\bADRs?\b|depositary|preferred|ordinary shares|\bunits?\b", pin.class_quote, re.I)
        or not re.search(r"\b(?:USD|U\.S\. dollars)\b.*\bper share\b", pin.currency_unit_quote, re.I)
        or re.search(r"\bor\b|\b(?:EUR|GBP|CAD|JPY|CNY)\b", pin.currency_unit_quote, re.I)
        or not re.fullmatch(r"close is the unadjusted raw traded close\.", pin.raw_basis_quote, re.I)
        or not re.fullmatch(r"conversion terms are unchanged with no split or FX conversion\.", pin.conversion_basis_quote, re.I)
        or re.search(r"\b(?:close|conversion terms) (?:is|are|can be|may be) (?:not\b|adjusted\b|changed\b|split-adjusted\b)|\bconversion terms (?:may|can|will) change\b", text, re.I)
        or pin.valid_from.isoformat() not in pin.validity_quote or pin.valid_to.isoformat() not in pin.validity_quote
        or pin.valid_to < pin.valid_from):
        raise ValueError("native economic class/currency/unit/raw/split/FX/date proof unsupported")
    T.utc(pin.available_at)
    if pin.clock_basis != "retrieval_floor" or T.utc(pin.available_at) != T.utc(pin.fetched_at):
        raise ValueError("economic evidence requires independently pinned conservative retrieval floor")
    return pin


def read_vendor_price(source: bytes | Path, pin: VendorPricePin, economic_blob: bytes,
                      economic_pin: EconomicPin, admission_id: str) -> T.Price:
    """Stream original row groups; exact SID/date/ticker and economic proof replay."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    if not admission_id or _sha(source) != pin.sha256:
        raise ValueError("original vendor source identity mismatch")
    proof = read_economic(economic_blob, economic_pin)
    if (pin.economic_source_id != proof.source_id or pin.ticker != proof.ticker
        or not proof.valid_from <= pin.session_date <= proof.valid_to):
        raise ValueError("dated native security/ticker/economic binding mismatch")
    handle = pq.ParquetFile(pa.BufferReader(source) if isinstance(source, bytes) else source)
    if str(handle.schema_arrow) != pin.expected_schema:
        raise ValueError("complete original vendor schema mismatch")
    required = ("securityID", "tradingDate", "ticker_tk", "close")
    if any(name not in handle.schema_arrow.names for name in required):
        raise ValueError("native original vendor close columns missing")
    matches, ticker_security_ids = [], set()
    for batch in handle.iter_batches(batch_size=32768, columns=list(required)):
        # Bounded batch, never materialize the full 3.6GB native history.
        for row in batch.to_pylist():
            if row["tradingDate"] == pin.session_date and row["ticker_tk"] == pin.ticker:
                ticker_security_ids.add(row["securityID"])
                if len(ticker_security_ids) > 1:
                    raise ValueError("ambiguous native vendor ticker/date security identity")
            if row["securityID"] == pin.security_id and row["tradingDate"] == pin.session_date:
                matches.append(row)
                if len(matches) > 1:
                    raise ValueError("ambiguous native vendor SID/date price")
    if len(matches) != 1 or matches[0]["ticker_tk"] != pin.ticker:
        raise ValueError("native vendor security/date/ticker price missing or mismatched")
    close = Decimal(str(matches[0]["close"]))
    if not close.is_finite() or close <= 0:
        raise ValueError("native raw traded close invalid")
    if _sha(source) != pin.sha256:
        raise ValueError("original vendor changed during replay")
    price_floor = dt.datetime.combine(pin.session_date, dt.time(22), tzinfo=T.UTC)
    ev = T.Evidence(pin.source_id, pin.sha256, max(T.utc(pin.available_at), T.utc(proof.available_at), price_floor), admission_id)
    return T.Price(pin.security_id, pin.session_date, close, proof.currency, proof.share_class,
                   proof.unit, proof.basis, ev)
