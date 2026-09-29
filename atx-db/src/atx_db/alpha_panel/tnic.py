"""Stage ``classification_tnic`` (S7.2): Hoberg-Phillips TNIC-style text industries from 10-K Item 1, yearly.

Input: the Item 1 (Business) sections landed by ``filing_text`` (``data/raw/sec_text/sections``; 10-K family only,
``source`` primary, or the EX-13 exhibit when Item 1 was incorporated by reference), the landed filing clocks, and
``sec_filings/issuer_profile.parquet`` SIC codes (threshold calibration only). No network.

Method (Hoberg and Phillips 2016, "Text-Based Network Industries and Endogenous Product Differentiation", with a
documented simplification):

* Year ``Y`` = calendar year of the 10-K's filing date; one filing per (CIK, year): the latest original 10-K-family
  filing of the year with an Item 1 of at least ``MIN_WORDS`` distinct vocabulary words. Co-registrant filings
  count once per linked CIK; two CIKs of one accession are never peers of each other.
* Vocabulary: lowercase alphabetic words of 3+ letters, minus English function words, generic corporate words and
  geographic words (``STOPWORDS`` / ``GEO_WORDS``), minus words used by more than ``MAX_DF_SHARE`` of the previous
  year's Item 1s. HP keep nouns and proper nouns only (a part-of-speech dictionary); no dictionary is used here
  (simplification: the 25% document-frequency cap removes most common verbs and adjectives).
* Each filing is a binary word vector; the score of a pair is the cosine |A n B| / sqrt(|A| |B|).
* TNIC-3 peers: pairs scoring above the year's threshold ``tau_Y``. ``tau_Y`` is set on year ``Y-1`` so that the
  share of ``Y-1`` pairs above it equals the share of ``Y-1`` pairs with the same three-digit SIC (HP's calibration:
  TNIC-3 as coarse as SIC-3). The first year (``CALIBRATION_YEAR``) calibrates itself and is not published.

Point in time: the document-frequency filter and ``tau_Y`` use only year ``Y-1`` (complete before year ``Y``
starts) and a pair's score depends only on its two filings, so a pair is known at the later filing's acceptance:
``available_at`` = max(acceptance of the two 10-Ks). ``firms`` aggregates (peer count, total similarity) are
year-end values with ``available_at`` = the latest clock among the firm and its peers; ``n_peers_own_clock``
counts the peers already public at the firm's own acceptance. Calibration uses the 2026 SIC snapshot
(``vintage_risk = 'sic_snapshot_calibration'``).

Outputs: ``pairs/year=YYYY.parquet`` (both directions), ``firms.parquet``, ``years.parquet`` (threshold, SIC-3 pair
share, vocabulary, coverage), ``manifest.json``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import re
import sys
import time
from collections.abc import Iterable
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from . import common as C
from . import filing_text as FT

STAGE = "classification_tnic"
SCHEMA = "atx.alpha-panel.classification-tnic/v1"
CALIBRATION_YEAR = 2018
FIRST_YEAR = 2019
MAX_DF_SHARE = 0.25
MIN_WORDS = 50
HIST_BINS = 20_000
#: A firm with no TNIC-3 peer gets its FALLBACK_K nearest neighbours (score > 0), flagged ``peer_basis``.
FALLBACK_K = 5
FALLBACK_BASIS = f"nearest{FALLBACK_K}_fallback"
TOKEN_RE = re.compile(r"[a-z]{3,}")
RULE = "tnic-hp2016-binary-cosine-prior-year-df25-sic3-calibrated-v1"

STOPWORDS = frozenset("""
about above across after again against all almost along already also although always among amongst and another
any are around because been before being below between both but can cannot could did does doing done down during
each either else ever every few for from further had has have having her here hers herself him himself his how
however into its itself just least less many may might more most much must neither never nor not now off often
once one only other others otherwise our ours ourselves out over own per rather same several shall she should
since some such than that the their theirs them themselves then there thereby therefore these they this those
though through throughout thus together too toward towards under unless until upon very via was were what whatever
when whenever where whereas whether which while who whom whose why will with within without would yet you your
yours yourself also etc inc corp corporation company companies llc ltd plc limited incorporated holdings holding
group subsidiary subsidiaries million billion thousand percent approximately january february march april june july
august september october november december fiscal year years quarter annual report form item part section note
notes herein hereof thereof therein pursuant including include includes included generally primarily principal
two three four five six seven eight nine ten first second third new certain various based related use used using
""".split())
GEO_WORDS = frozenset("""
alabama alaska arizona arkansas california colorado connecticut delaware florida georgia hawaii idaho illinois
indiana iowa kansas kentucky louisiana maine maryland massachusetts michigan minnesota mississippi missouri montana
nebraska nevada hampshire jersey mexico york carolina dakota ohio oklahoma oregon pennsylvania rhode tennessee texas
utah vermont virginia washington wisconsin wyoming columbia puerto rico america american americas north south east
west northern southern eastern western midwest midwestern northeast northeastern southeast southeastern northwest
northwestern southwest southwestern central europe european asia asian africa african pacific atlantic latin
caribbean canada canadian china chinese japan japanese india indian germany german france french britain british
england english kingdom ireland irish scotland switzerland swiss netherlands dutch belgium italy italian spain
spanish portugal russia russian israel israeli korea korean taiwan singapore australia australian zealand brazil
brazilian argentina chile colombia peru hong kong luxembourg bermuda cayman islands island sweden swedish norway
norwegian denmark danish finland poland austria greece turkey egypt saudi arabia emirates dubai qatar indonesia
malaysia thailand vietnam philippines pakistan nigeria kenya chicago houston dallas boston atlanta angeles francisco
diego seattle denver miami phoenix philadelphia london paris tokyo beijing shanghai toronto vancouver montreal
""".split())
_DROP = STOPWORDS | GEO_WORDS


def vocabulary_words(text: str) -> list[str]:
    """Distinct vocabulary words of one Item 1 (sorted), before the document-frequency cap."""
    return sorted({w for w in TOKEN_RE.findall(text.lower()) if w not in _DROP})


def sic3_pair_share(sics: Iterable[str | None]) -> tuple[float | None, int]:
    """Share of firm pairs sharing a three-digit SIC among firms with a SIC; (share, firms counted)."""
    groups: dict[str, int] = {}
    for s in sics:
        s = (s or "").strip()
        if len(s) >= 3 and s[:3].isdigit() and s != "0000":
            groups[s[:3]] = groups.get(s[:3], 0) + 1
    n = sum(groups.values())
    if n < 2:
        return None, n
    same = sum(k * (k - 1) // 2 for k in groups.values())
    return same / (n * (n - 1) / 2), n


def threshold_from_hist(hist: np.ndarray, share: float) -> float:
    """Smallest bin edge t with share(score > t) <= ``share`` (``hist`` counts pairs over [0, 1] in HIST_BINS)."""
    total = float(hist.sum())
    if total <= 0:
        return 1.0
    above = np.cumsum(hist[::-1])[::-1]  # above[k] = pairs in bins k.. (score >= k / HIST_BINS)
    target = share * total
    k = int(np.argmax(above <= target)) if (above <= target).any() else len(hist)
    return k / HIST_BINS


# ---------------------------------------------------------------------------------------------------------
# Pairwise scores on bitsets (pure numpy)
# ---------------------------------------------------------------------------------------------------------


def bitsets(doc_ids: list[np.ndarray], vocab_size: int) -> np.ndarray:
    """(n_docs, ceil(V / 64)) uint64 bitsets from per-document word-id arrays."""
    width = max(1, (vocab_size + 63) // 64)
    bits = np.zeros((len(doc_ids), width), dtype=np.uint64)
    for i, ids in enumerate(doc_ids):
        if len(ids):
            ids = ids.astype(np.int64)
            np.bitwise_or.at(bits[i], ids >> 6, np.left_shift(np.uint64(1), (ids & 63).astype(np.uint64)))
    return bits


def pair_scores(bits: np.ndarray, sizes: np.ndarray, keep_above: float | None,
                exclude: np.ndarray | None = None,
                top: tuple[np.ndarray, np.ndarray] | None = None
                ) -> tuple[np.ndarray, list[tuple[int, int, float]], np.ndarray]:
    """Cosine of every pair i < j: (histogram over [0, 1], kept pairs above ``keep_above``, best score per doc).

    ``exclude`` (n_docs,) groups documents that may not pair (same accession): equal non-negative values are skipped.
    ``top`` = (scores, indices), two (n_docs, K) arrays filled with -1, receives each document's K nearest neighbours.
    """
    n = bits.shape[0]
    hist = np.zeros(HIST_BINS, dtype=np.int64)
    best = np.zeros(n, dtype=np.float64)
    kept: list[tuple[int, int, float]] = []
    sq = np.sqrt(sizes.astype(np.float64))
    for i in range(n - 1):
        inter = np.bitwise_count(bits[i + 1:] & bits[i]).sum(axis=1, dtype=np.int64)
        denom = sq[i] * sq[i + 1:]
        cos = np.where(denom > 0, inter / np.where(denom > 0, denom, 1.0), 0.0)
        if exclude is not None and exclude[i] >= 0:
            cos = np.where(exclude[i + 1:] == exclude[i], -1.0, cos)
        valid = cos >= 0
        hist += np.bincount(np.minimum((cos[valid] * HIST_BINS).astype(np.int64), HIST_BINS - 1),
                            minlength=HIST_BINS)
        if len(cos):
            best[i] = max(best[i], float(cos.max(initial=0.0)))
            np.maximum(best[i + 1:], cos, out=best[i + 1:])
        if keep_above is not None:
            for j in np.nonzero(cos > keep_above)[0]:
                kept.append((i, i + 1 + int(j), float(cos[j])))
        if top is not None and len(cos):
            _update_top(top, i, cos)
    return hist, kept, best


def _update_top(top: tuple[np.ndarray, np.ndarray], i: int, cos: np.ndarray) -> None:
    """Merge row ``i``'s scores against documents i+1.. into both sides' K-nearest lists."""
    tv, ti = top
    k = tv.shape[1]
    js = np.arange(i + 1, i + 1 + len(cos))
    allv = np.concatenate([tv[i], cos])
    alli = np.concatenate([ti[i], js])
    pick = np.argpartition(-allv, k - 1)[:k] if len(allv) > k else np.arange(len(allv))
    tv[i, :len(pick)], ti[i, :len(pick)] = allv[pick], alli[pick]
    sub_v, sub_i = tv[i + 1:], ti[i + 1:]
    slot = sub_v.argmin(axis=1)
    rows = np.arange(len(cos))
    upd = cos > sub_v[rows, slot]
    sub_v[rows[upd], slot[upd]] = cos[upd]
    sub_i[rows[upd], slot[upd]] = i


# ---------------------------------------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------------------------------------


def _tokens_path() -> Path:
    return C.build_root() / "_tmp" / "tnic_tokens.parquet"


TOKEN_SCHEMA = pa.schema([("accession", pa.string()), ("ciks", pa.list_(pa.int64())), ("filing_date", pa.date32()),
                          ("available_at", pa.timestamp("us")), ("source", pa.string()), ("ids", pa.list_(pa.int32()))])


def tokenize_pass() -> dict[str, Any]:
    """One pass over the landed Item 1 sections -> ``_tmp/tnic_tokens.parquet`` (word ids, one row per section).

    Word ids index a vocabulary built in landing order; only ids are kept (memory: a document is ~6 KB).
    """
    vocab: dict[str, int] = {}
    dest = _tokens_path()
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    rows = 0
    with pq.ParquetWriter(tmp, TOKEN_SCHEMA, compression="zstd") as writer:
        for part in FT._parts("sections"):
            t = pq.read_table(part, columns=["accession", "ciks", "form", "filing_date", "available_at", "section",
                                             "source", "flags", "text"])
            t = t.filter(pc.equal(t.column("section"), "business"))
            out = []
            for r in t.to_pylist():
                if FT.form_family(r["form"]) == "20-F" or "by_reference" in (r["flags"] or []):
                    continue
                words = vocabulary_words(r["text"])
                if len(words) < MIN_WORDS:
                    continue
                ids = [vocab.setdefault(w, len(vocab)) for w in words]
                out.append({"accession": r["accession"], "ciks": r["ciks"], "filing_date": r["filing_date"],
                            "available_at": r["available_at"], "source": r["source"], "ids": ids})
            if out:
                writer.write_table(pa.Table.from_pylist(out, schema=TOKEN_SCHEMA))
                rows += len(out)
            del t, out
    tmp.replace(dest)
    return {"token_rows": rows, "vocabulary": len(vocab)}


def _load_year(y: int) -> list[dict[str, Any]]:
    """The latest original 10-K-family Item 1 per (CIK, year ``y``); co-registrants once per CIK."""
    t = pq.read_table(_tokens_path(), filters=[("filing_date", ">=", dt.date(y, 1, 1)),
                                                ("filing_date", "<=", dt.date(y, 12, 31))])
    best: dict[int, dict[str, Any]] = {}
    for r in t.to_pylist():
        for cik in r["ciks"]:
            cur = best.get(int(cik))
            key = (r["filing_date"], r["source"] == "ex13", r["accession"])
            if cur is None or key > cur["_key"]:
                best[int(cik)] = {"cik": int(cik), "year": y, "accession": r["accession"],
                                  "filing_date": r["filing_date"], "available_at": r["available_at"],
                                  "source": r["source"], "ids": np.asarray(r["ids"], dtype=np.int32), "_key": key}
    return [best[c] for c in sorted(best)]


def _df_common(docs: list[dict[str, Any]]) -> np.ndarray:
    """Word ids used by more than MAX_DF_SHARE of ``docs`` (distinct accessions), sorted."""
    seen: dict[str, np.ndarray] = {}
    for d in docs:
        seen.setdefault(d["accession"], d["ids"])
    if not seen:
        return np.zeros(0, dtype=np.int32)
    ids, counts = np.unique(np.concatenate(list(seen.values())), return_counts=True)
    return ids[counts > MAX_DF_SHARE * len(seen)].astype(np.int32)


def _encode(docs: list[dict[str, Any]], common: np.ndarray) -> tuple[list[np.ndarray], np.ndarray, int]:
    """Compact ids over words shared by 2+ documents (after the cap), and each document's vocabulary size."""
    kept = [d["ids"][~np.isin(d["ids"], common, assume_unique=True)] for d in docs]
    sizes = np.array([len(k) for k in kept], dtype=np.int64)
    if not kept:
        return [], sizes, 0
    ids, counts = np.unique(np.concatenate(kept), return_counts=True)
    shared = ids[counts >= 2]
    out = []
    for k in kept:
        pos = np.searchsorted(shared, k)
        pos_c = np.minimum(pos, max(len(shared) - 1, 0))
        hit = (pos < len(shared)) & (shared[pos_c] == k) if len(shared) else np.zeros(len(k), dtype=bool)
        out.append(pos[hit].astype(np.int32))
    return out, sizes, len(shared)


