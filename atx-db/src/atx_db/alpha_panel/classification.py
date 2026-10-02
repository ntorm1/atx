"""Stage ``classification`` (S7.1): Fama-French 5/10/12/17/30/38/48/49 and approximate NAICS from the dated SIC.

Inputs: ``fundamentals/sic_events.parquet`` (the SIC in force per filing, with its clock), the French definition
files parsed by the reference stage (``reference/french/siccodes.parquet``) and the Census concordances landed
under ``data/raw/census/`` (``CENSUS``): 1987 SIC -> 2002 NAICS, then 2002 -> 2007 -> 2012 -> 2017 -> 2022 NAICS.
The ``.xls`` files are converted once to CSV at landing (``fetch`` uses ``xlrd`` when importable; the CSV and
its SHA-256 go in the receipt next to the source's); ``build`` reads the CSV / ``.xlsx`` only.

Rules (``classification-v1``):

* FF industry of a SIC for scheme N: the first industry (file order) with a range ``sic_lo <= sic <= sic_hi``;
  else the scheme's residual industry (the one listed without ranges: ``Other`` in 5/10/12/17/30/38); for 48 and
  49, whose ``Other`` has explicit ranges, an unlisted SIC also goes to ``Other`` with ``ff48_listed`` /
  ``ff49_listed`` false (French's own 48/49 portfolios leave such firms out).
* NAICS 2002 of a SIC (``naics2002_basis``): a curated primary for nine frequent SEC codes whose first concordance
  piece is a minor activity or that are not 1987 SICs (``CURATED_02``: ``curated_primary``); else the SIC's
  concordance row when there is one (``sic4_whole``), else the row whose SIC piece note reads "(except ...)"
  (``sic4_except_piece``: the SIC's main body), else its first row (``sic4_first_piece``); SEC codes that are not
  1987 SICs (industry-group codes such as 3570) take the hierarchical mode (sector, then longer prefixes) of the
  primaries of the SICs sharing the first three digits (``sic3_group``), then two digits (``sic2_group``).
  ``naics2002_candidates`` counts the candidate NAICS codes and ``naics2002_prefix`` is their longest common prefix
  (>= 2 digits, else NULL): the part of the code that holds whichever piece applies. The SEC-only codes 6189
  (asset-backed securities), 8880 (ADRs), 8888 (foreign governments) and 9995 (non-operating establishments) have
  no NAICS (``no_concordance``); 6770 (blank checks) is a 1987 SIC and maps.
* NAICS 2022: the 2002 code carried through each Census concordance with the same primary-piece rule
  (``naics_chain_split`` when a step split the industry). ``naics_approx`` is true on every row: SIC -> NAICS is
  many-to-many and has no weights.

Outputs:

* ``sic_map.parquet`` (static look-up): ``sic, sic2, sic3``, ``ff{N}`` (short name) and ``ff{N}_no`` for each
  scheme, ``ff48_listed, ff49_listed``, ``naics2002, naics2002_title, naics2002_basis, naics2002_candidates,
  naics2002_prefix, naics2022, naics2022_title, naics2, naics_chain_split, naics_approx``.
* ``issuer_industry.parquet``: one row per CIK and SIC run (a new row when the SIC in force changes), dated by the
  SIC event clock: ``cik, available_at, sic, accession, sic_basis`` plus every ``sic_map`` column.
"""

from __future__ import annotations

import argparse
import collections
import csv
import datetime as dt
import hashlib
import io
import json
import re
import sys
from pathlib import Path
from typing import Any

from . import common as C
from . import shortflow_common as S

STAGE = "classification"
SCHEMA = "atx.alpha-panel.classification/v1"
RULE = "classification-v1"
MODULES = ("classification", "common", "shortflow_common")
RAW = S.RAW_ROOT / "census"
CENSUS_BASE = "https://www.census.gov/naics/concordances/"
CENSUS = {
    "sic87_naics02": "1987_SIC_to_2002_NAICS.xls",
    "naics02_07": "2002_to_2007_NAICS.xls",
    "naics07_12": "2007_to_2012_NAICS.xls",
    "naics12_17": "2012_to_2017_NAICS.xlsx",
    "naics17_22": "2017_to_2022_NAICS.xlsx",
}
CHAIN = ("naics02_07", "naics07_12", "naics12_17", "naics17_22")
SCHEMES = (5, 10, 12, 17, 30, 38, 48, 49)
NO_CONCORDANCE = frozenset({6189, 8880, 8888, 9995})
HOST_INTERVAL_S = 1.1

