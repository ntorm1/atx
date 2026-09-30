"""Filing text landing (lane TXT, S7.2 / S7.3 input): 10-K / 20-F primary documents -> extracted Item sections.

Selection (``select_filings``): original annual reports of the 10-K family (``FORMS_10K``) and 20-F filed from
``FILED_FROM`` by a CIK of ``identity/link_table.parquet``, from ``sec_filings/filings.parquet`` (the resolved
acceptance clock). Priority 1: CIKs with an ``ever_member`` line, 2: every other linked CIK; oldest first within.

Per filing (resumable): GET ``Archives/edgar/data/<cik>/<acc>/<primaryDocument>`` through ``atx_db.sec_http``
(approved agent, host-wide 5 req/s limiter, shared 403/429 pause) -> ``html_to_text`` -> ``extract_sections`` ->
one row per found section in ``data/raw/sec_text/sections/part-NNNNN.parquet`` (zstd) and one row per filing in
``docs/part-NNNNN.parquet`` (url, bytes, sha256, parse diagnostics, whole-document readability) -> one line per
request in ``receipts.jsonl`` (url, bytes, sha256, http_status, fetched_at). The full document is never written
to disk. A filing is done once its accession is in a ``docs`` part, or its receipt carries a terminal HTTP status.

When a 10-K's MD&A (or Business / Risk Factors) is incorporated by reference to the annual report exhibit (bank
holding companies), ``--phase exhibits`` reads the filing index and extracts the section from the EX-13 exhibit
(``source = 'ex13'``), within the same request budget.

Sections (``SECTION_ITEMS``): ``business`` (10-K Item 1, 20-F Item 4), ``risk`` (10-K Item 1A, 20-F Item 3.D),
``mdna`` (10-K Item 7, 10-KSB Item 6, 20-F Item 5). The parser (``find_items`` / ``extract_sections``) works on
text lines: item headings (``ITEM_RE``) whose title matches the item's expected title are candidates; runs of
``TOC_MIN_ITEMS`` or more headings each within ``TOC_GAP`` characters of the next are a table of contents (or a
cross-reference index) and are ignored; a section runs from its heading to the next heading of a later item, and
among repeated headings the one giving the longest non-empty section wins (a table-of-contents entry is short).
When no item heading survives, a heading line that is exactly the section title is used (``method =
'title'``). Sections that overlap are cut at the later section's start.

Clock: every row carries the filing's ``available_at`` (``sec_filings`` acceptance-per-file-clock-v1).
"""

from __future__ import annotations

import argparse
import datetime as dt
import gc
import hashlib
import html
import json
import os
import re
import sys
import threading
import time
from collections.abc import Iterable, Iterator
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import text_features as TF

SOURCE = "sec_text"
FORMS_10K = ("10-K", "10-K405", "10-KSB", "10-KSB40", "10-KT", "10-KT405")
FORMS_20F = ("20-F",)
FILED_FROM = dt.date(2018, 1, 1)
ARCHIVES = "https://www.sec.gov/Archives/edgar/data"
MAX_DOC_BYTES = 40 * 1024 * 1024
#: Documents above this size are parsed one at a time (the parse holds ~3 copies of the document).
BIG_DOC_BYTES = 12 * 1024 * 1024
REQUEST_BUDGET = 60_000
BATCH_FILINGS = 100
PARQUET_ZSTD_LEVEL = 6
#: A table whose digits exceed this share of its digits + letters is a numeric table and is dropped.
NUMERIC_TABLE_SHARE = 0.15
TOC_GAP = 400
TOC_MIN_ITEMS = 7
TOC_REAPPEAR_SHARE = 0.5
#: A numbered section shorter than this (a cross-reference index entry, a stub) is re-tried by title.
SHORT_SECTION_CHARS = 300
RUNNING_MIN_REPEATS = 4
RUNNING_MAX_CHARS = 120
#: A section below this many characters whose text says "incorporated by reference" is flagged ``by_reference``.
BY_REFERENCE_MAX_CHARS = 4000
#: v1: the first 1,200 landed filings (2018-01..02); v2: "Item 1.A", "Item I", "Our Business", "Combined MD&A",
#: 20-F "Information on the Partnership" headings.
PARSER_VERSION = "filing-text-sections-v2"

SECTION_ITEMS: dict[str, dict[str, str]] = {
    "10-K": {"business": "1", "risk": "1A", "mdna": "7"},
    "10-KSB": {"business": "1", "risk": "1A", "mdna": "6"},
    "20-F": {"business": "4", "risk": "3", "mdna": "5"},
}

# ---------------------------------------------------------------------------------------------------------
# HTML / text -> lines (pure)
# ---------------------------------------------------------------------------------------------------------

_DROP_RE = re.compile(rb"<(ix:header|script|style|head)\b.*?</\1\s*>", re.I | re.S)
_COMMENT_RE = re.compile(rb"<!--.*?-->", re.S)
_IMG_RE = re.compile(rb"<img\b[^>]*>", re.I)
_TABLE_RE = re.compile(rb"<table\b.*?</table\s*>", re.I | re.S)
_BLOCK_RE = re.compile(
    rb"<(?:br|hr|/?(?:p|div|tr|li|h[1-6]|table|ul|ol|center|blockquote|pre|dd|dt|dl|section|article|title|"
    rb"page|document|type|sequence|filename|description|text))\b[^>]*>", re.I)
_CELL_RE = re.compile(rb"</t[dh]\s*>", re.I)
_TAG_RE = re.compile(rb"</?[a-zA-Z!][^<>]*>")
_ENTITY_RE = re.compile(rb"&#?[a-zA-Z0-9]{1,10};")
_LETTERS = bytes(range(65, 91)) + bytes(range(97, 123))
_DIGITS = b"0123456789"
_SPACE_RE = re.compile(r"[ \t\r\f\v]+")
_PAGE_LINE_RE = re.compile(
    r"^(?:page\s+)?[-(\[]?\s*(?:\d{1,4}|[ivxlc]{1,6}|[a-z]{1,2}\s?-\s?\d{1,3})\s*[-)\]]?$"
    r"|^(?:table of contents|index|back to (?:top|contents|table of contents)|return to table of contents)\.?$",
    re.I)
_CHAR_MAP = str.maketrans({
    "\xa0": " ", " ": " ", " ": " ", " ": " ", " ": " ", " ": " ", "　": " ",
    "​": "", "‌": "", "‍": "", "﻿": "", "\xad": "",
    "‘": "'", "’": "'", "′": "'", "“": '"', "”": '"',
    "‐": "-", "‑": "-", "‒": "-", "–": "-", "—": "-", "―": "-", "−": "-",
    "•": " ", "●": " ", "·": " ", "": " ", "": " ",
})