def _sic_map() -> dict[int, str | None]:
    path = C.build_root() / "sec_filings" / "issuer_profile.parquet"
    t = pq.read_table(path, columns=["cik", "sic"])
    return dict(zip(t.column("cik").to_pylist(), t.column("sic").to_pylist()))


def _linked_filers(years: list[int]) -> dict[int, set[int]]:
    """Per year, linked CIKs (link validity overlapping the year) with an original 10-K-family filing that year."""
    root = C.build_root()
    con = C.connect(memory="250MB", threads=1)
    fp = (root / "sec_filings" / "filings.parquet").as_posix()
    lt = (root / "identity" / "link_table.parquet").as_posix()
    rows = con.execute(f"""
        SELECT DISTINCT year(f.filing_date) AS y, f.cik FROM read_parquet('{fp}') f
        WHERE f.form IN ({FT._quote_list(FT.FORMS_10K)}) AND year(f.filing_date) BETWEEN {min(years)} AND {max(years)}
          AND EXISTS (SELECT 1 FROM read_parquet('{lt}') l WHERE l.cik = f.cik
                      AND l.valid_from <= make_date(year(f.filing_date), 12, 31)
                      AND l.valid_to >= make_date(year(f.filing_date), 1, 1))
    """).fetchall()
    con.close()
    out: dict[int, set[int]] = {y: set() for y in years}
    for y, cik in rows:
        out[int(y)].add(int(cik))
    return out


