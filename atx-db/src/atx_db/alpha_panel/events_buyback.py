"""Stage ``events`` table ``buyback`` (S6.3): share repurchase authorisation announcements from 8-K text.

Documents: independently admitted original retained objects in deterministic partitions
of the complete eligible2020+8-K/10-Qsource catalog. No network acquisition is performed.
Byte/reader replay does not certify source semantic correctness or prior-history completeness.

Extraction (:func:`extract_authorizations`, pure), per sentence:

* The sentence names a repurchase / buyback (program, plan, authorisation) and an authorising act (authorize,
  approve, adopt, establish, increase, expand, replenish, renew, "announces a new / an additional").
* The amount is the first US-dollar amount with a scale (or >= $1 million) that is not a remaining / available
  balance, a spent / completed / repurchased amount, or a total "bringing" figure; or a share count ("up to 10
  million shares"). "additional", "increase of", "incremental", "expand ... by" make it an ``increase``; "increase
  ... to $X" / "bringing the total ... to $X" fill ``total_after_usd``.
* A sentence about an older authorisation is skipped: "remaining under", "previously announced / authorized",
  "existing program" (unless "additional" / "increase"), or an authorisation dated (``authorized in <month> <year>``)
  more than 120 days before the filing.
* One event per (filing, amount); the first sentence wins. ``first_announcement`` is
  NULL for authorizations until complete global prior-history dependencies are admitted.
  A document-hash partition cannot prove the absence of an earlier180day authorization.
  Executions have false and a not-applicable basis, never first-announcement status.

XBRL join (no recomputation): the fundamentals stage's ``buyback_authorized`` / ``buyback_remaining``
(``fundamentals/events.parquet``, from ``StockRepurchaseProgramAuthorizedAmount1`` /
``...RemainingAuthorizedRepurchaseAmount1``): the latest value known before the announcement (``xbrl_*_before``, by
``clock_utc < available_at``) and the first reported after it (``xbrl_*_after``, with its accession).

Clock: matching original source receipt fetched_at/done_at conservativefloor. Original
acceptance/class semantics remain explicitly unqualified; event_date is unknown.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import common as C
from .guidance import SCALE, find_values, _apply_scale

EXTRACTOR_VERSION = "buyback-rules-v3"
TABLE = "buyback"
SCHEMA = "atx.alpha-panel.events.buyback/v3"

BUYBACK_OBJ = re.compile(r"\b(?:re-?purchases?|buy-?backs?|buy\s+back)\b", re.IGNORECASE)
#: An authorising act (a verb), or "new / additional [$X] share repurchase ..." (a noun phrase naming a new program).
AUTH_CUE = re.compile(
    r"\b(?:authoriz(?:e|es|ed|ing)|approv(?:e|es|ed|ing)|adopt(?:s|ed)|establish(?:es|ed)|increas(?:e|es|ed|ing)|"
    r"expand(?:s|ed)|replenish(?:es|ed)|renew(?:s|ed)|reinstat(?:e|es|ed)|upsiz(?:e|es|ed)|"
    r"announc(?:e|es|ed|ing))\b|\b(?:new|additional)\s+(?:\S+\s+){0,3}?(?:share\s+|stock\s+|common\s+stock\s+)?"
    r"(?:re-?purchase|buy-?back)",
    re.IGNORECASE,
)
STRONG_ACT = re.compile(r"\b(?:authoriz(?:ed|es)|approv(?:ed|es)|adopt(?:ed|s)|establish(?:ed|es)|increas(?:ed|es)|"
                        r"expand(?:ed|s)|replenish(?:ed|es)|renew(?:ed|s)|reinstat(?:ed|es)|announc(?:ed|es))\b",
                        re.IGNORECASE)
INTENT = re.compile(r"\b(?:target\w*|intend\w*|plan(?:s|ned)?\s+to|expect\w*\s+to|anticipat\w*|seek\w*|subject\s+to|"
                    r"consider\w*|would|could|will|may\s+(?:seek|request)|pace|per\s+year|annual\s+rate)\b",
                    re.IGNORECASE)
#: A value right after a non-buyback measure ("net income of $4.999 billion", "cash of $224 million").
BEFORE_BLOCK = re.compile(r"\b(?:income|revenues?|sales|cash|equity|earnings|dividends?|ebitda|flows?|debt|capital|"
                          r"liquidity|assets|proceeds|savings|cost|costs|price|target)\b[^$]{0,18}$", re.IGNORECASE)
#: A past repurchase report ("repurchased 2.0 million shares for $124 million ... under the $3.0 billion program").
EXECUTION = re.compile(r"\b(?:repurchased|purchased|bought\s+back|returned|completed\s+the\s+repurchase)\b.{0,120}?"
                       r"\b(?:shares|stock|for|at\s+a\s+total\s+cost)\b", re.IGNORECASE)
#: "under the Company's $2.5 billion authorization": an existing program named by its size.
UNDER_BEFORE = re.compile(r"\bunder\s+(?:the|its|our|their)\s+(?:\w+'s\s+)?(?:current\s+|existing\s+|remaining\s+)?$",
                          re.IGNORECASE)
PLAN_BEFORE = re.compile(r"\b(?:plans?|intends?|expects?|anticipates?|targets?|aims?)\s+to\s+(?:re-?purchase|buy\s*back)"
                         r"\s+(?:up\s+to\s+)?(?:approximately\s+)?$", re.IGNORECASE)
#: Words between a value and the repurchase keyword that tie the value to something else.
BLOCK_BETWEEN = re.compile(
    r"\b(?:cash|dividends?|debt|notes|revenues?|sales|flows?|income|earnings|capital\s+returns?|returns?\s+to|"
    r"accelerated|asr|including|includes|and\s+a|while|as\s+well\s+as|borrow\w*|credit|loan|proceeds|offering)\b|;",
    re.IGNORECASE,
)
REMAINING = re.compile(r"\b(?:remain(?:s|ed|ing)?|available|left|unused|unutilized|capacity)\b", re.IGNORECASE)
SPENT_BEFORE = re.compile(
    r"\b(?:repurchased|bought\s+back|returned|return|returning|returns|spent|completed|executed|purchased|deployed|"
    r"retired|buying\s+back|repurchasing|used|utilized|paid|generate|generated|total\s+cost)\b[^$]{0,45}$",
    re.IGNORECASE,
)
SPENT_AFTER = re.compile(r"^[^.;]{0,40}\b(?:was|were|has\s+been|have\s+been)\s+(?:repurchased|spent|used|"
                         r"returned|completed)\b|^\s*(?:of|in)\s+(?:its\s+|our\s+)?(?:share|stock)\s+repurchases\b|"
                         r"^\s*(?:\w+\s+){0,2}(?:returned|spent|repurchased|used|deployed|to\s+shareholders)\b",
                         re.IGNORECASE)
INCREASE_BEFORE = re.compile(r"\b(?:additional|incremental|increase\s+(?:of|by)|increas\w*\b[^$]{0,60}\bby|"
                             r"expand\w*\b[^$]{0,60}\bby|add(?:s|ed)?|another|further|top[- ]up|replenish\w*)\b"
                             r"[^$]{0,30}$", re.IGNORECASE)
TOTAL_BEFORE = re.compile(r"(?:\bbringing\b|\bbrings\b|\bfor\s+a\s+total\b|\btotal\s+(?:authoriz\w*|capacity)\b[^$]{0,20}"
                          r"|\bincreas\w*\s+(?:\S+\s+){0,5}?(?:authoriz\w*|program|plan|capacity|increase)\s+to\s+"
                          r"(?:approximately\s+|about\s+)?|\bincrease[sd]?\s+to\s+|\bto\s+a\s+total\s+of\s+"
                          r"|\bnow\s+totals?\b)[^$]{0,40}$",
                          re.IGNORECASE)
OLD_PROGRAM = re.compile(r"\b(?:previously\s+(?:announced|authorized|approved|disclosed)|existing|prior|current|"
                         r"remaining\s+under|under\s+the\s+terms\s+of|in\s+addition\s+to\s+(?:the|its|our)|"
                         r"(?:announced|authorized|approved)\s+(?:last|in\s+the\s+prior)\s+(?:year|quarter)|"
                         r"publicly\s+announced|since\s+inception)\b", re.IGNORECASE)
_MON = r"(?P<mon>january|february|march|april|may|june|july|august|september|october|november|december)"
DATED_AUTH = (
    re.compile(r"\b(?:authoriz|approv|adopt|announc|establish)\w*\s+(?:by\s+(?:the|its|our)\s+board\s+)?(?:in|on)\s+"
               r"(?:" + _MON + r"\s+(?:\d{1,2},\s+)?)?(?P<y>(?:19|20)\d{2})\b", re.IGNORECASE),
    re.compile(r"\b(?:in|on)\s+(?:" + _MON + r"\s+(?:\d{1,2},\s+)?)?(?P<y>(?:19|20)\d{2})\b.{0,90}?\b(?:authoriz|approv|"
               r"adopt|establish)\w*", re.IGNORECASE),
)
SHARES_RE = re.compile(r"\b(?:up\s+to\s+)?(?:an\s+additional\s+)?(?P<n>\d{1,3}(?:,\d{3})+|\d+(?:\.\d+)?)\s*"
                       r"(?P<u>million|thousand)?\s+(?:(?:of\s+(?:its|the\s+company's|our)\s+)?(?:outstanding\s+)?"
                       r"(?:common\s+|ordinary\s+|class\s+[a-c]\s+)?)(?:shares|units)\b", re.IGNORECASE)
NOT_BUYBACK = re.compile(r"\b(?:debt|notes?|bonds?|debentures?|preferred|warrants?|convertible|tender\s+offer|"
                         r"repurchase\s+agreements?|repos?|loans?|mortgage)\b", re.IGNORECASE)
_MONTH = {m: i for i, m in enumerate(("january", "february", "march", "april", "may", "june", "july", "august",
                                       "september", "october", "november", "december"), start=1)}
SENT_SPLIT = re.compile(r"(?<=[a-z0-9%)\"'])\.\s+(?=[A-Z(\"'$])|(?<=[.!?])\s+(?=[A-Z][a-z])|\s{2,}|;\s+")
MAX_GAP = 70  # repurchase keyword ... value ("program of up to $X")
MAX_GAP_AFTER = 30  # value ... repurchase keyword ("$X share repurchase program")


@dataclass
class Authorization:
    event_type: str  # new / increase
    amount_usd: float | None
    amount_shares: float | None
    total_after_usd: float | None
    evidence: str


def _sentences(text: str) -> list[str]:
    out = []
    for line in text.split("\n"):
        cells = [c.strip() for c in line.split("|") if c.strip()]
        line = " ".join(cells) if len(cells) <= 2 else ""
        if not line:
            continue
        start = 0
        for m in SENT_SPLIT.finditer(line):
            out.append(line[start:m.start() + 1])
            start = m.end()
        out.append(line[start:])
    return [s.strip() for s in out if s.strip()]


def _old_date(s: str, filed: dt.date | None) -> bool:
    """True when the sentence dates its authorisation more than 120 days before the filing."""
    if filed is None:
        return False
    for rx in DATED_AUTH:
        for m in rx.finditer(s):
            y = int(m.group("y"))
            mon = _MONTH.get((m.group("mon") or "").lower(), 12)
            if (filed - dt.date(y, mon, 28)).days > 120:
                return True
    return False


def _governed(s: str, start: int, end: int) -> bool:
    """A repurchase keyword within :data:`MAX_GAP` characters of the value with nothing tying it elsewhere."""
    for m in BUYBACK_OBJ.finditer(s):
        if re.search(r"\baccelerated\s+(?:share\s+|stock\s+)?$", s[max(0, m.start() - 25):m.start()], re.IGNORECASE):
            continue  # an accelerated share repurchase executes a program; it is not an authorisation
        if m.start() >= end:
            gap, limit = s[end:m.start()], MAX_GAP_AFTER
        elif m.end() <= start:
            gap, limit = s[m.end():start], MAX_GAP
        else:
            continue
        if len(gap) <= limit and not BLOCK_BETWEEN.search(gap) and "$" not in gap:
            return True
    return False


def extract_authorizations(text: str, filed: dt.date | None = None) -> list[Authorization]:
    """Repurchase authorisation events of one document (see the module doc)."""
    events: list[Authorization] = []
    for s in _sentences(text):
        if len(s) > 900 or not BUYBACK_OBJ.search(s) or not AUTH_CUE.search(s):
            continue
        if re.search(r"\b(?:forward[- ]looking|safe\s+harbor)\b", s, re.IGNORECASE):
            continue
        if not re.search(r"\b(?:share|stock|equity|common)\b[^.]{0,40}\b(?:re-?purchase|buy-?back)|"
                         r"\b(?:re-?purchase|buy-?back)\w*\b[^.]{0,60}\b(?:shares|stock|equity)\b|"
                         r"\bbuy-?backs?\b", s, re.IGNORECASE):
            continue
        if NOT_BUYBACK.search(s) and not re.search(r"\b(?:share|stock|equity)\s+(?:re-?purchase|buy-?back)", s,
                                                   re.IGNORECASE):
            continue
        if INTENT.search(s) and not STRONG_ACT.search(s):
            continue
        if not any(abs(m.start() - o.start()) <= 100 for m in AUTH_CUE.finditer(s) for o in BUYBACK_OBJ.finditer(s)):
            continue  # the authorising act is about something else ("equity increased ...")
        if EXECUTION.search(s) and not re.search(r"\b(?:authoriz|approv|announc|adopt|establish)\w*\s+(?:a|an|the)?\s*"
                                                 r"(?:new|additional|increase|\$)", s, re.IGNORECASE):
            continue  # a repurchase report under an existing program
        if _old_date(s, filed):
            continue
        amount = shares = total = None
        etype = "new"
        allv = find_values(s)
        for v in allv:  # "from $150M to $400M" parses as one range: an increase to the upper amount
            if v.kind == "range" and re.search(r"\bfrom\s+$", s[max(0, v.start - 20):v.start], re.IGNORECASE) \
                    and v.low is not None and v.high is not None and _governed(s, v.start, v.end):
                lo, hi = _apply_scale(v.low, v.scale_word), _apply_scale(v.high, v.scale_word)
                if lo and hi and hi > lo >= 1e6:
                    total, amount, etype = hi, hi - lo, "increase"
        vals = [v for v in allv if v.kind == "point"] if amount is None else []
        for i, v in enumerate(vals):
            val = _apply_scale(v.point, v.scale_word)
            if val is None or val < 1e6:
                continue
            before = s[max(0, v.start - 110):v.start]
            after = s[v.end:v.end + 60]
            nxt = vals[i + 1] if i + 1 < len(vals) else None
            if re.search(r"\bfrom\s+$", before, re.IGNORECASE) and nxt is not None and re.fullmatch(
                    r"\s*to\s+", s[v.end:nxt.start], re.IGNORECASE) and _governed(s, v.start, nxt.end):
                new_total = _apply_scale(nxt.point, nxt.scale_word)
                if new_total is not None and new_total > val:
                    total, amount, etype = new_total, new_total - val, "increase"  # "from $150M to $400M"
                    break
            if REMAINING.search(before[-50:]) or re.match(r"^\s*(?:\w+\s+){0,3}(?:remain\w*|available|left)\b",
                                                          after, re.IGNORECASE):
                continue
            if SPENT_BEFORE.search(before) or SPENT_AFTER.search(after) or UNDER_BEFORE.search(before) \
                    or PLAN_BEFORE.search(before):
                continue
            if TOTAL_BEFORE.search(before) and (amount is not None or _governed(s, v.start, v.end)
                                                or BUYBACK_OBJ.search(before)):
                total = total or val
                continue
            if not _governed(s, v.start, v.end) or BEFORE_BLOCK.search(before) or re.match(
                    r"^\s*(?:\w+\s+)?(?:accelerated\s+share\s+repurchase|asr)\b", after, re.IGNORECASE):
                continue
            if amount is None:
                amount = val
                if INCREASE_BEFORE.search(before):
                    etype = "increase"
        if amount is None and total is None:
            m = SHARES_RE.search(s)
            if m and re.search(r"\b(?:re-?purchase|buy\s*back|purchase)\s+(?:of\s+)?(?:up\s+to\s+)?(?:an\s+)?$",
                               s[max(0, m.start() - 40):m.start() + len(m.group(0)) - len(m.group(0).lstrip())],
                               re.IGNORECASE) or (m and re.search(r"^\s*(?:up\s+to\s+)?(?:an\s+additional\s+)?",
                                                                  m.group(0)) and _governed(s, m.start(), m.end())
                                                  and STRONG_ACT.search(s)):
                n = float(m.group("n").replace(",", ""))
                shares = n * SCALE.get((m.group("u") or "").lower(), 1.0)
                ctx = s[max(0, m.start() - 50):m.end() + 40]
                if shares < 10_000 or REMAINING.search(ctx) or SPENT_BEFORE.search(s[max(0, m.start() - 60):m.start()]):
                    shares = None
                elif re.search(r"\badditional\b", m.group(0), re.IGNORECASE):
                    etype = "increase"
        if amount is None and shares is None and total is None:
            continue
        if total is not None and amount is None:
            etype = "increase"
        if OLD_PROGRAM.search(s) and etype != "increase" and not re.search(
                r"\b(?:new|additional|replac\w*)\b", s, re.IGNORECASE):
            continue
        events.append(Authorization(etype, amount, shares, total, s[:700]))
    # one event per amount in a document: repeats (headline and body) merge, "increase" wins over "new"
    merged: list[Authorization] = []
    for e in events:
        same = next((m for m in merged if (e.amount_usd is not None and e.amount_usd in (m.amount_usd, m.total_after_usd))
                     or (e.total_after_usd is not None and e.total_after_usd in (m.total_after_usd, m.amount_usd))
                     or (e.amount_shares is not None and e.amount_shares == m.amount_shares)), None)
        if same is None:
            merged.append(e)
            continue
        if e.event_type == "increase" and same.event_type != "increase" and e.amount_usd == same.amount_usd:
            same.event_type = "increase"
        same.total_after_usd = same.total_after_usd or e.total_after_usd
        same.amount_usd = same.amount_usd or e.amount_usd
    return merged


# ---------------------------------------------------------------------------------------------------------
# Stage build
# ---------------------------------------------------------------------------------------------------------

OUT_COLUMNS = [
    ("cik", "BIGINT"), ("accession", "VARCHAR"), ("form", "VARCHAR"), ("items", "VARCHAR"), ("source_doc", "VARCHAR"),
    ("doc_source", "VARCHAR"), ("filing_date", "DATE"), ("event_date", "DATE"), ("available_at", "TIMESTAMP"),
    ("acceptance_clock", "VARCHAR"), ("vintage_risk", "VARCHAR"), ("event_type", "VARCHAR"),
    ("amount_usd", "DOUBLE"), ("amount_shares", "DOUBLE"), ("total_after_usd", "DOUBLE"), ("evidence", "VARCHAR"),
    ("extractor_version", "VARCHAR"),
    ("source_sha256", "VARCHAR"), ("execution_period", "VARCHAR"),
    ("execution_shares", "DOUBLE"), ("execution_average_price", "DOUBLE"),
    ("public_program_shares", "DOUBLE"), ("remaining_authorization_usd", "DOUBLE"),
]


def extract_part_ii(text: str) -> list[dict[str, Any]]:
    """Read a deliberately narrow native Part II Item 2 pipe table.

    Units must explicitly be shares and USD. Values in millions or thousands are
    rejected, as are unknown columns. Executions are never authorization events.
    """
    section = re.search(r"Part\s+II\b.*?Item\s+2\b(.*?)(?:\bItem\s+3\b|\Z)", text, re.I | re.S)
    if not section:
        return []
    lines = section.group(1).splitlines()
    if re.search(r"\b(?:millions?|thousands?|billions?|000s|in\s+\$?000)\b", section.group(1), re.I) or re.search(r"\b(?:shares?|amounts?|dollars?)\b[^.\n]{0,80}\b(?:in|expressed\s+in)\s+(?:millions?|thousands?|billions?|000s)\b", text, re.I):
        return []
    expected = ["period", "total shares purchased", "average price paid per share (usd)",
                "shares purchased under publicly announced plans", "remaining authorization (usd)"]
    active = False
    result = []
    for line in lines:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if [c.lower() for c in cells] == expected:
            active = True
            continue
        if not active or len(cells) != 5 or cells[0].lower() == "total":
            continue
        if not re.fullmatch(r"[A-Za-z]+\s+20\d{2}", cells[0]):
            continue
        try:
            numbers = [float(c.replace(",", "").replace("$", "")) for c in cells[1:]]
        except ValueError:
            continue
        import math
        if any(not math.isfinite(v) or v < 0 for v in numbers) or numbers[2] > numbers[0]:
            continue
        result.append(dict(zip(("execution_period", "execution_shares", "execution_average_price",
                               "public_program_shares", "remaining_authorization_usd"), [cells[0], *numbers]), evidence=line.strip()))
    return result


def _tmp() -> Path:
    p = C.build_root() / "_tmp" / "events"
    p.mkdir(parents=True, exist_ok=True)
    return p


def phase_parse(start: dt.date = dt.date(2019, 1, 1)) -> dict[str, Any]:
    """Legacy unadmitted catalog parsing cannot produce an admissible product."""
    raise ValueError("legacy catalog parse lacks independent native admission; use parse_native")


def native_rows(admission_path: Path, expected_sha256: str):
    from . import distributions_native as N
    from .distributions import clock
    originals, pins, admitted = N.replay_inputs(admission_path, expected_sha256)
    rows = []
    diagnostics = []
    for doc, blob, text in originals:
        base = {"cik": doc["cik"], "accession": doc["accession"], "form": doc["form"], "items": doc.get("items"),
                "source_doc": doc["source_doc"], "doc_source": doc["source_path"],
                "filing_date": dt.date.fromisoformat(doc["filing_date"]), "event_date": None,
                "available_at": clock(doc["fetched_at"]).replace(tzinfo=None), "acceptance_clock": "unqualified_fetched_at_floor",
                "vintage_risk": "clock_class_unqualified", "source_sha256": hashlib.sha256(blob).hexdigest(),
                "extractor_version": EXTRACTOR_VERSION}
        for event in extract_authorizations(text, base["filing_date"]):
            rows.append({**base, **event.__dict__})
        executions = extract_part_ii(text) if doc["form"].startswith("10-Q") else []
        for event in executions:
            rows.append({**base, **event, "event_type": "execution"})
        diagnostics.append({"accession": doc["accession"], "source_sha256": base["source_sha256"],
                            "execution_rows": len(executions), "unsupported_part_ii_reason":
                            "scaled_or_ambiguous_or_unsupported_caption" if doc["form"].startswith("10-Q") and not executions else None})
    N.validate_keys(rows, ["accession", "source_sha256", "event_type", "evidence"])
    N.recheck(pins)
    return rows, pins, admitted, diagnostics


def parse_native(admission_path: Path, expected_sha256: str) -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq
    rows, pins, admitted, diagnostics = native_rows(admission_path, expected_sha256)
    types = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"), "DOUBLE": pa.float64()}
    schema = pa.schema([(name, types[typ]) for name, typ in OUT_COLUMNS])
    dest = _tmp() / "buyback_rows.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), dest.with_suffix(".partial"))
    from . import distributions_native as N
    N.recheck(pins)
    dest.with_suffix(".partial").replace(dest)
    receipt = {"parse_rows_sha256": C.sha256_file(dest), "admission_sha256": expected_sha256,
               "native_input_code_pins": pins, "population": admitted["population"], "diagnostics": diagnostics}
    C.write_json_atomic(_tmp() / "buyback_parse_receipt.json", receipt)
    return receipt


def validate_product(target: Path, admission_path: Path, expected_sha256: str) -> dict[str, Any]:
    """Read-only full native and admitted join replay, without trusting a parse receipt."""
    import pyarrow as pa
    import pyarrow.parquet as pq
    from . import distributions_native as N, events_common as EC
    rows, pins, admitted, diagnostics = native_rows(admission_path, expected_sha256)
    before = C.sha256_file(target)
    types = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"), "DOUBLE": pa.float64()}
    table = pa.Table.from_pylist(rows, schema=pa.schema([(name, types[typ]) for name, typ in OUT_COLUMNS]))
    N.validate_keys(pq.read_table(target).to_pylist(), ["accession", "source_sha256", "event_type", "evidence"])
    con = C.connect(memory="128MB", threads=1)
    con.register("native", table)
    con.execute("""CREATE TABLE b AS SELECT r.*,
        CASE WHEN r.event_type='execution' THEN false ELSE CAST(NULL AS BOOLEAN) END AS first_announcement,
        CASE WHEN r.event_type='execution' THEN 'not_applicable_execution'
             ELSE 'unknown_incomplete_global_history' END AS first_announcement_basis FROM native r""")
    fund = Path(admitted["inputs"]["fundamentals"]["path"]).parent / "events.parquet"
    if EC.link_table_path().resolve() != (Path(admitted["inputs"]["identity"]["path"]).parent / "link_table.parquet").resolve():
        raise ValueError("security reader is not admitted identity path")
    con.execute(f"CREATE TABLE fx AS SELECT cik, accession AS xbrl_accession, clock_utc, buyback_authorized, buyback_remaining FROM read_parquet('{fund.as_posix()}') WHERE buyback_authorized IS NOT NULL OR buyback_remaining IS NOT NULL")
    con.execute("""CREATE TABLE b2 AS SELECT b.*, pb.buyback_authorized AS xbrl_authorized_before,
        pb.buyback_remaining AS xbrl_remaining_before, pa.buyback_authorized AS xbrl_authorized_after,
        pa.buyback_remaining AS xbrl_remaining_after, pa.xbrl_accession AS xbrl_after_accession,
        pa.clock_utc AS xbrl_after_available_at FROM b
        ASOF LEFT JOIN fx pb ON pb.cik=b.cik AND b.available_at>pb.clock_utc
        ASOF LEFT JOIN fx pa ON pa.cik=b.cik AND pa.clock_utc>b.available_at""")
    EC.attach_security(con, "b2", "b3", date_col="filing_date")
    con.execute(f"CREATE VIEW actual AS SELECT * FROM read_parquet('{target.as_posix()}')")
    actual_schema = dict((r[0], r[1]) for r in con.execute("DESCRIBE actual").fetchall())
    expected_schema = dict((r[0], r[1]) for r in con.execute("DESCRIBE b3").fetchall())
    if actual_schema != expected_schema:
        raise ValueError("exact buyback schema mismatch")
    columns = ",".join('"' + name + '"' for name in sorted(actual_schema))
    delta = con.execute(f"SELECT count(*) FROM ((SELECT {columns} FROM actual EXCEPT ALL SELECT {columns} FROM b3) UNION ALL (SELECT {columns} FROM b3 EXCEPT ALL SELECT {columns} FROM actual))").fetchone()[0]
    con.close()
    if delta:
        raise ValueError("buyback full native/join replay mismatch")
    N.recheck(pins)
    if C.sha256_file(target) != before:
        raise ValueError("buyback product changed during admission")
    return {"native_replay": "PASS", "rows": len(rows), "catalog_rows": admitted["population"]["catalog_rows"],
            "source_semantics": "UNQUALIFIED", "diagnostics": diagnostics, "sha256": before}


def phase_publish(*, admission_path: Path | None = None, expected_sha256: str | None = None) -> dict[str, Any]:
    from . import events_common as EC
    from . import distributions_native as N
    import pyarrow as pa
    import pyarrow.parquet as pq
    if admission_path is None or expected_sha256 is None:
        raise ValueError("independent complete native admission required; hash-only parse receipt refused")
    expected_rows, admitted_pins, admitted, diagnostics = native_rows(admission_path, expected_sha256)

    receipt: dict[str, Any] = {"parse": C.read_json(_tmp() / "buyback_parse_receipt.json")}
    parse_path = _tmp() / "buyback_rows.parquet"
    if receipt["parse"].get("admission_sha256") != expected_sha256 or receipt["parse"].get("native_input_code_pins") != admitted_pins:
        raise ValueError("abbreviated/stale parse provenance refused")
    types = {"BIGINT": pa.int64(), "VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"), "DOUBLE": pa.float64()}
    schema = pa.schema([(name, types[typ]) for name, typ in OUT_COLUMNS])
    parsed = pq.read_table(parse_path)
    if parsed.schema != schema or not parsed.equals(pa.Table.from_pylist(expected_rows, schema=schema)):
        raise ValueError("parsed schema/full native replay mismatch")
    if C.sha256_file(parse_path) != receipt["parse"]["parse_rows_sha256"]:
        raise ValueError("buyback parsed rows changed")
    pin_paths = [parse_path, C.build_root() / "fundamentals" / "events.parquet", EC.link_table_path(),
                 C.build_root() / "fundamentals" / "manifest.json", C.build_root() / "identity" / "manifest.json"]
    pinned = {str(p): C.sha256_file(p) for p in pin_paths}
    if any(str(p.resolve()) not in admitted_pins or C.sha256_file(p) != admitted_pins[str(p.resolve())] for p in pin_paths[1:]):
        raise ValueError("fundamental/identity reader inputs not independently admitted")
    pinned.update(admitted_pins)
    con = C.connect(memory="128MB", threads=1)
    rows = (_tmp() / "buyback_rows.parquet").as_posix()
    fund = (C.build_root() / "fundamentals" / "events.parquet").as_posix()
    con.execute(f"""
        CREATE TABLE b AS SELECT r.*,
            CASE WHEN r.event_type='execution' THEN false ELSE CAST(NULL AS BOOLEAN) END AS first_announcement,
            CASE WHEN r.event_type='execution' THEN 'not_applicable_execution'
                 ELSE 'unknown_incomplete_global_history' END AS first_announcement_basis
        FROM read_parquet('{rows}') r""")
    con.execute(f"""
        CREATE TABLE fx AS SELECT cik, accession AS xbrl_accession, clock_utc, buyback_authorized, buyback_remaining
        FROM read_parquet('{fund}') WHERE buyback_authorized IS NOT NULL OR buyback_remaining IS NOT NULL""")
    con.execute("""
        CREATE TABLE b2 AS
        SELECT b.*, pb.buyback_authorized AS xbrl_authorized_before, pb.buyback_remaining AS xbrl_remaining_before,
               pa.buyback_authorized AS xbrl_authorized_after, pa.buyback_remaining AS xbrl_remaining_after,
               pa.xbrl_accession AS xbrl_after_accession, pa.clock_utc AS xbrl_after_available_at
        FROM b
        ASOF LEFT JOIN fx pb ON pb.cik = b.cik AND b.available_at > pb.clock_utc
        ASOF LEFT JOIN (SELECT * FROM fx) pa ON pa.cik = b.cik AND pa.clock_utc > b.available_at""")
    EC.attach_security(con, "b2", "b3", date_col="filing_date")
    code = C.code_identity("events_buyback", "guidance", "events_common", "events_sources", "common")
    product_id = hashlib.sha256(json.dumps({"inputs": pinned, "code": {k: v for k, v in code.items() if k != "git_head"}}, sort_keys=True).encode()).hexdigest()
    product = C.stage_dir(EC.STAGE) / "buyback_products" / product_id
    if product.exists():
        raise FileExistsError("immutable buyback product already exists")
    product.mkdir(parents=True)
    out = product / f"{TABLE}.parquet"
    receipt["rows"] = C.copy_to_parquet(con, """
        SELECT cik, security_id, link_tier, link_basis, is_member_issuer, accession, form, items, source_doc,
               doc_source, filing_date, event_date, available_at, acceptance_clock, vintage_risk, event_type,
               amount_usd, amount_shares, total_after_usd, first_announcement, first_announcement_basis, xbrl_authorized_before,
               xbrl_remaining_before, xbrl_authorized_after, xbrl_remaining_after, xbrl_after_accession,
               xbrl_after_available_at, evidence, extractor_version, source_sha256, execution_period,
               execution_shares, execution_average_price, public_program_shares, remaining_authorization_usd
        FROM b3 ORDER BY cik, available_at, accession""", out)
    o = out.as_posix()
    receipt["per_year"] = {str(y): {"rows": int(n), "first": int(f), "member_first": int(mf), "ciks": int(c),
                                    "increase": int(i), "shares_only": int(s)}
                           for y, n, f, mf, c, i, s in con.execute(f"""
        SELECT year(filing_date), count(*), count(*) FILTER (WHERE first_announcement),
               count(*) FILTER (WHERE first_announcement AND is_member_issuer), count(DISTINCT cik),
               count(*) FILTER (WHERE event_type = 'increase'), count(*) FILTER (WHERE amount_usd IS NULL
                                                                                  AND amount_shares IS NOT NULL)
        FROM read_parquet('{o}') GROUP BY 1 ORDER BY 1""").fetchall()}
    receipt["xbrl_after_agrees"] = con.execute(f"""
        SELECT count(*) FILTER (WHERE amount_usd IS NOT NULL AND xbrl_authorized_after IS NOT NULL),
               count(*) FILTER (WHERE amount_usd IS NOT NULL AND xbrl_authorized_after IS NOT NULL
                                AND (abs(xbrl_authorized_after - amount_usd) <= 0.01 * amount_usd
                                     OR abs(xbrl_authorized_after - coalesce(total_after_usd, -1)) <= 0.01 * amount_usd))
        FROM read_parquet('{o}') WHERE first_announcement""").fetchone()
    con.close()
    replay_proof = validate_product(out, admission_path, expected_sha256)
    N.recheck(admitted_pins)
    for name, digest in pinned.items():
        if C.sha256_file(Path(name)) != digest:
            raise ValueError("buyback input changed during publication")
    C.write_json_atomic(product / "manifest.json", {"schema": SCHEMA, "product_id": product_id,
        "input_manifests_sha256": pinned, "file_sha256": C.sha256_file(out), "admission": replay_proof,
        "gate_status": {"buyback_200_native_handchecks": "FAIL", "measured": 0, "required": 200}, **receipt})
    import shutil
    alias = C.stage_dir(EC.STAGE) / f"{TABLE}.parquet"
    shutil.copyfile(out, alias.with_suffix(".partial"))
    alias.with_suffix(".partial").replace(alias)
    EC.publish_table(TABLE, ("events_buyback", "guidance", "events_common", "events_sources", "common"), {
        "schema": SCHEMA, "rule": __doc__, "extractor_version": EXTRACTOR_VERSION,
        "gate_status": {"buyback_200_native_handchecks": "FAIL", "measured": 0, "required": 200}, **receipt})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", choices=("parse", "publish", "all"), default="all")
    ap.add_argument("--admission", type=Path, required=True)
    ap.add_argument("--admission-sha256", required=True)
    ap.add_argument("--validate-product", type=Path)
    args = ap.parse_args(argv)
    out: dict[str, Any] = {}
    if args.validate_product:
        print(json.dumps(validate_product(args.validate_product, args.admission, args.admission_sha256), default=str))
        return 0
    if args.phase in ("parse", "all"):
        out["parse"] = parse_native(args.admission, args.admission_sha256)
    if args.phase in ("publish", "all"):
        out["publish"] = phase_publish(admission_path=args.admission, expected_sha256=args.admission_sha256)
    print(json.dumps(out, default=str, indent=1)[:5000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