def _numeric_share(blob: bytes) -> float:
    text = _ENTITY_RE.sub(b" ", _TAG_RE.sub(b" ", blob))
    letters = len(text) - len(text.translate(None, _LETTERS))
    digits = len(text) - len(text.translate(None, _DIGITS))
    total = letters + digits
    return digits / total if total else 1.0


def _table_sub(m: re.Match[bytes]) -> bytes:
    blob = m.group(0)
    if _numeric_share(blob) > NUMERIC_TABLE_SHARE:
        return b"\n"
    return b"\n" + _CELL_RE.sub(b" ", blob) + b"\n"


def decode(blob: bytes) -> str:
    """UTF-8, else Windows-1252 (EDGAR's legacy encoding); undecodable bytes become U+FFFD."""
    try:
        return blob.decode("utf-8")
    except UnicodeDecodeError:
        return blob.decode("cp1252", errors="replace")


def html_to_text(raw: bytes) -> str:
    """Readable text of an EDGAR HTML or ASCII document, one block element per line.

    Dropped: the inline-XBRL header (hidden facts), scripts, styles, comments, images, numeric tables
    (``NUMERIC_TABLE_SHARE``), page-number and "Table of Contents" back-link lines. Text tables keep one row per
    line. Inline tags are removed without a space (as a browser renders them); entities are unescaped.
    """
    blob = _DROP_RE.sub(b" ", raw)
    blob = _COMMENT_RE.sub(b" ", blob)
    blob = _IMG_RE.sub(b" ", blob)
    blob = _TABLE_RE.sub(_table_sub, blob)
    blob = _BLOCK_RE.sub(b"\n", blob)
    blob = _CELL_RE.sub(b" ", blob)
    blob = _TAG_RE.sub(b"", blob)
    text = html.unescape(decode(blob)).translate(_CHAR_MAP)
    del blob
    out: list[str] = []
    for line in text.split("\n"):
        line = _SPACE_RE.sub(" ", line).strip()
        if line and not _PAGE_LINE_RE.match(line):
            out.append(line)
    return "\n".join(out)


# ---------------------------------------------------------------------------------------------------------
# Item headings and sections (pure)
# ---------------------------------------------------------------------------------------------------------

ITEM_RE = re.compile(
    r"^(?:part\s+(?:iv|i{1,3}|[1-4])\b[\s,.:;-]*)?items?\s*(\d{1,2}|i{1,3}(?![a-z]))\s*(?:\.?\s*\(?([a-k])\)?(?![a-z]))?"
    r"\s*[.:\-)]?\s*(.*)$",
    re.I)
#: A title may open with one of these words ("Item 1. Our Business", "Item 7. Combined Management's Discussion").
_TITLE_PREFIX = r"(?:(?:our|the|combined|company'?s|registrant'?s)\s+)?"
_ROMAN = {"i": 1, "ii": 2, "iii": 3}
_SPLIT_ITEM_RE = re.compile(r"^(?:part\s+(?:iv|i{1,3})[\s,.:;-]*)?items?\s*[.:]?$", re.I)
_GENERIC_TITLE_RE = re.compile(
    r"^(?:and\s+\d|&\s*\d|\[?\(?(?:removed\s+and\s+)?reserved|not\s+applicable|none\b|omitted|intentionally)")
_CONTINUED_RE = re.compile(r"\(?\s*continued\s*\)?\.?$", re.I)

