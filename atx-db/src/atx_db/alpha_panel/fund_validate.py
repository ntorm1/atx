"""Stage F validation suite (tier1-v3 S4.1-S4.3, S4.7) and the mega-alpha exit measurement.

Writes ``<build root>/validation/fundamentals.json`` (one key per check, merged on rerun). Every check reads the
published stage (env ``ATX_FUND_STAGE``) and, where named, raw inputs; run under the memory guard::

    python -m atx_db.alpha_panel.fund_validate all            # every check below
    python -m atx_db.alpha_panel.fund_validate <check> [...]  # ttm_gaps fx coverage benchmark balance cutoff exit

Checks:

* ``ttm_gaps`` (S4.1): among events with clock in 2020-2022 and currency USD that carry the item's discrete quarter,
  the share without its TTM, for sale / oi / gp; ``excl_lt4q`` drops events of issuers with fewer than 4 distinct
  quarter period ends carrying that quarter up to the event.
* ``fx`` (S4.3): events whose reporting currency is not USD but covered by the FX table: fx_converted share and the
  finite share of the core money items (also over USD events, for scale).
* ``coverage`` (S4.2): Compustat-analog item coverage on the top 3000 issuers by market capitalisation per fiscal
  year FY2015-FY2025 (Compustat fyear: period_end year, minus 1 when the month is before June). Observation = the
  issuer's first annual-form event (10-K / 10-KT / 20-F / 40-F, not /A) with fiscal_period FY for that period end;
  market cap = sum over the CIK's linked lines (identity/link_table valid at period_end) of close x shares_vendor at
  the last session on or before period_end (prices stages). Per item: finite share, structural share (template /
  SIC applicability), ``covered`` = (finite + structural) / n (the brief's basis) and ``ex_structural`` = finite /
  (n - structural); the item passes when every fiscal year is >= 0.90 on the basis.
* ``benchmark`` (S4.7): 10,000 FSDS NUM cells stratified by (item, fiscal year 2016-2025) -- own-period, non-
  dimensional us-gaap facts of 10-K (4 quarters) and 10-Q (1 quarter or instant) filings in USD, the highest-priority
  benchmark tag present -- compared with the filing's own event (period_end within 16 days of the month-rounded
  ddate): ``mapped`` = our value exists, ``match`` = within 0.5%.
* ``balance`` (S4.7): per (cik, period_end) the latest event with at: |at - (lt + temporary equity + seq + mib)| <=
  max(0.5% x |at|, $1M); residual classes counted.
* ``cutoff`` (S4.7/S4.6): 200 random events (seeded); each issuer is rebuilt from the facts filed before (event
  available_at + 1 s) and compared with ``fund_asof`` views at that instant (events, catalog, quarterly history):
  every column of every visible row must be identical.
* ``exit``: the mega-alpha section 2 fields on member cells 2020-2022 (``_tmp/panel_member`` member = true, linked via
  ``identity/link_table`` valid on the session), each joined as of 22:00 UTC of the previous session to the issuer's
  latest visible event (400-day staleness on session - period_end); bases ``linked`` and ``linked_usd`` (visible event
  currency USD), each excluding cells whose visible fin_template makes the item structural.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import random
import sys
import time
from pathlib import Path
from typing import Any

from . import common
from . import fund_asof as fa
from . import fund_catalog as fcat
from . import fund_extract as fx
from . import fund_fx as ffx
from . import fund_items as fi
from . import fundamentals as fu

TARGET = 0.90
FYEARS = tuple(range(2015, 2026))
TOP_N = 3000
MEM = "350MB"

# Compustat analog of every v9 event column counted in the S4.2 coverage (annual basis; quarterly columns are
# reported separately and not counted)
EVENT_MNEMONICS = {
    "at": "AT", "lt": "LT", "che": "CHE", "debt": "DEBT(DLC+DLTT)", "be": "BE", "seq": "SEQ", "sale_ttm": "SALE",
    "cogs_ttm": "COGS", "xsga_ttm": "XSGA", "gp_ttm": "GP", "oi_ttm": "OIADP", "ni_ttm": "NI", "cfo_ttm": "OANCF",
    "capx_ttm": "CAPX", "xrd_ttm": "XRD", "dvc_ttm": "DVC", "prstkc_ttm": "PRSTKC", "sstk_ttm": "SSTK", "dp_ttm": "DP",
    "shrs_q": "CSHO", "noa": "NOA", "invt": "INVT", "rect": "RECT", "ppe": "PPENT", "xint_ttm": "XINT",
    "ebitda_ttm": "OIBDP", "dvt_ttm": "DVT", "act": "ACT", "lct": "LCT", "ap": "AP", "drev": "DRC+DRLT",
    "ppegt": "PPEGT", "gdwl": "GDWL", "intan": "INTAN", "mib": "MIB", "pstk": "PSTK",
}
QUARTERLY_COLUMNS = ("sale_q", "cogs_q", "xsga_q", "gp_q", "oi_q", "ni_q", "txt_q", "xint_q", "dp_q", "ebitda_q",
                     "dvt_q")
EXIT_FIELDS = {"gp_ttm": 0.90, "oi_ttm": 0.92, "xrd_ttm": 0.95, "capx_ttm": 0.95, "txt_q": 0.95, "sale_ttm": 0.96,
               "shrs_q": 0.97}
FX_ITEMS = ("at", "lt", "seq", "be", "che", "sale_ttm", "ni_ttm", "cfo_ttm", "oi_ttm", "capx_ttm")
ANNUAL_FORMS = ("10-K", "10-KT", "20-F", "40-F")

# FSDS benchmark: item -> (qtrs, tags in the stage's priority order, uom)
BENCH_ITEMS: dict[str, tuple[int, tuple[str, ...], str]] = {
    "at": (0, ("Assets",), "USD"), "lt": (0, ("Liabilities",), "USD"), "seq": (0, ("StockholdersEquity",), "USD"),
    "act": (0, ("AssetsCurrent",), "USD"), "lct": (0, ("LiabilitiesCurrent",), "USD"),
    "invt": (0, ("InventoryNet",), "USD"), "rect": (0, ("AccountsReceivableNetCurrent",), "USD"),
    "ppe": (0, ("PropertyPlantAndEquipmentNet",), "USD"), "gdwl": (0, ("Goodwill",), "USD"),
    "ap": (0, ("AccountsPayableCurrent",), "USD"), "re": (0, ("RetainedEarningsAccumulatedDeficit",), "USD"),
    "ch": (0, ("CashAndCashEquivalentsAtCarryingValue",), "USD"), "cstk": (0, ("CommonStockValue",), "USD"),
    "caps": (0, ("AdditionalPaidInCapital",), "USD"), "lse": (0, ("LiabilitiesAndStockholdersEquity",), "USD"),
    "tstk": (0, ("TreasuryStockValue",), "USD"), "acominc": (0, ("AccumulatedOtherComprehensiveIncomeLossNetOfTax",), "USD"),
    "sale_ttm": (4, ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"), "USD"),
    "ni_ttm": (4, ("NetIncomeLoss",), "USD"), "oi_ttm": (4, ("OperatingIncomeLoss",), "USD"),
    "gp_ttm": (4, ("GrossProfit",), "USD"), "cogs_ttm": (4, ("CostOfGoodsAndServicesSold", "CostOfRevenue"), "USD"),
    "cfo_ttm": (4, ("NetCashProvidedByUsedInOperatingActivities",), "USD"),
    "capx_ttm": (4, ("PaymentsToAcquirePropertyPlantAndEquipment",), "USD"),
    "xrd_ttm": (4, ("ResearchAndDevelopmentExpense",), "USD"), "txt_ttm": (4, ("IncomeTaxExpenseBenefit",), "USD"),
    "xsga_ttm": (4, ("SellingGeneralAndAdministrativeExpense",), "USD"), "xint_ttm": (4, ("InterestExpense",), "USD"),
    "dp_ttm": (4, ("DepreciationDepletionAndAmortization",), "USD"),
    "dvc_ttm": (4, ("PaymentsOfDividendsCommonStock",), "USD"),
    "prstkc_ttm": (4, ("PaymentsForRepurchaseOfCommonStock",), "USD"),
    "stkco_ttm": (4, ("ShareBasedCompensation",), "USD"),
    "ivncf_ttm": (4, ("NetCashProvidedByUsedInInvestingActivities",), "USD"),
    "fincf_ttm": (4, ("NetCashProvidedByUsedInFinancingActivities",), "USD"),
    "epspx_ttm": (4, ("EarningsPerShareBasic",), "USD/shares"), "epsfx_ttm": (4, ("EarningsPerShareDiluted",), "USD/shares"),
    "cshpri": (4, ("WeightedAverageNumberOfSharesOutstandingBasic",), "shares"),
    "sale_q": (1, ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"), "USD"),
    "ni_q": (1, ("NetIncomeLoss",), "USD"), "oi_q": (1, ("OperatingIncomeLoss",), "USD"),
    "gp_q": (1, ("GrossProfit",), "USD"), "cogs_q": (1, ("CostOfGoodsAndServicesSold", "CostOfRevenue"), "USD"),
    "txt_q": (1, ("IncomeTaxExpenseBenefit",), "USD"), "xint_q": (1, ("InterestExpense",), "USD"),
}
BENCH_CELLS = 10_000
BENCH_TOL = 0.005
BALANCE_REL, BALANCE_ABS = 0.005, 1e6
CUTOFF_EVENTS, CUTOFF_SEED = 200, 20260929


def out_path() -> Path:
    return common.build_root() / "validation" / "fundamentals.json"


def _stage() -> Path:
    return fu.fund_dir()


def _q(p: Path) -> str:
    return p.as_posix()


def _structural_sql(item: str, tmpl_col: str = "fin_template", sic_col: str = "sic_in_force") -> str:
    """SQL predicate: the item is structurally absent for the row's template (v9 items) or SIC / template (catalog)."""
    parts = []
    snan = fu.structural_na().get(item)
    if snan:
        parts.append(f"{tmpl_col} IN ({', '.join(repr(t) for t in snan)})")
    if item in fcat.BY_COL:
        it = fcat.BY_COL[item]
        if it.sic:
            ranges = " OR ".join(f"({sic_col} BETWEEN {lo} AND {hi})" for lo, hi in it.sic)
            parts.append(f"NOT coalesce({ranges}, false)")
        tmpls = [t for t, cols in fi.CAT_STRUCTURAL.items() if item in cols]
        if tmpls:
            parts.append(f"{tmpl_col} IN ({', '.join(repr(t) for t in tmpls)})")
    return "(" + " OR ".join(parts) + ")" if parts else "false"


