"""Insider net-buying measures (lane OWN, S5.4) -> stage ``insider_ext``. See docs/ALPHA_PANEL_OWNERSHIP.md.

Input: the published ``insider`` stage (Forms 3/4/5 transactions, 2015q1 on, ``available_at`` = EDGAR acceptance)
and ``insider_ext/form144_notices.parquet`` (:mod:`form144`). DuckDB, run under the memory guard.

Trades: open-market purchases (code P) and sales (code S) of non-derivative securities, original filings only
(``4/A`` restatements would double count), shares > 0; value = shares x price (price 0 / missing -> value NULL).

Rule ``cmp-routine-v1`` (Cohen, Malloy and Pomorski 2012, "Decoding Inside Information"): an insider (the filing's
primary reporting owner) is classified at the start of calendar year Y from his open-market trades in Y-3, Y-2 and
Y-1 whose filings were public by the end of that year (``available_at`` < Jan 1 of the following year, so the label
is known on Jan 1 of Y): ``routine`` if he traded in the same calendar month in each of the three years,
``opportunistic`` if he traded in each of the three years but not so, ``unclassified`` (too little history)
otherwise. The label applies to all his trades in Y.

Outputs:
* ``insider_trades.parquet``: one row per qualifying trade from 2019 with the CMP label, officer / director / 10%
  owner flags and the Rule 10b5-1 flag (``aff10b5one``, reported on Forms 4 from 2023-04);
* ``net_buying_monthly.parquet``: per (issuer CIK, month of ``available_at`` UTC) from 2019-01: buy / sell counts,
  shares and value, net shares and value, distinct buyers and sellers, the same for opportunistic and routine
  insiders and for officers + directors, the share of trades flagged 10b5-1, and ``available_at`` = the latest
  trade clock of the month (the measure for month M is complete at the end of M);
* ``form144_monthly.parquet``: per (issuer CIK, month of ``available_at``): 144 notices, distinct filers, and over the
  parsed notices the units and aggregate market value proposed for sale.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any

from . import common as C

STAGE = "insider_ext"
SCHEMA = "atx.alpha-panel.insider-ext/v1"
RULE = "cmp-routine-v1"
FIRST_YEAR = 2019
MEMORY = "400MB"
MODULES = ("insider_measures", "form144", "sec_docs", "common")


def cmp_label(years_months: set[tuple[int, int]], year: int) -> str:
    """Python twin of the SQL rule: ``{(trade year, month)}`` of one insider's public trades -> label for ``year``."""
    prior = [year - 3, year - 2, year - 1]
    if not all(any(y == py for y, _ in years_months) for py in prior):
        return "unclassified"
    for m in range(1, 13):
        if all((py, m) in years_months for py in prior):
            return "routine"
    return "opportunistic"


TRADES_SQL = """
    SELECT accession, form, issuer_cik, owner_cik, transaction_date, timezone('UTC', available_at) AS available_at,
           available_basis, transaction_code,
           shares, CASE WHEN price > 0 THEN price END AS price, CASE WHEN price > 0 THEN shares * price END AS value,
           coalesce(any_officer, false) AS officer, coalesce(any_director, false) AS director,
           coalesce(any_ten_percent_owner, false) AS ten_pct_owner, aff10b5one, officer_title
    FROM read_parquet('{tx}', hive_partitioning = false)
    WHERE table_type = 'non_derivative' AND transaction_code IN ('P', 'S') AND NOT coalesce(is_amendment, false)
      AND shares > 0 AND issuer_cik IS NOT NULL AND owner_cik IS NOT NULL AND transaction_date IS NOT NULL
"""


HIST_SQL = """CREATE OR REPLACE TABLE hist AS
    SELECT DISTINCT owner_cik, year(transaction_date) AS y, month(transaction_date) AS m FROM t
    WHERE available_at < make_timestamptz(year(transaction_date) + 1, 1, 1, 0, 0, 0.0, 'UTC')"""


