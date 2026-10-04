"""Conditional native semantic readers; immutable input admission remains mandatory.

These functions parse evidence rather than trusting qualified/matches flags. They
do not fetch, search a lake, certify quote currency, or close actual task gates.
"""
from __future__ import annotations
import datetime as dt
import re
from zoneinfo import ZoneInfo
from . import distributions as D


def accepted_clock(blob: bytes, accession: str) -> dt.datetime:
    """Original SEC index/header clock, explicit accession, DST ambiguity rejects."""
    text = blob.decode("utf-8")
    if accession not in text:
        raise ValueError("clock accession not present in original")
    match = re.search(r"Accepted</div>\s*<div[^>]*>(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})</div>", text)
    if match:
        local = dt.datetime.fromisoformat(match[1])
    else:
        match = re.search(r"<ACCEPTANCE-DATETIME>(\d{14})", text)
        if not match:
            raise ValueError("original acceptance clock missing")
        local = dt.datetime.strptime(match[1], "%Y%m%d%H%M%S")
    zone = ZoneInfo("America/New_York")
    values = {local.replace(tzinfo=zone, fold=fold).astimezone(dt.timezone.utc)
              for fold in (0, 1) if local.replace(tzinfo=zone, fold=fold).astimezone(dt.timezone.utc).astimezone(zone).replace(tzinfo=None) == local}
    if len(values) != 1:
        raise ValueError("ambiguous/nonexistent original acceptance wall clock")
    return values.pop()


def native_classes(blob: bytes) -> list[dict]:
    """Same-context native DEI title/symbol pairs; no issuer-primary expansion."""
    from lxml import html
    tree = html.fromstring(blob)
    facts = {}
    ciks = set()
    for node in tree.iter():
        name = node.get("name", "").split(":")[-1]
        value = " ".join(node.text_content().split())
        if name == "EntityCentralIndexKey" and value.isdigit():
            ciks.add(int(value))
        if name in {"TradingSymbol", "Security12bTitle", "Security12gTitle"}:
            context = node.get("contextref")
            if context:
                facts.setdefault(context, {}).setdefault(name, set()).add(value)
    if len(ciks) != 1:
        raise ValueError("native issuer CIK missing/ambiguous")
    result = []
    for context, values in facts.items():
        contexts = [node for node in tree.iter() if str(node.tag).split(":")[-1].lower() == "context" and node.get("id") == context]
        if len(contexts) != 1:
            continue
        identifiers = ["".join(node.itertext()).strip() for node in contexts[0].iter()
                       if str(node.tag).split(":")[-1].lower() == "identifier"]
        if len(identifiers) != 1 or not identifiers[0].isdigit() or int(identifiers[0]) != next(iter(ciks)):
            continue
        if any("legalentity" in node.get("dimension", "").lower() for node in contexts[0].iter()):
            continue
        symbols = values.get("TradingSymbol", set())
        titles = values.get("Security12bTitle", set()) | values.get("Security12gTitle", set())
        if len(symbols) == len(titles) == 1:
            title = next(iter(titles))
            if not re.search(r"preferred|depositary|\bADR\b|\bADS\b|warrant|units?|rights?|debt", title, re.I):
                result.append({"cik": next(iter(ciks)), "context": context,
                               "symbol": next(iter(symbols)), "title": title})
    return result