# ---------------------------------------------------------------------------------------------------------------
# S4.1 TTM gaps, S4.3 FX
# ---------------------------------------------------------------------------------------------------------------

def ttm_gaps(con, events: Path) -> dict[str, Any]:
    out: dict[str, Any] = {"rule": "2020-2022 clock, currency USD, quarter value present; excl_lt4q drops events whose "
                                   "issuer has < 4 distinct quarter period ends with that quarter up to the event"}
    for it in ("sale", "oi", "gp"):
        n, m, n4, m4 = con.execute(f"""
            WITH x AS (
                SELECT *, count(DISTINCT CASE WHEN {it}_q IS NOT NULL THEN period_end END)
                           OVER (PARTITION BY cik ORDER BY clock_utc, accession
                                 ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS nq
                FROM read_parquet('{_q(events)}'))
            SELECT count(*), count(*) FILTER (WHERE {it}_ttm IS NULL),
                   count(*) FILTER (WHERE nq >= 4), count(*) FILTER (WHERE nq >= 4 AND {it}_ttm IS NULL)
            FROM x WHERE year(clock_utc) BETWEEN 2020 AND 2022 AND currency = 'USD' AND {it}_q IS NOT NULL
        """).fetchone()
        out[it] = {"events_with_q": n, "missing_ttm": m, "share": round(m / n, 4) if n else None,
                   "events_with_q_excl_lt4q": n4, "missing_ttm_excl_lt4q": m4,
                   "share_excl_lt4q": round(m4 / n4, 4) if n4 else None, "pass_lt_1pct": bool(n4 and m4 / n4 < 0.01)}
    return out


