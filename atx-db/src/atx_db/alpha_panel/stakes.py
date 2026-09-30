"""Stage ``stakes`` (lane OWN, S5.3): Schedules 13D / 13G beneficial-ownership stakes. See docs/ALPHA_PANEL_OWNERSHIP.md.

Sources: the primary document of each filing, fetched once through :mod:`sec_docs` into ``data/raw/sec_13dg/``
(FetchLedgerStore: gzip objects + ``fetch-ledger.jsonl``); the filing list and clocks come from the published
``sec_filings`` stage (``acceptance_utc`` per accession, per-file clock rule ``acceptance-per-file-clock-v1``).

Fetch phases (``fetch --phase``; SEC budget order, newest first inside a phase):

| phase | forms | window | document |
| --- | --- | --- | --- |
| ``xml13d`` | SCHEDULE 13D, SCHEDULE 13D/A | 2024-12-01 .. | ``primary_doc.xml`` (EDGAR XML since 2024-12-18) |
| ``xml13g`` | SCHEDULE 13G, SCHEDULE 13G/A | 2024-12-01 .. 2025-12-31 | ``primary_doc.xml`` |
| ``text13d`` | SC 13D (originals) | 2019-01-01 .. 2024-12-31 | the filing's primary HTML / text document |
| ``xml13g2026`` | SCHEDULE 13G, SCHEDULE 13G/A | 2026-01-01 .. | ``primary_doc.xml`` |
| ``text13da`` | SC 13D/A | 2019-01-01 .. 2024-12-31 | primary document |

Outputs (``build``):
* ``stakes/filings.parquet``: one row per fetched accession: form, schedule (13D/13G), amendment flag, issuer CIK /
  CUSIP / name, event date, 13G rule (13d-1(b)/(c)/(d)), group flag, number of reporting persons, stake as the
  maximum over reporting persons of shares and percent of class (persons in a group report the same block), the
  Item 4 purpose text (13D) and ``activism_flag``, filer CIK, ``parse_basis`` (``xml`` / ``text``), ``parse_ok``,
  ``available_at``;
* ``stakes/persons.parquet``: one row per (accession, reporting person) of the XML filings: name, CIK, voting and
  dispositive power, aggregate shares, percent, type codes (IA, HC, BD, IN, CO, PN, ...), member-of-group flag;
* ``stakes/notices.parquet``: every 13D/13G accession 2019+ in ``sec_filings`` (fetched or not) with its clock, so a
  consumer sees the event even where the document was not landed (``doc_status``).

Clock: ``available_at`` = the accession's resolved EDGAR acceptance from ``sec_filings`` (earliest over the CIK
rows of the accession; ``vintage_risk`` carries ``acceptance_clock_unresolved`` when that clock was the
conservative reading). Amendments are separate rows (new vintages). Pre-XML text parsing rules: see
:func:`parse_text_13d`.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html as _html
import json
import re
import sys
import time
from typing import Any

from . import common as C
from . import sec_docs as D

STAGE = "stakes"
SCHEMA = "atx.alpha-panel.stakes/v1"
SOURCE = "sec_13dg"
MODULES = ("stakes", "sec_docs", "common")
XML_START = dt.date(2024, 12, 1)
TEXT_START = dt.date(2019, 1, 1)
PHASES: dict[str, tuple[tuple[str, ...], dt.date, dt.date | None]] = {
    "xml13d": (("SCHEDULE 13D", "SCHEDULE 13D/A"), XML_START, None),
    "xml13g": (("SCHEDULE 13G", "SCHEDULE 13G/A"), XML_START, dt.date(2025, 12, 31)),
    "text13d": (("SC 13D",), TEXT_START, dt.date(2024, 12, 31)),
    "xml13g2026": (("SCHEDULE 13G", "SCHEDULE 13G/A"), dt.date(2026, 1, 1), None),
    "text13da": (("SC 13D/A",), TEXT_START, dt.date(2024, 12, 31)),
}
ALL_FORMS = ("SCHEDULE 13D", "SCHEDULE 13D/A", "SCHEDULE 13G", "SCHEDULE 13G/A", "SC 13D", "SC 13D/A", "SC 13G",
             "SC 13G/A")


def phase_urls(phase: str) -> list[tuple[str, str]]:
    forms, start, end = PHASES[phase]
    out = []
    for acc, cik, _form, _fd, pdoc, _av, _ciks in D.select_filings(forms, start, end):
        doc = D.structured_document(pdoc) if phase.startswith("xml") else pdoc
        if doc:
            out.append((acc, D.doc_url(cik, acc, doc)))
    return out


def fetch(phase: str, max_new: int, threads: int = 2) -> dict[str, Any]:
    urls = [u for _a, u in phase_urls(phase)]
    print(f"stakes {phase}: {len(urls)} filings", flush=True)
    return D.fetch_urls(SOURCE, urls, max_new=max_new, threads=threads)


# ---------------------------------------------------------------- XML (EDGAR Schedule 13D / 13G technical spec)
def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _num(s: str | None) -> float | None:
    if s is None:
        return None
    t = re.sub(r"[,\s$%*]", "", s)
    try:
        return float(t)
    except ValueError:
        return None


def _first(el, name: str) -> str | None:
    for e in el.iter():
        if _local(e.tag) == name and e.text and e.text.strip():
            return e.text.strip()
    return None


def _all(el, name: str) -> list[str]:
    return [e.text.strip() for e in el.iter() if _local(e.tag) == name and e.text and e.text.strip()]


def _date_mdy(s: str | None) -> dt.date | None:
    if not s:
        return None
    for fmt in ("%m/%d/%Y", "%Y-%m-%d"):
        try:
            return dt.datetime.strptime(s.strip(), fmt).date()
        except ValueError:
            continue
    return None


def parse_xml(body: bytes) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Schedule 13D / 13G primary_doc.xml -> (filing fields, reporting persons)."""
    import xml.etree.ElementTree as ET

    root = ET.fromstring(body)
    sub = _first(root, "submissionType") or ""
    is13g = "13G" in sub.upper()
    f: dict[str, Any] = {"submission_type": sub, "schedule": "13G" if is13g else "13D",
                         "filer_cik": _int(_first(root, "cik")),
                         "security_title": _first(root, "securitiesClassTitle"),
                         "issuer_cik": _int(_first(root, "issuerCIK") or _first(root, "issuerCik")),
                         "issuer_name": _first(root, "issuerName"),
                         "cusip": _clean_cusip(_first(root, "issuerCUSIP") or _first(root, "issuerCusipNumber")),
                         "cusips": ",".join(_all(root, "issuerCusipNumber")) or None,
                         "event_date": _date_mdy(_first(root, "dateOfEvent") or _first(root, "eventDateRequiresFilingThisStatement")),
                         "amendment_no": _int(_first(root, "amendmentNo")),
                         "rule_13g": ",".join(_all(root, "designateRulePursuantThisScheduleFiled")) or None,
                         "previously_filed_13g": (_first(root, "previouslyFiledFlag") or "").lower() == "true" or None}
    persons = []
    tag = "coverPageHeaderReportingPersonDetails" if is13g else "reportingPersonInfo"
    for p in (e for e in root.iter() if _local(e.tag) == tag):
        shares = _num(_first(p, "reportingPersonBeneficiallyOwnedAggregateNumberOfShares") or _first(p, "aggregateAmountOwned"))
        pct = _num(_first(p, "classPercent") or _first(p, "percentOfClass"))
        persons.append({"person_name": _first(p, "reportingPersonName"), "person_cik": _int(_first(p, "reportingPersonCIK")),
                        "member_of_group": _first(p, "memberOfGroup"), "citizenship": _first(p, "citizenshipOrOrganization"),
                        "fund_source": _first(p, "fundType"), "sole_voting": _num(_first(p, "soleVotingPower")),
                        "shared_voting": _num(_first(p, "sharedVotingPower")),
                        "sole_dispositive": _num(_first(p, "soleDispositivePower")),
                        "shared_dispositive": _num(_first(p, "sharedDispositivePower")), "shares": shares, "percent": pct,
                        "person_type": ",".join(_all(p, "typeOfReportingPerson")) or None})
    purpose = None
    for e in (() if is13g else root.iter()):
        if _local(e.tag) == "item4":
            purpose = " ".join(t.strip() for t in e.itertext() if t and t.strip()) or None
            break
    f["purpose_text"] = purpose[:20000] if purpose else None
    return f, persons