def build(last_year: int) -> dict[str, Any]:
    t0 = time.time()
    tok = tokenize_pass()
    print(json.dumps(tok), flush=True)
    sic = _sic_map()
    years = list(range(CALIBRATION_YEAR, last_year + 1))
    linked = _linked_filers([y for y in years if y >= FIRST_YEAR] or [FIRST_YEAR])
    out_dir = C.stage_dir(STAGE)
    (out_dir / "pairs").mkdir(exist_ok=True)
    prev_hist: np.ndarray | None = None
    prev_share: float | None = None
    prev_docs: list[dict[str, Any]] | None = None
    year_rows: list[dict[str, Any]] = []
    firm_rows: list[dict[str, Any]] = []
    pair_counts: dict[str, int] = {}
    for y in years:
        docs = _load_year(y)
        if not docs:
            continue
        year_docs = docs
        common = _df_common(prev_docs if prev_docs else docs)
        ids, sizes, vocab = _encode(docs, common)
        valid = sizes >= MIN_WORDS
        docs = [d for d, v in zip(docs, valid) if v]
        ids = [a for a, v in zip(ids, valid) if v]
        sizes = sizes[valid]
        acc_index = {a: k for k, a in enumerate(sorted({d["accession"] for d in docs}))}
        exclude = np.array([acc_index[d["accession"]] for d in docs], dtype=np.int64)
        share, n_sic = sic3_pair_share(sic.get(d["cik"]) for d in docs)
        if prev_hist is None:
            # calibration year: score the year first, then set its own threshold (not published)
            hist, kept, best = pair_scores(bitsets(ids, vocab), sizes, None, exclude)
            tau = threshold_from_hist(hist, share or 0.0)
            calibrated_on = y
        else:
            tau = threshold_from_hist(prev_hist, prev_share or 0.0)
            calibrated_on = y - 1
            top = (np.full((len(docs), FALLBACK_K), -1.0), np.full((len(docs), FALLBACK_K), -1, dtype=np.int64))
            hist, kept, best = pair_scores(bitsets(ids, vocab), sizes, tau, exclude, top)
        yr = {"year": y, "docs": len(docs), "vocabulary": vocab, "common_words_removed": len(common),
              "threshold": tau, "calibrated_on": calibrated_on, "sic3_pair_share": share, "sic_firms": n_sic,
              "pairs_scored": int(hist.sum()), "published": y >= FIRST_YEAR and calibrated_on != y,
              "median_best_score": float(np.median(best)) if len(best) else None}
        if yr["published"]:
            peers: dict[int, list[tuple[int, float]]] = {i: [] for i in range(len(docs))}
            for i, j, s in kept:
                peers[i].append((j, s))
                peers[j].append((i, s))
            prow: list[dict[str, Any]] = []
            year_clock = max(d["available_at"] for d in docs)
            for i, d in enumerate(docs):
                own = d["available_at"]
                clocks = [max(own, docs[j]["available_at"]) for j, _ in peers[i]]
                for (j, s), clk in zip(peers[i], clocks):
                    prow.append({"year": y, "cik": d["cik"], "peer_cik": docs[j]["cik"], "score": s,
                                 "accession": d["accession"], "peer_accession": docs[j]["accession"],
                                 "available_at": clk, "threshold": tau, "peer_basis": "tnic3"})
                fallback: list[tuple[int, float]] = []
                if not peers[i]:  # flagged nearest neighbours; the set is final only at the year's last filing
                    order = np.argsort(-top[0][i])
                    fallback = [(int(top[1][i][o]), float(top[0][i][o])) for o in order if top[0][i][o] > 0]
                    for j, s in fallback:
                        prow.append({"year": y, "cik": d["cik"], "peer_cik": docs[j]["cik"], "score": s,
                                     "accession": d["accession"], "peer_accession": docs[j]["accession"],
                                     "available_at": year_clock, "threshold": tau, "peer_basis": FALLBACK_BASIS})
                firm_rows.append({
                    "year": y, "cik": d["cik"], "accession": d["accession"], "filing_date": d["filing_date"],
                    "filing_available_at": own, "vocabulary_words": int(sizes[i]), "n_peers": len(peers[i]),
                    "n_peers_own_clock": sum(1 for j, _ in peers[i] if docs[j]["available_at"] <= own),
                    "n_fallback_peers": len(fallback),
                    "total_similarity": float(sum(s for _, s in peers[i])), "best_score": float(best[i]),
                    "threshold": tau,
                    "available_at": max([own, *clocks]) if peers[i] else (year_clock if fallback else own),
                    "vintage_risk": "sic_snapshot_calibration"})
            dest = out_dir / "pairs" / f"year={y}.parquet"
            tmp = dest.with_name(dest.name + ".partial")
            schema = pa.schema([("year", pa.int16()), ("cik", pa.int64()), ("peer_cik", pa.int64()),
                                ("score", pa.float32()), ("accession", pa.string()), ("peer_accession", pa.string()),
                                ("available_at", pa.timestamp("us")), ("threshold", pa.float32()),
                                ("peer_basis", pa.string())])
            pq.write_table(pa.Table.from_pylist(sorted(prow, key=lambda r: (r["cik"], -r["score"])), schema=schema),
                           tmp, compression="zstd", row_group_size=32768)
            tmp.replace(dest)
            pair_counts[str(y)] = len(prow)
            filers = linked.get(y, set())
            with_peers = {r["cik"] for r in firm_rows if r["year"] == y and r["n_peers"] > 0}
            with_set = {r["cik"] for r in firm_rows if r["year"] == y and (r["n_peers"] or r["n_fallback_peers"])}
            with_doc = {d["cik"] for d in docs}
            yr.update({"linked_10k_filers": len(filers), "linked_with_item1": len(filers & with_doc),
                       "linked_with_peers": len(filers & with_peers), "linked_with_peer_set": len(filers & with_set),
                       "coverage_linked_with_peers": round(len(filers & with_peers) / len(filers), 4) if filers else None,
                       "coverage_linked_with_peer_set": round(len(filers & with_set) / len(filers), 4)
                       if filers else None,
                       "pair_rows": len(prow), "share_pairs_above": round(len(kept) / max(1, int(hist.sum())), 5)})
        year_rows.append(yr)
        print(json.dumps({k: v for k, v in yr.items()}, default=str), flush=True)
        prev_hist, prev_share, prev_docs = hist, share, year_docs
    firms_schema = pa.schema([
        ("year", pa.int16()), ("cik", pa.int64()), ("accession", pa.string()), ("filing_date", pa.date32()),
        ("filing_available_at", pa.timestamp("us")), ("vocabulary_words", pa.int32()), ("n_peers", pa.int32()),
        ("n_peers_own_clock", pa.int32()), ("n_fallback_peers", pa.int32()), ("total_similarity", pa.float64()),
        ("best_score", pa.float32()),
        ("threshold", pa.float32()), ("available_at", pa.timestamp("us")), ("vintage_risk", pa.string())])
    ftmp = out_dir / "firms.parquet.partial"
    pq.write_table(pa.Table.from_pylist(firm_rows, schema=firms_schema), ftmp, compression="zstd")
    ftmp.replace(out_dir / "firms.parquet")
    ytmp = out_dir / "years.parquet.partial"
    pq.write_table(pa.Table.from_pylist(year_rows), ytmp, compression="zstd")
    ytmp.replace(out_dir / "years.parquet")
    inputs = {}
    for name in ("sec_filings", "identity"):
        m = C.build_root() / name / ("manifest.json" if name == "sec_filings" else "link_table_manifest.json")
        if m.exists():
            inputs[f"{name}/{m.name}"] = C.sha256_file(m)
    landing = {"docs_parts": len(FT._parts("docs")), "sections_parts": len(FT._parts("sections")),
               "receipts_sha256": C.sha256_file(FT.receipts_path()) if FT.receipts_path().exists() else None}
    payload = {"rule": RULE, "method": __doc__, "years": year_rows, "pair_rows": pair_counts,
               "firm_rows": len(firm_rows), "input_manifests_sha256": inputs, "landing": landing,
               "clock": "pair available_at = max(acceptance of the two 10-Ks); firms available_at = latest clock "
                        "among the firm and its peers (year-end aggregate)",
               "staleness": "a year's peer set is replaced by the next year's; consumers use the latest year whose "
                            "pair is visible, at most 550 days old",
               "built_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
               "elapsed_s": round(time.time() - t0, 1)}
    C.write_stage_manifest(STAGE, SCHEMA, ("tnic", "filing_text"), payload)
    return {"years": year_rows, "firm_rows": len(firm_rows), "elapsed_s": round(time.time() - t0, 1)}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m atx_db.alpha_panel.tnic", description="Build classification_tnic.")
    ap.add_argument("--last-year", type=int, default=dt.date.today().year)
    args = ap.parse_args(argv)
    out = build(args.last_year)
    print(json.dumps({"firm_rows": out["firm_rows"], "elapsed_s": out["elapsed_s"]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
