"""Stage ``text`` (S7.3): Lazy Prices similarity, length and readability of 10-K / 20-F sections, per filing.

Input: the extracted sections landed by ``filing_text`` (``data/raw/sec_text/sections/part-*.parquet``: Item 1
Business, Item 1A Risk Factors, Item 7 MD&A of 10-K family filings; Item 4 / Item 3.D / Item 5 of 20-F) and the
per-document stats in ``data/raw/sec_text/docs/part-*.parquet``. No network.

Output ``text/features.parquet``: one row per landed filing (cik, accession), with for each section ``s`` in
``business``, ``risk`` (1A / 3.D) and ``mdna`` (7 / 5):

* ``{s}_chars``, ``{s}_words``, ``{s}_sentences``, ``{s}_fog`` (Gunning Fog = 0.4 x (words / sentences + 100 x
  complex / words), complex = 3+ syllables by the vowel-group heuristic ``syllables``), ``{s}_pct_complex``;
* Lazy Prices (Cohen, Malloy and Nguyen 2020) against the same issuer's prior annual filing of the same form family
  (the latest one filed ``PRIOR_MIN_DAYS``..``PRIOR_MAX_DAYS`` earlier with that section present):
  ``{s}_sim_cosine`` (term-frequency cosine), ``{s}_sim_jaccard`` (word-set Jaccard), ``{s}_sim_minedit``
  (sentence-sequence edit similarity 2M / (n + m), M = sentences matched by ``difflib``), ``{s}_words_chg``
  (log words / prior words); ``prior_accession`` and ``prior_gap_days`` say which filing was compared;
* whole-document size and readability measured at landing (the full document is not kept): ``doc_bytes``
  (Loughran-McDonald 2014 file-size readability proxy), ``doc_words``, ``doc_fog``.

Tokens: lowercase alphabetic words (internal ``'`` and ``-`` kept), no stemming, no stop-word removal (the
bag-of-words baseline of Lazy Prices). Sentences: text lines (block elements) are joined into paragraphs when a line
does not end a sentence and the next starts in lower case (``paragraphs``); paragraphs split after ``.``, ``!`` or
``?`` followed by a capital, digit or quote.

Clock: ``available_at`` = the filing's EDGAR acceptance (``sec_filings`` resolved clock); the prior filing is
always earlier, so every value is known at the filing's own clock. Staleness for consumers: 400 days.
Sentiment: the Loughran-McDonald dictionary is free for academic research only (commercial use needs a license
from its authors), so no sentiment shares are produced (see ``docs/ALPHA_PANEL_TEXT.md``).
"""

from __future__ import annotations

import argparse
import datetime as dt
import difflib
import hashlib
import json
import math
import re
import sys
import time
import zlib
from collections import Counter
from collections.abc import Iterable, Sequence
from functools import lru_cache
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C

STAGE = "text"
SCHEMA = "atx.alpha-panel.text-features/v1"
PRIOR_MIN_DAYS = 180
PRIOR_MAX_DAYS = 550
STALE_DAYS = 400
SECTIONS = ("business", "risk", "mdna")
#: Section-level measures are skipped below this many words (a stub such as "Not applicable." has no readability).
MIN_WORDS_READABILITY = 50
#: Sentences above this count are compared on their first MAX_EDIT_SENTENCES (edit similarity is quadratic).
MAX_EDIT_SENTENCES = 6000

WORD_RE = re.compile(r"[a-z](?:[a-z'\-]*[a-z])?")
SENT_SPLIT_RE = re.compile(r"(?<=[.!?])[\"')\]]?\s+(?=[\"'(\[]?[A-Z0-9])")
_SYL_TAIL_RE = re.compile(r"(?:[^laeiouy]es|ed|[^laeiouy]e)$")
_SYL_GROUP_RE = re.compile(r"[aeiouy]{1,2}")
_WS_RE = re.compile(r"\s+")
_PARA_END = frozenset(".!?:;\"')")

# ---------------------------------------------------------------------------------------------------------
# Tokens, sentences, readability (pure)
# ---------------------------------------------------------------------------------------------------------


def words(text: str) -> list[str]:
    """Lowercase alphabetic word tokens (``don't``, ``long-term`` stay whole)."""
    return WORD_RE.findall(text.lower())