def fx_check(con, events: Path) -> dict[str, Any]:
    table = ffx.load_optional()
    if table is None:
        return {"error": "no fx_daily.parquet"}
    covered = ", ".join(repr(c) for c in sorted(table.days))
    res: dict[str, Any] = {"covered_currencies": sorted(table.days)}
    for name, where in (("non_usd_covered", f"currency <> 'USD' AND currency IN ({covered})"),
                        ("non_usd_not_covered", f"currency <> 'USD' AND currency NOT IN ({covered})"),
                        ("usd", "currency = 'USD'")):
        row = con.execute(f"""
            SELECT count(*), avg(fx_converted::INT), avg((available_at > clock_utc)::INT),
                   {", ".join(f'avg(coalesce(isfinite("{c}"), false)::INT)' for c in FX_ITEMS)},
                   avg((coalesce(isfinite("at"), false) AND coalesce(isfinite(ni_ttm), false))::INT)
            FROM read_parquet('{_q(events)}') WHERE clock_utc >= TIMESTAMP '2016-01-01' AND {where}""").fetchone()
        res[name] = {"events_2016_plus": row[0], "fx_converted": _r(row[1]), "available_at_after_clock": _r(row[2]),
                     "finite": {c: _r(v) for c, v in zip(FX_ITEMS, row[3:3 + len(FX_ITEMS)])},
                     "at_and_ni_ttm_finite": _r(row[-1])}
    nc = res["non_usd_covered"]
    res["pass_95"] = bool(nc["finite"]["at"] is not None and nc["finite"]["at"] >= 0.95)
    res["by_currency"] = {r[0]: {"events": r[1], "fx_converted": _r(r[2]), "at_finite": _r(r[3])} for r in con.execute(
        f"""SELECT currency, count(*), avg(fx_converted::INT), avg(coalesce(isfinite("at"), false)::INT)
            FROM read_parquet('{_q(events)}') WHERE clock_utc >= TIMESTAMP '2016-01-01' AND currency <> 'USD'
            GROUP BY 1 ORDER BY 2 DESC""").fetchall()}
    return res


def _r(v: Any, nd: int = 4) -> Any:
    return None if v is None else round(float(v), nd)


# ---------------------------------------------------------------------------------------------------------------
# S4.2 coverage on the top 3000 by market cap
# ---------------------------------------------------------------------------------------------------------------

def _prices_glob() -> list[str]:
    root = common.build_root()
    return [_q(p) for p in sorted((root / "prices_history").glob("year=*/prices.parquet"))
            + sorted((root / "prices").glob("year=*/*.parquet"))]