#: 10-K family item -> (order, expected title prefix regex)
ITEMS_10K: dict[str, tuple[int, str]] = {
    "1": (10, r"(?:description\s+of\s+(?:the\s+)?)?business"),
    "1A": (11, r"risk\s*factors"),
    "1B": (12, r"unresolved"),
    "1C": (13, r"cyber"),
    "2": (20, r"(?:description\s+of\s+)?propert"),
    "3": (30, r"legal"),
    "4": (40, r"mine\s+safety|submission|\(?removed|\[?reserved|\(?reserved"),
    "4A": (41, r"executive\s+officers|\(?removed|\[?reserved"),
    "5": (50, r"market"),
    "6": (60, r"selected|\[?reserved|\(?reserved|management|md\s*&\s*a"),
    "7": (70, r"management|md\s*&\s*a"),
    "7A": (71, r"quantitative|qualitative"),
    "8": (80, r"(?:consolidated\s+)?financial\s+statements|financial"),
    "9": (90, r"changes\s+in|disagreements"),
    "9A": (91, r"controls"),
    "9B": (92, r"other\s+information"),
    "9C": (93, r"disclosure|foreign"),
    "10": (100, r"directors|executive\s+officers"),
    "11": (110, r"executive\s+compensation|compensation"),
    "12": (120, r"security\s+ownership|ownership"),
    "13": (130, r"certain\s+relationships|relationships"),
    "14": (140, r"principal\s+account|accountant"),
    "15": (150, r"exhibits?|financial\s+statement\s+schedules"),
    "16": (160, r"form\s+10-?k\s+summary|summary"),
}
#: 20-F item -> (order, expected title prefix regex); lettered sub-items of 3, 4, 5, ... share their item's order.
ITEMS_20F: dict[str, tuple[int, str]] = {
    "1": (10, r"identity"),
    "2": (20, r"offer\s+statistics"),
    "3": (30, r"key\s+information"),
    "4": (40, r"information\s+(?:on|of|about)\s+(?:the\s+)?(?:company|partnership|group|registrant|issuer|trust|bank)"
              r"|company\s+information"),
    "4A": (41, r"unresolved"),
    "5": (50, r"operating\s+and\s+financial|financial\s+review"),
    "6": (60, r"directors"),
    "7": (70, r"major\s+(?:share|unit)?holders"),
    "8": (80, r"financial\s+information"),
    "9": (90, r"the\s+offer|offer\s+and\s+listing"),
    "10": (100, r"additional\s+information"),
    "11": (110, r"quantitative|qualitative"),
    "12": (120, r"description\s+of\s+securities"),
    "13": (130, r"defaults"),
    "14": (140, r"material\s+modifications"),
    "15": (150, r"controls"),
    "16": (160, r"\[?reserved|\(?reserved"),
    **{f"16{c}": (160 + i + 1, r".") for i, c in enumerate("ABCDEFGHIJK")},
    "17": (170, r"financial\s+statements"),
    "18": (180, r"financial\s+statements"),
    "19": (190, r"exhibits"),
}
#: Title-only heading lines (fallback when no numbered heading survives) per section.
TITLE_LINES: dict[str, dict[str, str]] = {
    "10-K": {
        "business": r"(?:description\s+of\s+)?business|business\s+overview",
        "risk": r"risk\s+factors",
        "mdna": r"management'?s?\s+discussion\s+and\s+analysis(?:\s+of\s+(?:the\s+)?(?:consolidated\s+)?financial"
                r"\s+condition\s+and\s+(?:the\s+)?results\s+of\s+operations|\s+of\s+(?:the\s+)?results\s+of\s+operations"
                r"\s+and\s+financial\s+condition)?|md\s*&\s*a",
    },
    "20-F": {
        "business": r"information\s+(?:on|of|about)\s+(?:the\s+)?(?:company|partnership|group)|business\s+overview",
        "risk": r"risk\s+factors",
        "mdna": r"operating\s+and\s+financial\s+review(?:\s+and\s+prospects)?",
    },
}
#: Other known section titles (fallback ends): a title-only line of any of these ends a fallback section.
OTHER_TITLES = (
    r"unresolved\s+staff\s+comments|cybersecurity|properties|legal\s+proceedings|mine\s+safety\s+disclosures|"
    r"market\s+for\s+(?:the\s+)?registrant'?s\s+common\s+equity.*|selected\s+(?:consolidated\s+)?financial\s+data|"
    r"quantitative\s+and\s+qualitative\s+disclosures?\s+about\s+market\s+risk|"
    r"(?:consolidated\s+)?financial\s+statements(?:\s+and\s+supplementary\s+data)?|"
    r"changes\s+in\s+and\s+disagreements\s+with\s+accountants.*|controls\s+and\s+procedures|other\s+information|"
    r"directors,?\s+executive\s+officers\s+and\s+corporate\s+governance|executive\s+compensation|"
    r"exhibits(?:\s+and\s+financial\s+statement\s+schedules)?|signatures|report\s+of\s+independent\s+registered"
    r"\s+public\s+accounting\s+firm.*|forward-looking\s+statements|cautionary\s+(?:note|statement).*|"
    r"consolidated\s+(?:balance\s+sheets?|statements?\s+of\s+(?:income|operations|earnings|financial\s+condition|"
    r"cash\s+flows?)).*|management'?s\s+report\s+on\s+internal\s+control.*|"
    r"(?:directors|major\s+shareholders|financial\s+information|additional\s+information|key\s+information|"
    r"the\s+offer\s+and\s+listing).*"
)
_TITLE_TAIL = r"\s*[.:]?\s*(?:\(continued\))?$"
_SUB_RISK_20F_RE = re.compile(r"^(?:(?:item\s*)?3\s*\.?\s*)?d\s*[.:)\-]?\s*risk\s+factors|^risk\s+factors\s*\.?$", re.I)
_BY_REFERENCE_RE = re.compile(
    r"incorporated\s+(?:herein\s+|into\s+this\s+item\s+)?by\s+reference|is\s+included\s+in\s+(?:the\s+)?"
    r"(?:registrant'?s\s+|our\s+|company'?s\s+)?(?:\d{4}\s+)?annual\s+report|appears?\s+on\s+pages?|"
    r"can\s+be\s+found\s+(?:in|on|under)|see\s+pages?\s+\d|is\s+set\s+forth\s+(?:in|on|under)", re.I)
#: The end of a cross-reference index line: page numbers ("24-31", "4-7, 9-10") or a note marker ("(a)").
_PAGE_REF_TAIL_RE = re.compile(r"(?:\d{1,3}(?:\s*[-,]\s*\d{1,3})*|\([a-e]\))\s*$")


def form_family(form: str) -> str:
    f = (form or "").upper().strip()
    if f.startswith("20-F"):
        return "20-F"
    if f.startswith("10-KSB"):
        return "10-KSB"
    return "10-K"


def _items_for(family: str) -> dict[str, tuple[int, str]]:
    return ITEMS_20F if family == "20-F" else ITEMS_10K


@dataclass(frozen=True)
class Heading:
    line: int
    offset: int
    key: str
    order: int
    sub: str | None
    title: str
    toc: bool = False


def item_key(number: str, letter: str | None, family: str) -> tuple[str, str | None] | None:
    """(item key, sub-item letter) for a heading's number and optional letter; None if not an item of the form."""
    items = _items_for(family)
    num = str(int(number)) if number.isdigit() else str(_ROMAN[number.lower()])
    if letter:
        k = f"{num}{letter.upper()}"
        if k in items:
            return k, None
        if family == "20-F" and num in items:
            return num, letter.upper()  # 20-F sub-item (3.D -> item 3, sub D)
        return None
    return (num, None) if num in items else None


def _title_ok(key: str, title: str, family: str) -> bool:
    t = _CONTINUED_RE.sub("", title.strip().lstrip(".:-) ").strip()).strip().lower()
    if len(t) <= 3:
        return True
    if _GENERIC_TITLE_RE.match(t):
        return True
    return re.match(_TITLE_PREFIX + "(?:" + _items_for(family)[key][1] + ")", t) is not None


def _line_offsets(lines: list[str]) -> list[int]:
    offs, pos = [], 0
    for ln in lines:
        offs.append(pos)
        pos += len(ln) + 1
    return offs


def find_items(lines: list[str], family: str) -> list[Heading]:
    """Numbered item headings, in document order, with table-of-contents runs flagged ``toc``."""
    items = _items_for(family)
    offs = _line_offsets(lines)
    found: list[Heading] = []
    for i, ln in enumerate(lines):
        if len(ln) < 4 or ln[0] not in "IiPp":
            continue
        if _SPLIT_ITEM_RE.match(ln):  # "Item" alone, the number (and title) on the next line(s)
            ln = " ".join(lines[i:i + 3])
        m = ITEM_RE.match(ln)
        if not m:
            continue
        kk = item_key(m.group(1), m.group(2), family)
        if kk is None:
            continue
        key, sub = kk
        title = m.group(3) or ""
        if sub is None and not _title_ok(key, title, family) and not (
                i + 1 < len(lines) and _title_ok(key, f"{title} {lines[i + 1]}", family)):  # title split over lines
            continue
        found.append(Heading(i, offs[i], key, items[key][0], sub, title.strip()))
    # table of contents: a run of TOC_MIN_ITEMS+ distinct items, each within TOC_GAP characters of the next and in
    # item order (the body's first heading right after a table of contents starts a new run), whose items mostly
    # reappear after the run as headings. Short body items packed together (small filers' Items 9B-15) do not
    # reappear, so they stay headings.
    flagged = [False] * len(found)
    start = 0
    for j in range(1, len(found) + 1):
        if (j == len(found) or found[j].offset - found[j - 1].offset > TOC_GAP
                or found[j].order < found[j - 1].order):
            keys = {h.key for h in found[start:j]}
            if len(keys) >= TOC_MIN_ITEMS:
                later = {h.key for h in found[j:]}
                if len(keys & later) >= TOC_REAPPEAR_SHARE * len(keys):
                    for k in range(start, j):
                        flagged[k] = True
            start = j
    return [Heading(h.line, h.offset, h.key, h.order, h.sub, h.title, flagged[n]) for n, h in enumerate(found)]