def paragraphs(text: str) -> list[str]:
    """Lines joined into paragraphs: a line that does not end a sentence continues when the next line starts in
    lower case or with a digit (documents converted from print layout break every visual line)."""
    out: list[str] = []
    for line in text.split("\n"):
        line = line.strip()
        if not line:
            continue
        if out and out[-1][-1] not in _PARA_END and (line[0].islower() or line[0].isdigit()):
            out[-1] = f"{out[-1]} {line}"
        else:
            out.append(line)
    return out


def sentences(text: str) -> list[str]:
    """Sentences: paragraphs (``paragraphs``) split after terminal punctuation; empty pieces dropped."""
    out: list[str] = []
    for para in paragraphs(text):
        out.extend(s.strip() for s in SENT_SPLIT_RE.split(para) if s.strip())
    return out


@lru_cache(maxsize=200_000)
def syllables(word: str) -> int:
    """Vowel-group syllable estimate (silent final ``e`` / ``es`` / ``ed`` dropped); at least 1."""
    w = word.lower().strip("'-")
    if len(w) <= 3:
        return 1
    w = _SYL_TAIL_RE.sub("", w)
    if w.startswith("y"):
        w = w[1:]
    return max(1, len(_SYL_GROUP_RE.findall(w)))


def readability(text: str) -> dict[str, float | int | None]:
    """Word, sentence and complex-word counts plus Gunning Fog for one text."""
    toks = words(text)
    sents = sentences(text)
    n_w, n_s = len(toks), len(sents)
    complex_n = sum(1 for t in toks if syllables(t) >= 3)
    fog = None
    pct = None
    if n_w >= MIN_WORDS_READABILITY and n_s:
        pct = complex_n / n_w
        fog = 0.4 * (n_w / n_s + 100.0 * pct)
    return {"chars": len(text), "words": n_w, "sentences": n_s, "complex_words": complex_n,
            "fog": fog, "pct_complex": pct}


# ---------------------------------------------------------------------------------------------------------
# Lazy Prices similarity (pure)
# ---------------------------------------------------------------------------------------------------------


def cosine_tf(a: Counter[str], b: Counter[str]) -> float | None:
    """Cosine similarity of two term-frequency vectors; None when either is empty."""
    if not a or not b:
        return None
    if len(a) > len(b):
        a, b = b, a
    dot = sum(v * b.get(k, 0) for k, v in a.items())
    na = math.sqrt(sum(v * v for v in a.values()))
    nb = math.sqrt(sum(v * v for v in b.values()))
    return dot / (na * nb)


def jaccard(a: set[str] | Iterable[str], b: set[str] | Iterable[str]) -> float | None:
    """|A n B| / |A u B| of two word sets; None when both are empty."""
    sa, sb = set(a), set(b)
    union = len(sa | sb)
    return len(sa & sb) / union if union else None


def _sentence_key(s: str) -> str:
    return hashlib.blake2b(_WS_RE.sub(" ", s.lower()).encode("utf-8"), digest_size=8).hexdigest()


def edit_similarity(a: Sequence[str], b: Sequence[str]) -> float | None:
    """Sentence-sequence edit similarity 2M / (n + m) (M = sentences in ``difflib`` matching blocks).

    Equals 1 - (insertions + deletions) / (n + m) for the matched alignment: the sentence-level version of the
    Lazy Prices minimum-edit measure (a word-level edit distance is quadratic in 10-50k words per section).
    """
    if not a and not b:
        return None
    ka = [_sentence_key(s) for s in a[:MAX_EDIT_SENTENCES]]
    kb = [_sentence_key(s) for s in b[:MAX_EDIT_SENTENCES]]
    if not ka or not kb:
        return 0.0
    return difflib.SequenceMatcher(None, ka, kb, autojunk=False).ratio()


def section_profile(text: str | None) -> dict[str, Any] | None:
    """Everything the features need from one section text (tokens counted once)."""
    if text is None:
        return None
    toks = words(text)
    sents = sentences(text)
    tf = Counter(toks)
    complex_n = sum(n for t, n in tf.items() if syllables(t) >= 3)
    n_w, n_s = len(toks), len(sents)
    fog = pct = None
    if n_w >= MIN_WORDS_READABILITY and n_s:
        pct = complex_n / n_w
        fog = 0.4 * (n_w / n_s + 100.0 * pct)
    return {"chars": len(text), "words": n_w, "sentences": n_s, "fog": fog, "pct_complex": pct,
            "tf": tf, "sents": sents}