def _int(s: str | None) -> int | None:
    if not s:
        return None
    t = re.sub(r"\D", "", s)
    return int(t) if t else None


def _clean_cusip(s: str | None) -> str | None:
    if not s:
        return None
    c = re.sub(r"[^0-9A-Za-z]", "", s).upper()
    return c if len(c) in (8, 9) and not set(c) <= {"0"} else None


# ---------------------------------------------------------------- text (SC 13D before the XML mandate)
_BLOCK_TAGS = re.compile(r"(?i)<br\s*/?>|</(p|div|tr|td|th|li|h\d|table)>")
_NUM = r"(\d{1,3}(?:[,\.]\d{3})+|\d+)(?:\.\d+)?"


def render_text(body: bytes) -> str:
    """HTML or plain-text filing document -> text with one line per block element (entities decoded)."""
    t = body.decode("utf-8", errors="replace")
    if re.search(r"(?i)<html|<body|<table|<p[ >]|<div", t[:20000]):
        t = re.sub(r"(?is)<(script|style|head)\b.*?</\1>", " ", t)
        t = _BLOCK_TAGS.sub("\n", t)
        t = re.sub(r"<[^>]+>", " ", t)
    t = _html.unescape(t).replace("\xa0", " ")
    t = re.sub(r"[ \t\r\f\v]+", " ", t)
    return re.sub(r"\n\s*\n+", "\n", t)