def coverage(con, stage: Path) -> dict[str, Any]:
    ev, cat = _q(stage / "events.parquet"), _q(stage / "catalog.parquet")
    lt = _q(common.build_root() / "identity" / "link_table.parquet")
    forms = ", ".join(repr(f) for f in ANNUAL_FORMS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE obs AS
        SELECT * FROM (
            SELECT e.*, {", ".join(f'c."{x}"' for x in fcat.CAT_COLUMNS)},
                   CASE WHEN month(e.period_end) >= 6 THEN year(e.period_end) ELSE year(e.period_end) - 1 END AS fyear
            FROM read_parquet('{ev}') e LEFT JOIN read_parquet('{cat}') c USING (cik, accession)
            WHERE e.form IN ({forms}) AND coalesce(e.fiscal_period, 'FY') = 'FY')
        WHERE fyear BETWEEN {FYEARS[0]} AND {FYEARS[-1]}
        QUALIFY row_number() OVER (PARTITION BY cik, period_end ORDER BY available_at, accession) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE obs_lines AS
        SELECT DISTINCT o.cik, o.period_end, l.security_id
        FROM (SELECT DISTINCT cik, period_end FROM obs) o
        JOIN read_parquet('{lt}') l ON l.cik = o.cik AND o.period_end BETWEEN l.valid_from - INTERVAL 7 DAY
                                     AND coalesce(l.valid_to, DATE '2100-01-01')""")
    prices = ", ".join(f"'{p}'" for p in _prices_glob())
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE px AS
        SELECT security_id, session_date, close * shares_vendor AS mcap
        FROM read_parquet([{prices}], union_by_name = true)
        WHERE security_id IN (SELECT security_id FROM obs_lines) AND close > 0 AND shares_vendor > 0""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE mcap AS
        SELECT l.cik, l.period_end, sum(p.mcap) AS mcap
        FROM obs_lines l ASOF JOIN px p ON p.security_id = l.security_id AND p.session_date <= l.period_end
        WHERE p.session_date >= l.period_end - INTERVAL 10 DAY
        GROUP BY 1, 2""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE top AS
        SELECT o.* FROM obs o JOIN mcap m USING (cik, period_end)
        QUALIFY row_number() OVER (PARTITION BY o.fyear ORDER BY m.mcap DESC, o.cik) <= {TOP_N}""")
    n_by_year = dict(con.execute("SELECT fyear, count(*) FROM top GROUP BY 1 ORDER BY 1").fetchall())
    items = list(EVENT_MNEMONICS) + list(fcat.CAT_COLUMNS) + list(QUARTERLY_COLUMNS)
    per_item: dict[str, Any] = {}
    for item in items:
        s_sql = _structural_sql(item)
        rows = con.execute(f"""
            SELECT fyear, count(*), sum(coalesce(isfinite("{item}"), false)::INT),
                   sum(("{item}" IS NULL AND {s_sql})::INT)
            FROM top GROUP BY 1 ORDER BY 1""").fetchall()
        years = {}
        for y, n, f, s in rows:
            years[str(y)] = {"n": n, "finite": _r(f / n), "structural": _r(s / n),
                             "covered": _r((f + s) / n), "ex_structural": _r(f / (n - s)) if n > s else None}
        cov_min = min((v["covered"] for v in years.values()), default=None)
        exs = [v["ex_structural"] for v in years.values() if v["ex_structural"] is not None]
        mnem = EVENT_MNEMONICS.get(item) or (fcat.BY_COL[item].mnemonic if item in fcat.BY_COL else item.upper())
        per_item[item] = {
            "mnemonic": mnem, "seed_item_id": fcat.BY_COL[item].seed if item in fcat.BY_COL else None,
            "counted": item not in QUARTERLY_COLUMNS,
            "covered_min_year": cov_min, "ex_structural_min_year": min(exs) if exs else None,
            "pass_covered": bool(cov_min is not None and cov_min >= TARGET),
            "pass_ex_structural": bool(exs and min(exs) >= TARGET), "years": years}
    counted = [k for k, v in per_item.items() if v["counted"]]
    return {"rule": __doc__.split("* ``coverage``")[1].split("* ``benchmark``")[0].strip(),
            "observations_by_fyear": {str(k): v for k, v in n_by_year.items()},
            "items_counted": len(counted),
            "items_pass_covered": sum(per_item[k]["pass_covered"] for k in counted),
            "items_pass_ex_structural": sum(per_item[k]["pass_ex_structural"] for k in counted),
            "items": per_item}


# ---------------------------------------------------------------------------------------------------------------
# S4.7 FSDS benchmark and A = L + E
# ---------------------------------------------------------------------------------------------------------------