def compare(cur: dict[str, Any] | None, prev: dict[str, Any] | None) -> dict[str, float | None]:
    """Lazy Prices measures of a section against the prior filing's same section."""
    if cur is None or prev is None or not cur["words"] or not prev["words"]:
        return {"sim_cosine": None, "sim_jaccard": None, "sim_minedit": None, "words_chg": None}
    return {"sim_cosine": cosine_tf(cur["tf"], prev["tf"]),
            "sim_jaccard": jaccard(cur["tf"].keys(), prev["tf"].keys()),
            "sim_minedit": edit_similarity(cur["sents"], prev["sents"]),
            "words_chg": math.log(cur["words"] / prev["words"])}


# ---------------------------------------------------------------------------------------------------------
# Compact profiles (hashed term counts + sentence keys): what the build keeps per section
# ---------------------------------------------------------------------------------------------------------


def compact_profile(text: str) -> dict[str, Any]:
    """``section_profile`` with the term counts as sorted CRC-32 word keys + counts and the sentences as 64-bit
    keys (``_sentence_key``), so a section costs ~30 KB instead of its text."""
    p = section_profile(text)
    assert p is not None
    tf: dict[int, int] = {}
    for w, n in p["tf"].items():
        k = zlib.crc32(w.encode("utf-8"))
        tf[k] = tf.get(k, 0) + n
    keys = np.fromiter(sorted(tf), dtype=np.uint32, count=len(tf))
    counts = np.fromiter((tf[int(k)] for k in keys), dtype=np.uint32, count=len(keys))
    sent = np.array([int(_sentence_key(s), 16) for s in p["sents"][:MAX_EDIT_SENTENCES]], dtype=np.uint64)
    complex_n = None if p["pct_complex"] is None else round(p["pct_complex"] * p["words"])
    return {"chars": p["chars"], "words": p["words"], "sentences": p["sentences"], "complex_words": complex_n,
            "fog": p["fog"], "pct_complex": p["pct_complex"], "tf_keys": keys, "tf_counts": counts,
            "sent_keys": sent}


def compare_compact(cur: dict[str, Any] | None, prev: dict[str, Any] | None) -> dict[str, float | None]:
    """``compare`` on compact profiles (identical up to CRC-32 collisions)."""
    if cur is None or prev is None or not cur["words"] or not prev["words"]:
        return {"sim_cosine": None, "sim_jaccard": None, "sim_minedit": None, "words_chg": None}
    ka, ca = np.asarray(cur["tf_keys"], dtype=np.uint32), np.asarray(cur["tf_counts"], dtype=np.float64)
    kb, cb = np.asarray(prev["tf_keys"], dtype=np.uint32), np.asarray(prev["tf_counts"], dtype=np.float64)
    common, ia, ib = np.intersect1d(ka, kb, assume_unique=True, return_indices=True)
    cos = float(ca[ia] @ cb[ib]) / math.sqrt(float(ca @ ca) * float(cb @ cb))
    union = len(ka) + len(kb) - len(common)
    sa = [int(x) for x in cur["sent_keys"]]
    sb = [int(x) for x in prev["sent_keys"]]
    edit = difflib.SequenceMatcher(None, sa, sb, autojunk=False).ratio() if sa and sb else 0.0
    return {"sim_cosine": cos, "sim_jaccard": len(common) / union if union else None, "sim_minedit": edit,
            "words_chg": math.log(cur["words"] / prev["words"])}


# ---------------------------------------------------------------------------------------------------------
# Build: profile pass (per landed part) -> per-bucket prior matching -> text/features.parquet
# ---------------------------------------------------------------------------------------------------------

N_BUCKETS = 32
PROFILE_SCHEMA = pa.schema([
    ("accession", pa.string()), ("cik", pa.int64()), ("form", pa.string()), ("filing_date", pa.date32()),
    ("available_at", pa.timestamp("us")), ("section", pa.string()), ("source", pa.string()), ("method", pa.string()),
    ("flags", pa.list_(pa.string())), ("chars", pa.int64()), ("words", pa.int64()), ("sentences", pa.int64()),
    ("complex_words", pa.int64()), ("fog", pa.float64()), ("pct_complex", pa.float64()),
    ("tf_keys", pa.list_(pa.uint32())), ("tf_counts", pa.list_(pa.uint32())), ("sent_keys", pa.list_(pa.uint64())),
])


def profiles_dir() -> Path:
    return C.build_root() / "_tmp" / "text_profiles"