LABEL_SQL = """CREATE OR REPLACE TABLE lab AS
    WITH yrs AS (SELECT DISTINCT owner_cik, year(transaction_date) AS y FROM t),
    j AS (SELECT yrs.owner_cik, yrs.y, h.y AS hy, h.m FROM yrs
          JOIN hist h ON h.owner_cik = yrs.owner_cik AND h.y BETWEEN yrs.y - 3 AND yrs.y - 1),
    ny AS (SELECT owner_cik, y, count(DISTINCT hy) AS n_years FROM j GROUP BY 1, 2),
    best AS (SELECT owner_cik, y, max(nm) AS best FROM (SELECT owner_cik, y, m, count(DISTINCT hy) AS nm FROM j
                                                       GROUP BY 1, 2, 3) GROUP BY 1, 2)
    SELECT yrs.owner_cik, yrs.y,
           CASE WHEN coalesce(ny.n_years, 0) < 3 THEN 'unclassified' WHEN best.best = 3 THEN 'routine'
                ELSE 'opportunistic' END AS cmp_label
    FROM yrs LEFT JOIN ny USING (owner_cik, y) LEFT JOIN best USING (owner_cik, y)"""


def _side(cond: str, pfx: str) -> str:
    return (f"count(*) FILTER (WHERE transaction_code = 'P' AND {cond}) AS {pfx}n_buy, "
            f"count(*) FILTER (WHERE transaction_code = 'S' AND {cond}) AS {pfx}n_sell, "
            f"coalesce(sum(value) FILTER (WHERE transaction_code = 'P' AND {cond}), 0) "
            f"- coalesce(sum(value) FILTER (WHERE transaction_code = 'S' AND {cond}), 0) AS {pfx}net_value, "
            f"count(DISTINCT owner_cik) FILTER (WHERE transaction_code = 'P' AND {cond}) "
            f"- count(DISTINCT owner_cik) FILTER (WHERE transaction_code = 'S' AND {cond}) AS {pfx}net_buyers")