def benchmark(con, stage: Path) -> dict[str, Any]:
    ev, cat = _q(stage / "events.parquet"), _q(stage / "catalog.parquet")
    num = _q(common.FSDS_DIR / "num" / "*.parquet")
    sub = _q(common.FSDS_DIR / "sub" / "*.parquet")
    spec_rows = [(item, q, rank, tag, uom) for item, (q, tags, uom) in BENCH_ITEMS.items()
                 for rank, tag in enumerate(tags)]
    con.execute("CREATE OR REPLACE TEMP TABLE spec (item VARCHAR, qtrs INTEGER, rnk INTEGER, tag VARCHAR, uom VARCHAR)")
    con.executemany("INSERT INTO spec VALUES (?, ?, ?, ?, ?)", spec_rows)
    tags = ", ".join(sorted({f"'{r[3]}'" for r in spec_rows}))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE cells AS
        WITH s AS (
            SELECT adsh, TRY_CAST(cik AS BIGINT) AS cik, form, period, fy FROM read_parquet('{sub}')
            WHERE form IN ('10-K', '10-Q') AND fy BETWEEN 2016 AND 2025),
        n AS (
            SELECT n.adsh, n.tag, n.ddate, n.qtrs, n.uom, max(CAST(n.value AS DOUBLE)) AS value, count(*) AS k
            FROM read_parquet('{num}') n
            WHERE n.tag IN ({tags}) AND n.version LIKE 'us-gaap/%' AND n.segments IS NULL
              AND (n.coreg IS NULL OR n.coreg = '') AND n.value IS NOT NULL
            GROUP BY ALL HAVING count(*) = 1),
        c AS (
            SELECT s.cik, s.fy, s.form, n.*, sp.item, sp.rnk
            FROM n JOIN s USING (adsh) JOIN spec sp ON sp.tag = n.tag AND sp.uom = n.uom
              AND sp.qtrs = n.qtrs AND n.ddate = s.period
              AND ((sp.qtrs = 4 AND s.form = '10-K') OR (sp.qtrs = 1 AND s.form = '10-Q') OR sp.qtrs = 0))
        SELECT * FROM c
        QUALIFY rnk = min(rnk) OVER (PARTITION BY adsh, item, ddate, qtrs)""")
    strata = con.execute("SELECT count(DISTINCT (item, fy)) FROM cells").fetchone()[0]
    quota = max(1, math.ceil(BENCH_CELLS / max(strata, 1)))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE sample AS
        SELECT * FROM cells
        QUALIFY row_number() OVER (PARTITION BY item, fy ORDER BY hash(adsh || tag || CAST(ddate AS VARCHAR))) <= {quota}""")
    # ours: the accession's own event (catalog joined), period_end within 16 days of the month-rounded ddate
    cols = sorted(BENCH_ITEMS)
    cases = " ".join(f"WHEN '{c}' THEN x.\"{c}\"" for c in cols)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE bm AS
        SELECT s.item, s.fy, s.form, s.value AS fsds, CASE s.item {cases} END AS ours, x.currency, x.accession IS NOT NULL AS has_event
        FROM sample s
        LEFT JOIN (SELECT e.*, {", ".join(f'c."{c}"' for c in fcat.CAT_COLUMNS)}
                   FROM read_parquet('{ev}') e LEFT JOIN read_parquet('{cat}') c USING (cik, accession)) x
          ON x.cik = s.cik AND x.accession = s.adsh AND abs(datediff('day', x.period_end, s.ddate)) <= 16""")
    tol = f"abs(ours - fsds) <= {BENCH_TOL} * greatest(abs(fsds), 1e-9)"
    n, has_ev, mapped, match = con.execute(f"""
        SELECT count(*), sum(has_event::INT), sum((ours IS NOT NULL)::INT), sum((ours IS NOT NULL AND {tol})::INT)
        FROM bm""").fetchone()
    by_item = {r[0]: {"cells": r[1], "mapped": _r(r[2] / r[1]), "match_of_mapped": _r(r[3] / r[2]) if r[2] else None}
               for r in con.execute(f"""SELECT item, count(*), sum((ours IS NOT NULL)::INT),
                                              sum((ours IS NOT NULL AND {tol})::INT) FROM bm GROUP BY 1 ORDER BY 1""").fetchall()}
    misses = [dict(zip(("item", "fy", "form", "fsds", "ours", "currency"), r)) for r in con.execute(
        f"SELECT item, fy, form, fsds, ours, currency FROM bm WHERE ours IS NOT NULL AND NOT {tol} LIMIT 25").fetchall()]
    return {"rule": __doc__.split("* ``benchmark``")[1].split("* ``balance``")[0].strip(), "cells": n, "strata": strata,
            "quota_per_stratum": quota, "with_event": has_ev, "mapped": mapped, "matched": match,
            "mapped_share": _r(mapped / n), "match_share_of_mapped": _r(match / mapped) if mapped else None,
            "pass": bool(n and mapped / n >= 0.95 and match / mapped >= 0.98), "by_item": by_item,
            "mismatch_examples": misses}


def balance(con, stage: Path) -> dict[str, Any]:
    ev, cat = _q(stage / "events.parquet"), _q(stage / "catalog.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE bal AS
        SELECT e.cik, e.period_end, e."at", e.lt, e.seq, e.mib, c.mibt, c.lse, c.teq, e.currency, e.fin_template
        FROM read_parquet('{ev}') e LEFT JOIN read_parquet('{cat}') c USING (cik, accession)
        WHERE e."at" IS NOT NULL AND e.period_end >= DATE '2016-01-01'
        QUALIFY row_number() OVER (PARTITION BY e.cik, e.period_end ORDER BY e.available_at DESC, e.accession DESC) = 1""")
    tol = f"greatest({BALANCE_REL} * abs(\"at\"), {BALANCE_ABS})"
    rhs = "(lt + coalesce(mibt, 0) + coalesce(teq, seq + coalesce(mib, 0)))"
    n, ok, no_lt = con.execute(f"""
        SELECT count(*), sum((lt IS NOT NULL AND abs("at" - {rhs}) <= {tol})::INT), sum((lt IS NULL)::INT) FROM bal""").fetchone()
    classes = dict(con.execute(f"""
        SELECT CASE WHEN lt IS NULL THEN 'lt_missing'
                    WHEN seq IS NULL THEN 'seq_missing'
                    WHEN lse IS NOT NULL AND abs(lse - "at") > {tol} THEN 'lse_differs_from_at (reported totals disagree)'
                    WHEN abs("at" - (lt + coalesce(mibt, 0) + seq + coalesce(mib, 0))) <= {tol}
                         THEN 'teq_vs_seq_plus_mib_inconsistent'
                    WHEN mibt IS NULL AND lse IS NOT NULL AND abs(lse - {rhs}) > {tol}
                         THEN 'unreported_mezzanine_or_other_equity_component'
                    ELSE 'other' END, count(*)
        FROM bal WHERE NOT (lt IS NOT NULL AND abs("at" - {rhs}) <= {tol}) GROUP BY 1 ORDER BY 2 DESC""").fetchall())
    return {"rule": "per (cik, period_end >= 2016) the latest event with at: |at - (lt + temporary equity + equity incl. "
                    "NCI)| <= max(0.5% |at|, $1M); equity incl. NCI = teq (reported), else seq + mib",
            "issuer_periods": n, "ok": ok, "ok_share": _r(ok / n), "pass_99": bool(n and ok / n >= 0.99),
            "lt_missing": no_lt, "residual_classes": classes}