def profile_pass() -> dict[str, Any]:
    """Compact profile of every landed section, written per landed part into ``N_BUCKETS`` CIK buckets
    (``bucket = cik % N_BUCKETS``; a co-registrant section is written once per CIK). Resumable per part."""
    from . import filing_text as FT
    root = profiles_dir()
    done_dir = root / "_done"
    done_dir.mkdir(parents=True, exist_ok=True)
    stats = {"parts": 0, "skipped": 0, "rows": 0}
    for part in FT._parts("sections"):
        marker = done_dir / part.stem
        if marker.exists():
            stats["skipped"] += 1
            continue
        buckets: dict[int, list[dict[str, Any]]] = {}
        # small record batches: only a few section texts are Python strings at a time (guard cap)
        for r in (x for b in pq.ParquetFile(part).iter_batches(batch_size=16) for x in b.to_pylist()):
            prof = compact_profile(r["text"])
            for cik in r["ciks"]:
                buckets.setdefault(int(cik) % N_BUCKETS, []).append({
                    "accession": r["accession"], "cik": int(cik), "form": r["form"], "filing_date": r["filing_date"],
                    "available_at": r["available_at"], "section": r["section"], "source": r["source"],
                    "method": r["method"], "flags": r["flags"], **{k: prof[k] for k in (
                        "chars", "words", "sentences", "complex_words", "fog", "pct_complex")},
                    "tf_keys": prof["tf_keys"], "tf_counts": prof["tf_counts"], "sent_keys": prof["sent_keys"]})
        for b, rows in buckets.items():
            dest = root / f"bucket={b:02d}" / f"{part.stem}.parquet"
            dest.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_name(dest.name + ".partial")
            pq.write_table(_profiles_table(rows), tmp, compression="zstd")
            tmp.replace(dest)
            stats["rows"] += len(rows)
        marker.write_text("done", encoding="utf-8")
        stats["parts"] += 1
    return stats


_LIST_COLS = (("tf_keys", pa.uint32(), np.uint32), ("tf_counts", pa.uint32(), np.uint32),
              ("sent_keys", pa.uint64(), np.uint64))


def _profiles_table(rows: list[dict[str, Any]]) -> pa.Table:
    """Profile rows (numpy list values) -> Arrow table without per-element Python objects."""
    scalar = [f.name for f in PROFILE_SCHEMA if f.name not in {c for c, _, _ in _LIST_COLS}]
    arrays = [pa.array([r[c] for r in rows], type=PROFILE_SCHEMA.field(c).type) for c in scalar]
    for c, typ, np_t in _LIST_COLS:
        vals = [np.asarray(r[c], dtype=np_t) for r in rows]
        offsets = np.zeros(len(vals) + 1, dtype=np.int32)
        np.cumsum([len(v) for v in vals], out=offsets[1:])
        flat = np.concatenate(vals) if vals else np.zeros(0, dtype=np_t)
        arrays.append(pa.ListArray.from_arrays(pa.array(offsets), pa.array(flat, type=typ)))
    table = pa.Table.from_arrays(arrays, names=scalar + [c for c, _, _ in _LIST_COLS])
    return table.select([f.name for f in PROFILE_SCHEMA]).cast(PROFILE_SCHEMA)


def load_profiles(files: list[Path]) -> list[dict[str, Any]]:
    """Profile rows with the list columns as numpy views (zero-copy slices of the column buffers)."""
    out: list[dict[str, Any]] = []
    for f in files:
        t = pq.read_table(f).combine_chunks()
        scalar = t.drop([c for c, _, _ in _LIST_COLS]).to_pylist()
        lists = {}
        for c, _, np_t in _LIST_COLS:
            col = t.column(c).chunk(0) if t.column(c).num_chunks else pa.array([], type=PROFILE_SCHEMA.field(c).type)
            offs = col.offsets.to_numpy()
            vals = col.values.to_numpy(zero_copy_only=False).astype(np_t, copy=False)
            lists[c] = (offs, vals)
        for i, r in enumerate(scalar):
            for c, (offs, vals) in lists.items():
                r[c] = vals[offs[i]:offs[i + 1]]
            out.append(r)
    return out


def _family(form: str) -> str:
    return "20-F" if (form or "").upper().startswith("20-F") else "10-K"


def _pick(profiles: list[dict[str, Any]]) -> dict[str, Any]:
    """One profile per (cik, accession, section): the EX-13 exhibit over a by-reference stub, else the longest."""
    return max(profiles, key=lambda p: (p["source"] == "ex13", p["words"]))