LAKE_STAGES = [
    {"name": "classification", "lane": "MKT", "schema": "atx.alpha-panel.classification/v1",
     "module": "atx_db.alpha_panel.classification", "args": ["build"], "fetch": ["fetch"],
     "inputs": ["fundamentals", "reference"],
     "outputs": [{"glob": "classification/issuer_industry.parquet", "view": "classification_issuer_industry"},
                 {"glob": "classification/sic_map.parquet", "view": "classification_sic_map", "clock": None}],
     "staleness": "event data; SIC in force carries 550 days in the panel", "vintage": "event", "guard_gb": 0.3},
]

_NOTE = re.compile(r"\(([^()]*(?:\([^()]*\)[^()]*)*)\)\s*$")


# ---------------------------------------------------------------- concordance parsing
def concordance_rows(rows: list[list[Any]]) -> list[tuple[int, str, int, str]]:
    """``(from_code, from_title, to_code, to_title)`` rows after the header row (first cell ``SIC`` or
    ``... NAICS Code``); non-numeric codes are skipped."""
    out, started = [], False
    for r in rows:
        cells = [("" if c is None else c) for c in list(r) + [""] * 4][:4]
        head = str(cells[0]).strip()
        if not started:
            started = head.upper() == "SIC" or head.upper().endswith("NAICS CODE")
            continue
        try:
            a, b = int(float(str(cells[0]).strip())), int(float(str(cells[2]).strip()))
        except ValueError:
            continue
        out.append((a, str(cells[1]).strip(), b, str(cells[3]).strip()))
    return out


def primary_map(rows: list[tuple[int, str, int, str]]) -> dict[int, tuple[int, str, str, int]]:
    """``{from: (to, to_title, basis, n_candidates)}``: the whole-industry row, else the "(except ...)" piece, else
    the first piece."""
    by: dict[int, list[tuple[int, str, int, str]]] = collections.defaultdict(list)
    for r in rows:
        by[r[0]].append(r)
    out = {}
    for k, rs in by.items():
        whole = [r for r in rs if not _NOTE.search(r[1])]
        exc = [r for r in rs if (m := _NOTE.search(r[1])) and m.group(1).strip().lower().startswith("except")]
        if len({r[2] for r in rs}) == 1:
            pick, basis = rs[0], "whole"
        elif whole:
            pick, basis = whole[0], "whole"
        elif exc:
            pick, basis = exc[0], "except_piece"
        else:
            pick, basis = rs[0], "first_piece"
        out[k] = (pick[2], pick[3], basis, len({r[2] for r in rs}))
    return out


# Curated 2002 primaries for frequent SEC codes whose concordance first piece is a minor activity (7372's first
# piece is software reproducing, 5812's dinner theaters, 4911's hydroelectric) or that are not 1987 SICs (6770).
# Candidates and ``naics2002_prefix`` are unchanged by an override.
CURATED_02: dict[int, int] = {
    6770: 525990,  # blank checks -> other financial vehicles
    7372: 511210,  # prepackaged software -> software publishers
    7389: 561990,  # business services NEC -> all other support services
    7370: 518210,  # computer programming, data processing (SEC group) -> data processing, hosting
    1000: 212299,  # metal mining (SEC group) -> all other metal ore mining
    4911: 221122,  # electric services -> electric power distribution
    4931: 221122,  # electric and other services combined -> electric power distribution
    5812: 722110,  # eating places -> full-service restaurants
    6799: 523910,  # investors NEC -> miscellaneous intermediation
}


def candidates_map(rows: list[tuple[int, str, int, str]]) -> dict[int, set[int]]:
    out: dict[int, set[int]] = collections.defaultdict(set)
    for a, _, b, _ in rows:
        out[a].add(b)
    return out


def hier_mode(codes: list[int]) -> int:
    """Most frequent 2-digit sector, then the most frequent longer prefix inside it, down to 6 digits (ties: the
    smallest code)."""
    s = [str(c) for c in codes]
    prefix = ""
    for k in range(2, 7):
        cnt = collections.Counter(x[:k] for x in s if x.startswith(prefix) and len(x) >= k)
        prefix = max(cnt.items(), key=lambda kv: (kv[1], -int(kv[0])))[0]
    return int(prefix)