# ---------------------------------------------------------------------------------------------------------------
# S4.7 cutoff rebuild through the S4.6 as-of views
# ---------------------------------------------------------------------------------------------------------------

def _norm(v: Any) -> Any:
    if isinstance(v, float) and math.isnan(v):
        return "nan"
    if isinstance(v, dt.datetime) and v.tzinfo is not None:
        return v.astimezone(dt.UTC).replace(tzinfo=None)
    return v


def cutoff(con, stage: Path, n_events: int = CUTOFF_EVENTS, seed: int = CUTOFF_SEED) -> dict[str, Any]:
    ev_path = stage / "events.parquet"
    rows = con.execute(f"SELECT cik, accession, available_at FROM read_parquet('{_q(ev_path)}') "
                       "WHERE clock_utc >= TIMESTAMP '2016-01-01' ORDER BY cik, accession").fetchall()
    rng = random.Random(seed)
    picks = rng.sample(rows, min(n_events, len(rows)))
    ciks = sorted({p[0] for p in picks})
    fx_table = ffx.load_optional()
    cf_glob = _q(fx.out_dir() / "batch-[0-9][0-9][0-9][0-9].parquet")
    fu._register_static(con)
    ev_cols = [f.name for f in fu.EVENT_SCHEMA] + ["nonreliance_402_at"]
    cat_cols = [f.name for f in fu.CATALOG_SCHEMA]
    hist_cols = ["cik", "item", "period_end", "fiscal_period", "accession", "value", "currency", "available_at",
                 "zero_filled"]
    res: dict[str, Any] = {"events_sampled": len(picks), "issuers": len(ciks), "seed": seed, "compared_rows": 0,
                           "differences": 0, "examples": []}
    t0 = time.perf_counter()
    for i in range(0, len(ciks), 25):
        chunk = ciks[i:i + 25]
        base = fu._base_sql(cf_glob, f"AND CAST(b.cik AS BIGINT) IN ({', '.join(map(str, chunk))})")
        sic_by, pre_by, basis_by, _ = fu._issuer_extras(con, base)
        full_rows = {cik: r for cik, r in fu.stream_issuers(con, base)}
        for cik, acc, avail in [p for p in picks if p[0] in chunk]:
            cut = avail + dt.timedelta(seconds=1)
            part = [r for r in full_rows.get(cik, []) if r[0] < cut]
            h: list[tuple] = []
            counters: dict[str, int] = {}
            rebuilt = [e for e in fi.issuer_events(cik, part, fu.EMIT_FROM, counters, sic_by.get(cik), pre_by.get(cik),
                                                   h, fx=fx_table) if e["available_at"] < cut]
            view = con.execute(fa.events_as_of_sql(stage, cut, ciks=[cik])).fetchall()
            names = [d[0] for d in con.description]
            cview = con.execute(
                f"SELECT * FROM read_parquet('{_q(stage / 'catalog.parquet')}') WHERE cik = {cik} "
                f"AND available_at < TIMESTAMP '{cut.isoformat(sep=' ')}' ORDER BY cik, available_at, clock_utc, accession"
            ).fetchall()
            cnames = [d[0] for d in con.description]
            hview = con.execute(
                f"SELECT {', '.join(hist_cols)} FROM read_parquet('{_q(stage / 'quarterly_history.parquet')}') "
                f"WHERE cik = {cik} AND available_at < TIMESTAMP '{cut.isoformat(sep=' ')}'").fetchall()
            got = {r[names.index("accession")]: dict(zip(names, r)) for r in view}
            cgot = {r[cnames.index("accession")]: dict(zip(cnames, r)) for r in cview}
            diffs = []
            if set(got) != {e["accession"] for e in rebuilt}:
                diffs.append(("event_set", len(got), len(rebuilt)))
            for e in rebuilt:
                g, cg = got.get(e["accession"]), cgot.get(e["accession"])
                if g is None or cg is None:
                    continue
                res["compared_rows"] += 1
                for c in ev_cols:
                    if c == "nonreliance_402_at":
                        continue  # joined at finalize from sec_filings (not part of the Company Facts rebuild)
                    if _norm(_f(e.get(c), fu.EVENT_SCHEMA, c)) != _norm(g.get(c)):
                        diffs.append((e["accession"], c, e.get(c), g.get(c)))
                for c in cat_cols:
                    if _norm(_f(e.get(c), fu.CATALOG_SCHEMA, c)) != _norm(cg.get(c)):
                        diffs.append((e["accession"], c, e.get(c), cg.get(c)))
            hv = {tuple(_norm(x) for x in (r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8])) for r in hview}
            hb = {(cik, item, pe, fp, accn, value, cur, clock, zero) for (item, pe, fp, accn, value, clock, zero, cur)
                  in [tuple(_norm(x) for x in row) for row in h]}
            if hb != hv:
                diffs.append(("history", len(hb - hv), len(hv - hb)))
            res["compared_rows"] += len(hv)
            if diffs:
                res["differences"] += len(diffs)
                if len(res["examples"]) < 10:
                    res["examples"].append({"cik": cik, "accession": acc, "diffs": [str(d)[:300] for d in diffs[:5]]})
    res["seconds"] = round(time.perf_counter() - t0, 1)
    res["pass"] = res["differences"] == 0
    res["rule"] = ("each sampled event's issuer rebuilt from Company Facts rows with filing clock < cutoff = event "
                   "available_at + 1 s (full FX table; rebuilt events kept when available_at < cutoff) must equal "
                   "fund_asof.events_as_of_sql(stage, cutoff) row for row and column for column (catalog.parquet "
                   "likewise), and quarterly_history rows with available_at < cutoff must equal the rebuilt history")
    return res