def _filings_meta() -> list[dict[str, Any]]:
    """Landed filings (docs parts), one row per (cik, accession) with the whole-document stats."""
    from . import filing_text as FT
    cols = ["accession", "ciks", "form", "filing_date", "report_date", "acceptance_utc", "acceptance_clock",
            "vintage_risk", "available_at", "doc_bytes", "doc_words", "doc_sentences", "doc_complex_words", "doc_fog",
            "sections_found"]
    out: dict[tuple[int, str], dict[str, Any]] = {}
    for part in FT._parts("docs"):
        for r in pq.read_table(part, columns=cols).to_pylist():
            for cik in r["ciks"]:
                out[(int(cik), r["accession"])] = {**{k: r[k] for k in cols if k != "ciks"}, "cik": int(cik)}
    return list(out.values())


FEATURE_COLS = ("chars", "words", "sentences", "fog", "pct_complex")
SIM_COLS = ("sim_cosine", "sim_jaccard", "sim_minedit", "words_chg")


def _feature_schema() -> pa.Schema:
    fields = [("cik", pa.int64()), ("accession", pa.string()), ("form", pa.string()), ("filing_date", pa.date32()),
              ("report_date", pa.date32()), ("acceptance_utc", pa.timestamp("us")), ("acceptance_clock", pa.string()),
              ("vintage_risk", pa.string()), ("available_at", pa.timestamp("us")), ("prior_accession", pa.string()),
              ("prior_filing_date", pa.date32()), ("prior_gap_days", pa.int32()), ("doc_bytes", pa.int64()),
              ("doc_words", pa.int64()), ("doc_fog", pa.float64())]
    for s in SECTIONS:
        fields += [(f"{s}_source", pa.string()), (f"{s}_method", pa.string()), (f"{s}_chars", pa.int64()),
                   (f"{s}_words", pa.int64()), (f"{s}_sentences", pa.int64()), (f"{s}_fog", pa.float64()),
                   (f"{s}_pct_complex", pa.float64())]
        fields += [(f"{s}_{c}", pa.float64()) for c in SIM_COLS]
    fields += [("n_sections", pa.int8())]
    return pa.schema(fields)


def prior_filing(history: list[dict[str, Any]], cur: dict[str, Any]) -> dict[str, Any] | None:
    """The latest earlier filing of the same form family filed PRIOR_MIN_DAYS..PRIOR_MAX_DAYS before ``cur``."""
    fam = _family(cur["form"])
    best = None
    for h in history:
        gap = (cur["filing_date"] - h["filing_date"]).days
        if _family(h["form"]) == fam and PRIOR_MIN_DAYS <= gap <= PRIOR_MAX_DAYS and h["accession"] != cur["accession"]:
            if best is None or (h["filing_date"], h["accession"]) > (best["filing_date"], best["accession"]):
                best = h
    return best


