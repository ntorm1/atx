"""Stage ``events`` table ``mna`` (S6.1): merger and tender-offer deals from 8-K text and deal-form metadata.

Documents (2019+): the 8-K main documents (item 1.01 / 2.01 summaries of a merger agreement) and EX-99 releases that
EDGAR full-text search hits for "converted into the right to receive" (``events_sources`` query ``merger``), and the
SC TO-T / SC 14D9 documents hitting "net to the seller" (query ``tender``), fetched into ``data/raw/sec_events``.

Per document (:func:`parse_merger_text`, pure):

* **Consideration** after the first "converted into the right to receive" (or a tender offer's "at a price of $X per
  Share, net to the seller in cash"): ``cash_per_share`` (a dollar amount below $10,000 "in cash" / "without
  interest"), ``stock_ratio`` (N shares / "0.2800 of a share" of the acquirer's stock), ``cvr`` (a contingent value
  right), ``election`` (cash or stock at the holder's election); ``consideration_type`` cash / stock / mixed.
* **Filer role**: ``target`` when the text merges a subsidiary "with and into the Company" / converts "Company
  Common Stock" / leaves "the Company surviving"; ``acquirer`` when the merger subsidiary is "a wholly owned
  subsidiary of the Company"; else ``unknown``. The counterparty name is read from its defined term ("Parent",
  "Purchaser", "Buyer" for a target filer).

Deals (``build``): one row per target CIK and announcement cluster (filings within 60 days). The target is the
filer of a ``target``-role document, the SC TO-T / SC 14D9 subject, or (``acquirer`` role) the counterparty resolved
by a co-filed 425 / SC TO-T accession or an exact normalised-name match to the SEC issuer profile. Acquirer CIK:
the ``acquirer``-role filer, else the other CIK of a co-filed 425 / SC TO-T accession within -10..+400 days, else an
exact name match; private buyers keep only ``acquirer_name``.

Status from metadata (known at each filing's own ``*_available_at``): ``completed`` = the target's first 8-K with
item 2.01 / 5.01 / 3.01, Form 25 / 25-NSE or ``delisting`` stage ``mna`` terminal event in (announce, announce + 900
days]; ``terminated`` = a target 8-K with item 1.02 in that window before any completion; else ``pending`` (announced
within 400 days of the snapshot) or ``unknown``. The delisting link (``delisting_security_id``,
``delisting_last_session``) comes from ``delisting/events.parquet``. Row clock: ``available_at`` = acceptance of the
announcing filing.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import common as C
from .guidance import find_values

EXTRACTOR_VERSION = "mna-rules-v1"
TABLE = "mna"
SCHEMA = "atx.alpha-panel.events.mna/v1"
CLUSTER_DAYS = 60
STATUS_WINDOW_DAYS = 900
PENDING_DAYS = 400

CONVERT = re.compile(r"\bconverted\s+(?:automatically\s+)?into\s+(?:and\s+(?:shall|will)\s+(?:thereafter\s+)?"
                     r"represent\s+)?(?:only\s+)?the\s+right\s+to\s+receive\b", re.IGNORECASE)
TENDER_PRICE = re.compile(r"\b(?:at\s+a\s+(?:purchase\s+)?price\s+of|offer\s+price\s+of|for)\s+\$\s?(?P<p>\d{1,4}(?:\.\d+)?)"
                          r"\s+per\s+(?:share|Share)[^.;]{0,60}?\bnet\s+to\s+the\s+sellers?\b", re.IGNORECASE)
_WORDNUM = {"one": 1.0, "two": 2.0, "three": 3.0, "four": 4.0, "five": 5.0, "one-half": 0.5, "one half": 0.5}
RATIO = re.compile(
    r"(?P<r>\d{1,3}(?:\.\d{1,6})?|\.\d{1,6}|one|two|three|four|five|one[- ]half)\s+"
    r"(?:of\s+(?:a|one)\s+)?(?:\(\s*[\d.]+\s*\)\s+)?(?:validly\s+issued,?\s+)?(?:fully[- ]paid\s*(?:and\s+|,\s*)?)?"
    r"(?:non-?assessable\s+)?(?:(?:ordinary|common|class\s+[a-c](?:\s+common)?)\s+)?(?:shares?|ADSs?|units?)\b"
    r"(?P<tail>[^;]{0,80})",
    re.IGNORECASE,
)
CASH = re.compile(r"\$\s?(?P<v>\d{1,4}(?:,\d{3})?(?:\.\d{1,4})?)\s*(?:per\s+share\s+)?(?:in\s+cash|net\s+to|,?\s*without\s+"
                  r"interest|in\s+cash,?\s+without|cash)|(?:cash|amount)\s+(?:in\s+an\s+amount\s+)?equal\s+to\s+"
                  r"\$\s?(?P<v2>\d{1,4}(?:,\d{3})?(?:\.\d{1,4})?)", re.IGNORECASE)
CVR = re.compile(r"\bcontingent\s+value\s+rights?\b|\bCVRs?\b", re.IGNORECASE)
ELECTION = re.compile(r"\b(?:at\s+the\s+election\s+of|cash\s+election|stock\s+election|elect(?:s|ion)?\s+to\s+receive|"
                      r"subject\s+to\s+proration)\b", re.IGNORECASE)
TARGET_ROLE = re.compile(
    r"\bmerge\w*\s+with\s+and\s+into\s+the\s+Company\b|\bthe\s+Company\s+(?:will\s+|shall\s+)?(?:surviv\w+|continu\w+\s+"
    r"as\s+the\s+surviving)|\bCompany\s+Common\s+Stock\b|\bCompany\s+Shares?\b|"
    r"\bthe\s+Company\s+(?:will\s+)?become\s+a\s+(?:direct\s+|indirect\s+)?wholly[- ]owned\b|"
    r"\bacquisition\s+of\s+the\s+Company\b|\bwill\s+acquire\s+the\s+Company\b",
)
ACQUIRER_ROLE = re.compile(
    r"\b(?:direct\s+|indirect\s+)?wholly[- ]owned\s+(?:direct\s+|indirect\s+)?subsidiary\s+of\s+the\s+Company\b|"
    r"\bthe\s+Company\s+(?:will|agreed\s+to|has\s+agreed\s+to)\s+acquire\b|\bshares\s+of\s+(?:the\s+)?Company\s+Common"
    r"\s+Stock\s+to\s+be\s+issued\b|\bthe\s+Company'?s?\s+acquisition\s+of\b",
)
PARTY_TERM = re.compile(
    r"(?P<name>[A-Z][A-Za-z0-9&.,'\- ]{2,90}?),?\s+(?:a|an)\s+[A-Za-z .,'\-]{0,80}?\(\s*(?:the\s+)?[\"“]?"
    r"(?P<term>Parent|Purchaser|Buyer|Acquiror|Acquirer|Target)[\"”]?\s*\)|"
    r"(?P<name2>[A-Z][A-Za-z0-9&.,'\- ]{2,90}?)\s+\(\s*(?:the\s+)?[\"“](?P<term2>Parent|Purchaser|Buyer|Acquiror|"
    r"Acquirer|Target)[\"”]\s*\)"
)
_SPLIT = re.compile(r"(?<=[a-z0-9)\"'”])\.\s+(?=[A-Z(\"“])")


@dataclass
class MergerTerms:
    role: str  # target / acquirer / unknown
    consideration_type: str | None  # cash / stock / mixed / None
    cash_per_share: float | None
    stock_ratio: float | None
    cvr: bool
    election: bool
    tender_offer: bool
    counterparty_name: str | None
    evidence: str
    extras: dict[str, Any] = field(default_factory=dict)


def _flat(text: str) -> str:
    return re.sub(r"\s+", " ", text.replace("|", " "))


def _num(s: str) -> float | None:
    s = s.lower().strip()
    if s in _WORDNUM:
        return _WORDNUM[s]
    try:
        return float(s.replace(",", ""))
    except ValueError:
        return None


def parse_consideration(clause: str) -> tuple[float | None, float | None, bool, bool]:
    """(cash per share, stock ratio, cvr, election) from the text after "converted into the right to receive"."""
    cash = None
    for m in CASH.finditer(clause):
        v = _num(m.group("v") or m.group("v2") or "")
        if v is not None and 0 < v < 10_000:
            cash = v
            break
    if cash is None:  # a bare amount right after "the right to receive" ("... receive $25.00 per share")
        for v in find_values(clause[:60]):
            if (v.kind == "point" and v.scale_word is None and v.point is not None and 0 < v.point < 10_000
                    and not re.search(r"\bpar\s+value\b", clause[max(0, v.start - 20):v.start], re.IGNORECASE)):
                cash = v.point
                break
    ratio = None
    for m in RATIO.finditer(clause):
        tail = m.group("tail") or ""
        if not re.match(r"^\s*(?:of|\(the\b|,\s*par\s+value)", tail, re.IGNORECASE) and not re.search(
                r"\b(?:common|stock|shares?)\b", tail[:40], re.IGNORECASE):
            continue
        if re.search(r"\bper\s+share\b|\$\s*$", clause[max(0, m.start() - 12):m.start()]):
            continue
        r = _num(m.group("r"))
        if r is not None and 0 < r < 1000:
            ratio = r
            break
    return cash, ratio, bool(CVR.search(clause)), bool(ELECTION.search(clause))


def _counterparty(text: str, role: str) -> str | None:
    want = ("Parent", "Purchaser", "Buyer", "Acquiror", "Acquirer") if role == "target" else ("Target",)
    for m in PARTY_TERM.finditer(text[:20000]):
        term = m.group("term") or m.group("term2")
        name = (m.group("name") or m.group("name2") or "").strip(" ,.")
        if term in want and name and not name.lower().startswith(("the ", "and ", "by ", "among ")):
            name = re.split(r"\b(?:by and among|among|between|with|and)\b", name)[-1].strip(" ,.")
            if 2 < len(name) <= 90 and name[0].isupper():
                return name
    return None


def parse_merger_text(text: str) -> MergerTerms | None:
    """Terms of one merger / tender-offer document (see the module doc); None without a consideration clause."""
    flat = _flat(text)
    m = CONVERT.search(flat)
    tender = None
    if m is None:
        tender = TENDER_PRICE.search(flat)
        if tender is None:
            return None
    t_score = len(TARGET_ROLE.findall(flat[:40000]))
    a_score = len(ACQUIRER_ROLE.findall(flat[:40000]))
    role = "target" if t_score > a_score else "acquirer" if a_score > t_score else "unknown"
    if m is not None:
        clause = flat[m.end():m.end() + 700]
        stop = re.search(r"(?<=[a-z0-9)\"”])\.\s+(?:The|In|Each|At|If|Upon|Following|Pursuant|As|Under|Any|No)\b",
                         clause)
        clause = clause[:stop.start() + 1] if stop else clause
        cash, ratio, cvr, election = parse_consideration(clause)
        evidence = flat[max(0, m.start() - 250):m.end() + len(clause)]
        tender_offer = bool(re.search(r"\btender\s+offer\b", flat[:30000], re.IGNORECASE))
    else:
        assert tender is not None
        cash, ratio, cvr, election = float(tender.group("p")), None, bool(CVR.search(flat[tender.start():
                                                                                            tender.end() + 300])), False
        evidence = flat[max(0, tender.start() - 250):tender.end() + 200]
        tender_offer = True
        if role == "unknown":
            role = "target" if re.search(r"\bSolicitation/Recommendation\b", flat[:5000], re.IGNORECASE) else "unknown"
    ctype = ("mixed" if cash is not None and ratio is not None else "cash" if cash is not None
             else "stock" if ratio is not None else None)
    return MergerTerms(role, ctype, cash, ratio, cvr, election, tender_offer, _counterparty(flat, role),
                       evidence[:900])


# ---------------------------------------------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------------------------------------------

_SUFFIX = re.compile(r"\b(?:INCORPORATED|INC|CORPORATION|CORP|COMPANY|CO|LTD|LIMITED|LLC|L\.L\.C|LP|L\.P|PLC|HOLDINGS?|"
                     r"GROUP|N\.V|NV|S\.A|SA|AG|SE|THE|TRUST|BANCORP)\b\.?")


def norm_name(name: str | None) -> str | None:
    if not name:
        return None
    n = name.upper().replace("&", " AND ")
    n = _SUFFIX.sub(" ", n)
    n = re.sub(r"[^A-Z0-9 ]+", " ", n)
    n = re.sub(r"\s+", " ", n).strip()
    return n or None


# ---------------------------------------------------------------------------------------------------------
# Stage build
# ---------------------------------------------------------------------------------------------------------

DOC_COLUMNS = [
    ("cik", "BIGINT"), ("accession", "VARCHAR"), ("form", "VARCHAR"), ("items", "VARCHAR"), ("source_doc", "VARCHAR"),
    ("file_type", "VARCHAR"), ("filing_date", "DATE"), ("event_date", "DATE"), ("available_at", "TIMESTAMP"),
    ("acceptance_clock", "VARCHAR"), ("vintage_risk", "VARCHAR"), ("role", "VARCHAR"),
    ("consideration_type", "VARCHAR"), ("cash_per_share", "DOUBLE"), ("stock_ratio", "DOUBLE"), ("cvr", "BOOLEAN"),
    ("election", "BOOLEAN"), ("tender_offer", "BOOLEAN"), ("counterparty_name", "VARCHAR"),
    ("counterparty_norm", "VARCHAR"), ("evidence", "VARCHAR"),
]
DEAL_FORMS = ("425", "SC TO-T", "SC TO-T/A", "SC 14D9", "SC 14D9/A", "DEFM14A", "PREM14A", "DEFM14C", "PREM14C",
              "S-4", "F-4", "SC 13E3", "SC TO-C", "SC 14D9C")
SNAPSHOT = dt.date(2026, 9, 19)


def _tmp() -> Path:
    p = C.build_root() / "_tmp" / "events"
    p.mkdir(parents=True, exist_ok=True)
    return p


def phase_parse(start: dt.date = dt.date(2019, 1, 1)) -> dict[str, Any]:
    """Pure Python over the document catalog (ruling C-1: no DuckDB)."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    from . import events_sources as ES

    store = ES.open_store()

    def pred(r: dict[str, Any]) -> bool:
        q = r["qids"] or []
        return ("merger" in q or "tender" in q) and (r["filing_date"] or dt.date.min) >= start

    types = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"),
             "DOUBLE": pa.float64(), "BOOLEAN": pa.bool_()}
    schema = pa.schema([(n, types[t]) for n, t in DOC_COLUMNS])
    rows: list[dict[str, Any]] = []
    stats: dict[str, Any] = {"documents": 0, "unreadable": 0, "parsed": 0, "with_consideration": 0}
    for r in ES.iter_catalog(pred):
        stats["documents"] += 1
        blob = ES.read_document(r, store)
        if not blob:
            stats["unreadable"] += 1
            continue
        t = parse_merger_text(ES.document_text(blob, r["source_doc"] or ""))
        if t is None:
            continue
        stats["parsed"] += 1
        stats["with_consideration"] += t.consideration_type is not None
        rows.append({"cik": r["cik"], "accession": r["accession"], "form": r["form"], "items": r["items"],
                     "source_doc": r["source_doc"], "file_type": r["file_type"], "filing_date": r["filing_date"],
                     "event_date": r["event_date"], "available_at": r["available_at"],
                     "acceptance_clock": r["acceptance_clock"], "vintage_risk": r["vintage_risk"], "role": t.role,
                     "consideration_type": t.consideration_type, "cash_per_share": t.cash_per_share,
                     "stock_ratio": t.stock_ratio, "cvr": t.cvr, "election": t.election,
                     "tender_offer": t.tender_offer, "counterparty_name": t.counterparty_name,
                     "counterparty_norm": norm_name(t.counterparty_name), "evidence": t.evidence})
    dest = _tmp() / "mna_docs.parquet"
    part = dest.with_name(dest.name + ".partial")
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), part, compression="zstd")
    os.replace(part, dest)
    stats["peak_memory"] = ES.peak_memory()
    C.write_json_atomic(_tmp() / "mna_parse_receipt.json", stats)
    return stats