@dataclass
class Section:
    name: str
    item: str
    start_line: int
    end_line: int
    method: str
    ends_at_eof: bool
    text: str = ""
    flags: list[str] = field(default_factory=list)


def _span_chars(offs: list[int], total: int, start: int, end: int) -> int:
    return (offs[end] if end < len(offs) else total) - offs[start]


def _best_numbered(heads: list[Heading], key: str, offs: list[int], total: int) -> tuple[int, int, bool] | None:
    body = [h for h in heads if not h.toc]
    order = next((h.order for h in body if h.key == key), None)
    if order is None:
        return None
    n_lines = len(offs)
    best: tuple[tuple[int, int, int], int, int, bool] | None = None
    for idx, h in enumerate(body):
        if h.key != key or h.sub is not None:
            continue
        nxt = next((g for g in body[idx + 1:] if g.order > order), None)
        end = nxt.line if nxt else n_lines
        eof = nxt is None
        chars = (offs[end] if end < n_lines else total) - h.offset
        cand = ((0 if eof else 1, chars, h.line), h.line, end, eof)
        if best is None or cand[0] > best[0]:
            best = cand
    if best is None:
        return None
    return best[1], best[2], best[3]


def _title_line_re(pattern: str) -> re.Pattern[str]:
    return re.compile(rf"^(?:(?:part\s+[iv]+\s*[,.:-]?\s*)?items?\s*\d{{1,2}}[a-j]?\s*[.:\-)]?\s*)?(?:{pattern}){_TITLE_TAIL}",
                      re.I)


def _best_title(lines: list[str], heads: list[Heading], section: str, family: str,
                offs: list[int]) -> tuple[int, int, bool] | None:
    fam = "20-F" if family == "20-F" else "10-K"
    own = _title_line_re(TITLE_LINES[fam][section])
    others = [_title_line_re(p) for s, p in TITLE_LINES[fam].items() if s != section] + [_title_line_re(OTHER_TITLES)]
    head_lines = {h.line for h in heads if not h.toc}
    toc_lines = sorted(h.line for h in heads if h.toc)
    toc_span = (toc_lines[0], toc_lines[-1]) if toc_lines else (-1, -1)
    total = offs[-1] + len(lines[-1]) if lines else 0
    starts = [i for i, ln in enumerate(lines) if len(ln) <= 200 and own.match(ln)
              and not (toc_span[0] <= i <= toc_span[1])]
    best: tuple[tuple[int, int], int, int, bool] | None = None
    for s in starts:
        end, eof = len(lines), True
        for j in range(s + 1, len(lines)):
            ln = lines[j]
            if j in head_lines or (len(ln) <= 200 and any(o.match(ln) for o in others)):
                end, eof = j, False
                break
        cand = ((0 if eof else 1, _span_chars(offs, total, s, end)), s, end, eof)
        if best is None or cand[0] > best[0]:
            best = cand
    if best is None or best[2] - best[1] < 2:
        return None
    return best[1], best[2], best[3]


def extract_sections(text: str, form: str) -> tuple[dict[str, Section], dict[str, Any]]:
    """``({section: Section}, diagnostics)`` for one document's text (see the module docstring for the rules)."""
    family = form_family(form)
    targets = SECTION_ITEMS[family]
    lines = text.split("\n")
    offs = _line_offsets(lines)
    heads = find_items(lines, family)
    found: dict[str, Section] = {}
    total = len(text)
    index_only: list[str] = []
    for name, key in targets.items():
        got = _best_numbered(heads, key, offs, total)
        method = "item"
        weak = False
        if got is not None:
            chars = _span_chars(offs, total, got[0], got[1])
            weak = got[2] or chars < SHORT_SECTION_CHARS or (
                chars <= BY_REFERENCE_MAX_CHARS and _BY_REFERENCE_RE.search(text, offs[got[0]], offs[got[0]] + chars)
                is not None)
        if got is None or weak:
            alt = _best_title(lines, heads, name, family, offs)
            if alt is not None and not alt[2] and (
                    got is None or _span_chars(offs, total, alt[0], alt[1]) > _span_chars(offs, total, got[0], got[1])):
                got, method = alt, "title"
        if got is None:
            continue
        s, e, eof = got
        if method == "item" and e - s == 1 and _PAGE_REF_TAIL_RE.search(lines[s]):
            index_only.append(name)  # a cross-reference index entry ("Item 1A. Risk Factors 24-31"): no text here
            continue
        found[name] = Section(name, key, s, e, method, eof)
    # a section that contains another section's start is cut there
    for sec in found.values():
        for other in found.values():
            if other is not sec and sec.start_line < other.start_line < sec.end_line:
                sec.end_line = other.start_line
                sec.flags.append(f"cut_at_{other.name}")
    # 20-F risk factors: sub-item 3.D inside item 3
    if family == "20-F" and "risk" in found:
        sec = found["risk"]
        sub = next((h.line for h in heads if not h.toc and h.key == "3" and h.sub == "D"
                    and sec.start_line <= h.line < sec.end_line), None)
        if sub is None:
            sub = next((i for i in range(sec.start_line + 1, sec.end_line)
                        if len(lines[i]) <= 120 and (_SUB_RISK_20F_RE.match(lines[i]) or _SUB_RISK_20F_RE.match(
                            f"{lines[i]} {lines[i + 1] if i + 1 < len(lines) else ''}"))), None)
        if sub is not None:
            sec.start_line = sub
            sec.flags.append("item3d")
        else:
            sec.flags.append("item3_whole")
    running = running_lines(lines)
    for sec in found.values():
        body = [ln for ln in lines[sec.start_line + 1:sec.end_line] if _digit_key(ln) not in running]
        sec.text = "\n".join([lines[sec.start_line], *body])
        if len(sec.text) <= BY_REFERENCE_MAX_CHARS and _BY_REFERENCE_RE.search(sec.text):
            sec.flags.append("by_reference")
        if sec.ends_at_eof:
            sec.flags.append("eof")
    diag = {"lines": len(lines), "chars": len(text), "headings": len(heads),
            "toc_headings": sum(h.toc for h in heads), "family": family, "index_only": index_only,
            "running_lines": len(running)}
    return found, diag


