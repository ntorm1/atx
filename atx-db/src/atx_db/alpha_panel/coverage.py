"""Coverage measurement for the alpha panel and characteristics (writes docs/ALPHA_PANEL_COVERAGE.md).

For every field and calendar year of the coverage window the report gives the share of cells with a
finite value among:
* ``member``: the scorecard universe (research-prior63-usd-adv-topn-v1, ETFs included);
* ``equity``: ``member_equity`` = member, operating company (vendor earnings reaction day within
  400 days), not a vendor index line;
* ``linked``: equity members with a CIK link (issuer fields can only exist there).
"""

from __future__ import annotations

import datetime as dt
import sys
from typing import Any

from . import common as C

PANEL_FIELDS = (
    "close", "raw_close", "volume", "ret", "mkt_ret", "shares_out", "me_line", "me_company", "cik",
    "iv_atm_21d", "iv_atm_63d", "iv_atm_126d", "iv_atm_252d", "earn_recent", "si_shares", "si_dtc",
    "sv_short_volume", "grp_ff12", "grp_ff49", "grp_sic2",
    "at", "at_lag4", "lt", "che", "debt", "be", "be_lag1q", "be_lag1q_lag4", "sale_q", "sale_ttm", "cogs_ttm",
    "xsga_ttm", "gp_ttm", "oi_ttm", "ni_q", "ni_ttm", "ni_q_lag4", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm",
    "prstkc_ttm", "sstk_ttm", "dp_ttm", "txt_q", "txt_q_lag4", "shrs_q", "shrs_q_lag4", "noa", "noa_lag4", "sue",
    "fscore",
)
ISSUER = set(PANEL_FIELDS[PANEL_FIELDS.index("grp_ff12"):]) | {"me_company"}