def qualify(event: dict, native_text: str, *, declaration_accession: str, declaration_clock: bytes,
            class_document: bytes, class_accession: str, class_clock: bytes,
            line_rows: list[dict]) -> dict:
    """Source-backed fixed regular rate on an exact native common class/dated line.

    Caller must independently admit every original blob and complete line rows.
    Output is an event-level primitive; this does not assume indefinite carry.
    """
    candidates = D.extract_declarations(native_text)
    fields = ("amount", "security_class", "currency", "special_flag", "declared_cadence", "evidence")
    if not any(all(row.get(k) == event.get(k) for k in fields) for row in candidates):
        raise ValueError("declaration does not replay original native evidence")
    if event.get("special_flag") is not False or event.get("declared_cadence") not in D.CADENCE or event.get("currency") != "USD":
        raise ValueError("fixed regular cadence/currency not established")
    if re.search(r"variable\s+(?:cash\s+)?dividend|dividend\s+policy.{0,80}(?:net\s+income|profit)|one.third\s+of", native_text, re.I | re.S):
        raise ValueError("variable-income policy cannot create fixed indicated rate")
    publication = accepted_clock(declaration_clock, declaration_accession)
    class_publication = accepted_clock(class_clock, class_accession)
    classes = [row for row in native_classes(class_document)
               if re.match(re.escape(event["security_class"]) + r"(?:\b|,)", row["title"], re.I)]
    matches = []
    for klass in classes:
        for line in line_rows:
            if (line["cik"] == klass["cik"] and line["ticker"] == klass["symbol"]
                    and line["valid_from"] <= publication.date() <= line["valid_to"]
                    and line["available_at"] <= publication and class_publication <= publication):
                matches.append((klass, line))
    if len(matches) != 1:
        raise ValueError("native class to dated security mapping missing/ambiguous/future")
    klass, line = matches[0]
    if event.get("cik") is not None and event["cik"] != klass["cik"]:
        raise ValueError("declaration/native class issuer conflict")
    available = max(publication, class_publication, line["available_at"])
    validity = re.search(r"effective\s+through\s+(" + D.DATE + r")", event["evidence"], re.I)
    valid_until = dt.datetime.combine(D._date(validity[1]), dt.time(22), D.UTC) if validity else None
    return {**event, "security_id": line["security_id"], "cik": klass["cik"],
            "available_at": available, "dividend_announcement_available_at": publication,
            "indicated_annual_dividend_rate": event["amount"] * D.CADENCE[event["declared_cadence"]],
            "security_class_currency_basis": event["security_class"] + "/USD",
            "source_qualification": "native_fixed_regular_event", "quote_currency_qualified": False,
            "rate_carry_status": "explicit_validity" if validity else "not_established",
            "source_valid_until": valid_until, "class_context": klass["context"]}


def carry(event: dict, cutoff: dt.datetime) -> float | None:
    """Only source-explicit validity; no arbitrary cadence-based expiry/forever carry.

    The qualifier parses explicit validity from the original declaration evidence.
    """
    source_valid_until = event.get("source_valid_until")
    if (event.get("source_qualification") != "native_fixed_regular_event" or source_valid_until is None
            or not event["available_at"] < cutoff <= source_valid_until):
        return None
    return event["indicated_annual_dividend_rate"]


def policy_state(native_text: str) -> str:
    """Explicit policy/change diagnostics; no fixed rate from silence or paid facts."""
    if re.search(r"will\s+not\s+pay\s+(?:a\s+)?dividend|(?:suspend|cancel|discontinue)\w*.{0,40}dividend", native_text, re.I):
        return "no_pay_or_cancel"
    if re.search(r"variable\s+(?:cash\s+)?dividend|dividend\s+policy.{0,80}(?:net\s+income|profit)|one.third\s+of", native_text, re.I | re.S):
        return "variable"
    rows = D.extract_declarations(native_text)
    if rows and all(row["special_flag"] is False and row["declared_cadence"] in D.CADENCE for row in rows):
        return "fixed_regular_declaration"
    return "unknown"


def rate_asof(events: list[dict], security_id: int, cutoff: dt.datetime) -> float | None:
    """Use only a complete independently admitted inventory with explicit validity.

    Caller must admit all event/clock dependencies through cutoff; this helper
    does not infer completeness from a list or successful document partitions.
    Any latest known unsupported/tombstone event invalidates older carried state.
    """
    visible = [event for event in events if event.get("security_id") == security_id and event["available_at"] < cutoff]
    if not visible:
        return None
    latest_clock = max(event["available_at"] for event in visible)
    latest = [event for event in visible if event["available_at"] == latest_clock]
    if len(latest) != 1 or latest[0].get("policy_state", "unknown") != "fixed_regular_declaration":
        return None
    return carry(latest[0], cutoff)