_DIGITS_RE = re.compile(r"\d+")


def _digit_key(line: str) -> str:
    return _DIGITS_RE.sub("#", line)


def running_lines(lines: list[str]) -> set[str]:
    """Page headers / footers: short lines (digits masked) repeated ``RUNNING_MIN_REPEATS``+ times that do not end
    a sentence ("Apple Inc. | 2024 Form 10-K | 26"). They are dropped from section text (never from parsing)."""
    counts: dict[str, int] = {}
    for ln in lines:
        if len(ln) <= RUNNING_MAX_CHARS and ln[-1] not in ".;:":
            k = _digit_key(ln)
            counts[k] = counts.get(k, 0) + 1
    return {k for k, n in counts.items() if n >= RUNNING_MIN_REPEATS}


# ---------------------------------------------------------------------------------------------------------
# Landing: selection, fetch, parts, receipts
# ---------------------------------------------------------------------------------------------------------

DOC_SCHEMA = pa.schema([
    ("accession", pa.string()), ("cik", pa.int64()), ("ciks", pa.list_(pa.int64())), ("form", pa.string()),
    ("filing_date", pa.date32()), ("report_date", pa.date32()), ("acceptance_utc", pa.timestamp("us")),
    ("acceptance_clock", pa.string()), ("vintage_risk", pa.string()), ("available_at", pa.timestamp("us")),
    ("priority", pa.int8()), ("primary_document", pa.string()), ("url", pa.string()), ("http_status", pa.int32()),
    ("doc_bytes", pa.int64()), ("sha256", pa.string()), ("fetched_at", pa.string()), ("requests", pa.int32()),
    ("error", pa.string()), ("text_chars", pa.int64()), ("doc_words", pa.int64()), ("doc_sentences", pa.int64()),
    ("doc_complex_words", pa.int64()), ("doc_fog", pa.float64()), ("headings", pa.int32()),
    ("toc_headings", pa.int32()), ("running_lines", pa.int32()), ("index_only", pa.list_(pa.string())),
    ("sections_found", pa.list_(pa.string())), ("parser", pa.string()),
])
SECTION_SCHEMA = pa.schema([
    ("accession", pa.string()), ("cik", pa.int64()), ("ciks", pa.list_(pa.int64())), ("form", pa.string()),
    ("filing_date", pa.date32()), ("report_date", pa.date32()), ("available_at", pa.timestamp("us")),
    ("section", pa.string()), ("item", pa.string()), ("method", pa.string()), ("flags", pa.list_(pa.string())),
    ("n_chars", pa.int64()), ("source", pa.string()), ("parser", pa.string()), ("text", pa.large_string()),
])
EXHIBIT_SCHEMA = pa.schema([
    ("accession", pa.string()), ("cik", pa.int64()), ("index_url", pa.string()), ("index_status", pa.int32()),
    ("exhibit_urls", pa.list_(pa.string())), ("exhibit_bytes", pa.int64()), ("exhibit_sha256", pa.list_(pa.string())),
    ("wanted", pa.list_(pa.string())), ("sections_found", pa.list_(pa.string())), ("requests", pa.int32()),
    ("fetched_at", pa.string()), ("error", pa.string()), ("parser", pa.string()),
])
DISK_FLOOR_BYTES = 40 * 1024 ** 3
_INDEX_ROW_RE = re.compile(rb"<tr[^>]*>(.*?)</tr>", re.I | re.S)
_HREF_RE = re.compile(rb'href="([^"]+)"', re.I)
_CELL_TEXT_RE = re.compile(rb"<td[^>]*>(.*?)</td>", re.I | re.S)


def raw_root() -> Path:
    return Path(os.environ.get("ATX_SEC_TEXT_ROOT", str(C.PACKAGE_ROOT / "data" / "raw" / SOURCE)))


def _quote_list(items: Iterable[str]) -> str:
    return ", ".join("'" + s.replace("'", "''") + "'" for s in items)


def select_filings(con: Any, start: dt.date = FILED_FROM) -> list[dict[str, Any]]:
    """Filings to land, one row per accession (co-registrant CIKs in ``ciks``), priority then filing date order.

    10-K family: every CIK of ``identity/link_table.parquet``; 20-F: CIKs with an ``ever_member`` line only.
    ``available_at`` is the latest of the accession's per-CIK clocks (conservative for co-registrants).
    """
    root = C.build_root()
    fp = (root / "sec_filings" / "filings.parquet").as_posix()
    lt = (root / "identity" / "link_table.parquet").as_posix()
    sql = f"""
    WITH l AS (SELECT cik, bool_or(coalesce(ever_member, false)) AS ever_member
               FROM read_parquet('{lt}') GROUP BY cik),
    f AS (SELECT f.*, l.ever_member FROM read_parquet('{fp}') f JOIN l USING (cik)
          WHERE f.filing_date >= DATE '{start.isoformat()}' AND coalesce(f.primary_document, '') <> ''
            AND (f.form IN ({_quote_list(FORMS_10K)}) OR (f.form IN ({_quote_list(FORMS_20F)}) AND l.ever_member)))
    SELECT accession, min(cik) AS cik, list(DISTINCT cik ORDER BY cik) AS ciks, min(form) AS form,
           min(filing_date) AS filing_date, min(report_date) AS report_date, max(acceptance_utc) AS acceptance_utc,
           min(acceptance_clock) AS acceptance_clock, max(vintage_risk) AS vintage_risk,
           max(available_at) AS available_at, min(primary_document) AS primary_document,
           CASE WHEN bool_or(ever_member) THEN 1 ELSE 2 END AS priority
    FROM f GROUP BY accession ORDER BY priority, filing_date, accession
    """
    cur = con.execute(sql)
    cols = [d[0] for d in cur.description]
    return [dict(zip(cols, r)) for r in cur.fetchall()]


def document_url(cik: int, accession: str, document: str) -> str:
    return f"{ARCHIVES}/{int(cik)}/{accession.replace('-', '')}/{document}"


class BudgetExhausted(Exception):
    """The lane's SEC request budget is used up: the run stops after the current batch."""