def _before_marker(text: str, marker: str, lines: int = 3) -> str | None:
    m = re.search(marker, text, re.IGNORECASE)
    if not m:
        return None
    prev = [ln.strip(" _-") for ln in text[:m.start()].split("\n")]
    prev = [ln for ln in prev if ln and not re.fullmatch(r"[_\-=\s]*", ln)]
    return " ".join(prev[-lines:]) if prev else None


_AMEND_PREFIX = re.compile(r"(?i)^\s*\)?\s*\*?\s*(?:\(\s*amendment\s*(?:no\.?)?\s*[_\d\s]*\)\s*\*?\s*\d?\s*)?\)?\*?")
_ISSUER_NOISE = re.compile(r"(?i)securities\s+exchange\s+act|schedule\s+13d|rule\s+13d|13d-\d|amendments?\s+thereto|"
                           r"information\s+to\s+be\s+included|pursuant\s+to|securities\s+and\s+exchange\s+commission|"
                           r"washington|omb\b|burden|hours\s+per\s+response|\(\s*amendment")


def _issuer_name(text: str) -> str | None:
    """Up to two lines before ``(Name of Issuer)``, stopping at form boilerplate (the form title, the Exchange Act
    line, the Rule 13d-1 / 13d-2 caption, the amendment number), with an ``(Amendment No. )*`` prefix removed."""
    m = re.search(r"\(\s*Name\s+of\s+(?:the\s+)?Issuer\s*\)", text, re.IGNORECASE)
    if not m:
        return None
    out: list[str] = []
    for ln in reversed(text[:m.start()].split("\n")[-8:]):
        ln = ln.strip(" _-\t")
        if _ISSUER_NOISE.search(ln):
            stripped = _AMEND_PREFIX.sub("", ln).strip(" _-*")
            if stripped and not _ISSUER_NOISE.search(stripped):
                out.append(stripped)
            break
        ln = _AMEND_PREFIX.sub("", ln).strip(" _-*")
        if not ln or re.fullmatch(r"[_\-=\s\d()*]*", ln):
            if out:
                break
            continue
        out.append(ln)
        if len(out) == 2:
            break
    return " ".join(reversed(out)) or None


ACTIVISM_TERMS = (
    "nominat", "proxy contest", "solicit proxies", "solicitation of proxies", "board seat", "board representation",
    "representation on the board", "director candidates", "replace directors", "reconstitut", "strategic alternatives",
    "sale of the issuer", "sale of the company", "explore a sale", "letter to the board", "delivered a letter",
    "sent a letter", "cooperation agreement", "settlement agreement", "nomination agreement", "special meeting",
    "shareholder proposal", "stockholder proposal", "vote against", "oppose the", "unsolicited proposal",
    "non-binding proposal", "non-binding indication", "indication of interest", "take the issuer private",
    "take the company private", "going private", "going-private", "acquire all of the outstanding",
    "acquire all outstanding", "merger proposal", "tender offer", "demand", "consent solicitation")
