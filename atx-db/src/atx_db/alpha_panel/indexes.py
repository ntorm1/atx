"""Stage ``indexes`` (S3.6): rule-based Russell 1000 / 2000 / 3000 and S&P-500-like proxies, point in time.

Official constituent lists and weights are licensed (FTSE Russell, S&P DJI); their public pages are terms-gated,
so nothing is scraped and no overlap with the official lists is measured (documented gap; the LIC lane's index
adapter takes the licensed file when bought). The proxies follow the public methodologies on our data.

Eligible line (both families), on the rank date's panel row: ``security_type`` common / common_unverified /
REIT, not ADR (``is_adr``, ``is_adr_likely``), not a foreign private issuer (``is_fpi``: the US-domicile proxy),
not LP or royalty trust (Russell excludes both; business development companies stay), ``is_operating``, listed on
NYSE, NYSE American, Nasdaq, NYSE Arca or Cboe BZX (``exchange``), raw close >= $1, ``market/shares_daily.shrout``
known. Company = CIK (an unlinked line is its own company); company ME = sum of its eligible lines' shrout x raw
close on the rank date. Every eligible line of a selected company is a member (share classes, as both indexes do).

Russell proxy (rule ``russell-proxy-v1``; FTSE Russell US Indexes construction and methodology):
* rank days (``RUSSELL``): 2018-05-11, 2019-05-10, 2020-05-08, 2021-05-07, 2022-05-06, then the last business day
  of April (2023-04-28, 2024-04-30, 2025-04-30, 2026-04-30); effective after the close of the fourth Friday of
  June (2018-06-22 ... 2026-06-26). From 2026 the reconstitution is semi-annual (rank day = last business day of
  October, effective after the second Friday of December, 2026-12-11), after the last vendor session here.
* companies with total ME >= $30M ranked by ME; R3000 = the top 3,000 (no band at 3,000); R1000 / R2000 split at
  rank 1,000 with the +-2.5% cumulative market-cap percentile band: a prior R1000 company stays while its
  cumulative percentile is <= p1000 + 2.5%; a prior R2000 company moves up only below p1000 - 2.5%; others by
  rank. The first proxy year (2018) has no prior membership, so no band.
* between reconstitutions a line leaves on its last vendor session (deletions are not replaced); IPO quarterly
  additions and spin-off additions are not modelled.

S&P-500-like proxy (rule ``sp500-proxy-v1``): quarterly, rank date = last session of Feb / May / Aug / Nov,
effective after the third Friday of Mar / Jun / Sep / Dec. New entrants must have positive GAAP earnings (panel
``ni_ttm`` > 0 and ``ni_q`` > 0, as reported) and annual dollar volume >= 0.75 x company ME (``market/liquidity``
``adv_63`` x 252; float is unknown, so total ME stands in); existing members stay while ranked <= 600 (buffer)
without the earnings screen; the index is filled to 500 companies by ME rank. No committee discretion, sector
balance or float adjustment.

``available_at`` of a membership = the rank date's 22:00 UTC mark (every input is known by then); membership is
effective from the first session after the reconstitution date.

Outputs: ``constituents.parquet`` (``index_id, security_id, cik, effective_from, effective_to`` (inclusive; NULL
while open), ``weight`` (line ME share of the index on the rank date), ``company_rank, rank_date, recon_date,
basis, available_at``); ``membership/year=YYYY/membership.parquet`` (``session_date, security_id, cik, r1000,
r2000, r3000, sp500, available_at``); ``recon_summary.parquet`` (per index and reconstitution: lines, companies,
additions, deletions, one-way and cap-weighted turnover, breakpoints).
"""

from __future__ import annotations

import argparse
import bisect
import datetime as dt
import json
import sys
from typing import Any

from . import common as C
from . import market_common as M

STAGE = "indexes"
SCHEMA = "atx.alpha-panel.indexes/v1"
MODULES = ("indexes", "market_common", "common")
LISTED = ("XNYS", "XNAS", "XASE", "ARCX", "BATS")
RUSSELL = [("2018-05-11", "2018-06-22"), ("2019-05-10", "2019-06-28"), ("2020-05-08", "2020-06-26"),
           ("2021-05-07", "2021-06-25"), ("2022-05-06", "2022-06-24"), ("2023-04-28", "2023-06-23"),
           ("2024-04-30", "2024-06-28"), ("2025-04-30", "2025-06-27"), ("2026-04-30", "2026-06-26")]
RUSSELL_MIN_ME = 30e6
BAND = 0.025
SP_N, SP_BUFFER = 500, 600
SP_LIQ_RATIO = 0.75
INPUTS = ("panel", "market/market_shares_manifest.json", "market/market_liquidity_manifest.json", "security_master")