class CountingLimiter:
    """The host-wide SEC limiter, counting every attempt (per process and per thread) against a budget."""

    def __init__(self, inner: Any, budget: int) -> None:
        self.inner = inner
        self.lock_path = getattr(inner, "lock_path", "")
        self.budget = int(budget)
        self.count = 0
        self._lock = threading.Lock()
        self._local = threading.local()

    def acquire(self, label: str | None = None) -> float:
        with self._lock:
            if self.count >= self.budget:
                raise BudgetExhausted(f"SEC request budget of {self.budget} used")
            self.count += 1
        self._local.n = getattr(self._local, "n", 0) + 1
        return float(self.inner.acquire(label))

    def record_response(self, *args: Any, **kwargs: Any) -> None:
        self.inner.record_response(*args, **kwargs)

    def thread_count(self) -> int:
        return int(getattr(self._local, "n", 0))

    @property
    def exhausted(self) -> bool:
        return self.count >= self.budget


_SESSION_LOCAL = threading.local()
_BIG_PARSE_LOCK = threading.Lock()


def _session(limiter: CountingLimiter) -> Any:
    from .. import sec_http
    sess = getattr(_SESSION_LOCAL, "session", None)
    if sess is None or getattr(_SESSION_LOCAL, "limiter", None) is not limiter:
        sess = sec_http.sec_session(limiter=limiter)  # type: ignore[arg-type]
        _SESSION_LOCAL.session, _SESSION_LOCAL.limiter = sess, limiter
    return sess


def http_get(url: str, limiter: CountingLimiter, maximum: int = MAX_DOC_BYTES,
             timeout: float = 120.0) -> tuple[int, bytes, str | None, int, str]:
    """``(status, payload, error, requests, fetched_at)`` for one SEC GET (payload only on 200)."""
    import requests

    from .. import sec_http
    before = limiter.thread_count()
    status, payload, error = 0, b"", None
    try:
        resp = _session(limiter).get(url, timeout=timeout, stream=True)
        try:
            status = int(resp.status_code)
            if status == 200:
                payload = sec_http.read_bounded_response(resp, maximum)
        finally:
            resp.close()
    except ValueError as exc:
        error, payload = (str(exc)[:200] or type(exc).__name__), b""
    except requests.RequestException as exc:
        error, payload = type(exc).__name__, b""
    except MemoryError:  # the guard's job cap: retried on a later run
        error, payload = "memory_error", b""
    fetched_at = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
    return status, payload, error, limiter.thread_count() - before, fetched_at


def terminal(status: int, error: str | None) -> bool:
    """A final outcome (never refetched): 200, or a 4xx other than 403/429."""
    if status == 200:
        return error is None or error == "response_too_large"
    return 400 <= status < 500 and status not in (403, 429)


def parse_document(raw: bytes, form: str) -> tuple[str, dict[str, Section], dict[str, Any]]:
    """``(text, sections, diagnostics)``; big documents parse one at a time (memory)."""
    if len(raw) > BIG_DOC_BYTES:
        with _BIG_PARSE_LOCK:
            text = html_to_text(raw)
    else:
        text = html_to_text(raw)
    secs, diag = extract_sections(text, form)
    return text, secs, diag


def land_one(row: dict[str, Any], limiter: CountingLimiter) -> dict[str, Any]:
    """Fetch, parse and summarise one filing: ``{"receipt", "doc", "sections"}`` (doc/sections empty on failure)."""
    url = document_url(row["cik"], row["accession"], row["primary_document"])
    status, raw, error, n_req, fetched_at = http_get(url, limiter)
    sha = hashlib.sha256(raw).hexdigest() if raw else None
    receipt: dict[str, Any] = {
        "kind": "primary", "url": url, "accession": row["accession"], "cik": int(row["cik"]), "http_status": status,
        "bytes": len(raw), "sha256": sha, "fetched_at": fetched_at, "requests": n_req, "error": error,
        "terminal": terminal(status, error)}
    if status != 200 or error is not None:
        return {"receipt": receipt, "doc": None, "sections": []}
    try:
        text, secs, diag = parse_document(raw, row["form"])
    except Exception as exc:  # noqa: BLE001 - a parser failure is recorded, never fatal to the batch
        receipt["error"] = f"parse_error: {type(exc).__name__}: {exc}"[:300]
        receipt["terminal"] = False
        return {"receipt": receipt, "doc": None, "sections": []}
    del raw
    stats = TF.readability(text)
    base = {k: row[k] for k in ("accession", "cik", "ciks", "form", "filing_date", "report_date")}
    doc = {**base, "acceptance_utc": row["acceptance_utc"], "acceptance_clock": row["acceptance_clock"],
           "vintage_risk": row["vintage_risk"], "available_at": row["available_at"], "priority": row["priority"],
           "primary_document": row["primary_document"], "url": url, "http_status": status,
           "doc_bytes": receipt["bytes"], "sha256": sha, "fetched_at": fetched_at, "requests": n_req, "error": None,
           "text_chars": len(text), "doc_words": stats["words"], "doc_sentences": stats["sentences"],
           "doc_complex_words": stats["complex_words"], "doc_fog": stats["fog"], "headings": diag["headings"],
           "toc_headings": diag["toc_headings"], "running_lines": diag["running_lines"],
           "index_only": diag["index_only"], "sections_found": sorted(secs), "parser": PARSER_VERSION}
    sections = [{**base, "available_at": row["available_at"], "section": s.name, "item": s.item, "method": s.method,
                 "flags": s.flags, "n_chars": len(s.text), "source": "primary", "parser": PARSER_VERSION,
                 "text": s.text} for s in secs.values()]
    receipt["sections"] = sorted(secs)
    return {"receipt": receipt, "doc": doc, "sections": sections}


def _parts(kind: str) -> list[Path]:
    d = raw_root() / kind
    return sorted(d.glob("part-*.parquet")) if d.is_dir() else []


def _next_seq() -> int:
    seqs = [int(p.stem.split("-")[1]) for k in ("docs", "sections", "exhibits") for p in _parts(k)]
    return max(seqs, default=0) + 1


def _write_part(kind: str, seq: int, rows: list[dict[str, Any]], schema: pa.Schema) -> Path | None:
    if not rows:
        return None
    dest = raw_root() / kind / f"part-{seq:05d}.parquet"
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    table = pa.Table.from_pylist(rows, schema=schema)
    pq.write_table(table, tmp, compression="zstd", compression_level=PARQUET_ZSTD_LEVEL, row_group_size=4096)
    os.replace(tmp, dest)
    return dest


def receipts_path() -> Path:
    return raw_root() / "receipts.jsonl"


def append_receipts(lines: list[dict[str, Any]]) -> None:
    """Append receipts (fsynced) as filings complete, before the batch's parts: a kill can only cause a refetch."""
    if not lines:
        return
    path = receipts_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        for rec in lines:
            fh.write(json.dumps(rec, default=str, sort_keys=True) + "\n")
        fh.flush()
        os.fsync(fh.fileno())