def common_prefix(codes: set[int] | list[int]) -> str | None:
    import os

    p = os.path.commonprefix([str(c) for c in codes]) if codes else ""
    return p if len(p) >= 2 else None


def sic_to_naics02(sic: int, pm: dict[int, tuple[int, str, str, int]], cands: dict[int, set[int]] | None = None,
                   titles: dict[int, str] | None = None) -> tuple[int | None, str | None, str, int, str | None]:
    """(naics2002, title, basis, candidates, common prefix of the candidates) of an SEC SIC code: a curated
    primary, else the SIC's concordance primary, else the hierarchical mode of the primaries of the SICs sharing
    its first three, then two digits (SEC group codes)."""
    cands = cands or {k: {v[0]} for k, v in pm.items()}
    titles = titles or {v[0]: v[1] for v in pm.values()}
    if sic in NO_CONCORDANCE:
        return None, None, "no_concordance", 0, None
    if sic in pm:
        to, title, basis, n = pm[sic]
        pre = common_prefix(cands.get(sic, {to}))
        if sic in CURATED_02:
            return CURATED_02[sic], titles.get(CURATED_02[sic]), "curated_primary", n, pre
        return to, title, f"sic4_{basis}", n, pre
    for width, label in ((3, "sic3_group"), (2, "sic2_group")):
        div = 10 ** (4 - width)
        members = [k for k in pm if k // div == sic // div]
        if members:
            prim = [CURATED_02.get(k, pm[k][0]) for k in members]
            allc = set().union(*(cands.get(k, {pm[k][0]}) for k in members))
            if sic in CURATED_02:
                return CURATED_02[sic], titles.get(CURATED_02[sic]), "curated_primary", len(allc), common_prefix(allc)
            to = hier_mode(prim)
            return to, titles.get(to), label, len(allc), common_prefix(allc)
    return None, None, "no_concordance", 0, None


def chain_naics(code: int | None, steps: list[dict[int, tuple[int, str, str, int]]]) -> tuple[int | None, str | None, bool]:
    """Carry a 2002 code through the concordance steps; (final code, title, any step split)."""
    title, split = None, False
    for pm in steps:
        if code is None:
            return None, None, split
        if code not in pm:
            continue  # unchanged industry absent from a change-only table: keep the code
        code, title, basis, n = pm[code]
        split = split or n > 1
    return code, title, split


# ---------------------------------------------------------------- Fama-French
def ff_assigner(siccodes: list[dict[str, Any]]) -> dict[int, Any]:
    """Per scheme: (ranges [(lo, hi, no, short)], residual (no, short) or None, other (no, short))."""
    out: dict[int, Any] = {}
    for n in SCHEMES:
        rows = [r for r in siccodes if r["scheme"] == n]
        ranges = [(r["sic_lo"], r["sic_hi"], r["industry_no"], r["industry_short"]) for r in rows
                  if r["sic_lo"] is not None]
        ranges.sort(key=lambda x: x[2])
        resid = next(((r["industry_no"], r["industry_short"]) for r in rows if r["sic_lo"] is None), None)
        other = next(((r["industry_no"], r["industry_short"]) for r in rows if r["industry_short"] == "Other"),
                     max(((r["industry_no"], r["industry_short"]) for r in rows), default=None))
        out[n] = (ranges, resid, other)
    return out


def ff_of(sic: int, scheme: tuple) -> tuple[int, str, bool]:
    ranges, resid, other = scheme
    for lo, hi, no, short in ranges:
        if lo <= sic <= hi:
            return no, short, True
    no, short = resid if resid is not None else other
    return no, short, resid is not None


def sic_map_rows(sics: list[int], siccodes: list[dict[str, Any]], pm02: dict, steps: list[dict],
                 cands: dict[int, set[int]] | None = None, titles: dict[int, str] | None = None) -> list[dict[str, Any]]:
    ff = ff_assigner(siccodes)
    rows = []
    for sic in sorted(set(sics)):
        r: dict[str, Any] = {"sic": sic, "sic2": sic // 100, "sic3": sic // 10}
        for n in SCHEMES:
            no, short, listed = ff_of(sic, ff[n])
            r[f"ff{n}"], r[f"ff{n}_no"] = short, no
            if n in (48, 49):
                r[f"ff{n}_listed"] = listed
        code, title, basis, cand, prefix = sic_to_naics02(sic, pm02, cands, titles)
        c22, t22, split = chain_naics(code, steps)
        r.update(naics2002=code, naics2002_title=title, naics2002_basis=basis, naics2002_candidates=cand,
                 naics2002_prefix=prefix,
                 naics2022=c22, naics2022_title=t22 if t22 is not None else (title if c22 == code else None),
                 naics2=(c22 // 10000 if c22 else None), naics_chain_split=split, naics_approx=True)
        rows.append(r)
    return rows


def sic_runs(events: list[tuple[int, dt.datetime, int, str, str]]) -> list[tuple[int, dt.datetime, int, str, str]]:
    """First event of each run of an unchanged SIC per CIK, from ``(cik, clock, sic, accession, basis)``."""
    out, last = [], {}
    for cik, clock, sic, acc, basis in sorted(events, key=lambda e: (e[0], e[1])):
        if sic is None or sic <= 0:
            continue
        if last.get(cik) != sic:
            out.append((cik, clock, sic, acc, basis))
            last[cik] = sic
    return out


def sic_runs_sql(path: Path) -> tuple[list[tuple], int]:
    """:func:`sic_runs` in DuckDB (the 418k-event table does not fit the job cap as Python objects)."""
    con = C.connect(memory="120MB", threads=1)
    src = f"read_parquet('{path.as_posix()}')"
    n = con.execute(f"SELECT count(*) FROM {src}").fetchone()[0]
    runs = con.execute(f"""
        SELECT cik, clock_utc, sic, accession, sic_basis FROM (
            SELECT *, lag(sic) OVER (PARTITION BY cik ORDER BY clock_utc, accession) AS prev_sic
            FROM {src} WHERE sic > 0)
        WHERE prev_sic IS NULL OR prev_sic <> sic
        ORDER BY cik, clock_utc
    """).fetchall()
    con.close()
    return runs, int(n)


# ---------------------------------------------------------------- landing
def _ledger() -> S.Ledger:
    return S.Ledger(RAW / "receipts.jsonl")


def fetch(refetch: bool = False) -> dict[str, Any]:
    RAW.mkdir(parents=True, exist_ok=True)
    host = S.PoliteHost(min_interval=HOST_INTERVAL_S)
    led = _ledger()
    have = led.latest()
    stats: dict[str, Any] = {"fetched": 0, "converted": 0, "bytes": 0}
    for key, fname in CENSUS.items():
        rec = have.get(key)
        if not refetch and rec and rec.get("http_status") == 200 and (RAW / rec["file"]).exists():
            continue
        url = CENSUS_BASE + fname
        st, body, hdr = host.request(url)
        rec = {"key": key, "kind": "census_concordance", "url": url, "http_status": st, "fetched_at": S.utc_now(),
               "last_modified": hdr.get("last-modified")}
        if st == 200 and body:
            (RAW / fname).write_bytes(body)
            rec.update(file=fname, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
            stats["fetched"] += 1
            stats["bytes"] += len(body)
            if fname.endswith(".xls"):
                rec["csv"] = convert_xls(RAW / fname)
                stats["converted"] += 1
        led.append(rec)
    return stats


def convert_xls(path: Path) -> dict[str, Any]:
    """First sheet of a BIFF ``.xls`` as UTF-8 CSV next to it (needs ``xlrd``; run once at landing)."""
    import xlrd  # optional: only the landing step converts

    book = xlrd.open_workbook(str(path))
    sh = book.sheet_by_index(0)
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    for i in range(sh.nrows):
        w.writerow([(repr(int(v)) if isinstance(v, float) and v == int(v) else v) for v in sh.row_values(i)])
    blob = buf.getvalue().encode("utf-8")
    dest = path.with_suffix(".csv")
    dest.write_bytes(blob)
    return {"file": dest.name, "sha256": hashlib.sha256(blob).hexdigest(), "bytes": len(blob),
            "converter": f"xlrd {xlrd.__version__}", "sheet": sh.name}


def read_census(key: str) -> list[list[Any]]:
    rec = _ledger().latest().get(key)
    if not rec or rec.get("http_status") != 200:
        raise RuntimeError(f"{key}: not landed (run fetch)")
    if rec["file"].endswith(".xls"):
        c = rec["csv"]
        blob = (RAW / c["file"]).read_bytes()
        if hashlib.sha256(blob).hexdigest() != c["sha256"]:
            raise RuntimeError(f"{c['file']}: sha256 differs from its receipt")
        return list(csv.reader(io.StringIO(blob.decode("utf-8"))))
    import openpyxl

    blob = (RAW / rec["file"]).read_bytes()
    if hashlib.sha256(blob).hexdigest() != rec["sha256"]:
        raise RuntimeError(f"{rec['file']}: sha256 differs from its receipt")
    wb = openpyxl.load_workbook(io.BytesIO(blob), read_only=True)
    return [list(r) for r in wb.worksheets[0].iter_rows(values_only=True)]


# ---------------------------------------------------------------- build
def build() -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    root = C.build_root()
    out = C.stage_dir(STAGE)
    receipt: dict[str, Any] = {"rule": RULE}
    rows02 = concordance_rows(read_census("sic87_naics02"))
    pm02 = primary_map(rows02)
    steps = [primary_map(concordance_rows(read_census(k))) for k in CHAIN]
    receipt["concordance_sizes"] = {"sic87_naics02": len(pm02), **{k: len(s) for k, s in zip(CHAIN, steps)}}
    siccodes = pq.read_table(root / "reference" / "french" / "siccodes.parquet").to_pylist()
    runs, n_events = sic_runs_sql(root / "fundamentals" / "sic_events.parquet")
    smap = sic_map_rows([r[2] for r in runs], siccodes, pm02, steps, candidates_map(rows02),
                        {b: t for _, _, b, t in rows02})
    by_sic = {r["sic"]: r for r in smap}
    _write(pq, pa.Table.from_pylist(smap), out / "sic_map.parquet")
    rows = [{"cik": cik, "available_at": clock, "sic": sic, "accession": acc, "sic_basis": basis,
             **{k: v for k, v in by_sic[sic].items() if k != "sic"}} for cik, clock, sic, acc, basis in runs]
    _write(pq, pa.Table.from_pylist(rows), out / "issuer_industry.parquet")
    basis = collections.Counter(r["naics2002_basis"] for r in smap)
    receipt.update(sic_events=n_events, sic_runs=len(runs), issuers=len({r[0] for r in runs}), sics=len(smap),
                   naics2002_basis=dict(basis), coverage=coverage(runs, by_sic))
    inputs = {"fundamentals/manifest.json": _sha(root / "fundamentals" / "manifest.json"),
              "reference/manifest.json": _sha(root / "reference" / "manifest.json")}
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, {
        "rule": RULE, "rule_text": __doc__, "staleness": "event data", "input_manifests_sha256": inputs,
        "sources": {"census_receipts_sha256": _sha(RAW / "receipts.jsonl"),
                    "census": {k: {f: v.get(f) for f in ("url", "file", "sha256", "csv", "fetched_at")}
                               for k, v in _ledger().latest().items()}},
        "receipt": receipt})
    return receipt


def coverage(runs: list[tuple], by_sic: dict[int, dict[str, Any]]) -> dict[str, Any]:
    """Linked issuers (identity link table) with a SIC event, and the share of them with each classification."""
    import pyarrow.parquet as pq

    linked = set(pq.read_table(C.build_root() / "identity" / "link_table.parquet", columns=["cik"])
                 .column("cik").to_pylist())
    latest: dict[int, int] = {}
    for cik, _, sic, _, _ in runs:
        latest[cik] = sic
    with_sic = [c for c in linked if c in latest]
    res = {"linked_issuers": len(linked), "linked_with_sic": len(with_sic)}
    for col in [f"ff{n}" for n in SCHEMES] + ["naics2002", "naics2022"]:
        res[f"{col}_share"] = round(sum(1 for c in with_sic if by_sic[latest[c]][col] is not None) / max(len(with_sic), 1), 6)
    res["naics_missing_sics"] = sorted({latest[c] for c in with_sic if by_sic[latest[c]]["naics2022"] is None})
    res["ff49_unlisted_share"] = round(sum(1 for c in with_sic if not by_sic[latest[c]]["ff49_listed"]) / max(len(with_sic), 1), 6)
    return res


def _sha(p: Path) -> str | None:
    return C.sha256_file(p) if p.exists() else None


def _write(pq, table, dest: Path) -> None:
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(table, tmp, compression="zstd")
    tmp.replace(dest)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("fetch", "build", "all"), default="all")
    ap.add_argument("--refetch", action="store_true")
    args = ap.parse_args(argv)
    if args.step in ("fetch", "all"):
        print(json.dumps(fetch(args.refetch)), flush=True)
    if args.step in ("build", "all"):
        print(json.dumps(build(), default=str)[:4000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