def monthly_sql(trades: str) -> str:
    """Per (issuer CIK, calendar month UTC of ``available_at``) from ``FIRST_YEAR``: net buying over the labelled trades."""
    return f"""
        SELECT issuer_cik, CAST(date_trunc('month', available_at) AS DATE) AS month,
               count(*) FILTER (WHERE transaction_code = 'P') AS n_buy, count(*) FILTER (WHERE transaction_code = 'S') AS n_sell,
               coalesce(sum(shares) FILTER (WHERE transaction_code = 'P'), 0) AS buy_shares,
               coalesce(sum(shares) FILTER (WHERE transaction_code = 'S'), 0) AS sell_shares,
               coalesce(sum(shares) FILTER (WHERE transaction_code = 'P'), 0)
                 - coalesce(sum(shares) FILTER (WHERE transaction_code = 'S'), 0) AS net_shares,
               coalesce(sum(value) FILTER (WHERE transaction_code = 'P'), 0) AS buy_value,
               coalesce(sum(value) FILTER (WHERE transaction_code = 'S'), 0) AS sell_value,
               coalesce(sum(value) FILTER (WHERE transaction_code = 'P'), 0)
                 - coalesce(sum(value) FILTER (WHERE transaction_code = 'S'), 0) AS net_value,
               count(DISTINCT owner_cik) FILTER (WHERE transaction_code = 'P') AS n_buyers,
               count(DISTINCT owner_cik) FILTER (WHERE transaction_code = 'S') AS n_sellers,
               {_side("cmp_label = 'opportunistic'", 'opp_')}, {_side("cmp_label = 'routine'", 'rtn_')},
               {_side("(officer OR director)", 'od_')},
               avg(CASE WHEN aff10b5one THEN 1.0 WHEN aff10b5one IS NOT NULL THEN 0.0 END) AS share_10b5_1,
               max(available_at) AS available_at, '{RULE}' AS cmp_rule
        FROM read_parquet('{trades}')
        WHERE available_at >= TIMESTAMPTZ '{FIRST_YEAR}-01-01 00:00:00+00'
        GROUP BY 1, 2 ORDER BY 2, 1"""


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    root = C.build_root()
    ins = root / "insider"
    if C.read_json(ins / "manifest.json").get("status") != "complete":
        raise SystemExit("insider stage not published")
    out = C.stage_dir(STAGE)
    con = C.connect(memory=MEMORY, threads=2, temp_dir=root / "_tmp" / "insider_ext", db_file="insider_ext.duckdb")
    tx = (ins / "transactions" / "*" / "*.parquet").as_posix()
    con.execute(f"CREATE TABLE t AS {TRADES_SQL.format(tx=tx)}")
    con.execute(HIST_SQL)
    con.execute(LABEL_SQL)
    n_tr = C.copy_to_parquet(con, f"""
        SELECT t.*, l.cmp_label FROM t LEFT JOIN lab l ON l.owner_cik = t.owner_cik AND l.y = year(t.transaction_date)
        WHERE year(t.transaction_date) >= {FIRST_YEAR}
        ORDER BY t.available_at, t.issuer_cik, t.accession""", out / "insider_trades.parquet")
    trades = (out / "insider_trades.parquet").as_posix()

    n_m = C.copy_to_parquet(con, monthly_sql(trades), out / "net_buying_monthly.parquet")
    f144 = out / "form144_notices.parquet"
    n_144 = None
    if f144.exists():
        n_144 = C.copy_to_parquet(con, f"""
            SELECT issuer_cik, CAST(date_trunc('month', available_at) AS DATE) AS month, count(*) AS n_notices,
                   count(DISTINCT filer_ciks) AS n_filers, count(*) FILTER (WHERE doc_status = 'parsed') AS n_parsed,
                   sum(units_to_sell) AS units_to_sell, sum(aggregate_market_value) AS aggregate_market_value,
                   sum(pct_outstanding) AS pct_outstanding_sum, max(available_at) AS available_at
            FROM read_parquet('{f144.as_posix()}') WHERE issuer_cik IS NOT NULL AND NOT is_amendment
            GROUP BY 1, 2 ORDER BY 2, 1""", out / "form144_monthly.parquet")
    receipt: dict[str, Any] = {"rows": {"insider_trades": n_tr, "net_buying_monthly": n_m, "form144_monthly": n_144}}
    receipt["labels_by_year"] = {str(y): dict(zip(("routine", "opportunistic", "unclassified"), (r, o, u), strict=True))
                                 for y, r, o, u in con.execute(f"""
        SELECT year(transaction_date), count(*) FILTER (WHERE cmp_label = 'routine'),
               count(*) FILTER (WHERE cmp_label = 'opportunistic'), count(*) FILTER (WHERE cmp_label = 'unclassified')
        FROM read_parquet('{trades}') GROUP BY 1 ORDER BY 1""").fetchall()}
    receipt["monthly_by_year"] = {str(y): {"issuer_months": n, "issuers": k, "net_value_usd": v}
                                  for y, n, k, v in con.execute(f"""
        SELECT year(month), count(*), count(DISTINCT issuer_cik), sum(net_value)
        FROM read_parquet('{(out / 'net_buying_monthly.parquet').as_posix()}') GROUP BY 1 ORDER BY 1""").fetchall()}
    if n_144:
        receipt["form144_by_year"] = {str(y): {"notices": n, "parsed": p, "issuers": k} for y, n, p, k in con.execute(f"""
            SELECT year(filing_date), count(*), count(*) FILTER (WHERE doc_status = 'parsed'), count(DISTINCT issuer_cik)
            FROM read_parquet('{f144.as_posix()}') GROUP BY 1 ORDER BY 1""").fetchall()}
    con.close()
    for f in (root / "_tmp" / "insider_ext").glob("insider_ext.duckdb*"):
        f.unlink(missing_ok=True)
    receipt["timings_s"] = {"total": round(time.perf_counter() - t0, 1)}
    publish(receipt)
    return receipt


def publish(receipt: dict[str, Any]) -> None:
    from . import form144 as F
    from . import sec_docs as D

    root = C.build_root()
    payload = {
        "rules": {"trades": "non-derivative P / S, original filings, shares > 0; value = shares x price when price > 0",
                  "cmp": __doc__.split("Rule ``cmp-routine-v1``")[1].split("Outputs:")[0].strip(),
                  "monthly": "month = calendar month (UTC) of available_at; available_at = latest trade clock of the month",
                  "form144": F.__doc__.split("Outputs")[0].strip()},
        "staleness": "monthly event aggregates; a month with no row had no qualifying trade",
        "inputs": {"input_manifests_sha256": {k: C.sha256_file(root / k / "manifest.json")
                                              for k in ("insider", "sec_filings") if (root / k / "manifest.json").exists()}},
        "sources": {"sec_144_fetch_ledger": D.ledger_counts(F.SOURCE), "sec_144_objects_bytes": D.objects_bytes(F.SOURCE)},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.parse_args(argv)
    print(json.dumps(build(), default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