def read_receipts() -> list[dict[str, Any]]:
    path = receipts_path()
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            try:
                out.append(json.loads(line))
            except ValueError:
                continue  # a line torn by a kill
    return out


def _column(kind: str, column: str) -> set[Any]:
    out: set[Any] = set()
    for p in _parts(kind):
        out.update(pq.read_table(p, columns=[column]).column(column).to_pylist())
    return out


def landing_state() -> dict[str, Any]:
    """Accessions landed (docs parts), terminal failures, and SEC requests used (every receipt line)."""
    receipts = read_receipts()
    landed = _column("docs", "accession")
    failed = {r["accession"] for r in receipts if r.get("kind") == "primary" and r.get("terminal")
              and (r.get("http_status") != 200 or r.get("error"))}
    return {"landed": landed, "failed": failed - landed,
            "requests_used": sum(int(r.get("requests") or 0) for r in receipts), "receipts": len(receipts)}


def disk_free_bytes() -> int:
    import shutil
    return shutil.disk_usage(raw_root().anchor or "C:/").free


def _chunks(items: list[Any], n: int) -> Iterator[list[Any]]:
    for i in range(0, len(items), n):
        yield items[i:i + n]


def _run_batches(todo: list[dict[str, Any]], worker: Any, limiter: CountingLimiter, threads: int,
                 extra_kind: str, extra_schema: pa.Schema, extra_key: str) -> dict[str, Any]:
    """Run ``worker`` over ``todo`` in batches: receipts first, then the sections part and the ``extra_kind`` part."""
    summary: dict[str, Any] = {"todo": len(todo), "batches": 0, "ok": 0, "failed": 0, "sections": 0, "stopped": None}
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=threads) as ex:
        for chunk in _chunks(todo, BATCH_FILINGS):
            if disk_free_bytes() < DISK_FLOOR_BYTES:
                summary["stopped"] = "disk_floor"
                break
            results: list[dict[str, Any]] = []
            stop = None
            for fut in as_completed([ex.submit(worker, r, limiter) for r in chunk]):
                try:
                    res = fut.result()
                except BudgetExhausted:
                    stop = "budget"
                    continue
                results.append(res)
                # receipts as each filing completes: a killed batch loses no request count (its filings refetch)
                append_receipts(res["receipts"] if "receipts" in res else [res["receipt"]])
            results.sort(key=lambda r: r["receipt"]["accession"] if "receipt" in r else r["receipts"][0]["accession"])
            seq = _next_seq()
            _write_part("sections", seq, [s for r in results for s in r["sections"]], SECTION_SCHEMA)
            _write_part(extra_kind, seq, [r[extra_key] for r in results if r[extra_key] is not None], extra_schema)
            summary["batches"] += 1
            summary["ok"] += sum(r[extra_key] is not None for r in results)
            summary["failed"] += sum(r[extra_key] is None for r in results)
            summary["sections"] += sum(len(r["sections"]) for r in results)
            del results
            gc.collect()
            pa.default_memory_pool().release_unused()  # keep the guard's committed-memory cap honest
            el = time.time() - t0
            print(json.dumps({"batch": summary["batches"], "ok": summary["ok"], "failed": summary["failed"],
                              "requests": limiter.count, "elapsed_s": round(el, 1),
                              "req_per_s": round(limiter.count / el, 2) if el else None,
                              "last_filing_date": str(chunk[-1]["filing_date"]),
                              "disk_free_gb": round(disk_free_bytes() / 1024 ** 3, 1)}), flush=True)
            if stop or limiter.exhausted:
                summary["stopped"] = "budget"
                break
    summary["requests"] = limiter.count
    summary["elapsed_s"] = round(time.time() - t0, 1)
    return summary


def incomplete_older_parser() -> set[str]:
    """Landed accessions parsed by an older parser that found fewer than all three sections (re-landed)."""
    out: set[str] = set()
    for p in _parts("docs"):
        for r in pq.read_table(p, columns=["accession", "parser", "sections_found"]).to_pylist():
            if r["parser"] != PARSER_VERSION and len(r["sections_found"] or []) < 3:
                out.add(r["accession"])
    for p in _parts("docs"):  # already re-landed by the current parser
        for r in pq.read_table(p, columns=["accession", "parser"]).to_pylist():
            if r["parser"] == PARSER_VERSION:
                out.discard(r["accession"])
    return out


def run_fetch(max_requests: int, threads: int, limit: int | None, max_priority: int,
              reland: bool = False) -> dict[str, Any]:
    from .. import sec_http
    con = C.connect(memory="150MB", threads=1)
    rows = [r for r in select_filings(con) if r["priority"] <= max_priority]
    con.close()
    state = landing_state()
    skip = state["landed"] | state["failed"]
    if reland:
        skip -= incomplete_older_parser()
    todo = [r for r in rows if r["accession"] not in skip]
    if limit is not None:
        todo = todo[:limit]
    budget = max(0, min(max_requests, REQUEST_BUDGET - state["requests_used"]))
    limiter = CountingLimiter(sec_http.default_sec_limiter(), budget)
    out = _run_batches(todo, land_one, limiter, threads, "docs", DOC_SCHEMA, "doc")
    return {"selected": len(rows), "already": len(skip), "budget": budget,
            "requests_used_before": state["requests_used"], **out}


# ---------------------------------------------------------------------------------------------------------
# EX-13 (annual report exhibit) for sections incorporated by reference
# ---------------------------------------------------------------------------------------------------------


def exhibit_links(index_html: bytes, prefix: str = "EX-13") -> list[str]:
    """Absolute URLs of the documents whose Type starts with ``prefix`` on an EDGAR ``-index.htm`` page."""
    out: list[str] = []
    for row in _INDEX_ROW_RE.findall(index_html):
        cells = [re.sub(rb"<[^>]+>", b"", c).strip().upper() for c in _CELL_TEXT_RE.findall(row)]
        if not any(c.startswith(prefix.encode()) for c in cells):
            continue
        m = _HREF_RE.search(row)
        if not m:
            continue
        href = m.group(1).decode("ascii", "replace")
        if href.startswith("/ix?doc="):
            href = href[len("/ix?doc="):]
        if not href.lower().endswith((".htm", ".html", ".txt")):
            continue
        out.append(href if href.startswith("http") else f"https://www.sec.gov{href}")
    return out