def _status_sql(dl: str) -> str:
    w = STATUS_WINDOW_DAYS
    return f"""
        SELECT d.*,
               (SELECT arg_min(t.accession, t.available_at) FROM tev t WHERE t.cik = d.target_cik
                  AND t.kind = 'completion' AND t.form LIKE '8-K%' AND t.available_at > d.available_at
                  AND t.filing_date <= d.announce_date + {w}) AS completion_accession,
               (SELECT min(t.available_at) FROM tev t WHERE t.cik = d.target_cik AND t.kind = 'completion'
                  AND t.form LIKE '8-K%' AND t.available_at > d.available_at
                  AND t.filing_date <= d.announce_date + {w}) AS completion_8k_at,
               (SELECT min(t.available_at) FROM tev t WHERE t.cik = d.target_cik AND t.form IN ('25', '25-NSE')
                  AND t.available_at > d.available_at AND t.filing_date <= d.announce_date + {w}) AS form25_at,
               (SELECT arg_min(t.accession, t.available_at) FROM tev t WHERE t.cik = d.target_cik
                  AND t.kind = 'termination' AND t.available_at > d.available_at
                  AND t.filing_date <= d.announce_date + {w}) AS termination_accession,
               (SELECT min(t.available_at) FROM tev t WHERE t.cik = d.target_cik AND t.kind = 'termination'
                  AND t.available_at > d.available_at AND t.filing_date <= d.announce_date + {w}) AS termination_at,
               (SELECT arg_min(x.security_id, x.last_session) FROM read_parquet('{dl}') x
                  WHERE x.cik = d.target_cik AND x.cause = 'mna' AND x.last_session > d.announce_date
                    AND x.last_session <= d.announce_date + {w}) AS delisting_security_id,
               (SELECT min(x.last_session) FROM read_parquet('{dl}') x
                  WHERE x.cik = d.target_cik AND x.cause = 'mna' AND x.last_session > d.announce_date
                    AND x.last_session <= d.announce_date + {w}) AS delisting_last_session
        FROM deals d"""