LAKE_STAGES = [
    {"name": "indexes", "lane": "MKT", "schema": "atx.alpha-panel.indexes/v1",
     "module": "atx_db.alpha_panel.indexes", "args": ["build"],
     "inputs": ["panel", "market_shares", "market_liquidity", "security_master"],
     "outputs": [{"glob": "indexes/constituents.parquet", "view": "indexes_constituents", "vintage": "interval"},
                 {"glob": "indexes/membership/year=*/membership.parquet", "view": "indexes_membership",
                  "vintage": "daily"},
                 {"glob": "indexes/recon_summary.parquet", "view": "indexes_recon_summary", "clock": None}],
     "staleness": "annual (Russell) / quarterly (S&P proxy) reconstitution", "vintage": "interval",
     "guard_gb": 0.4},
]


# ---------------------------------------------------------------- selection rules (pure)
def russell_assign(companies: list[tuple[Any, float]], prev_r1000: set, prev_r2000: set,
                   band: float = BAND) -> tuple[dict[Any, tuple[str, int, float]], float]:
    """``{company: (R1000|R2000, rank, cumulative ME share)}`` for the top 3,000 of ``(company, me)`` (ME >= $30M),
    with the +-``band`` cumulative-percentile band around rank 1,000; also returns p1000."""
    ranked = sorted((c for c in companies if c[1] >= RUSSELL_MIN_ME), key=lambda c: (-c[1], str(c[0])))
    total = sum(me for _, me in ranked) or 1.0
    cum, out, p1000 = 0.0, {}, None
    cums = []
    for _, me in ranked:
        cum += me
        cums.append(cum / total)
    p1000 = cums[min(999, len(cums) - 1)] if cums else 0.0
    for i, ((key, _), cp) in enumerate(zip(ranked[:3000], cums[:3000]), start=1):
        if key in prev_r1000:
            idx = "R1000" if cp <= p1000 + band else "R2000"
        elif key in prev_r2000:
            idx = "R1000" if cp < p1000 - band else "R2000"
        else:
            idx = "R1000" if i <= 1000 else "R2000"
        out[key] = (idx, i, cp)
    return out, p1000


def sp_select(companies: list[tuple[Any, float, bool]], prev: set, n: int = SP_N,
              buffer: int = SP_BUFFER) -> dict[Any, tuple[int, str]]:
    """``{company: (rank, basis)}`` of an S&P-500-like set from ``(company, me, passes_entry_screens)``."""
    ranked = sorted(companies, key=lambda c: (-c[1], str(c[0])))
    rank = {c[0]: i for i, c in enumerate(ranked, start=1)}
    keep = sorted((k for k in prev if k in rank and rank[k] <= buffer), key=lambda k: rank[k])[:n]
    out = {k: (rank[k], "buffer_stay") for k in keep}
    for key, _, ok in ranked:
        if len(out) >= n:
            break
        if key not in out and ok:
            out[key] = (rank[key], "entry")
    return out


def third_friday(y: int, m: int) -> dt.date:
    d = dt.date(y, m, 15)
    return d + dt.timedelta(days=(4 - d.weekday()) % 7)


def sp_schedule(first: dt.date, last: dt.date) -> list[tuple[dt.date, dt.date]]:
    """(rank month end, rebalance Friday) for every quarter whose rebalance falls in [first, last]."""
    out = []
    for y in range(first.year, last.year + 1):
        for m in (3, 6, 9, 12):
            reb = third_friday(y, m)
            rank_month_end = dt.date(y, m, 1) - dt.timedelta(days=1)
            if first <= rank_month_end and reb <= last:
                out.append((rank_month_end, reb))
    return out


def last_session_on_or_before(cal: list[dt.date], d: dt.date) -> dt.date | None:
    i = bisect.bisect_right(cal, d)
    return cal[i - 1] if i else None