BOILERPLATE_RE = re.compile(r"(?is)(may|might|could|reserve[sd]? the right to|intend[s]? to)\W+(?:\w+\W+){0,40}?"
                            r"(propose|take|consider)\W+(?:\w+\W+){0,12}?(actions?|matters?|transactions?)\W+(?:\w+\W+){0,12}?"
                            r"\(a\)\s*(?:through|to|-|–)\s*\(j\)")  # noqa: RUF001


def activism_terms(purpose: str | None) -> list[str]:
    """Item 4 purpose text -> the strong engagement terms it contains (the generic '(a) through (j)' reservation
    of rights that nearly every 13D carries is removed first)."""
    if not purpose:
        return []
    t = BOILERPLATE_RE.sub(" ", purpose).lower()
    return sorted({k for k in ACTIVISM_TERMS if k in t})


def parse_text_13d(body: bytes) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Pre-XML SC 13D document -> (filing fields, reporting persons). Rule ``13d-text-v1``:

    * issuer: :func:`_issuer_name` (up to two lines before ``(Name of Issuer)``, form boilerplate dropped); CUSIP:
      the 9-character token before ``(CUSIP Number)`` (else after ``CUSIP No.``); event date: the last date in the
      three lines before ``(Date of Event``;
    * per cover page (one per ``NAME(S) OF REPORTING PERSON``): the name that follows, the first number after row 11
      (``AGGREGATE AMOUNT BENEFICIALLY OWNED``) and the first percentage after row 13 (``PERCENT OF CLASS``);
    * Item 4 (``Purpose of Transaction``) up to Item 5, and :func:`activism_terms` over it.
    """
    text = render_text(body)
    f: dict[str, Any] = {"schedule": "13D", "issuer_name": _issuer_name(text)}
    cus = _before_marker(text, r"\(\s*CUSIP\s+Number\s*\)", 1)
    m = re.search(r"\b([0-9A-Z]{6}\s?[0-9A-Z]{2}\s?[0-9A-Z]?)\b", (cus or "").upper())
    if not m:
        m = re.search(r"CUSIP\s+(?:No\.?|Number)\s*:?\s*([0-9A-Z]{6}\s?[0-9A-Z]{2}\s?[0-9A-Z]?)", text, re.IGNORECASE)
    f["cusip"] = _clean_cusip(m.group(1)) if m else None
    ev = _before_marker(text, r"\(\s*Date\s+of\s+(?:the\s+)?Event", 3)
    f["event_date"] = _parse_long_date(ev)
    persons = []
    starts = [m.end() for m in re.finditer(r"NAMES?\s+OF\s+REPORTING\s+PERSONS?", text, re.IGNORECASE)]
    for i, s in enumerate(starts):
        seg = text[s: starts[i + 1] if i + 1 < len(starts) else min(len(text), s + 6000)]
        nm = re.search(r"^\s*(?:S\.?S\.?\s+OR\s+I\.?R\.?S\.?.*?PERSONS?\)?\s*)?\n?\s*([^\n]{2,200})", seg, re.IGNORECASE)
        name = nm.group(1).strip(" :-") if nm else None
        if name and re.match(r"(?i)I\.?R\.?S|S\.S\.", name):
            nxt = seg[nm.end():].strip().split("\n", 1)[0]
            name = nxt.strip(" :-") or name
        agg = re.search(r"AGGREGATE\s+AMOUNT\s+BENEFICIALLY\s+OWNED(?:\s+BY\s+EACH\s+REPORTING\s+PERSON)?", seg, re.IGNORECASE)
        shares = _first_amount(seg[agg.end(): agg.end() + 400]) if agg else None
        pc = re.search(r"PERCENT(?:AGE)?\s+OF\s+CLASS\s+REPRESENTED", seg, re.IGNORECASE)
        pct = None
        if pc:
            pm = re.search(r"(?:ROW\s*\(?11\)?)?[^%]{0,250}?(\d{1,3}(?:\.\d+)?)\s*%", seg[pc.end(): pc.end() + 400],
                           re.IGNORECASE | re.S)
            pct = float(pm.group(1)) if pm else None
        tp = re.search(r"TYPE\s+OF\s+REPORTING\s+PERSON[^\n]*\n?\s*([A-Z]{2}(?:\s*[,;/]\s*[A-Z]{2})*)", seg)
        persons.append({"person_name": name, "shares": shares, "percent": pct if pct is None or pct <= 100 else None,
                        "person_type": tp.group(1).replace(" ", "") if tp else None})
    i4 = re.search(r"ITEM\s*4\.?\s*[:\-–]?\s*PURPOSE\s+OF\s+(?:THE\s+)?TRANSACTIONS?", text, re.IGNORECASE)  # noqa: RUF001
    if i4:
        i5 = re.search(r"ITEM\s*5\.?\s*[:\-–]?\s*INTERESTS?\s+IN\s+(?:THE\s+)?SECURITIES", text[i4.end():], re.IGNORECASE)  # noqa: RUF001
        f["purpose_text"] = text[i4.end(): i4.end() + (i5.start() if i5 else 20000)].strip()[:20000] or None
    else:
        f["purpose_text"] = None
    return f, persons


def _first_amount(s: str) -> float | None:
    """First share amount in a cover-page cell: skips the next row number (a lone 1-2 digit token directly followed by
    a row label), accepts 0 / -0- / None."""
    s2 = s.replace("-0-", " 0 ")
    for m in re.finditer(r"(?<![\w.])(\d{1,3}(?:,\d{3})+(?:\.\d+)?|\d+(?:\.\d+)?)(?![\w%])", s2):
        tok = m.group(1)
        after = s2[m.end(): m.end() + 60]
        if re.fullmatch(r"\d{1,2}", tok) and re.match(r"\s*\n?\s*(CHECK|PERCENT|TYPE|\(|SEC USE|SOLE|SHARED)", after, re.I):
            continue
        return float(tok.replace(",", ""))
    return None


def _parse_long_date(s: str | None) -> dt.date | None:
    """The last date in ``s`` (``May 25, 2023``, ``05/25/2023``, ``25 May 2023``); None when there is none."""
    if not s:
        return None
    cands = re.findall(r"([A-Za-z]+\.?\s+\d{1,2}\s*,?\s*\d{4}|\d{1,2}/\d{1,2}/\d{2,4}|\d{1,2}\s+[A-Za-z]+,?\s+\d{4})", s)
    for c in reversed(cands):
        c = re.sub(r"\s*,\s*", ", ", re.sub(r"\s+", " ", c.replace(".", ""))).strip()
        for fmt in ("%B %d, %Y", "%B %d %Y", "%b %d, %Y", "%b %d %Y", "%m/%d/%Y", "%m/%d/%y", "%d %B %Y", "%d %B, %Y"):
            try:
                return dt.datetime.strptime(c, fmt).date()
            except ValueError:
                continue
    return None


# ---------------------------------------------------------------- build
TEXT_RULE = "13d-text-v1"
PURPOSE_MAX_CHARS = 4000
FILING_FIELDS = ("accession", "form", "schedule", "is_amendment", "filing_date", "available_at", "acceptance_basis",
                 "issuer_cik", "issuer_name", "cusip", "security_id", "map_basis", "event_date", "rule_13g", "filer_cik",
                 "n_persons", "stake_shares", "stake_percent", "lead_person", "lead_person_type", "any_group",
                 "purpose_text", "activism_flag", "activism_terms", "parse_basis", "parse_ok", "doc_url", "doc_sha256")
PERSON_FIELDS = ("accession", "seq", "person_name", "person_cik", "member_of_group", "citizenship", "fund_source",
                 "sole_voting", "shared_voting", "sole_dispositive", "shared_dispositive", "shares", "percent", "person_type")


def _cusip_map(cusips: set[str] | None = None) -> dict[str, list[tuple[dt.date, int]]]:
    """13F PIT CUSIP map (stage thirteenf, rule 13f-cusip-ftd-window-v1): cusip -> [(quarter end, security_id)],
    restricted to ``cusips`` when given (keeps the Python-side map small)."""
    import pyarrow as pa
    import pyarrow.compute as pc
    import pyarrow.parquet as pq

    t = pq.read_table(C.build_root() / "thirteenf" / "cusip_map_pit.parquet", columns=["period_q", "cusip", "security_id"])
    if cusips is not None:
        t = t.filter(pc.is_in(t["cusip"], value_set=pa.array(sorted(cusips), pa.string())))
    out: dict[str, list[tuple[dt.date, int]]] = {}
    for p, c, sid in zip(*(t.column(k).to_pylist() for k in ("period_q", "cusip", "security_id")), strict=True):
        if sid is not None:
            out.setdefault(c, []).append((p, sid))
    for v in out.values():
        v.sort()
    return out


def map_cusip(cmap: dict[str, list[tuple[dt.date, int]]], cusip: str | None, on: dt.date) -> tuple[int | None, str | None]:
    """security_id of the latest 13F quarter end <= ``on`` whose PIT map carries the CUSIP (``13f_map_asof``), else
    the earliest later quarter (``13f_map_next_quarter``: identity evidence only, no stake value moves in time)."""
    if not cusip:
        return None, None
    rows = cmap.get(cusip) or []
    if not rows:
        return None, "unmapped"
    before = [r for r in rows if r[0] <= on]
    if before:
        return before[-1][1], "13f_map_asof"
    return rows[0][1], "13f_map_next_quarter"


def build() -> dict[str, Any]:
    """Parse every landed document, write filings / persons / notices, publish the manifest (pyarrow + stdlib only)."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    t0 = time.perf_counter()
    store = D.store(SOURCE)
    out = C.stage_dir(STAGE)
    fw = pq.ParquetWriter(out / "filings.parquet.partial", _filing_schema(), compression="zstd")
    pw = pq.ParquetWriter(out / "persons.parquet.partial", _person_schema(), compression="zstd")
    small: list[dict[str, Any]] = []
    n_f = n_p = 0
    stats: dict[str, Any] = {}
    for phase in PHASES:
        frows: list[dict[str, Any]] = []
        prows: list[dict[str, Any]] = []
        forms, start, end = PHASES[phase]
        st = {"selected": 0, "landed": 0, "parsed": 0, "parse_error": 0, "not_landed": 0}
        for acc, cik, form, fd, pdoc, av, _ciks in D.select_filings(forms, start, end):
            st["selected"] += 1
            doc = D.structured_document(pdoc) if phase.startswith("xml") else pdoc
            url = D.doc_url(cik, acc, doc) if doc else None
            rec = store.lookup(url) if url else None
            if rec is None or not rec.ok:
                st["not_landed"] += 1
                continue
            st["landed"] += 1
            body = store.read(rec)
            try:
                f, persons = parse_xml(body) if phase.startswith("xml") else parse_text_13d(body)
                ok = bool(persons) and any(p.get("shares") is not None or p.get("percent") is not None for p in persons)
            except Exception as exc:
                f, persons, ok = {"schedule": "13G" if "13G" in form else "13D", "parse_error": str(exc)[:200]}, [], False
            st["parsed" if ok else "parse_error"] += 1
            lead = max(persons, key=lambda p: (p.get("shares") or -1, p.get("percent") or -1)) if persons else {}
            terms = activism_terms(f.get("purpose_text")) if f.get("schedule") == "13D" else []
            frows.append({"accession": acc, "form": form, "schedule": f.get("schedule"), "is_amendment": form.endswith("/A"),
                          "filing_date": fd, "available_at": av, "acceptance_basis": "sec_filings_available_at",
                          "issuer_cik": f.get("issuer_cik"), "issuer_name": f.get("issuer_name"), "cusip": f.get("cusip"),
                          "security_id": None, "map_basis": None, "event_date": f.get("event_date"),
                          "rule_13g": f.get("rule_13g"), "filer_cik": f.get("filer_cik"), "n_persons": len(persons),
                          "stake_shares": max((p["shares"] for p in persons if p.get("shares") is not None), default=None),
                          "stake_percent": max((p["percent"] for p in persons if p.get("percent") is not None), default=None),
                          "lead_person": lead.get("person_name"), "lead_person_type": lead.get("person_type"),
                          "any_group": any((p.get("member_of_group") or "").lower() == "a" for p in persons) or None,
                          "purpose_text": (f.get("purpose_text") or "")[:PURPOSE_MAX_CHARS] or None,
                          "activism_flag": bool(terms) if f.get("schedule") == "13D" else None,
                          "activism_terms": ",".join(terms) or None,
                          "parse_basis": "xml" if phase.startswith("xml") else TEXT_RULE, "parse_ok": ok, "doc_url": url,
                          "doc_sha256": rec.sha256})
            for i, p in enumerate(persons):
                prows.append({"accession": acc, "seq": i + 1, **{k: p.get(k) for k in PERSON_FIELDS[2:]}})
        stats[phase] = st
        cmap = _cusip_map({r["cusip"] for r in frows if r["cusip"]})
        for r in frows:
            r["security_id"], r["map_basis"] = map_cusip(cmap, r["cusip"], r["filing_date"])
        del cmap
        if frows:
            fw.write_table(pa.Table.from_pylist(frows, schema=_filing_schema()), row_group_size=8192)
        if prows:
            pw.write_table(pa.Table.from_pylist(prows, schema=_person_schema()), row_group_size=32768)
        small += [{"accession": r["accession"], "parse_ok": r["parse_ok"], "security_id": r["security_id"]} for r in frows]
        n_f, n_p = n_f + len(frows), n_p + len(prows)
        del frows, prows
        print(f"stakes build {phase}: {st}", flush=True)
    fw.close()
    pw.close()
    for n in ("filings", "persons"):
        (out / f"{n}.parquet.partial").replace(out / f"{n}.parquet")
    notices = _notices({r["accession"] for r in small})
    _write(notices, out / "notices.parquet")
    receipt = {"phases": stats, "rows": {"filings": n_f, "persons": n_p, "notices": notices.num_rows},
               "coverage_by_year_form": coverage(small, notices), "timings_s": {"total": round(time.perf_counter() - t0, 1)}}
    root = C.build_root()
    payload = {
        "rules": {"xml": "EDGAR Schedule 13D/13G XML (primary_doc.xml): cover-page rows per reporting person",
                  "text": parse_text_13d.__doc__, "activism": activism_terms.__doc__,
                  "stake": "filing stake = the maximum over reporting persons of aggregate shares and of percent of class",
                  "security_id": map_cusip.__doc__,
                  "available_at": "the accession's resolved EDGAR acceptance from sec_filings (earliest over its CIK rows)",
                  "vintage": "amendments are separate accessions and rows"},
        "staleness": "event data, no staleness",
        "sources": {"store": str(D.raw_root(SOURCE)), "fetch_ledger": D.ledger_counts(SOURCE),
                    "objects_bytes": D.objects_bytes(SOURCE)},
        "inputs": {"input_manifests_sha256": {k: C.sha256_file(root / k / "manifest.json")
                                              for k in ("sec_filings", "thirteenf") if (root / k / "manifest.json").exists()}},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def _filing_schema():
    import pyarrow as pa

    types = {"is_amendment": pa.bool_(), "filing_date": pa.date32(), "available_at": pa.timestamp("us", tz="UTC"),
             "issuer_cik": pa.int64(), "security_id": pa.int64(), "event_date": pa.date32(), "filer_cik": pa.int64(),
             "n_persons": pa.int32(), "stake_shares": pa.float64(), "stake_percent": pa.float64(), "any_group": pa.bool_(),
             "activism_flag": pa.bool_(), "parse_ok": pa.bool_()}
    return pa.schema([(k, types.get(k, pa.string())) for k in FILING_FIELDS])


def _person_schema():
    import pyarrow as pa

    types = {"seq": pa.int32(), "person_cik": pa.int64(), **{k: pa.float64() for k in PERSON_FIELDS[7:13]}}
    return pa.schema([(k, types.get(k, pa.string())) for k in PERSON_FIELDS])


def _write(tab, dest) -> None:
    import os

    import pyarrow.parquet as pq

    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(tab, tmp, compression="zstd", row_group_size=32768)
    os.replace(tmp, dest)


def _notices(parsed: set[str]):
    """Every 13D/13G accession from 2019 in sec_filings with its clock, a subject / filer CIK split and doc status.

    Subject company = the one CIK of the accession that EDGAR lists with a SIC code (issuers carry one, reporting
    persons rarely); ``subject_basis`` records ``single_cik_with_sic`` / ``ambiguous`` / ``none_with_sic``."""
    import pyarrow as pa
    import pyarrow.compute as pc
    root = C.build_root() / "sec_filings"
    prof = D.sic_ciks()
    rows = D.read_filtered(root / "filings.parquet", ["accession", "cik", "form", "filing_date", "available_at"],
                           D.forms_mask(ALL_FORMS, TEXT_START))
    rows = rows.append_column("has_sic", pc.is_in(rows["cik"], value_set=prof))
    g = rows.group_by("accession", use_threads=False).aggregate([("form", "min"), ("filing_date", "min"), ("available_at", "min"),
                                              ("cik", "count_distinct")])
    s = rows.filter(rows["has_sic"]).group_by("accession", use_threads=False).aggregate([("cik", "count_distinct"), ("cik", "min")])
    s = s.rename_columns(["accession", "n_sic", "subject_cik"])
    f = rows.filter(pc.invert(rows["has_sic"])).sort_by("cik").group_by("accession", use_threads=False).aggregate(
        [("cik", "distinct")])
    f = pa.table({"accession": f["accession"],
                  "filer_ciks": pc.binary_join(pc.cast(f["cik_distinct"], pa.list_(pa.string())), ",")})
    t = g.join(s, "accession", join_type="left outer").join(f, "accession", join_type="left outer")
    n_sic = pc.fill_null(t["n_sic"], 0)
    basis = pc.if_else(pc.equal(n_sic, 1), "single_cik_with_sic", pc.if_else(pc.greater(n_sic, 1), "ambiguous", "none_with_sic"))
    filer = t["filer_ciks"]
    form = t["form_min"]
    out = pa.table({
        "accession": t["accession"], "form": form,
        "schedule": pc.if_else(pc.match_substring(form, "13G"), "13G", "13D"),
        "is_amendment": pc.ends_with(form, "/A"), "filing_date": t["filing_date_min"],
        "available_at": pc.cast(t["available_at_min"], pa.timestamp("us", tz="UTC")),
        "subject_cik": pc.if_else(pc.equal(n_sic, 1), t["subject_cik"], pa.scalar(None, pa.int64())),
        "subject_basis": basis, "filer_ciks": filer,
        "n_ciks": pc.cast(t["cik_count_distinct"], pa.int32()),
        "doc_status": pc.if_else(pc.is_in(t["accession"], value_set=pa.array(sorted(parsed), pa.string())), "parsed",
                                 "not_landed")})
    return out.sort_by([("filing_date", "ascending"), ("accession", "ascending")])


def coverage(small: list[dict[str, Any]], notices) -> dict[str, Any]:
    """Per filing year and form: notices, documents parsed, parse_ok share of notices, CUSIP-mapped documents."""
    out: dict[str, Any] = {}
    by_acc = {r["accession"]: r for r in small}
    for acc, form, fd in zip(*(notices.column(k).to_pylist() for k in ("accession", "form", "filing_date")), strict=True):
        c = out.setdefault(f"{fd.year}|{form}", {"notices": 0, "parsed_docs": 0, "parse_ok": 0, "mapped": 0})
        c["notices"] += 1
        f = by_acc.get(acc)
        if f:
            c["parsed_docs"] += 1
            c["parse_ok"] += bool(f["parse_ok"])
            c["mapped"] += f["security_id"] is not None
    for c in out.values():
        c["parse_ok_share_of_notices"] = round(c["parse_ok"] / c["notices"], 4) if c["notices"] else None
    return dict(sorted(out.items()))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--phase", choices=sorted(PHASES), action="append", required=True)
    f.add_argument("--max-new", type=int, required=True, help="new SEC requests allowed in this run (all phases)")
    f.add_argument("--threads", type=int, default=2)
    sub.add_parser("build")
    args = ap.parse_args(argv)
    if args.cmd == "build":
        D.start_memory_trace()
        rec = build()
        print(json.dumps({k: rec[k] for k in ("phases", "rows")}, default=str), flush=True)
        print(json.dumps({"peak_memory_gb": D.peak_memory_gb(), "run": "unguarded per C-1 (pyarrow + stdlib, no DuckDB)"}),
              flush=True)
        return 0
    if args.cmd == "fetch":
        D.start_memory_trace()
        left = args.max_new
        for ph in args.phase:
            t0 = time.perf_counter()
            st = fetch(ph, left, args.threads)
            left -= st["attempted"]
            print(json.dumps({"phase": ph, **st, "elapsed_s": round(time.perf_counter() - t0, 1)}), flush=True)
            if left <= 0:
                break
        print(json.dumps({"peak_memory_gb": D.peak_memory_gb(), "run": "unguarded per C-1 (network landing, no DuckDB)"}),
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