def phase_publish() -> dict[str, Any]:
    from . import events_common as EC

    receipt: dict[str, Any] = {"parse": C.read_json(_tmp() / "mna_parse_receipt.json")}
    root = C.build_root()
    f = (root / "sec_filings" / "filings.parquet").as_posix()
    prof = (root / "sec_filings" / "issuer_profile.parquet").as_posix()
    dl = (root / "delisting" / "events.parquet").as_posix()
    docs = (_tmp() / "mna_docs.parquet").as_posix()
    forms = ", ".join(f"'{x}'" for x in DEAL_FORMS)
    con = C.connect(memory="350MB", threads=2, db_file="mna.duckdb")
    con.execute(f"""
        CREATE TABLE df AS SELECT cik, accession, form, filing_date, available_at FROM read_parquet('{f}')
        WHERE filing_date >= DATE '2018-06-01' AND form IN ({forms})""")
    # co-filed deal forms: the same accession under two CIKs (filer and subject company)
    con.execute("""
        CREATE TABLE cof AS
        SELECT a.cik AS cik, b.cik AS other_cik, a.form, a.filing_date FROM df a JOIN df b
          ON a.accession = b.accession AND a.cik <> b.cik""")
    con.create_function("norm_name", norm_name, ["VARCHAR"], "VARCHAR", null_handling="special")
    con.execute(f"""
        CREATE TABLE names2 AS
        WITH n AS (
            SELECT cik, name FROM read_parquet('{prof}')
            UNION ALL SELECT cik, unnest(former_names).name AS name FROM read_parquet('{prof}')
        )
        SELECT norm_name(name) AS nn, min(cik) AS cik FROM n WHERE name IS NOT NULL GROUP BY 1
        HAVING count(DISTINCT cik) = 1""")
    con.execute(f"""
        CREATE TABLE d AS
        SELECT x.*,
               CASE WHEN x.role <> 'unknown' THEN x.role
                    WHEN EXISTS (SELECT 1 FROM df WHERE df.cik = x.cik AND df.form IN ('DEFM14A', 'PREM14A', 'SC 14D9',
                                 'DEFM14C', 'PREM14C') AND df.filing_date BETWEEN x.filing_date - 30 AND x.filing_date + 400)
                         THEN 'target'
                    WHEN EXISTS (SELECT 1 FROM df WHERE df.cik = x.cik AND df.form IN ('S-4', 'F-4')
                                 AND df.filing_date BETWEEN x.filing_date - 30 AND x.filing_date + 400) THEN 'acquirer'
                    ELSE 'unknown' END AS role2,
               (SELECT mode(c.other_cik) FROM cof c WHERE c.cik = x.cik
                  AND c.filing_date BETWEEN x.filing_date - 10 AND x.filing_date + 400) AS cofiled_cik,
               n.cik AS name_cik
        FROM read_parquet('{docs}') x LEFT JOIN names2 n ON n.nn = x.counterparty_norm""")
    con.execute("""
        CREATE TABLE d2 AS
        SELECT *,
               CASE WHEN role2 = 'target' THEN cik WHEN role2 = 'acquirer' THEN coalesce(cofiled_cik, name_cik) END
                   AS target_cik,
               CASE WHEN role2 = 'acquirer' THEN cik WHEN role2 = 'target' THEN coalesce(cofiled_cik, name_cik) END
                   AS acquirer_cik,
               CASE WHEN role2 = 'target' AND cofiled_cik IS NOT NULL THEN 'cofiled_deal_form'
                    WHEN role2 = 'target' AND name_cik IS NOT NULL THEN 'name_match'
                    WHEN role2 = 'acquirer' THEN 'filer' END AS acquirer_basis,
               (coalesce(items, '') LIKE '%1.01%' OR form NOT LIKE '8-K%' OR coalesce(items, '') LIKE '%8.01%'
                OR coalesce(items, '') LIKE '%7.01%')
                   AND NOT (coalesce(items, '') LIKE '%2.01%' AND coalesce(items, '') NOT LIKE '%1.01%') AS is_announcement
        FROM d""")
    con.execute("""
        CREATE TABLE a2 AS
        WITH a AS (
            SELECT *, coalesce(target_cik, acquirer_cik, cik) AS deal_cik,
                   CASE WHEN target_cik IS NOT NULL THEN 'target' ELSE 'acquirer_only' END AS deal_side
            FROM d2 WHERE is_announcement
        ), o AS (
            SELECT *, first_value(filing_date) OVER (PARTITION BY deal_cik, deal_side ORDER BY available_at, accession)
                      AS first_date,
                   lag(filing_date) OVER (PARTITION BY deal_cik, deal_side ORDER BY available_at, accession) AS prev_date
            FROM a
        )
        SELECT *, sum(CASE WHEN prev_date IS NULL OR filing_date - prev_date > """ + str(CLUSTER_DAYS) + """ THEN 1
                           ELSE 0 END)
                  OVER (PARTITION BY deal_cik, deal_side ORDER BY available_at, accession) AS grp
        FROM o""")
    con.execute("""
        CREATE TABLE deals AS
        SELECT deal_cik, deal_side, grp,
               arg_min(accession, available_at) AS announce_accession,
               min(available_at) AS available_at, arg_min(filing_date, available_at) AS announce_date,
               arg_min(event_date, available_at) AS event_date,
               arg_min(acceptance_clock, available_at) AS acceptance_clock,
               arg_min(vintage_risk, available_at) AS vintage_risk,
               arg_min(form, available_at) AS announce_form, arg_min(items, available_at) AS announce_items,
               max(target_cik) AS target_cik, mode(acquirer_cik) AS acquirer_cik,
               arg_min(acquirer_basis, available_at) FILTER (WHERE acquirer_cik IS NOT NULL) AS acquirer_basis,
               arg_min(counterparty_name, available_at) FILTER (WHERE counterparty_name IS NOT NULL) AS counterparty_name,
               arg_min(consideration_type, available_at) FILTER (WHERE consideration_type IS NOT NULL)
                   AS consideration_type,
               arg_min(cash_per_share, available_at) FILTER (WHERE consideration_type IS NOT NULL) AS cash_per_share,
               arg_min(stock_ratio, available_at) FILTER (WHERE consideration_type IS NOT NULL) AS stock_ratio,
               bool_or(cvr) AS cvr, bool_or(election) AS election, bool_or(tender_offer) AS tender_offer,
               arg_max(cash_per_share, available_at) FILTER (WHERE consideration_type IS NOT NULL) AS last_cash_per_share,
               arg_max(stock_ratio, available_at) FILTER (WHERE consideration_type IS NOT NULL) AS last_stock_ratio,
               count(DISTINCT accession) AS n_documents,
               coalesce(arg_min(evidence, available_at) FILTER (WHERE consideration_type IS NOT NULL),
                        arg_min(evidence, available_at)) AS evidence,
               arg_min(source_doc, available_at) AS source_doc
        FROM a2 GROUP BY 1, 2, 3""")
    con.execute(f"""
        CREATE TABLE tev AS
        SELECT cik, accession, form, items, filing_date, available_at,
               CASE WHEN form LIKE '8-K%' AND items LIKE '%1.02%' THEN 'termination' ELSE 'completion' END AS kind
        FROM read_parquet('{f}')
        WHERE filing_date >= DATE '2019-01-01' AND cik IN (SELECT target_cik FROM deals WHERE target_cik IS NOT NULL)
          AND ((form IN ('8-K', '8-K/A') AND (items LIKE '%2.01%' OR items LIKE '%5.01%' OR items LIKE '%3.01%'
                                               OR items LIKE '%1.02%'))
               OR form IN ('25', '25-NSE'))""")
    con.execute("CREATE TABLE deals2 AS " + _status_sql(dl))
    con.execute(f"""
        CREATE TABLE deals4 AS
        SELECT *, least(completion_8k_at, form25_at) AS completion_available_at,
               CASE WHEN termination_at IS NOT NULL AND (least(completion_8k_at, form25_at) IS NULL
                                                         OR termination_at < least(completion_8k_at, form25_at))
                        THEN 'terminated'
                    WHEN completion_8k_at IS NOT NULL OR form25_at IS NOT NULL OR delisting_last_session IS NOT NULL
                        THEN 'completed'
                    WHEN announce_date >= DATE '{SNAPSHOT}' - {PENDING_DAYS} THEN 'pending'
                    ELSE 'unknown' END AS status,
               deal_cik AS cik, announce_date AS link_date
        FROM deals2""")
    EC.attach_security(con, "deals4", "deals5", date_col="link_date")
    con.execute("""
        CREATE TABLE acq AS SELECT DISTINCT acquirer_cik AS cik, announce_date AS link_date FROM deals5
        WHERE acquirer_cik IS NOT NULL""")
    EC.attach_security(con, "acq", "acq2", date_col="link_date")
    out = C.stage_dir(EC.STAGE) / f"{TABLE}.parquet"
    receipt["rows"] = C.copy_to_parquet(con, f"""
        SELECT md5(CAST(d.deal_cik AS VARCHAR) || d.announce_accession) AS deal_id, d.cik, d.security_id, d.link_tier,
               d.link_basis, d.is_member_issuer, d.deal_side, d.target_cik, d.acquirer_cik, d.acquirer_basis,
               d.counterparty_name, q.security_id AS acquirer_security_id, q.link_tier AS acquirer_link_tier,
               d.announce_accession AS accession, d.announce_form AS form, d.announce_items AS items,
               d.announce_date AS filing_date, d.event_date, d.available_at, d.acceptance_clock, d.vintage_risk,
               d.consideration_type, d.cash_per_share, d.stock_ratio, d.cvr, d.election, d.tender_offer,
               d.last_cash_per_share, d.last_stock_ratio, d.n_documents, d.status, d.completion_accession,
               d.completion_available_at, d.termination_accession, d.termination_at AS termination_available_at,
               d.delisting_security_id, d.delisting_last_session, d.source_doc, d.evidence,
               '{EXTRACTOR_VERSION}' AS extractor_version
        FROM deals5 d LEFT JOIN acq2 q ON q.cik = d.acquirer_cik AND q.link_date = d.announce_date
        ORDER BY d.available_at, d.cik""", out)
    o = out.as_posix()
    receipt["per_year"] = {str(y): {"deals": int(n), "member_target": int(mt), "with_consideration": int(wc),
                                    "completed": int(cp), "terminated": int(tm), "pending": int(pn),
                                    "acquirer_resolved": int(ar), "delisting_linked": int(dk)}
                           for y, n, mt, wc, cp, tm, pn, ar, dk in con.execute(f"""
        SELECT year(filing_date), count(*), count(*) FILTER (WHERE is_member_issuer AND deal_side = 'target'),
               count(*) FILTER (WHERE consideration_type IS NOT NULL), count(*) FILTER (WHERE status = 'completed'),
               count(*) FILTER (WHERE status = 'terminated'), count(*) FILTER (WHERE status = 'pending'),
               count(*) FILTER (WHERE acquirer_cik IS NOT NULL),
               count(*) FILTER (WHERE delisting_security_id IS NOT NULL)
        FROM read_parquet('{o}') GROUP BY 1 ORDER BY 1""").fetchall()}
    receipt["consideration_share"] = con.execute(
        f"SELECT avg(CASE WHEN consideration_type IS NOT NULL THEN 1.0 ELSE 0.0 END) FROM read_parquet('{o}')"
    ).fetchone()[0]
    con.close()
    (C.build_root() / "_tmp" / "mna.duckdb").unlink(missing_ok=True)
    EC.publish_table(TABLE, ("events_mna", "guidance", "events_common", "events_sources", "common"), {
        "schema": SCHEMA, "rule": __doc__, "extractor_version": EXTRACTOR_VERSION, **receipt})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", choices=("parse", "publish", "all"), default="all")
    args = ap.parse_args(argv)
    out: dict[str, Any] = {}
    if args.phase in ("parse", "all"):
        out["parse"] = phase_parse()
    if args.phase in ("publish", "all"):
        out["publish"] = phase_publish()
    print(json.dumps(out, default=str, indent=1)[:5000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