def _f(v: Any, schema, name: str) -> Any:
    import pyarrow as pa

    if v is not None and schema.field(name).type == pa.float64():
        return float(v)
    return v


# ---------------------------------------------------------------------------------------------------------------
# exit measurement on member cells
# ---------------------------------------------------------------------------------------------------------------

def exit_measure(con, stage: Path, years: tuple[int, ...] = (2020, 2021, 2022)) -> dict[str, Any]:
    root = common.build_root()
    ev = _q(stage / "events.parquet")
    lt = _q(root / "identity" / "link_table.parquet")
    cal = con.execute(f"SELECT session_date FROM read_parquet('{_q(common.calendar_path())}') ORDER BY 1").fetchall()
    sessions = [r[0] for r in cal]
    con.execute("CREATE OR REPLACE TEMP TABLE cal (session_date DATE, prev_session DATE)")
    con.executemany("INSERT INTO cal VALUES (?, ?)", list(zip(sessions[1:], sessions[:-1])))
    fields = list(EXIT_FIELDS)
    res: dict[str, Any] = {"targets": EXIT_FIELDS, "years": list(years), "stage": stage.name, "by_year": {}}
    tot: dict[str, list[int]] = {}
    for y in years:
        mem = _q(root / "_tmp" / "panel_member" / f"year={y}.parquet")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE cells AS
            SELECT m.session_date, m.security_id, l.cik, l.link_tier,
                   CAST(c.prev_session AS TIMESTAMP) + INTERVAL {common.MARK_HOUR_UTC} HOUR AS cutoff
            FROM read_parquet('{mem}') m JOIN cal c USING (session_date)
            LEFT JOIN read_parquet('{lt}') l ON l.security_id = m.security_id
                 AND m.session_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01')
            WHERE m.member
            QUALIFY row_number() OVER (PARTITION BY m.session_date, m.security_id
                                       ORDER BY l.is_issuer_primary DESC NULLS LAST, l.cik) = 1""")
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE j AS
            SELECT x.*, e.period_end, e.currency, e.fin_template, e.sic_in_force,
                   {", ".join(f'CASE WHEN x.session_date - e.period_end <= 400 THEN e."{f}" END AS "{f}"' for f in fields)}
            FROM cells x ASOF LEFT JOIN (SELECT * FROM read_parquet('{ev}')
                                         QUALIFY row_number() OVER (PARTITION BY cik, available_at
                                                                    ORDER BY period_end DESC, accession DESC) = 1) e
              ON x.cik = e.cik AND x.cutoff > e.available_at""")
        yr: dict[str, Any] = {}
        n_cells, n_linked = con.execute("SELECT count(*), count(cik) FROM j").fetchone()
        yr["member_cells"], yr["linked_cells"] = n_cells, n_linked
        for f in fields:
            s = _structural_sql(f)
            r = con.execute(f"""
                SELECT count(*) FILTER (WHERE cik IS NOT NULL AND NOT coalesce({s}, false)),
                       count(*) FILTER (WHERE cik IS NOT NULL AND NOT coalesce({s}, false) AND "{f}" IS NOT NULL AND isfinite("{f}")),
                       count(*) FILTER (WHERE cik IS NOT NULL AND currency = 'USD' AND session_date - period_end <= 400
                                        AND NOT coalesce({s}, false)),
                       count(*) FILTER (WHERE cik IS NOT NULL AND currency = 'USD' AND session_date - period_end <= 400
                                        AND NOT coalesce({s}, false) AND "{f}" IS NOT NULL AND isfinite("{f}"))
                FROM j""").fetchone()
            yr[f] = {"linked": _r(r[1] / r[0]) if r[0] else None, "linked_usd": _r(r[3] / r[2]) if r[2] else None,
                     "linked_usd_cells": r[2]}
            acc = tot.setdefault(f, [0, 0, 0, 0])
            for k in range(4):
                acc[k] += r[k]
        res["by_year"][str(y)] = yr
    res["pooled"] = {f: {"linked": _r(a[1] / a[0]) if a[0] else None, "linked_usd": _r(a[3] / a[2]) if a[2] else None,
                         "target": EXIT_FIELDS[f], "met": bool(a[2] and a[3] / a[2] >= EXIT_FIELDS[f])}
                     for f, a in tot.items()}
    res["rule"] = __doc__.split("* ``exit``:")[1].strip()
    return res