def _cols(con, glob: str) -> list[str]:
    return [r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{glob}', hive_partitioning = false)").fetchall()]


def measure(con, glob: str, fields: list[str], start: dt.date) -> dict[str, dict[str, dict[str, float]]]:
    avail = set(_cols(con, glob))
    fields = [f for f in fields if f in avail]
    has_eq = "member_equity" in avail
    out: dict[str, dict[str, dict[str, float]]] = {}
    for f in fields:
        q = f'"{f}"'
        test = f"({q} IS NOT NULL AND (NOT isfinite(TRY_CAST({q} AS DOUBLE)) IS NOT TRUE))" if f != "cik" else "cik IS NOT NULL"
        parts = [f"avg(CASE WHEN {test} THEN 1.0 ELSE 0.0 END) FILTER (WHERE member) AS m"]
        if has_eq:
            parts.append(f"avg(CASE WHEN {test} THEN 1.0 ELSE 0.0 END) FILTER (WHERE member_equity) AS e")
            parts.append(f"avg(CASE WHEN {test} THEN 1.0 ELSE 0.0 END) FILTER (WHERE member_equity AND cik IS NOT NULL) AS l")
        rows = con.execute(f"""
            SELECT year(session_date) AS y, {', '.join(parts)}
            FROM read_parquet('{glob}', hive_partitioning = false)
            WHERE session_date >= DATE '{start}' GROUP BY 1 ORDER BY 1""").fetchall()
        out[f] = {str(r[0]): {k: (round(float(v), 4) if v is not None else None) for k, v in zip(("member", "equity", "linked"), r[1:], strict=False)}
                  for r in rows}
    return out


def universe(con, glob: str, start: dt.date) -> dict[str, Any]:
    rows = con.execute(f"""
        SELECT year(session_date), count(DISTINCT session_date), avg(n_m), avg(n_e), min(n_m), min(n_e)
        FROM (SELECT session_date, sum(member::INT) n_m, sum(member_equity::INT) n_e
              FROM read_parquet('{glob}', hive_partitioning = false) WHERE session_date >= DATE '{start}' GROUP BY 1)
        GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(y): {"sessions": s, "members_mean": round(a, 1), "equity_mean": round(b, 1), "members_min": c,
                     "equity_min": d} for y, s, a, b, c, d in rows}


def _table(cov: dict[str, dict[str, dict[str, float]]], key: str, years: list[str]) -> list[str]:
    lines = ["| field | " + " | ".join(years) + " |", "|---|" + "---:|" * len(years)]
    for f, per in cov.items():
        cells = []
        for y in years:
            v = per.get(y, {}).get(key)
            cells.append("" if v is None else f"{100 * v:.1f}")
        lines.append(f"| `{f}` | " + " | ".join(cells) + " |")
    return lines


def run(start: dt.date = C.COVERAGE_START) -> dict[str, Any]:
    root = C.build_root()
    con = C.connect(memory="600MB", threads=2)
    panel = (root / "panel" / "*" / "*.parquet").as_posix()
    chars = (root / "characteristics" / "*" / "*.parquet").as_posix()
    res: dict[str, Any] = {"window_start": str(start), "universe": universe(con, panel, start)}
    res["panel"] = measure(con, panel, list(PANEL_FIELDS), start)
    if any((root / "characteristics").glob("*/*.parquet")):
        # characteristics files carry member only; join member_equity/cik from the panel
        con.execute(f"""CREATE TEMP VIEW cj AS SELECT c.*, p.member_equity, p.cik
            FROM read_parquet('{chars}', hive_partitioning = false) c
            JOIN read_parquet('{panel}', hive_partitioning = false) p USING (session_date, security_id)""")
        con.execute(f"COPY (SELECT * FROM cj WHERE session_date >= DATE '{start}') TO '{(root / '_tmp' / 'cj.parquet').as_posix()}' (FORMAT PARQUET)")
        cfields = [c for c in _cols(con, (root / '_tmp' / 'cj.parquet').as_posix())
                   if c not in ("session_date", "security_id", "member", "member_equity", "cik")]
        res["characteristics"] = measure(con, (root / "_tmp" / "cj.parquet").as_posix(), cfields, start)
    C.write_json_atomic(root / "coverage.json", res)
    years = sorted(res["universe"])
    doc = [
        "# Alpha panel coverage", "",
        f"Measured {dt.datetime.now(dt.UTC):%Y-%m-%d %H:%M} UTC from `{root}` over sessions from {start}. "
        "Percent of cells with a finite value. `member` is the scorecard universe (top 3,000 by prior 63-session "
        "dollar volume, ETFs included). `equity` restricts it to operating companies (vendor earnings reaction day "
        "within 400 days) that are not index lines. Issuer fields are also shown on `linked` equity members "
        "(a CIK link exists), which separates identity gaps from filing gaps.", "",
        "## Universe", "", "| year | sessions | members/day | equity members/day | min members | min equity |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for y in years:
        u = res["universe"][y]
        doc.append(f"| {y} | {u['sessions']} | {u['members_mean']} | {u['equity_mean']} | {u['members_min']} | {u['equity_min']} |")
    doc += ["", "## Panel fields on equity members (%)", ""] + _table(res["panel"], "equity", years)
    issuer = {k: v for k, v in res["panel"].items() if k in ISSUER}
    doc += ["", "## Issuer fields on CIK-linked equity members (%)", ""] + _table(issuer, "linked", years)
    doc += ["", "## Panel fields on all scorecard members (%)", ""] + _table(res["panel"], "member", years)
    if "characteristics" in res:
        doc += ["", "## Characteristics on equity members (%)", ""] + _table(res["characteristics"], "equity", years)
    path = C.PACKAGE_ROOT / "docs" / "ALPHA_PANEL_COVERAGE.md"
    path.write_text("\n".join(doc) + "\n", encoding="utf-8")
    return {"doc": str(path), "json": str(root / "coverage.json")}


def main() -> int:
    print(run(), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