def build_features() -> dict[str, Any]:
    t0 = time.time()
    meta = _filings_meta()
    by_bucket: dict[int, list[dict[str, Any]]] = {}
    for m in meta:
        by_bucket.setdefault(m["cik"] % N_BUCKETS, []).append(m)
    rows: list[dict[str, Any]] = []
    n_cmp = 0
    for b in range(N_BUCKETS):
        files = sorted((profiles_dir() / f"bucket={b:02d}").glob("*.parquet"))
        prof: dict[tuple[int, str, str], list[dict[str, Any]]] = {}
        for r in load_profiles(files):
            prof.setdefault((r["cik"], r["accession"], r["section"]), []).append(r)
        picked = {k: _pick(v) for k, v in prof.items()}
        del prof
        filings = sorted(by_bucket.get(b, []), key=lambda m: (m["cik"], m["filing_date"], m["accession"]))
        by_cik: dict[int, list[dict[str, Any]]] = {}
        for m in filings:
            by_cik.setdefault(m["cik"], []).append(m)
        for cik, hist in by_cik.items():
            for m in hist:
                prior = prior_filing(hist, m)
                row: dict[str, Any] = {k: m[k] for k in ("cik", "accession", "form", "filing_date", "report_date",
                                                          "acceptance_utc", "acceptance_clock", "vintage_risk",
                                                          "available_at", "doc_bytes", "doc_words", "doc_fog")}
                row.update({"prior_accession": prior["accession"] if prior else None,
                            "prior_filing_date": prior["filing_date"] if prior else None,
                            "prior_gap_days": (m["filing_date"] - prior["filing_date"]).days if prior else None})
                n_sec = 0
                for s in SECTIONS:
                    cur = picked.get((cik, m["accession"], s))
                    prev = picked.get((cik, prior["accession"], s)) if prior else None
                    row[f"{s}_source"] = cur["source"] if cur else None
                    row[f"{s}_method"] = cur["method"] if cur else None
                    for c in FEATURE_COLS:
                        row[f"{s}_{c}"] = cur[c] if cur else None
                    cmp = compare_compact(cur, prev)
                    n_cmp += cmp["sim_cosine"] is not None
                    for c in SIM_COLS:
                        row[f"{s}_{c}"] = cmp[c]
                    n_sec += cur is not None
                row["n_sections"] = n_sec
                rows.append(row)
        del picked
        print(json.dumps({"bucket": b, "rows": len(rows), "comparisons": n_cmp,
                          "elapsed_s": round(time.time() - t0, 1)}), flush=True)
    out_dir = C.stage_dir(STAGE)
    rows.sort(key=lambda r: (r["cik"], r["filing_date"], r["accession"]))
    dest = out_dir / "features.parquet"
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(pa.Table.from_pylist(rows, schema=_feature_schema()), tmp, compression="zstd",
                   row_group_size=32768)
    tmp.replace(dest)
    return {"rows": len(rows), "comparisons": n_cmp, "elapsed_s": round(time.time() - t0, 1)}


# ---------------------------------------------------------------------------------------------------------
# Coverage over member cells (as-of join by the acceptance clock, STALE_DAYS staleness)
# ---------------------------------------------------------------------------------------------------------

COVERAGE_MEASURES = {
    "any_section": "n_sections > 0",
    "risk_words": "risk_words IS NOT NULL",
    "mdna_words": "mdna_words IS NOT NULL",
    "any_similarity": "coalesce(risk_sim_cosine, mdna_sim_cosine, business_sim_cosine) IS NOT NULL",
    "risk_sim_cosine": "risk_sim_cosine IS NOT NULL",
    "mdna_sim_cosine": "mdna_sim_cosine IS NOT NULL",
    "doc_fog": "doc_fog IS NOT NULL",
}


def _cells_sql(year: int, basis: str) -> str | None:
    """Member cells (session_date, security_id, cik) of ``year``: ``member_equity`` rows of the published panel, or
    ``panel_member`` members with the link-table CIK known at the session (strict / name: ``available_at``, backfill:
    ``evidence_at``, before the session's 22:00 UTC mark)."""
    root = C.build_root()
    if basis == "panel_member_equity":
        files = sorted((root / "panel" / f"year={year}").glob("panel-*.parquet"))
        if not files:
            return None
        glob = (root / "panel" / f"year={year}" / "panel-*.parquet").as_posix()
        return (f"SELECT session_date, security_id, cik FROM read_parquet('{glob}', union_by_name = true) "
                f"WHERE member_equity")
    member = root / "_tmp" / "panel_member" / f"year={year}.parquet"
    if not member.exists():
        return None
    lt = (root / "identity" / "link_table.parquet").as_posix()
    return f"""
        SELECT m.session_date, m.security_id,
               min(l.cik) AS cik
        FROM read_parquet('{member.as_posix()}') m
        LEFT JOIN read_parquet('{lt}') l
          ON l.security_id = m.security_id AND m.session_date BETWEEN l.valid_from AND l.valid_to
         AND (CASE WHEN l.link_tier = 'backfill' THEN l.evidence_at ELSE l.available_at END)
             <= m.session_date + INTERVAL 22 HOUR
        WHERE m.member AND year(m.session_date) = {year}
        GROUP BY ALL"""