def extract_titled(text: str, wanted: Iterable[str]) -> dict[str, Section]:
    """Sections of an annual-report exhibit (no item numbers) located by their title lines (``method='title'``)."""
    lines = text.split("\n")
    offs = _line_offsets(lines)
    heads = find_items(lines, "10-K")
    running = running_lines(lines)
    out: dict[str, Section] = {}
    for name in wanted:
        got = _best_title(lines, heads, name, "10-K", offs)
        if got is None:
            continue
        s, e, eof = got
        sec = Section(name, SECTION_ITEMS["10-K"][name], s, e, "title", eof, flags=["ex13"] + (["eof"] if eof else []))
        body = [ln for ln in lines[s + 1:e] if _digit_key(ln) not in running]
        sec.text = "\n".join([lines[s], *body])
        if len(sec.text) >= SHORT_SECTION_CHARS:
            out[name] = sec
    return out


def exhibit_candidates() -> list[dict[str, Any]]:
    """10-K family filings with a section flagged ``by_reference`` and no EX-13 attempt yet."""
    want: dict[str, set[str]] = {}
    meta: dict[str, dict[str, Any]] = {}
    for p in _parts("sections"):
        t = pq.read_table(p, columns=["accession", "cik", "ciks", "form", "filing_date", "report_date",
                                      "available_at", "section", "flags", "source"])
        for r in t.to_pylist():
            if r["source"] == "primary" and "by_reference" in (r["flags"] or []) and form_family(r["form"]) != "20-F":
                want.setdefault(r["accession"], set()).add(r["section"])
                meta[r["accession"]] = r
    done = _column("exhibits", "accession")
    failed = {r["accession"] for r in read_receipts() if r.get("kind") == "index" and r.get("terminal")
              and r.get("http_status") != 200}
    out = []
    for acc in sorted(want):
        if acc in done or acc in failed:
            continue
        out.append({**{k: meta[acc][k] for k in ("accession", "cik", "ciks", "form", "filing_date", "report_date",
                                                   "available_at")}, "wanted": sorted(want[acc])})
    return out


def land_exhibit(row: dict[str, Any], limiter: CountingLimiter) -> dict[str, Any]:
    """Filing index -> EX-13 documents (at most 4) -> the wanted sections by title (``source = 'ex13'``)."""
    acc = row["accession"]
    index_url = f"{ARCHIVES}/{int(row['cik'])}/{acc.replace('-', '')}/{acc}-index.htm"
    status, raw, error, n_req, fetched_at = http_get(index_url, limiter, maximum=5_000_000)
    receipts = [{"kind": "index", "url": index_url, "accession": acc, "cik": int(row["cik"]), "http_status": status,
                 "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest() if raw else None,
                 "fetched_at": fetched_at, "requests": n_req, "error": error, "terminal": terminal(status, error)}]
    ex: dict[str, Any] = {
        "accession": acc, "cik": int(row["cik"]), "index_url": index_url, "index_status": status,
        "exhibit_urls": [], "exhibit_bytes": 0, "exhibit_sha256": [], "wanted": row["wanted"], "sections_found": [],
        "requests": n_req, "fetched_at": fetched_at, "error": error, "parser": PARSER_VERSION}
    if status != 200 or error is not None:
        return {"receipts": receipts, "exhibit": ex if terminal(status, error) else None, "sections": []}
    texts: list[str] = []
    for url in exhibit_links(raw)[:4]:
        s2, blob, e2, n2, f2 = http_get(url, limiter)
        sha2 = hashlib.sha256(blob).hexdigest() if blob else None
        receipts.append({"kind": "ex13", "url": url, "accession": acc, "cik": int(row["cik"]), "http_status": s2,
                         "bytes": len(blob), "sha256": sha2, "fetched_at": f2, "requests": n2, "error": e2,
                         "terminal": terminal(s2, e2)})
        ex["requests"] += n2
        if s2 == 200 and e2 is None:
            ex["exhibit_urls"].append(url)
            ex["exhibit_bytes"] += len(blob)
            ex["exhibit_sha256"].append(sha2)
            texts.append(html_to_text(blob))
    secs = extract_titled("\n".join(texts), row["wanted"]) if texts else {}
    ex["sections_found"] = sorted(secs)
    base = {k: row[k] for k in ("accession", "cik", "ciks", "form", "filing_date", "report_date")}
    sections = [{**base, "available_at": row["available_at"], "section": s.name, "item": s.item, "method": s.method,
                 "flags": s.flags, "n_chars": len(s.text), "source": "ex13", "parser": PARSER_VERSION,
                 "text": s.text} for s in secs.values()]
    return {"receipts": receipts, "exhibit": ex, "sections": sections}


def run_exhibits(max_requests: int, threads: int, limit: int | None) -> dict[str, Any]:
    from .. import sec_http
    todo = exhibit_candidates()
    if limit is not None:
        todo = todo[:limit]
    state = landing_state()
    budget = max(0, min(max_requests, REQUEST_BUDGET - state["requests_used"]))
    limiter = CountingLimiter(sec_http.default_sec_limiter(), budget)
    out = _run_batches(todo, land_exhibit, limiter, threads, "exhibits", EXHIBIT_SCHEMA, "exhibit")
    return {"budget": budget, "requests_used_before": state["requests_used"], **out}


def status() -> dict[str, Any]:
    con = C.connect(memory="200MB", threads=1)
    rows = select_filings(con)
    con.close()
    state = landing_state()
    by_pri: dict[int, dict[str, int]] = {}
    for r in rows:
        d = by_pri.setdefault(int(r["priority"]), {"selected": 0, "landed": 0, "failed": 0})
        d["selected"] += 1
        d["landed"] += r["accession"] in state["landed"]
        d["failed"] += r["accession"] in state["failed"]
    size = sum(p.stat().st_size for k in ("docs", "sections", "exhibits") for p in _parts(k))
    return {"by_priority": by_pri, "requests_used": state["requests_used"], "receipts": state["receipts"],
            "landing_bytes": size, "disk_free_gb": round(disk_free_bytes() / 1024 ** 3, 1)}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m atx_db.alpha_panel.filing_text",
                                 description="Land 10-K / 20-F Item sections (see the module docstring).")
    ap.add_argument("--phase", choices=("fetch", "exhibits", "status"), default="status")
    ap.add_argument("--max-requests", type=int, default=REQUEST_BUDGET)
    ap.add_argument("--threads", type=int, default=3)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--max-priority", type=int, default=2)
    ap.add_argument("--reland", action="store_true",
                    help="also re-land filings an older parser version left without all three sections")
    args = ap.parse_args(argv)
    if args.phase == "fetch":
        out = run_fetch(args.max_requests, args.threads, args.limit, args.max_priority, args.reland)
    elif args.phase == "exhibits":
        out = run_exhibits(args.max_requests, args.threads, args.limit)
    else:
        out = status()
    print(json.dumps(out, default=str, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