# ---------------------------------------------------------------------------------------------------------------

CHECKS = ("ttm_gaps", "fx", "coverage", "benchmark", "balance", "cutoff", "exit")


def run(checks: list[str], stages: list[str] | None = None) -> dict[str, Any]:
    path = out_path()
    doc = common.read_json(path) if path.exists() else {}
    stage = _stage()
    doc["stage"] = stage.name
    doc["stage_manifest_sha256"] = common.sha256_file(stage / "manifest.json") if (stage / "manifest.json").exists() else None
    doc["code"] = common.code_identity("fund_validate", "fund_asof", *fu.MODULES)
    con = common.connect(memory=MEM, threads=2)
    try:
        for c in checks:
            t0 = time.perf_counter()
            if c == "ttm_gaps":
                doc["ttm_gaps"] = {s: ttm_gaps(con, common.build_root() / s / "events.parquet")
                                   for s in (stages or [stage.name])}
            elif c == "fx":
                doc["fx"] = fx_check(con, stage / "events.parquet")
            elif c == "coverage":
                doc["coverage"] = coverage(con, stage)
            elif c == "benchmark":
                doc["benchmark"] = benchmark(con, stage)
            elif c == "balance":
                doc["balance"] = balance(con, stage)
            elif c == "cutoff":
                doc["cutoff"] = cutoff(con, stage)
            elif c == "exit":
                doc["exit"] = {s: exit_measure(con, common.build_root() / s) for s in (stages or [stage.name])}
            doc.setdefault("seconds", {})[c] = round(time.perf_counter() - t0, 1)
            doc["created_utc"] = dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
            common.write_json_atomic(path, doc)
            print(c, json.dumps(_summary(doc, c), default=str)[:1500], flush=True)
    finally:
        con.close()
    return doc


def _summary(doc: dict[str, Any], c: str) -> Any:
    v = doc.get(c)
    if c == "coverage":
        return {k: v[k] for k in ("items_counted", "items_pass_covered", "items_pass_ex_structural")}
    if c == "benchmark":
        return {k: v[k] for k in ("cells", "mapped_share", "match_share_of_mapped", "pass")}
    if c == "cutoff":
        return {k: v[k] for k in ("events_sampled", "compared_rows", "differences", "pass")}
    if c == "exit":
        return {s: x["pooled"] for s, x in v.items()}
    return v


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("checks", nargs="+", help=f"'all' or any of {CHECKS}")
    ap.add_argument("--stages", nargs="*", help="stage directories for ttm_gaps / exit (default: the current stage)")
    args = ap.parse_args(argv)
    checks = list(CHECKS) if args.checks == ["all"] else args.checks
    bad = [c for c in checks if c not in CHECKS]
    if bad:
        raise SystemExit(f"unknown checks {bad}")
    run(checks, args.stages)
    return 0


if __name__ == "__main__":
    sys.exit(main())