# ---------------------------------------------------------------- data
def snapshot(con, d: dt.date) -> list[dict[str, Any]]:
    """Eligible lines on session d with ME, earnings and liquidity inputs."""
    root = C.build_root()
    panel = (root / "panel" / f"year={d.year}" / "*.parquet").as_posix()
    shares = (root / "market" / "shares_daily" / f"year={d.year}" / "shares_daily.parquet").as_posix()
    liq = (root / "market" / "liquidity" / f"year={d.year}" / "liquidity.parquet").as_posix()
    listed = ", ".join(f"'{x}'" for x in LISTED)
    cur = con.execute(f"""
        SELECT p.security_id, p.cik, p.raw_close, s.shrout, s.shrout * p.raw_close AS me, p.ni_ttm, p.ni_q,
               l.adv_63
        FROM read_parquet('{panel}', union_by_name = true, hive_partitioning = false) p
        JOIN read_parquet('{shares}') s USING (session_date, security_id)
        LEFT JOIN read_parquet('{liq}') l USING (session_date, security_id)
        WHERE p.session_date = DATE '{d}' AND p.security_type IN ('common', 'common_unverified', 'REIT')
          AND NOT coalesce(p.is_adr, false) AND NOT coalesce(p.is_adr_likely, false) AND NOT coalesce(p.is_fpi, false)
          AND NOT coalesce(p.is_lp, false) AND NOT coalesce(p.is_royalty_trust, false)
          AND coalesce(p.is_operating, false) AND NOT coalesce(p.is_index, false)
          AND p.exchange IN ({listed}) AND p.raw_close >= 1.0 AND s.shrout > 0
    """)
    cols = [c[0] for c in cur.description]
    return [dict(zip(cols, r)) for r in cur.fetchall()]


def companies_of(lines: list[dict[str, Any]]) -> dict[Any, dict[str, Any]]:
    out: dict[Any, dict[str, Any]] = {}
    for r in lines:
        key = r["cik"] if r["cik"] is not None else -r["security_id"]
        c = out.setdefault(key, {"me": 0.0, "adv": 0.0, "lines": [], "ni_ttm": r["ni_ttm"], "ni_q": r["ni_q"]})
        c["me"] += r["me"]
        c["adv"] += r["adv_63"] or 0.0
        c["lines"].append(r)
    return out


def build() -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    root = C.build_root()
    receipt: dict[str, Any] = {}
    con = C.connect(memory="240MB", threads=2, db_file="indexes.duckdb")
    cal = [r[0] for r in con.execute(f"SELECT session_date FROM {M.calendar_sql()} ORDER BY 1").fetchall()]
    last_session = {r[0]: r[1] for r in con.execute(
        f"SELECT security_id, last_session FROM read_parquet('{(root / 'security_master' / 'lines.parquet').as_posix()}')"
    ).fetchall()}
    first_panel = con.execute(
        f"SELECT min(session_date) FROM read_parquet('{(root / 'panel' / 'year=2018' / '*.parquet').as_posix()}', "
        f"union_by_name = true)").fetchone()[0]
    recons: list[dict[str, Any]] = []   # {index_id, rank_date, recon_date, members: {sid: (cik, weight, rank, basis)}}
    # Russell
    prev1: set = set()
    prev2: set = set()
    for rank_s, recon_s in RUSSELL:
        rank_d = last_session_on_or_before(cal, dt.date.fromisoformat(rank_s))
        recon_d = last_session_on_or_before(cal, dt.date.fromisoformat(recon_s))
        if rank_d is None or recon_d is None or recon_d >= cal[-1] or rank_d < first_panel:
            continue
        comp = companies_of(snapshot(con, rank_d))
        assign, p1000 = russell_assign([(k, c["me"]) for k, c in comp.items()], prev1, prev2)
        sets = {"R1000P": {}, "R2000P": {}, "R3000P": {}}
        for key, (idx, rk, cp) in assign.items():
            basis = "rank" if (idx == "R1000") == (rk <= 1000) else "band_stay"
            for ln in comp[key]["lines"]:
                sets["R3000P"][ln["security_id"]] = (ln["cik"], ln["me"], rk, "rank")
                sets[f"{idx}P"][ln["security_id"]] = (ln["cik"], ln["me"], rk, basis)
        for iid, mem in sets.items():
            recons.append({"index_id": iid, "rank_date": rank_d, "recon_date": recon_d, "members": mem,
                           "p1000": p1000, "eligible_companies": sum(1 for c in comp.values() if c["me"] >= RUSSELL_MIN_ME)})
        prev1 = {k for k, v in assign.items() if v[0] == "R1000"}
        prev2 = {k for k, v in assign.items() if v[0] == "R2000"}
    # S&P-500-like
    prev: set = set()
    for rank_end, reb in sp_schedule(first_panel + dt.timedelta(days=31), cal[-1]):
        rank_d = last_session_on_or_before(cal, rank_end)
        recon_d = last_session_on_or_before(cal, reb)
        if recon_d is None or recon_d >= cal[-1]:
            continue
        comp = companies_of(snapshot(con, rank_d))
        cand = [(k, c["me"], bool((c["ni_ttm"] or 0) > 0 and (c["ni_q"] or 0) > 0
                                  and c["adv"] * 252 >= SP_LIQ_RATIO * c["me"])) for k, c in comp.items()]
        sel = sp_select(cand, prev)
        mem = {ln["security_id"]: (ln["cik"], ln["me"], rk, basis)
               for key, (rk, basis) in sel.items() for ln in comp[key]["lines"]}
        recons.append({"index_id": "SP500P", "rank_date": rank_d, "recon_date": recon_d, "members": mem,
                       "eligible_companies": len(comp)})
        prev = set(sel)
    rows, summary = intervals(recons, cal, last_session)
    out = C.stage_dir(STAGE)
    _write(pq, pa.Table.from_pylist(rows), out / "constituents.parquet")
    _write(pq, pa.Table.from_pylist(summary), out / "recon_summary.parquet")
    receipt["constituent_rows"] = len(rows)
    receipt["recon_summary"] = summary
    con.register("cons", pa.Table.from_pylist(rows))
    years = {}
    for y in sorted({d.year for d in cal if d >= min(r["effective_from"] for r in rows)}):
        years[str(y)] = C.copy_to_parquet(con, f"""
            WITH cal AS (SELECT session_date FROM {M.calendar_sql()} WHERE year(session_date) = {y})
            SELECT cal.session_date, c.security_id, any_value(c.cik) AS cik,
                   bool_or(c.index_id = 'R1000P') AS r1000, bool_or(c.index_id = 'R2000P') AS r2000,
                   bool_or(c.index_id = 'R3000P') AS r3000, bool_or(c.index_id = 'SP500P') AS sp500,
                   max(c.available_at) AS available_at
            FROM cal JOIN cons c ON cal.session_date >= c.effective_from
                               AND cal.session_date <= coalesce(c.effective_to, DATE '9999-12-31')
            GROUP BY 1, 2 ORDER BY 1, 2
        """, out / "membership" / f"year={y}" / "membership.parquet")
    receipt["membership_rows"] = years
    receipt["counts_on_recon"] = {f"{s['index_id']} {s['recon_date']}": s["lines"] for s in summary}
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, {
        "rule": ["russell-proxy-v1", "sp500-proxy-v1"], "rule_text": __doc__, "staleness": "annual / quarterly",
        "input_manifests_sha256": M.input_manifests(*INPUTS),
        "gap": "official constituent lists are licensed / terms-gated: no overlap measured", "receipt": receipt})
    return receipt