def member_coverage(years: Iterable[int], bases: Sequence[str] = ("panel_member_equity", "panel_member_linked"),
                    features: Path | None = None) -> dict[str, dict[str, dict[str, Any]]]:
    """Share of member cells with a visible (``available_at`` < 22:00 UTC of the previous session) and fresh
    (<= STALE_DAYS) text-feature row of the cell's CIK, per measure, basis and year. A year is measured on the first
    basis that has cells for it (the panel's ``member_equity``, else ``panel_member`` + link table)."""
    feats = (features or (C.stage_dir(STAGE) / "features.parquet")).as_posix()
    con = C.connect(memory="250MB", threads=1)
    con.execute(f"""CREATE TEMP TABLE cal AS SELECT session_date,
                    lag(session_date) OVER (ORDER BY session_date) AS prev_session
                    FROM read_parquet('{C.calendar_path().as_posix()}')""")
    con.execute(f"CREATE TEMP TABLE f AS SELECT * FROM read_parquet('{feats}') WHERE available_at IS NOT NULL")
    out: dict[str, dict[str, dict[str, Any]]] = {}
    done_years: set[int] = set()
    for basis in bases:
        for y in years:
            sql = None if y in done_years else _cells_sql(y, basis)
            if sql is None:
                continue
            done_years.add(y)
            meas = ", ".join(f"count(*) FILTER (WHERE fresh AND {cond}) AS {name}"
                             for name, cond in COVERAGE_MEASURES.items())
            r = con.execute(f"""
                WITH c AS (SELECT x.*, coalesce(cal.prev_session, x.session_date - 1) + INTERVAL 22 HOUR AS cutoff
                           FROM ({sql}) x LEFT JOIN cal USING (session_date)),
                j AS (SELECT c.session_date, c.cik AS cell_cik, f.* EXCLUDE (cik),
                             f.available_at IS NOT NULL
                             AND date_diff('day', CAST(f.available_at AS DATE), c.session_date) <= {STALE_DAYS} AS fresh
                      FROM c ASOF LEFT JOIN f ON c.cik = f.cik AND c.cutoff > f.available_at)
                SELECT count(*) AS cells, count(*) FILTER (WHERE cell_cik IS NOT NULL) AS linked, {meas} FROM j
            """).fetchone()
            cols = ["cells", "linked", *COVERAGE_MEASURES]
            vals = dict(zip(cols, r))
            cells = vals["cells"] or 0
            out.setdefault(basis, {})[str(y)] = {
                "cells": cells, "linked_share": round(vals["linked"] / cells, 4) if cells else None,
                **{m: round(vals[m] / cells, 4) if cells else None for m in COVERAGE_MEASURES}}
            print(json.dumps({"basis": basis, "year": y, **out[basis][str(y)]}), flush=True)
    con.close()
    return out


def publish(coverage: dict[str, Any], build: dict[str, Any], profile: dict[str, Any]) -> Path:
    from . import filing_text as FT
    inputs = {}
    for rel in ("sec_filings/manifest.json", "identity/link_table_manifest.json", "panel/manifest.json"):
        m = C.build_root() / rel
        if m.exists():
            inputs[rel] = C.sha256_file(m)
    landing = {"docs_parts": len(FT._parts("docs")), "sections_parts": len(FT._parts("sections")),
               "exhibits_parts": len(FT._parts("exhibits")),
               "receipts_sha256": C.sha256_file(FT.receipts_path()) if FT.receipts_path().exists() else None,
               "parser": FT.PARSER_VERSION}
    payload = {"method": __doc__, "build": build, "profile_pass": profile, "coverage": coverage,
               "input_manifests_sha256": inputs, "landing": landing,
               "clock": "available_at = the filing's EDGAR acceptance (sec_filings acceptance-per-file-clock-v1); "
                        "the compared prior filing is always earlier",
               "staleness_days": STALE_DAYS,
               "sentiment": "not produced: the Loughran-McDonald dictionary is free for academic research only; "
                            "commercial use requires a license from its authors (sraf.nd.edu, checked 2026-09-29)",
               "built_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")}
    return C.write_stage_manifest(STAGE, SCHEMA, ("text_features", "filing_text"), payload)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m atx_db.alpha_panel.text_features",
                                 description="Build the text stage (profiles -> features -> coverage -> manifest).")
    ap.add_argument("--phase", choices=("all", "profile", "build", "coverage"), default="all")
    ap.add_argument("--years", default="2019-2026")
    args = ap.parse_args(argv)
    lo, hi = (int(x) for x in args.years.split("-"))
    years = list(range(lo, hi + 1))
    prof = build = cov = None
    if args.phase in ("all", "profile"):
        prof = profile_pass()
        print(json.dumps({"profile": prof}), flush=True)
    if args.phase in ("all", "build"):
        build = build_features()
        print(json.dumps({"build": build}), flush=True)
    if args.phase in ("all", "coverage"):
        cov = member_coverage(years)
    if args.phase == "all":
        print(publish(cov or {}, build or {}, prof or {}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
