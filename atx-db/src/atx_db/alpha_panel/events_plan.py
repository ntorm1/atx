"""Fetch plans of the ``events`` lane: which SEC documents to request, in priority order, within the lane cap.

Every plan reads the full-text-search hits (``_tmp/events/fts_<qid>.parquet``), keeps 2019+ documents of issuers
linked to an ever-``member_equity`` line (``identity/link_table.parquet``), drops documents already landed (the v2
earnings-release exhibits, by accession) or already fetched (the ``sec_events`` ledger), and writes one URL per
line to ``_tmp/events/fetch_<plan>.txt`` in priority order. Pure pyarrow / Python (no DuckDB; ruling C-1).

* ``guidance``: EX-99 exhibits of 8-Ks with item 2.02 (then 7.01) hitting ``guidance OR outlook``, newest first; one
  exhibit per filing (EX-99.1 first, else the lowest-numbered EX-99).
* ``buyback``: 8-K main documents and EX-99 exhibits hitting the repurchase-program phrases: filings without item
  2.02 first (dedicated announcements), then 2.02 exhibits not landed and not in the guidance probe / follow plans.
* ``merger``: 8-K main documents hitting "converted into the right to receive", then EX-99 releases of filings
  without a main-document hit; EX-2.1 agreements are never fetched (large).
* ``tender``: per subject company and form (SC TO-T, SC 14D9), the first document hitting "net to the seller" (the
  schedule's main document first).
* ``fpi``: 6-K EX-99 exhibits (else the main document) hitting the period-results phrases
  (``fpi_period_results``), one per filing, at most ``FPI_PER_YEAR`` filings per issuer and calendar year.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
from collections.abc import Callable, Iterator
from typing import Any

import pyarrow.parquet as pq

from . import common as C
from . import events_sources as ES

START = dt.date(2019, 1, 1)
FPI_PER_YEAR = 4
_EX = re.compile(r"EX-99\.?(\d+)?", re.IGNORECASE)


def member_ciks() -> set[int]:
    t = pq.read_table(C.build_root() / "identity" / "link_table.parquet", columns=["cik", "ever_member"])
    return {c for c, m in zip(t.column("cik").to_pylist(), t.column("ever_member").to_pylist()) if m and c}


def _hits(qid: str, keep: Callable[[dict[str, Any]], bool]) -> Iterator[dict[str, Any]]:
    path = ES.fts_hits_path(qid)
    if not path.exists():
        return
    cols = ["adsh", "file", "file_type", "form", "items", "ciks", "file_date"]
    for batch in pq.ParquetFile(path).iter_batches(batch_size=20000, columns=cols):
        for r in batch.to_pylist():
            if r["file_date"] is not None and r["file_date"] >= START and keep(r):
                yield r


def guider_status(kind: str) -> set[int]:
    """``landed_recent``: CIKs with a landed v2 release filed 2023+; ``guiders``: CIKs with at least one guidance row
    in ``_tmp/events/guidance_rows.parquet`` (the parse of every available release)."""
    if kind == "landed_recent":
        ann = pq.read_table(C.build_root() / "earnings_calendar" / "announcements.parquet",
                            columns=["accession", "cik", "filing_date"])
        landed = set(ES.landed_release_index())
        return {c for a, c, d in zip(*(ann.column(x).to_pylist() for x in ("accession", "cik", "filing_date")))
                if a in landed and d is not None and d >= dt.date(2023, 1, 1)}
    rows = C.build_root() / "_tmp" / "events" / "guidance_rows.parquet"
    if not rows.exists():
        return set()
    return set(pq.read_table(rows, columns=["cik"]).column("cik").to_pylist())


def _ex_no(ft: str | None) -> int:
    m = _EX.match(ft or "")
    return int(m.group(1)) if m and m.group(1) else 0


def plan(name: str) -> dict[str, Any]:
    mem = member_ciks()
    landed = {a.replace("-", "") for a in ES.landed_release_index()}
    fetched = {u for u, _ in ES.fetched_documents()}

    def url(r: dict[str, Any]) -> str:
        return ES.document_url(r["ciks"][0], r["adsh"], r["file"])

    def base(r: dict[str, Any]) -> bool:
        return bool(r["ciks"]) and any(c in mem for c in r["ciks"]) and url(r) not in fetched

    picked: list[tuple[tuple[Any, ...], str, dt.date]] = []
    if name in ("guidance_probe", "guidance_follow"):
        # probe: the two newest 2.02 releases of every member issuer without a landed 2023+ release; follow: every
        # 2.02 release of the issuers the probe (or the landed releases) classified as guiders, newest first
        landed_recent = {c for c in guider_status("landed_recent")}
        guiders = guider_status("guiders") if name == "guidance_follow" else set()
        per_cik: dict[int, list[tuple[dt.date, str]]] = {}
        best_g: dict[str, tuple[tuple[Any, ...], dict[str, Any]]] = {}
        for r in _hits("guidance", lambda r: base(r) and (r["file_type"] or "").upper().startswith("EX-99")
                       and "2.02" in (r["items"] or [])):
            acc = r["adsh"].replace("-", "")
            if acc in landed:
                continue
            n = _ex_no(r["file_type"])
            rank = (n != 1, n, r["file"])
            if acc not in best_g or rank < best_g[acc][0]:
                best_g[acc] = (rank, r)
        for _rank, r in best_g.values():
            cik = next(c for c in r["ciks"] if c in mem)
            per_cik.setdefault(cik, []).append((r["file_date"], url(r)))
        for cik, lst in per_cik.items():
            lst.sort(reverse=True)
            if name == "guidance_probe":
                if cik in landed_recent:
                    continue
                for d, u in lst[:2]:
                    picked.append(((-d.toordinal(), u), u, d))
            elif cik in guiders:
                for d, u in lst:
                    picked.append(((-d.toordinal(), u), u, d))
    elif name == "guidance":
        best: dict[str, tuple[tuple[Any, ...], dict[str, Any]]] = {}
        for r in _hits("guidance", lambda r: base(r) and (r["file_type"] or "").upper().startswith("EX-99")
                       and ("2.02" in (r["items"] or []) or "7.01" in (r["items"] or []))):
            acc = r["adsh"].replace("-", "")
            if acc in landed:
                continue
            n = _ex_no(r["file_type"])
            rank = (n != 1, n, r["file"])
            if acc not in best or rank < best[acc][0]:
                best[acc] = (rank, r)
        for _rank, r in best.values():
            picked.append(((("2.02" not in r["items"]), -r["file_date"].toordinal(), url(r)), url(r), r["file_date"]))
    elif name == "buyback":
        planned: set[str] = set()
        for g in ("fetch_guidance_probe.txt", "fetch_guidance_follow.txt"):
            gpath = C.build_root() / "_tmp" / "events" / g
            if gpath.exists():
                planned |= set(gpath.read_text(encoding="utf-8").split())
        # one document per filing: the lowest-numbered EX-99 hit (the press release), else the main document
        best_b: dict[str, tuple[tuple[Any, ...], dict[str, Any]]] = {}
        for r in _hits("buyback", lambda r: base(r) and ((r["file_type"] or "").upper().startswith("EX-99")
                                                         or (r["file_type"] or "") in ("8-K", "8-K/A"))):
            u = url(r)
            is202 = "2.02" in (r["items"] or [])
            if is202 and (r["adsh"].replace("-", "") in landed or u in planned):
                continue
            ex = (r["file_type"] or "").upper().startswith("EX-99")
            rank = (not ex, _ex_no(r["file_type"]), r["file"])
            if r["adsh"] not in best_b or rank < best_b[r["adsh"]][0]:
                best_b[r["adsh"]] = (rank, r)
        for _rank, r in best_b.values():
            is202 = "2.02" in (r["items"] or [])
            picked.append(((is202, -r["file_date"].toordinal(), url(r)), url(r), r["file_date"]))
    elif name == "merger":
        main_acc: set[str] = set()
        rows = list(_hits("merger", lambda r: base(r) and ((r["file_type"] or "") in ("8-K", "8-K/A")
                                                           or (r["file_type"] or "").upper().startswith("EX-99"))))
        for r in rows:
            if r["file_type"] in ("8-K", "8-K/A"):
                main_acc.add(r["adsh"])
        seen: set[str] = set()
        for r in sorted(rows, key=lambda r: (r["file_type"] not in ("8-K", "8-K/A"), r["file"])):
            if r["adsh"] in seen or (r["file_type"] not in ("8-K", "8-K/A") and r["adsh"] in main_acc):
                continue
            seen.add(r["adsh"])
            picked.append(((r["file_type"] not in ("8-K", "8-K/A"), -r["file_date"].toordinal(), url(r)), url(r),
                           r["file_date"]))
    elif name == "tender":
        firsts: dict[tuple[int, bool], tuple[tuple[Any, ...], dict[str, Any]]] = {}
        for r in _hits("tender", lambda r: base(r) and (r["form"] or "").startswith(("SC TO-T", "SC 14D9"))):
            subj = next(c for c in r["ciks"] if c in mem)
            key = (subj, (r["form"] or "").startswith("SC TO-T"))
            rank = (r["file_date"], not (r["file_type"] or "").upper().startswith("SC"), r["file"])
            if key not in firsts or rank < firsts[key][0]:
                firsts[key] = (rank, r)
        for _rank, r in firsts.values():
            picked.append(((-r["file_date"].toordinal(), url(r)), url(r), r["file_date"]))
    elif name == "fpi":
        # period-results wording hits, one document per 6-K, at most FPI_PER_YEAR filings per issuer and year
        best2: dict[str, tuple[tuple[Any, ...], dict[str, Any]]] = {}
        for r in _hits("fpi_period_results", lambda r: base(r) and ((r["file_type"] or "").upper().startswith("EX-99")
                                                                    or (r["file_type"] or "") in ("6-K", "6-K/A"))):
            rank = (not (r["file_type"] or "").upper().startswith("EX-99"), _ex_no(r["file_type"]), r["file"])
            if r["adsh"] not in best2 or rank < best2[r["adsh"]][0]:
                best2[r["adsh"]] = (rank, r)
        per_year: dict[tuple[int, int], list[dict[str, Any]]] = {}
        for _rank, r in best2.values():
            cik = next(c for c in r["ciks"] if c in mem)
            per_year.setdefault((cik, r["file_date"].year), []).append(r)
        for lst in per_year.values():
            lst.sort(key=lambda r: r["file_date"], reverse=True)
            for r in lst[:FPI_PER_YEAR]:
                picked.append(((-r["file_date"].toordinal(), url(r)), url(r), r["file_date"]))
    else:
        raise ValueError(name)
    picked.sort()
    dest = C.build_root() / "_tmp" / "events" / f"fetch_{name}.txt"
    dest.write_text("".join(u + "\n" for _k, u, _d in picked), encoding="utf-8")
    by_year: dict[str, int] = {}
    for _k, _u, d in picked:
        by_year[str(d.year)] = by_year.get(str(d.year), 0) + 1
    out: dict[str, Any] = {"plan": name, "urls": len(picked), "by_year": dict(sorted(by_year.items())),
                           "output": str(dest)}
    if name == "buyback":
        out["first_tier_without_2_02"] = sum(1 for k, _u, _d in picked if not k[0])
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("plans", nargs="+", choices=("guidance", "guidance_probe", "guidance_follow", "buyback",
                                                 "merger", "tender", "fpi"))
    args = ap.parse_args(argv)
    out = [plan(p) for p in args.plans]
    out.append({"peak_memory": ES.peak_memory()})
    print(json.dumps(out, indent=1), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