def intervals(recons: list[dict[str, Any]], cal: list[dt.date], last_session: dict[int, dt.date]
              ) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Membership intervals (effective from the session after the recon date, to the next recon date or the line's
    last session) and per-reconstitution turnover."""
    rows, summary = [], []
    by_index: dict[str, list[dict[str, Any]]] = {}
    for r in recons:
        by_index.setdefault(r["index_id"], []).append(r)
    for iid, rs in by_index.items():
        rs.sort(key=lambda r: r["recon_date"])
        prev: dict[int, Any] = {}
        for k, r in enumerate(rs):
            eff = cal[bisect.bisect_right(cal, r["recon_date"])]
            nxt = rs[k + 1]["recon_date"] if k + 1 < len(rs) else None
            total = sum(v[1] for v in r["members"].values()) or 1.0
            mark = dt.datetime.combine(r["rank_date"], dt.time(C.MARK_HOUR_UTC))
            for sid, (cik, me, rk, basis) in r["members"].items():
                end = nxt
                ls = last_session.get(sid)
                if ls is not None and (end is None or ls < end):
                    end = ls
                if end is not None and end < eff:
                    continue
                rows.append({"index_id": iid, "security_id": sid, "cik": cik, "effective_from": eff,
                             "effective_to": end, "weight": me / total, "company_rank": rk,
                             "rank_date": r["rank_date"], "recon_date": r["recon_date"], "basis": basis,
                             "available_at": mark})
            alive_prev = {s for s in prev if last_session.get(s) is None or last_session[s] > r["recon_date"]}
            cur = set(r["members"])
            adds, dels = cur - alive_prev, alive_prev - cur
            summary.append({"index_id": iid, "rank_date": r["rank_date"], "recon_date": r["recon_date"],
                            "effective_from": eff, "lines": len(cur),
                            "companies": len({v[0] if v[0] is not None else -s for s, v in r["members"].items()}),
                            "additions": len(adds) if prev else None, "deletions": len(dels) if prev else None,
                            "turnover_oneway": (len(adds) / len(cur)) if prev and cur else None,
                            "turnover_cap": (sum(r["members"][s][1] for s in adds) / total) if prev else None,
                            "p1000_cum_share": r.get("p1000"), "eligible_companies": r.get("eligible_companies"),
                            "available_at": mark})
            prev = r["members"]
    return rows, summary


def _write(pq, table, dest) -> None:
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(table, tmp, compression="zstd")
    tmp.replace(dest)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", nargs="?", choices=("build",), default="build")
    ap.parse_args(argv)
    print(json.dumps(build(), default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
