"""Stage ``compustat_std`` (tier1-v4 task 3; gap analysis section 2 item 2): Compustat-shaped point-in-time
``fundq.parquet`` and ``funda.parquet`` with ``rdq``, built from a fundamentals stage (``--fund-stage``, default
``fundamentals_v10``; task 5 rebuilds it on ``fundamentals_v11``) and ``earnings_calendar``. No new source.

Rows (rule ``cstd-pit-v3``). One row per vintage, keyed (cik, datadate, fyearq, fqtr, accession, available_at):

* ``vintage_kind = 'filing'``: every event of the fundamentals stage (one per (cik, accession): 10-Q, 10-K, 10-QT,
  10-KT, 20-F, 40-F and their /A) whose ``period_end`` lies in [``DATADATE_MIN``, build date + 365 days];
  ``datadate`` = the event's ``period_end`` (the issuer's latest statement period end at that filing, actual date,
  not month-rounded). Rows outside the window are excluded and counted (the raw Company Facts extract carries period
  ends up to 6016-06-30).
* ``vintage_kind = 'restatement'``: a later filing that changed a ``quarterly_history`` value of an older datadate
  (a restated quarter, a restated annual comparative) adds a row of that older datadate at the restating filing's
  clock and accession, ``is_restated`` = true (``_vintages``: the base row's values moved by the history deltas).

``(cik, datadate, accession, available_at)`` is unique. Each distinct legitimate history clock preserves the
visible snapshot, including delayed components of the original accession. A filing can carry several datadates
and several clocks per datadate; a later component never leaks into an earlier snapshot. ``funda`` = the ``fundq``
rows with ``fqtr`` = 4 whose datadate's own filing is an annual form (10-K, 10-KT, 20-F, 40-F and /A; a 10-Q
DEI-labelled FY is never an annual row), keyed (cik, datadate, fyear, accession, available_at).

Clock: ``available_at`` = the event's ``available_at`` (filing clock, or later for FX-converted filers: the
publication clock of every H.10 rate used). Every derived value below uses only inputs with a clock at or before the
row's ``available_at``, so ``fundq_asof(t)`` / ``funda_asof(t)`` (per (cik, datadate) the latest vintage with
``available_at < t``) equal a rebuild from the inputs visible at ``t`` (the ``cutoff`` check).

Fiscal calendar (Compustat conventions):

* effective month of a date = its month, or the previous month when the day is <= 7 (52/53-week years ending in the
  first days of a month, e.g. Apple's 2022-10-01);
* ``fyr`` = the effective month of the issuer's latest ``FY``-labelled annual-form datadate visible at the row (an
  annual report received at or before ``available_at``); with none yet, inferred from the row's own label Qk / FY
  (effective month + 3 (4 - k) months; an FY label on a 10-Q is ignored); restatement vintages keep their base row's
  ``fyr`` / ``fqtr`` / ``fyearq``;
* ``fqtr`` = ((effective month - fyr - 1) mod 12) div 3 + 1 (``FY`` rows with fyr from themselves are 4);
* ``fyearq`` (and ``fyear`` in funda) = the calendar year of the fiscal year end the quarter belongs to (the first
  month index at or after the effective month whose month is ``fyr``), minus 1 when ``fyr`` < 6 (Compustat: a year
  ending January-May belongs to the previous fiscal year).

Items (all DOUBLE; money in USD -- the fundamentals stage converts FX-covered non-USD filers; per-share in USD per
share; shares in shares). Quarterly flows, stocks and shares come straight from the event (``FUNDQ_ITEMS``,
``FUNDA_ITEMS``; the catalog items under their Compustat mnemonics, ``catalog_names``). Derived:

* YTD (``oancfy capxy dvy prstkcy sstky xrdy``): fqtr 4 = the event's TTM (the fiscal-year fact at a 10-K);
  fqtr 1-3 = the sum of the discrete quarters of the fiscal year (the quarter ends whose effective month is
  3 j months before the row's, j = 0..fqtr-1) from ``quarterly_history`` (per (item, period_end) the latest vintage
  with available_at <= the row's); NULL when a quarter is missing, 0 when the event's TTM is 0 (no flow in the
  trailing year). FX-converted rows sum reporting-currency quarters at each quarter's 91-day mean H.10 rate
  (``fund_fx``; the current quarter at the event's ``fx_rate_avg_q``; a prior-quarter rate whose clock is after
  the row's ``available_at`` is not used and the YTD is NULL).
* ``ibq`` (income before extraordinary items and discontinued operations): the 3-month fact of the catalog ``ib``
  chain at datadate (PIT: latest clock <= available_at, concept priority first); at fqtr 4 else fiscal-year minus
  nine-month facts of one concept; else ``niq - doq`` when the catalog ``xido`` TTM is 0 or NULL, ``doq`` = the
  3-month (fqtr 4: FY - 9M) fact of the catalog ``do`` chain, else 0 when the catalog ``do`` TTM is 0 or when the
  extract holds no non-zero ``do`` fact of any duration ending at datadate (clock <= available_at: the filing's own
  current period, 3-month or year-to-date, shows no discontinued operations) and ``niq`` exists.
* ``epspxq`` / ``epsfxq`` (Compustat: EPS excluding extraordinary items and discontinued operations): the 3-month
  continuing-operations EPS fact (catalog ``epspi``/``epsfi`` chains), else -- when ``doq`` = 0 -- the 3-month
  total EPS fact (catalog ``epspx``/``epsfx`` chains); fqtr 4 falls back to FY - 9M of one concept.
  ``cshprq``: the 3-month weighted-average basic shares fact, fqtr 4 else 4 FY - 3 x 9M (when > 0).
  Facts come from ``<fund stage>/_work/cf`` (the stage's Company Facts extract), each at its accession's filing
  clock; FX-converted rows multiply money and per-share facts by ``fx_rate_avg_q``. ``*_src`` names the path.
* ``rdq`` (report date of quarterly earnings, informational, never a clock): the earliest of the ET date of a primary
  8-K item 2.02 announcement whose ``fiscal_period_end`` is within 7 days of datadate and whose ``available_at`` <=
  the row's, and the first-vintage filing date of the datadate (ET date of the acceptance; ``filed`` for the modeled
  fc1 clock); ``rdq_src`` = ``8k_202`` or ``filing`` (no 8-K before the filing: the 10-Q/10-K was the first release).

Descriptors: ``fyr``, ``curcd`` (reporting currency), ``fx_converted``, ``first_reported`` (earliest vintage of the
(cik, datadate)), ``is_restated`` / ``restated_items`` / ``is_amendment`` / ``nonreliance_402_at`` (fundamentals
stage S4.6), ``fiscal_year`` / ``fiscal_period`` (SEC DEI labels), ``fin_template``, ``sic``, ``zero_filled``
(fundamentals-stage item names that were zero-filled).

Commands (every one under ``run_memory_guarded.py``)::

    python -m atx_db.alpha_panel.compustat_std build    [--fund-stage S] [--earn-stage E] [--stage compustat_std]
    python -m atx_db.alpha_panel.compustat_std validate [--stage ...]       # coverage, rdq, sum4, balance, fsds
    python -m atx_db.alpha_panel.compustat_std cutoff   [--stage ...]       # rebuild at cutoffs, 0 diffs
    python -m atx_db.alpha_panel.compustat_std handcheck [--n 30]           # SEC 10-Q inline XBRL vs fundq (network)
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import random
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pyarrow.parquet as pq

from . import common as C
from . import fund_catalog as fcat
from . import fund_fx as ffx

STAGE = "compustat_std"
SCHEMA = "atx.alpha-panel.compustat-std/v2"
RULE = "cstd-pit-v3"
CODE_VERSION = "compustat-std-v3"
MODULES = ("compustat_std", "fund_catalog", "fund_fx", "common")
DEFAULT_FUND_STAGE = "fundamentals_v10"
DEFAULT_EARN_STAGE = "earnings_calendar"
DATADATE_MIN = dt.date(1990, 1, 1)
DATADATE_MAX_DAYS = 365
EFF_DAY = 7                      # a period ending on day 1..7 belongs to the previous month
RDQ_MATCH_DAYS = 7
Q_DUR, FY_DUR, NINE_DUR = (80, 120), (350, 380), (250, 290)
N_BUCKETS = 32
MEM = os.environ.get("ATX_CSTD_DUCKDB_MEM", "450MB")
QUARTERLY_FORMS = ("10-Q", "10-K", "10-QT", "10-KT", "10-Q/A", "10-K/A", "10-QT/A", "10-KT/A")
ANNUAL_FORMS = ("10-K", "10-K/A", "10-KT", "10-KT/A", "20-F", "20-F/A", "40-F", "40-F/A")
GATE_YEARS = tuple(range(2020, 2026))

# ---------------------------------------------------------------------------------------------------------------
# item specifications: (column, source expression over the joined row, unit, definition)
# ---------------------------------------------------------------------------------------------------------------

FUNDQ_ITEMS: tuple[tuple[str, str, str, str], ...] = (
    ("saleq", "sale_q", "USD", "Sales/turnover (net), discrete quarter (fundamentals sale_q: revenue chain; banks "
                               "interest + noninterest income; Q4 = FY - 9M)"),
    ("cogsq", "cogs_q", "USD", "Cost of goods sold, discrete quarter"),
    ("xsgaq", "xsga_q", "USD", "Selling, general and administrative expense, discrete quarter"),
    ("oiadpq", "oi_q", "USD", "Operating income after depreciation, discrete quarter"),
    ("oibdpq", "ebitda_q", "USD", "Operating income before depreciation = oiadpq + dpq"),
    ("dpq", "dp_q", "USD", "Depreciation and amortization, discrete quarter"),
    ("xintq", "xint_q", "USD", "Interest expense, discrete quarter"),
    ("txtq", "txt_q", "USD", "Income taxes - total, discrete quarter (0 when the income statement shows no tax line)"),
    ("niq", "ni_q", "USD", "Net income (loss) attributable to the parent, discrete quarter"),
    ("cshoq", "shrs_q", "shares", "Common shares outstanding (dei cover count dated end-15d..end+120d, else "
                                  "balance-sheet shares at datadate, else weighted-average shares)"),
    ("atq", '"at"', "USD", "Assets - total"),
    ("ltq", "lt", "USD", "Liabilities - total (Liabilities, else assets - equity incl. NCI)"),
    ("ceqq", "c_ceq", "USD", "Common/ordinary equity - total = seqq - pstkq"),
    ("seqq", "seq", "USD", "Stockholders' equity - parent"),
    ("teqq", "c_teq", "USD", "Stockholders' equity - total incl. noncontrolling interest (reported, else seqq + mibq)"),
    ("cheq", "che", "USD", "Cash and short-term investments"),
    ("actq", "act", "USD", "Current assets - total"),
    ("lctq", "lct", "USD", "Current liabilities - total"),
    ("invtq", "invt", "USD", "Inventories - total"),
    ("rectq", "rect", "USD", "Receivables - total"),
    ("apq", "ap", "USD", "Accounts payable - trade"),
    ("ppentq", "ppe", "USD", "Property, plant and equipment - total (net)"),
    ("dlcq", "c_dlc", "USD", "Debt in current liabilities"),
    ("dlttq", "c_dltt", "USD", "Long-term debt - total (excluding current maturities)"),
    ("pstkq", "pstk", "USD", "Preferred/preference stock (capital) - total (0 when unreported and assets exist)"),
    ("mibq", "mib", "USD", "Noncontrolling interest (balance sheet)"),
    ("gdwlq", "gdwl", "USD", "Goodwill"),
    ("intanq", "c_intano", "USD", "Intangible assets - total incl. goodwill (fundamentals catalog column intano = "
                                  "gdwl + intan)"),
)
# YTD flows: (column, quarterly_history item, event TTM column, definition)
YTD_ITEMS: tuple[tuple[str, str, str, str], ...] = (
    ("oancfy", "cfo_q", "cfo_ttm", "Operating activities - net cash flow, fiscal year to date"),
    ("capxy", "capx_q", "capx_ttm", "Capital expenditures, fiscal year to date"),
    ("dvy", "dvc_q", "dvc_ttm", "Cash dividends paid on common stock (fundamentals dvc chain; preferred excluded), "
                                "fiscal year to date"),
    ("prstkcy", "prstkc_q", "prstkc_ttm", "Purchase of common and preferred stock, fiscal year to date"),
    ("sstky", "sstk_q", "sstk_ttm", "Sale of common and preferred stock, fiscal year to date"),
    ("xrdy", "xrd_q", "xrd_ttm", "Research and development expense, fiscal year to date"),
)
DERIVED_Q: tuple[tuple[str, str, str], ...] = (
    ("ibq", "USD", "Income before extraordinary items and discontinued operations, discrete quarter (see ibq_src)"),
    ("epspxq", "USD/share", "EPS (basic) excluding extraordinary items and discontinued operations, quarter "
                            "(see epspxq_src)"),
    ("epsfxq", "USD/share", "EPS (diluted) excluding extraordinary items and discontinued operations, quarter"),
    ("cshprq", "shares", "Common shares used to calculate basic EPS (weighted average of the quarter)"),
)
FUNDA_ITEMS: tuple[tuple[str, str, str, str], ...] = (
    ("sale", "sale_ttm", "USD", "Sales/turnover (net), fiscal year"),
    ("cogs", "cogs_ttm", "USD", "Cost of goods sold, fiscal year"),
    ("xsga", "xsga_ttm", "USD", "Selling, general and administrative expense, fiscal year"),
    ("gp", "gp_ttm", "USD", "Gross profit (loss), fiscal year"),
    ("oiadp", "oi_ttm", "USD", "Operating income after depreciation, fiscal year"),
    ("oibdp", "ebitda_ttm", "USD", "Operating income before depreciation = oiadp + dp"),
    ("dp", "dp_ttm", "USD", "Depreciation and amortization, fiscal year"),
    ("xint", "xint_ttm", "USD", "Interest expense, fiscal year"),
    ("ni", "ni_ttm", "USD", "Net income (loss) attributable to the parent, fiscal year"),
    ("csho", "shrs_q", "shares", "Common shares outstanding (as cshoq)"),
    ("at", '"at"', "USD", "Assets - total"),
    ("lt", "lt", "USD", "Liabilities - total"),
    ("seq", "seq", "USD", "Stockholders' equity - parent"),
    ("che", "che", "USD", "Cash and short-term investments"),
    ("act", "act", "USD", "Current assets - total"),
    ("lct", "lct", "USD", "Current liabilities - total"),
    ("invt", "invt", "USD", "Inventories - total"),
    ("rect", "rect", "USD", "Receivables - total"),
    ("ap", "ap", "USD", "Accounts payable - trade"),
    ("ppent", "ppe", "USD", "Property, plant and equipment - total (net)"),
    ("ppegt", "ppegt", "USD", "Property, plant and equipment - total (gross)"),
    ("pstk", "pstk", "USD", "Preferred/preference stock (capital) - total"),
    ("mib", "mib", "USD", "Noncontrolling interest (balance sheet)"),
    ("gdwl", "gdwl", "USD", "Goodwill"),
    ("intano", "intan", "USD", "Other intangibles (excluding goodwill; fundamentals intan)"),
    ("oancf", "cfo_ttm", "USD", "Operating activities - net cash flow, fiscal year"),
    ("capx", "capx_ttm", "USD", "Capital expenditures, fiscal year"),
    ("dvc", "dvc_ttm", "USD", "Cash dividends paid on common stock, fiscal year"),
    ("dvt", "dvt_ttm", "USD", "Dividends - total (declared), fiscal year"),
    ("prstkc", "prstkc_ttm", "USD", "Purchase of common and preferred stock, fiscal year"),
    ("sstk", "sstk_ttm", "USD", "Sale of common and preferred stock, fiscal year"),
    ("xrd", "xrd_ttm", "USD", "Research and development expense, fiscal year"),
)
# fundamentals catalog columns whose content differs from their catalog mnemonic (Compustat semantics win):
# intano holds gdwl + intan = Compustat INTAN (total); epspx/epsfx hold total EPS (NI based) = Compustat EPSPI/EPSFI;
# epspi/epsfi hold continuing-operations EPS (total when do = 0) = Compustat EPSPX/EPSFX; mibt holds temporary
# (mezzanine) equity, not Compustat MIBT (total noncontrolling interest), and has no Compustat mnemonic (tempeq);
# nicon_ttm is net income available to common (basic), not Compustat NIADJ (the diluted-EPS numerator) (nicon).
CATALOG_RENAME = {"intano": "intan", "epspx_ttm": "epspi", "epsfx_ttm": "epsfi", "epspi_ttm": "epspx",
                  "epsfi_ttm": "epsfx", "mibt": "tempeq", "nicon_ttm": "nicon"}
CATALOG_NOT_COMPUSTAT = frozenset({"mibt", "nicon_ttm"})


def catalog_names() -> list[tuple[str, str, str, str]]:
    """(funda column, catalog column, unit, definition) for every fundamentals catalog item."""
    out = []
    for it in fcat.CATALOG:
        name = CATALOG_RENAME.get(it.col, it.mnemonic.split("/")[0].lower())
        unit = {"per_share": "USD/share", "avg_shares": "shares"}.get(it.kind, "USD")
        if it.col == "cshi":
            unit = "shares"
        span = "fiscal year" if it.kind in ("flow", "per_share") or it.col.endswith("_ttm") else "at datadate"
        note = f"catalog {it.col} (mnemonic {it.mnemonic}, {it.kind}), {span}" + (f"; {it.note}" if it.note else "")
        if it.col in CATALOG_NOT_COMPUSTAT:
            note += f"; no Compustat equivalent: published as {name} (catalog label {it.mnemonic} is not its content)"
        elif it.col in CATALOG_RENAME:
            note += f"; published as Compustat {name.upper()} (catalog label {it.mnemonic} differs)"
        out.append((name, it.col, unit, note))
    names = [n for n, _, _, _ in out]
    assert len(set(names)) == len(names), "duplicate catalog name"
    assert not set(names) & {n for n, _, _, _ in FUNDA_ITEMS}, "catalog name collides with a funda core item"
    return out


# Company Facts concepts read for ibq / doq / EPS / cshprq: key -> (unit kind, chain), the catalog chains
FACT_KEYS: dict[str, tuple[str, tuple[str, ...]]] = {
    "ib": ("money", fcat.BY_COL["ib_ttm"].chain),
    "do": ("money", fcat.BY_COL["do_ttm"].chain),
    "eps_b": ("per_share", fcat.BY_COL["epspx_ttm"].chain),
    "eps_d": ("per_share", fcat.BY_COL["epsfx_ttm"].chain),
    "epsc_b": ("per_share", fcat.BY_COL["epspi_ttm"].chain),
    "epsc_d": ("per_share", fcat.BY_COL["epsfi_ttm"].chain),
    "waso_b": ("shares", fcat.BY_COL["cshpri"].chain),
}

EVENT_COLS = ("cik", "accession", "form", "filed", "clock_utc", "clock_basis", "available_at", "period_end",
              "fiscal_year", "fiscal_period", "currency", "fx_converted", "fx_rate", "fx_rate_avg_q", "fx_rate_avg_ttm",
              "is_amendment", "is_restated", "restated_items", "nonreliance_402_at", "fin_template", "sic_in_force",
              "zero_filled")
CAT_EXTRA = ("do_ttm", "xido_ttm")
DESC_Q = ("cik", "datadate", "fyearq", "fqtr", "fyr", "accession", "form", "filed", "clock_utc", "available_at",
          "rdq", "rdq_src", "curcd", "fx_converted", "fx_rate", "fx_rate_avg_q", "first_reported", "vintage_kind",
          "is_restated", "restated_items", "is_amendment", "nonreliance_402_at", "fiscal_year", "fiscal_period",
          "fin_template", "sic", "zero_filled")
# restatement vintages: event column -> quarterly_history item, by conversion rate (FX-converted rows)
VINT_STOCK = {c: c for c in ("at", "lt", "seq", "che", "act", "lct", "invt", "rect", "ap", "ppe", "pstk", "mib",
                             "gdwl", "ppegt", "intan")}
VINT_FLOW_Q = {c: c for c in ("sale_q", "cogs_q", "xsga_q", "oi_q", "ebitda_q", "dp_q", "xint_q", "txt_q", "ni_q")}
VINT_TTM = {"sale_ttm": "sale_q", "cogs_ttm": "cogs_q", "xsga_ttm": "xsga_q", "gp_ttm": "gp_q", "oi_ttm": "oi_q",
            "ebitda_ttm": "ebitda_q", "dp_ttm": "dp_q", "xint_ttm": "xint_q", "ni_ttm": "ni_q", "cfo_ttm": "cfo_q",
            "capx_ttm": "capx_q", "dvc_ttm": "dvc_q", "dvt_ttm": "dvt_q", "prstkc_ttm": "prstkc_q",
            "sstk_ttm": "sstk_q", "xrd_ttm": "xrd_q"}
# catalog columns moved by component deltas
VINT_DERIVED = {"c_ceq": (("seq", 1), ("pstk", -1)), "c_teq": (("seq", 1), ("mib", 1)),
                "c_intano": (("gdwl", 1), ("intan", 1))}
HIST_ITEMS = tuple(sorted(set(VINT_STOCK.values()) | set(VINT_FLOW_Q.values()) | set(VINT_TTM.values())))
SRC_Q = ("ibq_src", "epspxq_src")


# ---------------------------------------------------------------------------------------------------------------
# inputs
# ---------------------------------------------------------------------------------------------------------------

@dataclass
class Inputs:
    fund: Path                       # fundamentals stage directory
    earn: Path                       # earnings_calendar stage directory
    facts: Path | None               # prepared facts parquet (None: no Company Facts extract)
    fx_rows: list[tuple] = field(default_factory=list)   # (obs_date, currency, usd_per_ccy, available_at)
    datadate_max: dt.date = dt.date(2100, 1, 1)


def _q(p: Path | str) -> str:
    return Path(p).as_posix()


def _ts(t: dt.datetime) -> str:
    return f"TIMESTAMP '{t.isoformat(sep=' ')}'"


def _d2(col: str) -> str:
    return f"(CASE WHEN day({col}) <= {EFF_DAY} THEN CAST({col} - INTERVAL {EFF_DAY + 1} DAY AS DATE) ELSE {col} END)"


def eff_idx_sql(col: str) -> str:
    """Effective month index (year * 12 + month - 1) of a DATE column."""
    return f"(year({_d2(col)}) * 12 + month({_d2(col)}) - 1)"


def fact_spec_rows() -> list[tuple[str, str, str, int, str]]:
    """(taxonomy, concept, key, rank, unit kind) for every Company Facts concept read."""
    rows = []
    for key, (ukind, chain) in FACT_KEYS.items():
        for rnk, full in enumerate(chain):
            tax, concept = full.split(":", 1) if ":" in full else ("us-gaap", full)
            rows.append((tax, concept, key, rnk, ukind))
    return rows


def prepare_facts(con, fund: Path, dest: Path, datadate_max: dt.date) -> dict[str, Any]:
    """Company Facts duration facts of ``FACT_KEYS`` (80-380 days, period end in the datadate window) with the
    filing clock of their accession (the fundamentals stage's event clock; facts of accessions without an event are
    dropped) -> ``dest``. Returns counts (window exclusions, conflicting duplicates)."""
    cf = fund / "_work" / "cf"
    batches = sorted(cf.glob("batch-[0-9][0-9][0-9][0-9].parquet")) if cf.exists() else []
    if not batches:
        return {"source": "absent", "path": cf.as_posix(), "rows": 0}
    con.execute("CREATE OR REPLACE TEMP TABLE fact_spec (taxonomy VARCHAR, concept VARCHAR, key VARCHAR, rnk INTEGER, "
                "ukind VARCHAR)")
    con.executemany("INSERT INTO fact_spec VALUES (?, ?, ?, ?, ?)", fact_spec_rows())
    glob = _q(cf / "batch-[0-9][0-9][0-9][0-9].parquet")
    ev = _q(fund / "events.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE fraw AS
        SELECT CAST(b.cik AS BIGINT) AS cik, s.key, s.rnk, s.ukind, b.unit, b.period_start, b.period_end,
               datediff('day', b.period_start, b.period_end) AS dur, b.value, b.accession_number AS accession
        FROM read_parquet('{glob}') b JOIN fact_spec s ON s.taxonomy = b.taxonomy AND s.concept = b.concept
        WHERE b.period_start IS NOT NULL AND b.value IS NOT NULL
          AND datediff('day', b.period_start, b.period_end) BETWEEN {Q_DUR[0]} AND {FY_DUR[1]}
          AND ((s.ukind = 'shares' AND b.unit = 'shares') OR (s.ukind = 'per_share' AND b.unit LIKE '%/shares')
               OR (s.ukind = 'money' AND b.unit NOT LIKE '%/%' AND b.unit <> 'shares'))""")
    n_raw, n_out = con.execute(f"""SELECT count(*), count(*) FILTER (WHERE period_end < DATE '{DATADATE_MIN}'
                                       OR period_end > DATE '{datadate_max}') FROM fraw""").fetchone()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE fdedup AS
        SELECT cik, key, rnk, ukind, unit, period_start, period_end, dur, accession, max(value) AS value,
               count(DISTINCT value) AS n_values
        FROM fraw WHERE period_end BETWEEN DATE '{DATADATE_MIN}' AND DATE '{datadate_max}'
        GROUP BY ALL""")
    conflicts = con.execute("SELECT count(*) FROM fdedup WHERE n_values > 1").fetchone()[0]
    n = C.copy_to_parquet(con, f"""
        SELECT f.cik, f.key, f.rnk, f.ukind, f.unit, f.period_start, f.period_end, f.dur, f.value, f.accession,
               k.clock_utc
        FROM fdedup f JOIN (SELECT DISTINCT cik, accession, clock_utc FROM read_parquet('{ev}')) k
          ON k.cik = f.cik AND k.accession = f.accession
        ORDER BY f.cik, f.key, f.period_end, f.accession""", dest)
    con.execute("DROP TABLE fraw")
    con.execute("DROP TABLE fdedup")
    return {"source": cf.as_posix(), "batches": len(batches), "rows_read": int(n_raw),
            "excluded_outside_datadate_window": int(n_out), "conflicting_duplicates_max_kept": int(conflicts),
            "rows": n}


# ---------------------------------------------------------------------------------------------------------------
# derivation (DuckDB), one CIK subset at a time
# ---------------------------------------------------------------------------------------------------------------

def _setup(con, inp: Inputs, cik_where: str, cutoff: dt.datetime | None) -> None:
    cut = f" AND available_at < {_ts(cutoff)}" if cutoff else ""
    ev, cat, hist = (_q(inp.fund / f) for f in ("events.parquet", "catalog.parquet", "quarterly_history.parquet"))
    ann = _q(inp.earn / "announcements.parquet")
    ev_cols = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{ev}')").fetchall()}
    sel = ", ".join(("coalesce(e.fx_converted, false) AS fx_converted" if c == "fx_converted" else f'e."{c}"')
                    if c in ev_cols else (f'false AS "{c}"' if c == "fx_converted" else f'NULL AS "{c}"')
                    for c in EVENT_COLS)
    if "nonreliance_402_at" not in ev_cols:
        raise ValueError("fundamentals events lack nonreliance_402_at (fundamentals v10+ required)")
    q_srcs = sorted({s for _, s, _, _ in FUNDQ_ITEMS + FUNDA_ITEMS if not s.startswith("c_")} |
                    {t for _, _, t, _ in YTD_ITEMS})
    ev_items = ", ".join(f"e.{s}" if s.startswith('"') else f'e."{s}"' for s in q_srcs)
    cat_cols = [c for _, c, _, _ in catalog_names()] + list(CAT_EXTRA)
    cat_items = ", ".join(f'c."{c}" AS "c_{c}"' for c in dict.fromkeys(cat_cols))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE r0 AS
        SELECT {sel}, {ev_items}, {cat_items}, c.catalog_zero_filled
        FROM (SELECT * FROM read_parquet('{ev}') WHERE {cik_where}{cut}) e
        LEFT JOIN (SELECT * FROM read_parquet('{cat}') WHERE {cik_where}) c USING (cik, accession)""")
    items = ", ".join(f"'{h}'" for h in sorted(set(HIST_ITEMS) | {h for _, h, _, _ in YTD_ITEMS}))
    ccut = f" AND clock_utc < {_ts(cutoff)}" if cutoff else ""
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE emeta AS
        SELECT cik, accession, form, filed, clock_utc, nonreliance_402_at FROM read_parquet('{ev}')
        WHERE {cik_where}{ccut}""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE hist AS
        SELECT cik, item, period_end, value, currency, available_at, accession, {eff_idx_sql('period_end')} AS h_idx
        FROM read_parquet('{hist}')
        WHERE {cik_where}{cut} AND item IN ({items})
          AND period_end BETWEEN DATE '{DATADATE_MIN}' AND DATE '{inp.datadate_max}'""")
    if inp.facts is not None and inp.facts.exists():
        fcut = f" AND clock_utc < {_ts(cutoff)}" if cutoff else ""
        con.execute(f"CREATE OR REPLACE TEMP TABLE fct AS SELECT * FROM read_parquet('{_q(inp.facts)}') "
                    f"WHERE {cik_where}{fcut}")
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE fct (cik BIGINT, key VARCHAR, rnk INTEGER, ukind VARCHAR, "
                    "unit VARCHAR, period_start DATE, period_end DATE, dur INTEGER, value DOUBLE, accession VARCHAR, "
                    "clock_utc TIMESTAMP)")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE ann AS
        SELECT cik, fiscal_period_end, available_at,
               CAST(timezone('America/New_York', CAST(announcement_utc AS TIMESTAMPTZ)) AS DATE) AS ann_date
        FROM read_parquet('{ann}') WHERE {cik_where}{cut} AND is_primary AND fiscal_period_end IS NOT NULL""")


def _fx_quarters(con, inp: Inputs, cutoff: dt.datetime | None) -> None:
    """``fxq``: 91-day mean H.10 rate and its clock per (currency, quarter end) needed by the YTD sums."""
    con.execute("CREATE OR REPLACE TEMP TABLE fxq (currency VARCHAR, period_end DATE, rate DOUBLE, clock TIMESTAMP)")
    need = con.execute("""SELECT DISTINCT h.currency, h.period_end FROM hist h
                          WHERE h.currency <> 'USD' AND h.cik IN (SELECT cik FROM r0 WHERE fx_converted)""").fetchall()
    if not need or not inp.fx_rows:
        return
    rows = inp.fx_rows if cutoff is None else [r for r in inp.fx_rows if r[3] is not None and
                                               ffx._naive_utc(r[3]) < cutoff]
    table = ffx.FxTable.from_rows(rows)
    out = []
    for ccy, pe in need:
        v = table.flow_q(ccy, pe) if table.covers(ccy) else None
        if v is not None:
            out.append((ccy, pe, v[0], v[1]))
    if out:
        con.executemany("INSERT INTO fxq VALUES (?, ?, ?, ?)", out)


def _derive(con, inp: Inputs) -> None:
    """Temp tables ``q_out`` (fundq rows) and ``a_out`` (funda rows) from ``r0``, ``hist``, ``fct``, ``ann``."""
    ei = eff_idx_sql("period_end")
    annual = ", ".join(f"'{f}'" for f in ANNUAL_FORMS)
    # a 10-Q DEI-labelled FY is neither a fiscal-year-end source nor a Q4 label
    lab_k = ("CASE WHEN fiscal_period = 'FY' AND form NOT IN ({a}) THEN NULL ELSE CASE fiscal_period "
             "WHEN 'Q1' THEN 1 WHEN 'Q2' THEN 2 WHEN 'Q3' THEN 3 WHEN 'Q4' THEN 4 WHEN 'FY' THEN 4 END END")
    lab_k = lab_k.format(a=annual)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE r1 AS
        SELECT *, arg_max(eidx % 12 + 1, struct_pack(t := available_at, a := accession))
                      FILTER (WHERE fiscal_period = 'FY' AND form IN ({annual}))
                      OVER (PARTITION BY cik ORDER BY available_at RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
                      AS fyr_fy,
               (eidx % 12 + 3 * (4 - {lab_k})) % 12 + 1 AS fyr_label
        FROM (SELECT *, {ei} AS eidx FROM r0
              WHERE period_end BETWEEN DATE '{DATADATE_MIN}' AND DATE '{inp.datadate_max}')""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE r2e AS
        SELECT *, CASE WHEN fyr IS NOT NULL THEN ((eidx % 12 + 1 - fyr - 1 + 24) % 12) // 3 + 1 END AS fqtr,
               CASE WHEN fyr IS NOT NULL THEN eidx + ((fyr - 1 - eidx % 12 + 24) % 12) END AS fye_idx,
               CASE WHEN fx_converted THEN currency ELSE 'USD' END AS mccy,
               CASE WHEN fx_converted THEN fx_rate_avg_q ELSE 1.0 END AS rq,
               CASE WHEN clock_basis = 'fsds_accepted_utc'
                    THEN CAST(timezone('America/New_York', CAST(clock_utc AS TIMESTAMPTZ)) AS DATE)
                    ELSE filed END AS filing_date,
               form AS dform, 'filing' AS vintage_kind
        FROM (SELECT *, coalesce(fyr_fy, fyr_label) AS fyr FROM r1)""")
    _vintages(con)
    # ---- YTD sums from quarterly_history (fqtr 1..3) ----
    con.execute("""
        CREATE OR REPLACE TEMP TABLE ytd_m AS
        SELECT r.cik, r.period_end AS pe, r.accession, r.fqtr, o.off, h.item, h.period_end AS hpe, h.value,
               r.fx_converted, r.currency,
               r.fx_rate_avg_q, r.available_at
        FROM r2 r JOIN range(0, 3) o(off) ON o.off < r.fqtr
        JOIN hist h ON h.cik = r.cik AND h.h_idx = r.eidx - 3 * o.off AND h.available_at <= r.available_at
                   AND h.currency = r.mccy
        WHERE r.fqtr BETWEEN 1 AND 3
        QUALIFY row_number() OVER (PARTITION BY r.cik, r.period_end, r.accession, r.available_at, o.off, h.item
                                   ORDER BY h.period_end DESC, h.available_at DESC, h.accession DESC) = 1""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE ytd_v AS
        SELECT m.cik, m.pe, m.accession, m.available_at, m.item, any_value(m.fqtr) AS fqtr, count(*) AS n,
               count(CASE WHEN NOT m.fx_converted THEN m.value
                          WHEN m.off = 0 THEN m.value * m.fx_rate_avg_q
                          WHEN x.clock <= m.available_at THEN m.value * x.rate END) AS n_conv,
               sum(CASE WHEN NOT m.fx_converted THEN m.value
                        WHEN m.off = 0 THEN m.value * m.fx_rate_avg_q
                        WHEN x.clock <= m.available_at THEN m.value * x.rate END) AS v
        FROM ytd_m m LEFT JOIN fxq x ON m.fx_converted AND x.currency = m.currency AND x.period_end = m.hpe
        GROUP BY 1, 2, 3, 4, 5""")
    ytd_cols = []
    for col, item, ttm, _ in YTD_ITEMS:
        ytd_cols.append(f"""CASE WHEN r.fqtr = 4 THEN r."{ttm}"
                 WHEN r.fqtr BETWEEN 1 AND 3 THEN coalesce(
                      max(CASE WHEN y.item = '{item}' AND y.n = r.fqtr AND y.n_conv = r.fqtr THEN y.v END),
                      CASE WHEN r."{ttm}" = 0 THEN 0.0 END) END AS {col}""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE ytd AS
        SELECT r.cik, r.period_end, r.accession, r.available_at, {', '.join(ytd_cols)}
        FROM r2 r LEFT JOIN ytd_v y ON y.cik = r.cik AND y.pe = r.period_end AND y.accession = r.accession AND y.available_at = r.available_at
        GROUP BY r.cik, r.period_end, r.accession, r.available_at, r.fqtr, {', '.join(f'r."{t}"' for _, _, t, _ in YTD_ITEMS)}""")
    # ---- Company Facts: 3-month facts at datadate, and FY - 9M at fqtr 4 ----
    unit_ok = ("f.unit = CASE f.ukind WHEN 'shares' THEN 'shares' WHEN 'per_share' THEN r.mccy || '/shares' "
               "ELSE r.mccy END")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE f3 AS
        SELECT r.cik, r.period_end, r.accession, r.available_at, f.key, f.value
        FROM r2 r JOIN fct f ON f.cik = r.cik AND f.period_end = r.period_end
             AND f.dur BETWEEN {Q_DUR[0]} AND {Q_DUR[1]} AND f.clock_utc <= r.available_at AND {unit_ok}
        QUALIFY row_number() OVER (PARTITION BY r.cik, r.period_end, r.accession, r.available_at, f.key
                                   ORDER BY f.rnk, f.clock_utc DESC, f.accession DESC) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE f4 AS
        SELECT r.cik, r.period_end, r.accession, r.available_at, f.key,
               CASE WHEN f.key = 'waso_b' THEN 4 * f.value - 3 * b.value ELSE f.value - b.value END AS value
        FROM r2 r JOIN fct f ON f.cik = r.cik AND f.period_end = r.period_end
             AND f.dur BETWEEN {FY_DUR[0]} AND {FY_DUR[1]} AND f.clock_utc <= r.available_at AND {unit_ok}
        JOIN fct b ON b.cik = f.cik AND b.key = f.key AND b.rnk = f.rnk AND b.unit = f.unit
             AND b.period_start = f.period_start AND b.dur BETWEEN {NINE_DUR[0]} AND {NINE_DUR[1]}
             AND b.clock_utc <= r.available_at
        WHERE r.fqtr = 4
        QUALIFY row_number() OVER (PARTITION BY r.cik, r.period_end, r.accession, r.available_at, f.key
                                   ORDER BY f.rnk, f.clock_utc DESC, b.clock_utc DESC, f.accession DESC,
                                            b.accession DESC) = 1""")
    piv3 = ", ".join(f"max(CASE WHEN f3.key = '{k}' THEN f3.value END) AS {k}_3" for k in FACT_KEYS)
    piv4 = ", ".join(f"max(CASE WHEN f4.key = '{k}' THEN f4.value END) AS {k}_4" for k in FACT_KEYS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE fw AS
        SELECT r.cik, r.period_end, r.accession, r.available_at, a.* EXCLUDE (cik, period_end, accession, available_at),
               b.* EXCLUDE (cik, period_end, accession, available_at),
               EXISTS (SELECT 1 FROM fct f WHERE f.cik = r.cik AND f.key = 'do' AND f.value <> 0
                       AND f.period_end = r.period_end AND f.clock_utc <= r.available_at) AS do_recent
        FROM r2 r
        LEFT JOIN (SELECT cik, period_end, accession, available_at, {piv3} FROM f3 GROUP BY 1, 2, 3, 4) a
          USING (cik, period_end, accession, available_at)
        LEFT JOIN (SELECT cik, period_end, accession, available_at, {piv4} FROM f4 GROUP BY 1, 2, 3, 4) b
          USING (cik, period_end, accession, available_at)""")
    # ---- rdq ----
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE rd AS
        SELECT r.cik, r.period_end, r.accession, r.available_at, k.rdq_8k,
               min(r.filing_date) OVER (PARTITION BY r.cik, r.period_end ORDER BY r.available_at
                                        RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS rdq_filing
        FROM r2 r LEFT JOIN (
            SELECT r.cik, r.period_end, r.accession, r.available_at, min(a.ann_date) AS rdq_8k
            FROM r2 r JOIN ann a ON a.cik = r.cik AND a.available_at <= r.available_at
                 AND abs(datediff('day', a.fiscal_period_end, r.period_end)) <= {RDQ_MATCH_DAYS}
            GROUP BY 1, 2, 3, 4) k USING (cik, period_end, accession, available_at)""")
    # ---- assemble ----
    no_do = "r.ni_q IS NOT NULL AND NOT w.do_recent" if inp.facts is not None else "false"
    doq = ("coalesce(w.do_3 * r.rq, CASE WHEN r.fqtr = 4 THEN w.do_4 * r.rq END, "
           f"CASE WHEN r.c_do_ttm = 0 OR ({no_do}) THEN 0.0 END)")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE j AS
        SELECT r.*, y.* EXCLUDE (cik, period_end, accession, available_at), w.* EXCLUDE (cik, period_end, accession, available_at), d.rdq_8k,
               d.rdq_filing,
               {doq} AS doq
        FROM r2 r JOIN ytd y USING (cik, period_end, accession, available_at) JOIN fw w USING (cik, period_end, accession, available_at)
        JOIN rd d USING (cik, period_end, accession, available_at)""")
    ibq = """CASE WHEN ib_3 IS NOT NULL THEN ib_3 * rq
                 WHEN fqtr = 4 AND ib_4 IS NOT NULL THEN ib_4 * rq
                 WHEN ni_q IS NOT NULL AND doq IS NOT NULL AND coalesce(c_xido_ttm, 0) = 0 THEN ni_q - doq END"""
    ibq_src = """CASE WHEN ib_3 IS NOT NULL THEN 'ib_3m'
                     WHEN fqtr = 4 AND ib_4 IS NOT NULL THEN 'ib_fy_less_9m'
                     WHEN ni_q IS NOT NULL AND doq IS NOT NULL AND coalesce(c_xido_ttm, 0) = 0
                          THEN CASE WHEN doq = 0 THEN 'niq_no_do' ELSE 'niq_less_doq' END END"""

    def eps(cont: str, tot: str) -> tuple[str, str]:
        v = (f"CASE WHEN {cont}_3 IS NOT NULL THEN {cont}_3 * rq WHEN doq = 0 AND {tot}_3 IS NOT NULL THEN {tot}_3 * rq "
             f"WHEN fqtr = 4 AND {cont}_4 IS NOT NULL THEN {cont}_4 * rq "
             f"WHEN fqtr = 4 AND doq = 0 AND {tot}_4 IS NOT NULL THEN {tot}_4 * rq END")
        s = (f"CASE WHEN {cont}_3 IS NOT NULL THEN 'cont_3m' WHEN doq = 0 AND {tot}_3 IS NOT NULL THEN 'total_3m' "
             f"WHEN fqtr = 4 AND {cont}_4 IS NOT NULL THEN 'cont_fy_less_9m' "
             f"WHEN fqtr = 4 AND doq = 0 AND {tot}_4 IS NOT NULL THEN 'total_fy_less_9m' END")
        return v, s

    epspx, epspx_src = eps("epsc_b", "eps_b")
    epsfx, _ = eps("epsc_d", "eps_d")
    cshpr = ("CASE WHEN waso_b_3 IS NOT NULL THEN waso_b_3 WHEN fqtr = 4 AND waso_b_4 > 0 THEN waso_b_4 END")
    rdq = "least(rdq_8k, rdq_filing)"
    rdq_src = "CASE WHEN rdq_8k IS NOT NULL AND rdq_8k <= rdq_filing THEN '8k_202' ELSE 'filing' END"
    zero = ("nullif(concat_ws(',', nullif(zero_filled, ''), nullif(catalog_zero_filled, '')), '')")
    desc = {"cik": "cik", "datadate": "period_end", "fyearq": "fye_idx // 12 - CASE WHEN fyr < 6 THEN 1 ELSE 0 END",
            "fqtr": "fqtr", "fyr": "fyr", "accession": "accession", "form": "form", "filed": "filed",
            "clock_utc": "clock_utc", "available_at": "available_at", "rdq": rdq, "rdq_src": rdq_src,
            "curcd": "currency", "fx_converted": "fx_converted", "fx_rate": "fx_rate",
            "fx_rate_avg_q": "fx_rate_avg_q", "first_reported": "first_reported", "vintage_kind": "vintage_kind",
            "is_restated": "is_restated",
            "restated_items": "restated_items", "is_amendment": "is_amendment",
            "nonreliance_402_at": "nonreliance_402_at", "fiscal_year": "fiscal_year", "fiscal_period": "fiscal_period",
            "fin_template": "fin_template", "sic": "sic_in_force", "zero_filled": zero}
    assert tuple(desc) == DESC_Q

    def src(s: str) -> str:
        return s if s.startswith('"') else f'"{s}"'

    q_items = [f'CAST({src(s)} AS DOUBLE) AS "{c}"' for c, s, _, _ in FUNDQ_ITEMS]
    q_items += [f"CAST({ibq} AS DOUBLE) AS ibq", f"CAST({epspx} AS DOUBLE) AS epspxq",
                f"CAST({epsfx} AS DOUBLE) AS epsfxq", f"CAST({cshpr} AS DOUBLE) AS cshprq"]
    q_items += [f"CAST({c} AS DOUBLE) AS {c}" for c, _, _, _ in YTD_ITEMS]
    q_desc = [f"{v} AS {k}" for k, v in desc.items()]
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE q_out AS
        SELECT {', '.join(q_desc)}, {', '.join(q_items)}, {ibq_src} AS ibq_src, {epspx_src} AS epspxq_src
        FROM j""")
    a_desc = [f"{v} AS {'fyear' if k == 'fyearq' else k}" for k, v in desc.items()
              if k not in ("fqtr", "fx_rate_avg_q")]
    a_desc.insert(a_desc.index("fx_rate AS fx_rate") + 1, "fx_rate_avg_ttm AS fx_rate_avg_ttm")
    a_items = [f'CAST({src(s)} AS DOUBLE) AS "{c}"' for c, s, _, _ in FUNDA_ITEMS]
    a_items += [f'CAST("c_{col}" AS DOUBLE) AS "{name}"' for name, col, _, _ in catalog_names()]
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE a_out AS
        SELECT {', '.join(a_desc)}, {', '.join(a_items)}
        FROM j WHERE fqtr = 4 AND dform IN ({annual})""")


def _vintages(con) -> None:
    """``r2`` = the event rows ``r2e`` plus restatement vintages: for every filing A that changed a
    ``quarterly_history`` value of datadate D after its first vintage, one row (D, A, source clock)
    at each distinct legitimate history clock. Delayed components of D's own accession are included; only an
    already-published filing row at the exact same clock is excluded. Future metadata cannot suppress earlier
    snapshots, and metadata whose acceptance clock is still in the future is not used. Base = D's latest event row at or before A; each item carried by the
    history moves by h(A) - h(base) (the item's latest D vintage visible at A minus at the base; reporting-currency
    deltas converted at the base row's spot / quarter / TTM rate), TTM flows by the summed delta over the four
    quarters ending at D, ceq / teq / intan by their components' deltas, fqtr 1-3 YTD sums are recomputed from the
    history visible at A; every other column is the base row's (identity, fiscal calendar, rdq inputs, cshoq,
    dlcq, dlttq, catalog items; ibq / EPS / cshprq are recomputed from the facts visible at A). Rows that change no
    column are dropped; ``restated_items`` lists the changed fundamentals item names."""
    con.execute("""
        CREATE OR REPLACE TEMP TABLE vk AS
        SELECT DISTINCT h.cik, h.period_end, h.accession AS v_acc, h.available_at AS t_a
        FROM hist h
        JOIN (SELECT cik, period_end, min(available_at) AS t0 FROM r2e GROUP BY 1, 2) f
          ON f.cik = h.cik AND f.period_end = h.period_end
        WHERE h.available_at > f.t0 AND NOT EXISTS (
            SELECT 1 FROM r2e e WHERE e.cik=h.cik AND e.period_end=h.period_end
            AND e.accession=h.accession AND e.available_at=h.available_at)""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE vbase AS
        SELECT k.v_acc, k.t_a, b.*
        FROM vk k JOIN r2e b ON b.cik = k.cik AND b.period_end = k.period_end AND b.available_at <= k.t_a
        QUALIFY row_number() OVER (PARTITION BY k.cik, k.period_end, k.v_acc, k.t_a
                                   ORDER BY b.available_at DESC, b.accession DESC) = 1""")
    key = "struct_pack(a := h.available_at, b := h.accession)"
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vd_long AS
        SELECT v.cik, v.period_end, v.v_acc, v.t_a, h.item,
               arg_max(h.value, {key}) FILTER (WHERE h.available_at <= v.t_a) AS h_n,
               arg_max(h.value, {key}) FILTER (WHERE h.available_at <= v.available_at) AS h_o
        FROM vbase v JOIN hist h ON h.cik = v.cik AND h.period_end = v.period_end AND h.currency = v.mccy
        GROUP BY 1, 2, 3, 4, 5""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vd4_long AS
        SELECT cik, period_end, v_acc, t_a, item, sum(h_n - h_o) AS d4 FROM (
            SELECT v.cik, v.period_end, v.v_acc, v.t_a, h.item, h.period_end AS qpe,
                   arg_max(h.value, {key}) FILTER (WHERE h.available_at <= v.t_a) AS h_n,
                   arg_max(h.value, {key}) FILTER (WHERE h.available_at <= v.available_at) AS h_o
            FROM vbase v JOIN range(0, 4) o(off) ON true
            JOIN hist h ON h.cik = v.cik AND h.h_idx = v.eidx - 3 * o.off AND h.currency = v.mccy
                       AND h.item IN ({", ".join(f"'{i}'" for i in sorted(set(VINT_TTM.values())))})
            GROUP BY 1, 2, 3, 4, 5, 6)
        WHERE h_n IS NOT NULL AND h_o IS NOT NULL GROUP BY 1, 2, 3, 4, 5""")
    ytd_h = sorted({h for _, h, _, _ in YTD_ITEMS})
    items = sorted(set(VINT_STOCK.values()) | set(VINT_FLOW_Q.values()) | set(ytd_h))
    piv = ", ".join(f"max(CASE WHEN item = '{i}' THEN h_n END) AS {i}_n, max(CASE WHEN item = '{i}' THEN h_o END) "
                    f"AS {i}_o" for i in items)
    piv4 = ", ".join(f"max(CASE WHEN item = '{i}' THEN d4 END) AS {i}_d4" for i in sorted(set(VINT_TTM.values())))
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vd AS
        SELECT k.cik, k.period_end, k.v_acc, k.t_a, a.* EXCLUDE (cik, period_end, v_acc, t_a), b.* EXCLUDE (cik, period_end, v_acc, t_a)
        FROM vk k
        LEFT JOIN (SELECT cik, period_end, v_acc, t_a, {piv} FROM vd_long GROUP BY 1, 2, 3, 4) a USING (cik, period_end, v_acc, t_a)
        LEFT JOIN (SELECT cik, period_end, v_acc, t_a, {piv4} FROM vd4_long GROUP BY 1, 2, 3, 4) b
          USING (cik, period_end, v_acc, t_a)""")
    spot = "CASE WHEN b.fx_converted THEN b.fx_rate ELSE 1.0 END"
    ttm = "CASE WHEN b.fx_converted THEN b.fx_rate_avg_ttm ELSE 1.0 END"
    new: dict[str, str] = {}
    for col, h in VINT_STOCK.items():
        rate = spot
        new[col] = (f"CASE WHEN d.{h}_n IS NULL OR d.{h}_n IS NOT DISTINCT FROM d.{h}_o THEN b.\"{col}\" "
                    f"WHEN b.\"{col}\" IS NULL OR d.{h}_o IS NULL THEN d.{h}_n * {rate} "
                    f"ELSE b.\"{col}\" + (d.{h}_n - d.{h}_o) * {rate} END")
    for col, h in VINT_FLOW_Q.items():
        new[col] = (f"CASE WHEN d.{h}_n IS NULL OR d.{h}_n IS NOT DISTINCT FROM d.{h}_o THEN b.\"{col}\" "
                    f"WHEN b.\"{col}\" IS NULL OR d.{h}_o IS NULL THEN d.{h}_n * b.rq "
                    f"ELSE b.\"{col}\" + (d.{h}_n - d.{h}_o) * b.rq END")
    for col, h in VINT_TTM.items():
        new[col] = (f"CASE WHEN coalesce(d.{h}_d4, 0) = 0 OR b.\"{col}\" IS NULL THEN b.\"{col}\" "
                    f"ELSE b.\"{col}\" + d.{h}_d4 * {ttm} END")
    for col, parts in VINT_DERIVED.items():
        # The stock can first appear in history after the base filing. Move derived components by the
        # actual published stock change, including that case, rather than requiring an old history fact.
        moves = " + ".join(f"({sign}) * (CASE WHEN ({new[h]}) IS NOT NULL THEN "
                           f"({new[h]}) - coalesce(b.\"{h}\", 0.0) ELSE 0.0 END)" for h, sign in parts)
        new[col] = f"CASE WHEN b.{col} IS NULL THEN NULL ELSE b.{col} + {moves} END"
    changed = " || ".join([f"CASE WHEN ({expr}) IS DISTINCT FROM b.\"{col}\" THEN ',{col}' ELSE '' END"
                           for col, expr in new.items()] +
                          # a restated discrete quarter of a YTD flow moves the row's YTD sum (recomputed downstream)
                          [f"CASE WHEN d.{h}_n IS NOT NULL AND d.{h}_n IS DISTINCT FROM d.{h}_o THEN ',{h}' ELSE '' END"
                           for h in ytd_h])
    repl = ", ".join(f"{expr} AS \"{col}\"" for col, expr in new.items())
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE vrows AS
        SELECT * FROM (
            SELECT b.* REPLACE ({repl},
                       b.v_acc AS accession, b.t_a AS available_at, coalesce(m.clock_utc, b.t_a) AS clock_utc,
                       m.filed AS filed, m.form AS form, coalesce(m.form LIKE '%/A', false) AS is_amendment,
                       true AS is_restated, m.nonreliance_402_at AS nonreliance_402_at,
                       CAST(timezone('America/New_York', CAST(b.t_a AS TIMESTAMPTZ)) AS DATE) AS filing_date,
                       'restatement' AS vintage_kind),
                   ltrim({changed}, ',') AS v_changed
            FROM vbase b JOIN vd d ON d.cik = b.cik AND d.period_end = b.period_end AND d.v_acc = b.v_acc AND d.t_a = b.t_a
            LEFT JOIN emeta m ON m.cik = b.cik AND m.accession = b.v_acc AND m.clock_utc <= b.t_a)
        WHERE v_changed <> ''""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE r2 AS
        SELECT * EXCLUDE (first_reported),
               row_number() OVER (PARTITION BY cik, period_end ORDER BY available_at, accession) = 1 AS first_reported
        FROM (SELECT * EXCLUDE (v_acc, t_a, v_changed, restated_items), v_changed AS restated_items FROM vrows
              UNION ALL BY NAME SELECT *, NULL AS first_reported FROM r2e)""")


ORDER = "ORDER BY cik, datadate, available_at, accession"


def derive_subset(con, inp: Inputs, cik_where: str, cutoff: dt.datetime | None = None) -> None:
    """Build ``q_out`` / ``a_out`` for the CIKs matching ``cik_where`` from inputs visible before ``cutoff``."""
    _setup(con, inp, cik_where, cutoff)
    _fx_quarters(con, inp, cutoff)
    _derive(con, inp)


# ---------------------------------------------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------------------------------------------

def _work(stage: str) -> Path:
    return C.build_root() / "_tmp" / stage


def load_fx_rows() -> tuple[list[tuple], dict[str, Any]]:
    path = ffx.fx_path()
    if not path.exists():
        return [], {"path": path.as_posix(), "present": False}
    tab = pq.read_table(path, columns=["obs_date", "currency", "usd_per_ccy", "available_at"])
    cols = [tab.column(c).to_pylist() for c in ("obs_date", "currency", "usd_per_ccy", "available_at")]
    return list(zip(*cols)), {**C.file_identity(path), "sha256": C.sha256_file(path), "rows": tab.num_rows}


def _manifest_sha(path: Path) -> str | None:
    return C.sha256_file(path) if path.exists() else None


def build(fund_stage: str = DEFAULT_FUND_STAGE, earn_stage: str = DEFAULT_EARN_STAGE, stage: str = STAGE,
          buckets: int = N_BUCKETS, build_date: dt.date | None = None) -> dict[str, Any]:
    root = C.build_root()
    fund, earn = root / fund_stage, root / earn_stage
    for p in (fund / "events.parquet", fund / "catalog.parquet", fund / "quarterly_history.parquet",
              earn / "announcements.parquet"):
        if not p.exists():
            raise FileNotFoundError(p)
    build_date = build_date or dt.datetime.now(dt.UTC).date()
    dmax = build_date + dt.timedelta(days=DATADATE_MAX_DAYS)
    work = _work(stage)
    parts = work / "parts"
    parts.mkdir(parents=True, exist_ok=True)
    for p in parts.glob("*.parquet"):
        p.unlink()
    receipt: dict[str, Any] = {"timings_s": {}}
    con = C.connect(memory=MEM, threads=2, db_file=f"{stage}_build.duckdb")
    try:
        with C.timed(receipt, "prepare_facts"):
            facts_info = prepare_facts(con, fund, work / "facts.parquet", dmax)
        fx_rows, fx_info = load_fx_rows()
        inp = Inputs(fund, earn, work / "facts.parquet" if facts_info.get("rows") else None, fx_rows, dmax)
        ev = _q(fund / "events.parquet")
        n_ev, n_out = con.execute(f"""SELECT count(*), count(*) FILTER (WHERE period_end < DATE '{DATADATE_MIN}'
                                          OR period_end > DATE '{dmax}') FROM read_parquet('{ev}')""").fetchone()
        h_out = con.execute(f"""SELECT count(*) FROM read_parquet('{_q(fund / 'quarterly_history.parquet')}')
                                WHERE period_end < DATE '{DATADATE_MIN}' OR period_end > DATE '{dmax}'""").fetchone()[0]
        with C.timed(receipt, "buckets"):
            for b in range(buckets):
                derive_subset(con, inp, f"cik % {buckets} = {b}")
                C.copy_to_parquet(con, "SELECT * FROM q_out", parts / f"fundq-{b:02d}.parquet", 32768)
                C.copy_to_parquet(con, "SELECT * FROM a_out", parts / f"funda-{b:02d}.parquet", 32768)
                print(f"bucket {b + 1}/{buckets} done", file=sys.stderr, flush=True)
        out = C.stage_dir(stage)
        with C.timed(receipt, "finalize"):
            nq = C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{_q(parts / 'fundq-*.parquet')}') {ORDER}",
                                   out / "fundq.parquet", 32768)
            na = C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{_q(parts / 'funda-*.parquet')}') {ORDER}",
                                   out / "funda.parquet", 16384)
        stats = summary_stats(con, out)
        stats["fundq_rows"], stats["funda_rows"] = nq, na
    finally:
        con.close()
    payload = {
        "rule": RULE, "code_version": CODE_VERSION, "rule_text": __doc__.split("Commands (")[0].strip(),
        "fund_stage": fund_stage, "earn_stage": earn_stage, "build_date": build_date.isoformat(),
        "datadate_window": [DATADATE_MIN.isoformat(), dmax.isoformat()],
        "excluded_outside_datadate_window": {"events": int(n_out), "events_total": int(n_ev),
                                             "quarterly_history": int(h_out),
                                             "company_facts": facts_info.get("excluded_outside_datadate_window")},
        "facts": facts_info, "fx": fx_info, "stats": stats, "timings_s": receipt["timings_s"],
        "outputs": {"fundq.parquet": "keys (cik, datadate, fyearq, fqtr, accession, available_at); one row per vintage",
                    "funda.parquet": "keys (cik, datadate, fyear, accession, available_at); annual-base periods and their clock vintages"},
        "items": {"fundq": [c for c, *_ in FUNDQ_ITEMS] + [c for c, *_ in DERIVED_Q] + [c for c, *_ in YTD_ITEMS],
                  "funda": [c for c, *_ in FUNDA_ITEMS] + [n for n, *_ in catalog_names()],
                  "catalog_rename": CATALOG_RENAME},
        "input_manifests_sha256": {f"{fund_stage}/manifest.json": _manifest_sha(fund / "manifest.json"),
                                   f"{earn_stage}/manifest.json": _manifest_sha(earn / "manifest.json"),
                                   "reference/manifest.json": _manifest_sha(root / "reference" / "manifest.json")},
        "gate_status": "pending: run validate, cutoff, handcheck",
    }
    path = C.write_stage_manifest(stage, SCHEMA, MODULES, payload, pattern="*.parquet")
    return {"manifest": path.as_posix(), **stats}


def summary_stats(con, out: Path) -> dict[str, Any]:
    fq, fa = _q(out / "fundq.parquet"), _q(out / "funda.parquet")
    r = con.execute(f"""SELECT count(*), count(DISTINCT cik), min(datadate), max(datadate), min(available_at),
                               max(available_at), count(*) FILTER (WHERE fqtr IS NULL),
                               count(*) FILTER (WHERE first_reported), count(*) FILTER (WHERE fx_converted),
                               count(*) FILTER (WHERE vintage_kind = 'restatement')
                        FROM read_parquet('{fq}')""").fetchone()
    lab = con.execute(f"""SELECT count(*) FILTER (WHERE fiscal_period IN ('Q1', 'Q2', 'Q3')),
                                 count(*) FILTER (WHERE fiscal_period IN ('Q1', 'Q2', 'Q3')
                                     AND fqtr <> TRY_CAST(substr(fiscal_period, 2, 1) AS INTEGER))
                          FROM read_parquet('{fq}')""").fetchone()
    src = {f"{c}": dict(con.execute(f"SELECT coalesce({c}, 'null'), count(*) FROM read_parquet('{fq}') GROUP BY 1 "
                                    "ORDER BY 2 DESC").fetchall()) for c in ("ibq_src", "epspxq_src", "rdq_src")}
    na = con.execute(f"SELECT count(*), count(DISTINCT cik) FROM read_parquet('{fa}')").fetchone()
    return {"fundq": {"rows": r[0], "ciks": r[1], "datadate_min": str(r[2]), "datadate_max": str(r[3]),
                      "available_at_min": str(r[4]), "available_at_max": str(r[5]), "fqtr_null": r[6],
                      "first_reported_rows": r[7], "fx_converted_rows": r[8], "restatement_rows": r[9],
                      "q_label_rows": lab[0], "fqtr_differs_from_q_label": lab[1], "sources": src},
            "funda": {"rows": na[0], "ciks": na[1]}}


# ---------------------------------------------------------------------------------------------------------------
# as-of macros (serving catalog)
# ---------------------------------------------------------------------------------------------------------------

def macros_sql(stage_path: Path) -> list[str]:
    """``fundq_asof(ts)`` / ``funda_asof(ts)``: per (cik, datadate) the latest vintage with available_at < ts."""
    out = []
    for name in ("fundq", "funda"):
        p = _q(Path(stage_path) / f"{name}.parquet")
        out.append(f"CREATE OR REPLACE MACRO {name}_asof(ts) AS TABLE SELECT * FROM read_parquet('{p}') "
                   "WHERE available_at < CAST(ts AS TIMESTAMP) QUALIFY row_number() OVER (PARTITION BY cik, datadate "
                   "ORDER BY available_at DESC, accession DESC) = 1")
    return out


# ---------------------------------------------------------------------------------------------------------------
# validation gates
# ---------------------------------------------------------------------------------------------------------------

COVERAGE_GATES = {"saleq": 0.95, "ibq": 0.95, "atq": 0.95, "ceqq": 0.95, "cheq": 0.95, "cshoq": 0.95,
                  "oancfy": 0.92, "capxy": 0.92}
COVERAGE_INFO = ("niq", "ltq", "seqq", "teqq", "epspxq", "epsfxq", "cshprq", "dvy", "prstkcy", "sstky", "xrdy",
                 "cogsq", "xsgaq", "oiadpq", "txtq", "dlttq", "dlcq", "actq", "lctq")
RDQ_GATE, SUM4_GATE, BAL_GATE, FSDS_GATE = 0.95, 0.98, 0.97, 0.98
REL_TOL = 0.005
FSDS_CELLS = 6000


def _r(v: Any, nd: int = 4) -> Any:
    return None if v is None else round(float(v), nd)


def member_ciks(con, years: tuple[int, ...] = GATE_YEARS) -> None:
    """Temp table ``mem (year, cik)``: CIKs linked (identity/link_table_v3 valid on the session) to a member cell of
    panel ``member_equity`` (``_tmp/panel_member``) in that year."""
    root = C.build_root()
    lt = _q(root / "identity" / "link_table_v3.parquet")
    parts = []
    for y in years:
        mem = root / "_tmp" / "panel_member" / f"year={y}.parquet"
        if mem.exists():
            parts.append(f"""SELECT {y} AS year, l.cik FROM read_parquet('{_q(mem)}') m
                             JOIN read_parquet('{lt}') l ON l.security_id = m.security_id
                              AND m.session_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01')
                             WHERE m.member AND l.cik IS NOT NULL""")
    if not parts:
        raise FileNotFoundError("no _tmp/panel_member/year=*.parquet")
    con.execute(f"CREATE OR REPLACE TEMP TABLE mem AS SELECT DISTINCT year, cik FROM ({' UNION ALL '.join(parts)})")


def coverage_gate(con, out: Path) -> dict[str, Any]:
    fq = _q(out / "fundq.parquet")
    qforms = ", ".join(f"'{f}'" for f in QUARTERLY_FORMS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE cov AS
        SELECT q.*, year(q.datadate) AS year FROM read_parquet('{fq}') q
        JOIN mem m ON m.cik = q.cik AND m.year = year(q.datadate)
        WHERE q.first_reported AND q.form IN ({qforms})""")
    res: dict[str, Any] = {"basis": "first-reported fundq row per (cik, datadate), datadate in the year, 10-Q/10-K-"
                                    "type forms (20-F/40-F annual-only rows excluded), CIK linked to a member_equity "
                                    "cell that year (identity/link_table_v3)", "by_year": {}, "gates": COVERAGE_GATES}
    fails = []
    for y in GATE_YEARS:
        n = con.execute(f"SELECT count(*) FROM cov WHERE year = {y}").fetchone()[0]
        yr = {"issuer_quarters": n}
        for c in list(COVERAGE_GATES) + list(COVERAGE_INFO):
            k = con.execute(f"SELECT count(*) FILTER (WHERE {c} IS NOT NULL AND isfinite({c})) FROM cov "
                            f"WHERE year = {y}").fetchone()[0]
            yr[c] = _r(k / n) if n else None
            if c in COVERAGE_GATES and (not n or k / n < COVERAGE_GATES[c]):
                fails.append(f"{c}@{y}")
        res["by_year"][str(y)] = yr
    res["fails"] = fails
    res["pass"] = not fails
    return res


def rdq_gate(con, out: Path, earn: Path) -> dict[str, Any]:
    ann = _q(earn / "announcements.parquet")
    if (out / "manifest.json").exists():
        man = C.read_json(out / "manifest.json")
        events = _q(C.build_root() / man.get("fund_stage", DEFAULT_FUND_STAGE) / "events.parquet")
        con.execute(f"CREATE OR REPLACE TEMP TABLE filing_clocks AS "
                    f"SELECT cik, accession, clock_basis FROM read_parquet('{events}')")
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE filing_clocks AS SELECT NULL::BIGINT AS cik, "
                    "NULL::VARCHAR AS accession, NULL::VARCHAR AS clock_basis WHERE false")
    con.execute(f"""CREATE OR REPLACE TEMP TABLE ann_all AS SELECT cik, fiscal_period_end, is_primary,
                        CAST(timezone('America/New_York', CAST(announcement_utc AS TIMESTAMPTZ)) AS DATE) AS ann_date,
                        CAST(timezone('America/New_York', CAST(available_at AS TIMESTAMPTZ)) AS DATE) AS accepted_date
                    FROM read_parquet('{ann}')""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE rq AS
        SELECT c.year, c.cik, c.datadate, c.rdq, c.rdq_src, c.available_at,
               EXISTS (SELECT 1 FROM ann_all a WHERE a.cik = c.cik AND a.is_primary
                       AND abs(datediff('day', a.fiscal_period_end, c.datadate)) <= {RDQ_MATCH_DAYS}) AS any_8k,
               EXISTS (SELECT 1 FROM ann_all a WHERE a.cik = c.cik AND a.is_primary AND a.ann_date = c.rdq
                       AND abs(datediff('day', a.fiscal_period_end, c.datadate)) <= {RDQ_MATCH_DAYS}) AS corroborated,
               EXISTS (SELECT 1 FROM ann_all a WHERE a.cik = c.cik AND a.is_primary
                       AND a.accepted_date <= CASE WHEN c.clock_utc IS NOT NULL
                           AND fc.clock_basis = 'fsds_accepted_utc'
                           THEN CAST(timezone('America/New_York', CAST(c.clock_utc AS TIMESTAMPTZ)) AS DATE)
                           ELSE c.filed END
                       AND abs(datediff('day', a.fiscal_period_end, c.datadate)) <= {RDQ_MATCH_DAYS}) AS same_date,
               NOT EXISTS (SELECT 1 FROM ann_all a WHERE a.cik = c.cik AND a.ann_date > c.datadate - 400
                           AND a.ann_date <= c.datadate + 120) AS issuer_no_202
        FROM cov c LEFT JOIN filing_clocks fc USING (cik, accession)
        WHERE c.form IN ('10-Q', '10-QT')""")
    res: dict[str, Any] = {"basis": "first-reported 10-Q/10-QT fundq rows of linked member issuers (as coverage)",
                           "gate": RDQ_GATE, "by_year": {}}
    fails = []
    for y in GATE_YEARS:
        n, pop, k8, any8, cor, no202, sd = con.execute(f"""
            SELECT count(*), count(rdq), count(*) FILTER (WHERE rdq_src = '8k_202'), count(*) FILTER (WHERE any_8k),
                   count(*) FILTER (WHERE corroborated), count(*) FILTER (WHERE issuer_no_202),
                   count(*) FILTER (WHERE same_date)
            FROM rq WHERE year = {y}""").fetchone()
        res["by_year"][str(y)] = {"periods": n, "rdq_populated": _r(pop / n) if n else None,
                                  "rdq_8k_same_date": _r(sd / n) if n else None,
                                  "rdq_from_8k_pit": _r(k8 / n) if n else None,
                                  "info_rdq_equals_matched_8k_date_any_order": _r(cor / n) if n else None,
                                  "info_period_has_primary_8k_any_time": _r(any8 / n) if n else None,
                                  "info_issuer_files_no_202_near_period": _r(no202 / n) if n else None}
        if not n or sd / n < RDQ_GATE:
            fails.append(str(y))
    res["metric"] = ("rdq_8k_same_date (ruling T3-b): a primary 8-K 2.02 matched to datadate accepted on or before the "
                     "ET calendar date of the row's filing (clock_utc, filed fallback); rdq_from_8k_pit (strict: accepted by the "
                     "row's available_at, rdq_src = 8k_202) is reported beside it")
    res["fails"], res["pass"] = fails, not fails
    return res


def sum4_gate(con, out: Path) -> dict[str, Any]:
    fq, fa = _q(out / "fundq.parquet"), _q(out / "funda.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE lq AS
        SELECT * FROM read_parquet('{fq}')
        QUALIFY row_number() OVER (PARTITION BY cik, datadate ORDER BY available_at DESC, accession DESC) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE la AS
        SELECT a.* FROM read_parquet('{fa}') a JOIN mem m ON m.cik = a.cik AND m.year = year(a.datadate)
        WHERE year(a.datadate) BETWEEN {GATE_YEARS[0]} AND {GATE_YEARS[-1]}
        QUALIFY row_number() OVER (PARTITION BY a.cik, a.datadate ORDER BY a.available_at DESC, a.accession DESC) = 1""")
    res: dict[str, Any] = {"basis": "latest funda row per (cik, datadate) of linked member issuers (any vintage) with "
                                    "four latest-vintage fundq quarters of the same (cik, fyearq) (fqtr 1-4, quarter "
                                    "ends within 400 days before datadate); |sum - annual| <= 0.5% of |annual|",
                           "gate": SUM4_GATE, "by_year": {}}
    fails = []
    pairs = (("saleq", "sale"), ("niq", "ni"), ("ibq", "ib"))
    for qc, ac in pairs:
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE s4_{qc} AS
            SELECT year(a.datadate) AS year, a.cik, a.datadate, a."{ac}" AS annual, sum(q.{qc}) AS qsum,
                   count(q.{qc}) AS nq, count(DISTINCT q.fqtr) AS nf, a.available_at AS a_av, a.fx_converted,
                   list(q.datadate) AS qds
            FROM la a JOIN lq q ON q.cik = a.cik AND q.fyearq = a.fyear AND q.fqtr BETWEEN 1 AND 4
                 AND q.datadate <= a.datadate AND q.datadate > a.datadate - INTERVAL 400 DAY
            WHERE a."{ac}" IS NOT NULL
            GROUP BY ALL HAVING count(q.{qc}) = 4 AND count(DISTINCT q.fqtr) = 4""")
    man = C.read_json(out / "manifest.json")
    hist = _q(C.build_root() / man.get("fund_stage", DEFAULT_FUND_STAGE) / "quarterly_history.parquet")
    for qc, item in (("saleq", "sale_q"), ("niq", "ni_q")):
        # info: the same quarters as the fundamentals stage knew them when the annual row was filed
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE s4k_{qc} AS
            SELECT cik, datadate, sum(value) AS ksum, count(*) AS nk FROM (
                SELECT s.cik, s.datadate, u.qd, h.value,
                       row_number() OVER (PARTITION BY s.cik, s.datadate, u.qd
                                          ORDER BY h.available_at DESC, h.accession DESC) AS rn
                FROM s4_{qc} s, unnest(s.qds) AS u(qd)
                JOIN (SELECT cik, period_end, value, available_at, accession FROM read_parquet('{hist}')
                      WHERE item = '{item}' AND currency = 'USD') h
                  ON h.cik = s.cik AND h.period_end = u.qd AND h.available_at <= s.a_av
                WHERE NOT s.fx_converted)
            WHERE rn = 1 GROUP BY 1, 2""")
    for y in GATE_YEARS:
        yr = {}
        for qc, ac in pairs:
            n, ok, ok_abs = con.execute(f"""
                SELECT count(*), count(*) FILTER (WHERE abs(qsum - annual) <= {REL_TOL} * abs(annual)),
                       count(*) FILTER (WHERE abs(qsum - annual) <= greatest({REL_TOL} * abs(annual), 1e6))
                FROM s4_{qc} WHERE year = {y}""").fetchone()
            yr[qc] = {"fiscal_years": n, "within_0_5pct": _r(ok / n) if n else None,
                      "within_0_5pct_or_1m": _r(ok_abs / n) if n else None}
            if qc in ("saleq", "niq"):
                nk, okk = con.execute(f"""
                    SELECT count(*), count(*) FILTER (WHERE k.nk = 4 AND abs(k.ksum - s.annual) <= {REL_TOL} * abs(s.annual))
                    FROM s4_{qc} s LEFT JOIN s4k_{qc} k USING (cik, datadate)
                    WHERE s.year = {y} AND NOT s.fx_converted""").fetchone()
                yr[qc]["info_within_0_5pct_quarters_as_known_at_annual"] = _r(okk / nk) if nk else None
            if qc in ("saleq", "niq") and (not n or ok / n < SUM4_GATE):
                fails.append(f"{qc}@{y}")
        res["by_year"][str(y)] = yr
    res["gated_items"] = ["saleq", "niq"]
    res["fails"], res["pass"] = fails, not fails
    ex = con.execute("""SELECT cik, datadate, annual, qsum FROM s4_saleq WHERE abs(qsum - annual) > 0.005 * abs(annual)
                        ORDER BY abs(qsum - annual) / greatest(abs(annual), 1) DESC LIMIT 8""").fetchall()
    res["saleq_examples"] = [{"cik": e[0], "datadate": str(e[1]), "annual": e[2], "qsum": e[3]} for e in ex]
    return res


def balance_gate(con, out: Path) -> dict[str, Any]:
    man = C.read_json(out / "manifest.json")
    cat = _q(C.build_root() / man.get("fund_stage", DEFAULT_FUND_STAGE) / "catalog.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE bq AS
        SELECT year(q.datadate) AS year, q.atq, q.ltq, q.teqq, q.seqq, q.mibq, a.mibt
        FROM lq q JOIN mem m ON m.cik = q.cik AND m.year = year(q.datadate)
        LEFT JOIN LATERAL (
            SELECT mibt FROM read_parquet('{cat}') a
            WHERE a.cik = q.cik AND a.period_end = q.datadate AND a.available_at <= q.available_at
            ORDER BY a.available_at DESC, a.accession DESC LIMIT 1) a ON true
        WHERE year(q.datadate) BETWEEN {GATE_YEARS[0]} AND {GATE_YEARS[-1]} AND q.atq IS NOT NULL""")
    res: dict[str, Any] = {"basis": "latest fundq row per (cik, datadate) with atq, linked member issuers; "
                                    "|atq - (ltq + teqq)| <= 0.5% |atq|; info: with the fundamentals catalog "
                                    "temporary-equity term mibt (atq = ltq + mibt + teqq)", "gate": BAL_GATE, "by_year": {}}
    fails = []
    for y in GATE_YEARS:
        n, ok, miss, okm = con.execute(f"""
            SELECT count(*), count(*) FILTER (WHERE abs(atq - (ltq + teqq)) <= {REL_TOL} * abs(atq)),
                   count(*) FILTER (WHERE ltq IS NULL OR teqq IS NULL),
                   count(*) FILTER (WHERE abs(atq - (ltq + coalesce(mibt, 0) + teqq)) <= {REL_TOL} * abs(atq))
            FROM bq WHERE year = {y}""").fetchone()
        res["by_year"][str(y)] = {"issuer_quarters": n, "ok": _r(ok / n) if n else None, "ltq_or_teqq_missing": miss,
                                  "info_ok_with_temporary_equity": _r(okm / n) if n else None}
        if not n or ok / n < BAL_GATE:
            fails.append(str(y))
    res["fails"], res["pass"] = fails, not fails
    return res


# FSDS benchmark: column -> (table, qtrs (-1 = fqtr, YTD), tags in priority order, FSDS uom; per-share is 'USD')
FSDS_ITEMS: dict[str, tuple[str, int, tuple[str, ...], str]] = {
    "saleq": ("q", 1, ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"), "USD"),
    "niq": ("q", 1, ("NetIncomeLoss",), "USD"), "oiadpq": ("q", 1, ("OperatingIncomeLoss",), "USD"),
    "cogsq": ("q", 1, ("CostOfGoodsAndServicesSold", "CostOfRevenue"), "USD"),
    "txtq": ("q", 1, ("IncomeTaxExpenseBenefit",), "USD"), "xintq": ("q", 1, ("InterestExpense",), "USD"),
    "xsgaq": ("q", 1, ("SellingGeneralAndAdministrativeExpense",), "USD"),
    "ibq": ("q", 1, ("IncomeLossFromContinuingOperations",), "USD"),
    "epspxq": ("q", 1, ("IncomeLossFromContinuingOperationsPerBasicShare", "EarningsPerShareBasic"), "USD"),
    "epsfxq": ("q", 1, ("IncomeLossFromContinuingOperationsPerDilutedShare", "EarningsPerShareDiluted"), "USD"),
    "cshprq": ("q", 1, ("WeightedAverageNumberOfSharesOutstandingBasic",), "shares"),
    "oancfy": ("q", -1, ("NetCashProvidedByUsedInOperatingActivities",), "USD"),
    "capxy": ("q", -1, ("PaymentsToAcquirePropertyPlantAndEquipment",), "USD"),
    "dvy": ("q", -1, ("PaymentsOfDividendsCommonStock",), "USD"),
    "prstkcy": ("q", -1, ("PaymentsForRepurchaseOfCommonStock",), "USD"),
    "xrdy": ("q", -1, ("ResearchAndDevelopmentExpense",), "USD"),
    "atq": ("q", 0, ("Assets",), "USD"), "ltq": ("q", 0, ("Liabilities",), "USD"),
    "seqq": ("q", 0, ("StockholdersEquity",), "USD"),
    "teqq": ("q", 0, ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",), "USD"),
    "cheq": ("q", 0, ("CashCashEquivalentsAndShortTermInvestments",), "USD"),
    "actq": ("q", 0, ("AssetsCurrent",), "USD"), "lctq": ("q", 0, ("LiabilitiesCurrent",), "USD"),
    "invtq": ("q", 0, ("InventoryNet",), "USD"), "rectq": ("q", 0, ("AccountsReceivableNetCurrent",), "USD"),
    "apq": ("q", 0, ("AccountsPayableCurrent",), "USD"), "ppentq": ("q", 0, ("PropertyPlantAndEquipmentNet",), "USD"),
    "gdwlq": ("q", 0, ("Goodwill",), "USD"), "dlttq": ("q", 0, ("LongTermDebtNoncurrent",), "USD"),
    "intanq": ("q", 0, ("IntangibleAssetsNetIncludingGoodwill",), "USD"),
    "sale": ("a", 4, ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax"), "USD"),
    "ni": ("a", 4, ("NetIncomeLoss",), "USD"), "oancf": ("a", 4, ("NetCashProvidedByUsedInOperatingActivities",), "USD"),
    "capx": ("a", 4, ("PaymentsToAcquirePropertyPlantAndEquipment",), "USD"),
    "ib": ("a", 4, ("IncomeLossFromContinuingOperations",), "USD"),
    "epspi": ("a", 4, ("EarningsPerShareBasic",), "USD"), "epsfi": ("a", 4, ("EarningsPerShareDiluted",), "USD"),
    "epspx": ("a", 4, ("IncomeLossFromContinuingOperationsPerBasicShare", "EarningsPerShareBasic"), "USD"),
    "cshpri": ("a", 4, ("WeightedAverageNumberOfSharesOutstandingBasic",), "shares"),
}


def fsds_gate(con, out: Path, n_cells: int = FSDS_CELLS) -> dict[str, Any]:
    """FSDS NUM cells (own period, non-dimensional us-gaap, 10-Q / 10-K, datadate years in ``GATE_YEARS``) compared
    with the filing's own fundq (funda for qtrs 4) row: mapped = our value exists, match = within 0.5%."""
    num = _q(C.FSDS_DIR / "num" / "*.parquet")
    sub = _q(C.FSDS_DIR / "sub" / "*.parquet")
    spec = [(c, t, q, rk, tag, uom) for c, (t, q, tags, uom) in FSDS_ITEMS.items() for rk, tag in enumerate(tags)]
    con.execute("CREATE OR REPLACE TEMP TABLE fspec (col VARCHAR, tbl VARCHAR, qtrs INTEGER, rnk INTEGER, tag VARCHAR, "
                "uom VARCHAR)")
    con.executemany("INSERT INTO fspec VALUES (?, ?, ?, ?, ?, ?)", spec)
    tags = ", ".join(sorted({f"'{s[4]}'" for s in spec}))
    fq, fa = _q(out / "fundq.parquet"), _q(out / "funda.parquet")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE own AS
        SELECT cik, accession, datadate, available_at, fqtr, form FROM read_parquet('{fq}')
        WHERE year(datadate) BETWEEN {GATE_YEARS[0]} AND {GATE_YEARS[-1]} AND form IN ('10-Q', '10-K')
          AND vintage_kind = 'filing'""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE fcells AS
        WITH n AS (
            SELECT n.adsh, n.tag, n.ddate, n.qtrs, n.uom, max(CAST(n.value AS DOUBLE)) AS value
            FROM read_parquet('{num}') n
            WHERE n.tag IN ({tags}) AND n.version LIKE 'us-gaap/%' AND n.segments IS NULL
              AND (n.coreg IS NULL OR n.coreg = '') AND n.value IS NOT NULL
              AND n.adsh IN (SELECT accession FROM own)
            GROUP BY ALL HAVING count(*) = 1)
        SELECT o.cik, o.accession, o.datadate, o.available_at, o.fqtr, s.col, s.tbl, s.rnk, n.tag, n.qtrs, n.value AS fsds,
               year(o.datadate) AS year
        FROM n JOIN own o ON o.accession = n.adsh AND abs(datediff('day', o.datadate, n.ddate)) <= 3
        JOIN fspec s ON s.tag = n.tag AND s.uom = n.uom
             AND ((s.qtrs = 0 AND n.qtrs = 0) OR (s.qtrs = 1 AND n.qtrs = 1)
                  OR (s.qtrs = -1 AND n.qtrs = o.fqtr AND o.fqtr BETWEEN 1 AND 3)
                  OR (s.qtrs = 4 AND n.qtrs = 4 AND o.fqtr = 4 AND o.form = '10-K'))
        QUALIFY s.rnk = min(s.rnk) OVER (PARTITION BY o.accession, s.col)""")
    strata = con.execute("SELECT count(DISTINCT (col, year)) FROM fcells").fetchone()[0]
    quota = max(1, math.ceil(n_cells / max(strata, 1)))
    con.execute(f"""CREATE OR REPLACE TEMP TABLE fsamp AS SELECT * FROM fcells
                    QUALIFY row_number() OVER (PARTITION BY col, year ORDER BY hash(accession || col)) <= {quota}""")
    qcols = [c for c, v in FSDS_ITEMS.items() if v[0] == "q"]
    acols = [c for c, v in FSDS_ITEMS.items() if v[0] == "a"]
    qcase = " ".join(f"WHEN '{c}' THEN q.{c}" for c in qcols)
    acase = " ".join(f"WHEN '{c}' THEN a.{c}" for c in acols)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE fbm AS
        SELECT s.*, CASE WHEN s.tbl = 'q' THEN (CASE s.col {qcase} END) ELSE (CASE s.col {acase} END) END AS ours
        FROM fsamp s
        LEFT JOIN read_parquet('{fq}') q ON q.cik = s.cik AND q.accession = s.accession AND q.datadate = s.datadate
          AND q.available_at = s.available_at
        LEFT JOIN read_parquet('{fa}') a ON a.cik = s.cik AND a.accession = s.accession AND a.datadate = s.datadate
          AND a.available_at = s.available_at""")
    tol = f"abs(ours - fsds) <= {REL_TOL} * greatest(abs(fsds), 1e-9)"
    n, mapped, match = con.execute(f"SELECT count(*), count(ours), count(*) FILTER (WHERE ours IS NOT NULL AND {tol}) "
                                   "FROM fbm").fetchone()
    by_col = {r[0]: {"cells": r[1], "mapped": _r(r[2] / r[1]), "match_of_mapped": _r(r[3] / r[2]) if r[2] else None}
              for r in con.execute(f"""SELECT col, count(*), count(ours), count(*) FILTER (WHERE ours IS NOT NULL
                                              AND {tol}) FROM fbm GROUP BY 1 ORDER BY 1""").fetchall()}
    miss = [dict(zip(("col", "cik", "accession", "datadate", "fsds", "ours"), (r[0], r[1], r[2], str(r[3]), r[4], r[5])))
            for r in con.execute(f"SELECT col, cik, accession, datadate, fsds, ours FROM fbm WHERE ours IS NOT NULL "
                                 f"AND NOT {tol} ORDER BY col, accession LIMIT 40").fetchall()]
    return {"rule": fsds_gate.__doc__.strip(), "cells": n, "strata": strata, "quota": quota, "mapped": mapped,
            "matched": match, "mapped_share": _r(mapped / n) if n else None,
            "match_share_of_mapped": _r(match / mapped) if mapped else None, "gate": FSDS_GATE,
            "pass": bool(mapped and match / mapped >= FSDS_GATE), "by_col": by_col, "mismatch_examples": miss}


def validate(stage: str = STAGE, checks: tuple[str, ...] = ("coverage", "rdq", "sum4", "balance", "fsds")
             ) -> dict[str, Any]:
    out = C.build_root() / stage
    man = C.read_json(out / "manifest.json")
    earn = C.build_root() / man.get("earn_stage", DEFAULT_EARN_STAGE)
    vpath = out / "validation.json"
    doc = C.read_json(vpath) if vpath.exists() else {}
    con = C.connect(memory=MEM, threads=2, db_file=f"{stage}_validate.duckdb")
    try:
        member_ciks(con)
        t0 = time.perf_counter()
        doc["coverage"] = coverage_gate(con, out)
        if "rdq" in checks:
            doc["rdq"] = rdq_gate(con, out, earn)
        if "sum4" in checks or "balance" in checks:
            doc["sum4"] = sum4_gate(con, out)
            doc["balance"] = balance_gate(con, out)
        if "fsds" in checks:
            doc["fsds"] = fsds_gate(con, out)
        doc["validated_utc"] = dt.datetime.now(dt.UTC).isoformat()
        doc["seconds"] = round(time.perf_counter() - t0, 1)
    finally:
        con.close()
    C.write_json_atomic(vpath, doc)
    publish_gate_status(stage)
    return {k: (v.get("pass") if isinstance(v, dict) else v) for k, v in doc.items()}


def publish_gate_status(stage: str = STAGE) -> dict[str, Any]:
    """Copy each gate's PASS/FAIL from ``validation.json`` into the manifest (files and hashes unchanged)."""
    out = C.build_root() / stage
    man = C.read_json(out / "manifest.json")
    doc = C.read_json(out / "validation.json")
    status = {k: ("PASS" if v.get("pass") else "FAIL") for k, v in doc.items() if isinstance(v, dict) and "pass" in v}
    man["gate_status"] = status
    man["validation_sha256"] = C.sha256_file(out / "validation.json")
    C.write_json_atomic(out / "manifest.json", man)
    return status


# ---------------------------------------------------------------------------------------------------------------
# cutoff rebuild
# ---------------------------------------------------------------------------------------------------------------

CUTOFF_CIKS, CUTOFF_FX_CIKS, CUTOFF_SEED = 80, 10, 20261002
CUTOFFS = (dt.datetime(2017, 6, 30, 22), dt.datetime(2020, 3, 31, 22), dt.datetime(2021, 8, 13, 22),
           dt.datetime(2023, 5, 10, 22), dt.datetime(2025, 11, 14, 22))


def _norm(v: Any) -> Any:
    if isinstance(v, float):
        return "nan" if math.isnan(v) else round(v, 6)
    return v


def cutoff_check(stage: str = STAGE, n_ciks: int = CUTOFF_CIKS, seed: int = CUTOFF_SEED,
                 cutoffs: tuple[dt.datetime, ...] = CUTOFFS) -> dict[str, Any]:
    """For ``n_ciks`` seeded CIKs (half of them linked member issuers) plus ``CUTOFF_FX_CIKS`` FX-converted issuers
    and each cutoff: rebuild the stage from the
    inputs visible before the cutoff (fundamentals rows / history / facts / announcements / FX observations with clock
    < cutoff) and compare with the published rows with available_at < cutoff, column by column."""
    root = C.build_root()
    out = root / stage
    man = C.read_json(out / "manifest.json")
    fund, earn = root / man["fund_stage"], root / man["earn_stage"]
    dmax = dt.date.fromisoformat(man["datadate_window"][1])
    facts = _work(stage) / "facts.parquet"
    fx_rows, _ = load_fx_rows()
    inp = Inputs(fund, earn, facts if facts.exists() else None, fx_rows, dmax)
    con = C.connect(memory=MEM, threads=2, db_file=f"{stage}_cutoff.duckdb")
    res: dict[str, Any] = {"seed": seed, "cutoffs": [c.isoformat() for c in cutoffs], "compared_rows": 0,
                           "compared_cells": 0, "differences": 0, "row_set_differences": 0, "examples": []}
    t0 = time.perf_counter()
    try:
        member_ciks(con)
        pool_m = [r[0] for r in con.execute("SELECT DISTINCT cik FROM mem ORDER BY 1").fetchall()]
        pool_a = [r[0] for r in con.execute(f"SELECT DISTINCT cik FROM read_parquet('{_q(out / 'fundq.parquet')}') "
                                            "ORDER BY 1").fetchall()]
        pool_x = [r[0] for r in con.execute(f"SELECT DISTINCT cik FROM read_parquet('{_q(out / 'fundq.parquet')}') "
                                            "WHERE fx_converted ORDER BY 1").fetchall()]
        rng = random.Random(seed)
        ciks = sorted(set(rng.sample(pool_m, min(n_ciks // 2, len(pool_m))) +
                          rng.sample(pool_a, min(n_ciks - n_ciks // 2, len(pool_a))) +
                          rng.sample(pool_x, min(CUTOFF_FX_CIKS, len(pool_x)))))
        res["fx_converted_ciks"] = len(set(ciks) & set(pool_x))
        res["ciks"] = len(ciks)
        where = f"cik IN ({', '.join(map(str, ciks))})"
        for cut in cutoffs:
            derive_subset(con, inp, where, cut)
            for name, tmp in (("fundq", "q_out"), ("funda", "a_out")):
                pub = con.execute(f"SELECT * FROM read_parquet('{_q(out / f'{name}.parquet')}') WHERE {where} "
                                  f"AND available_at < {_ts(cut)} {ORDER}").fetchall()
                cols = [d[0] for d in con.description]
                reb = con.execute(f"SELECT * FROM {tmp} {ORDER}").fetchall()
                rcols = [d[0] for d in con.description]
                if cols != rcols:
                    res["differences"] += 1
                    res["examples"].append({"cutoff": cut.isoformat(), "table": name, "columns_differ": True})
                    continue
                key = lambda row: (row[cols.index("cik")], row[cols.index("datadate")],  # noqa: E731
                                   row[cols.index("accession")], row[cols.index("available_at")])
                pm, rm = {key(r): r for r in pub}, {key(r): r for r in reb}
                if set(pm) != set(rm):
                    res["row_set_differences"] += len(set(pm) ^ set(rm))
                    if len(res["examples"]) < 10:
                        res["examples"].append({"cutoff": cut.isoformat(), "table": name,
                                                "only_published": [list(k) for k in sorted(set(pm) - set(rm))[:3]],
                                                "only_rebuilt": [list(k) for k in sorted(set(rm) - set(pm))[:3]]})
                for k in set(pm) & set(rm):
                    res["compared_rows"] += 1
                    for i, c in enumerate(cols):
                        res["compared_cells"] += 1
                        if _norm(pm[k][i]) != _norm(rm[k][i]):
                            res["differences"] += 1
                            if len(res["examples"]) < 10:
                                res["examples"].append({"cutoff": cut.isoformat(), "table": name, "key": list(k),
                                                        "column": c, "published": str(pm[k][i]),
                                                        "rebuilt": str(rm[k][i])})
    finally:
        con.close()
    res["seconds"] = round(time.perf_counter() - t0, 1)
    res["pass"] = res["differences"] == 0 and res["row_set_differences"] == 0 and res["compared_rows"] > 0
    vpath = out / "validation.json"
    doc = C.read_json(vpath) if vpath.exists() else {}
    doc["cutoff"] = res
    C.write_json_atomic(vpath, doc)
    publish_gate_status(stage)
    return res


# ---------------------------------------------------------------------------------------------------------------
# hand check against the 10-Q (SEC inline XBRL of the filing's primary document)
# ---------------------------------------------------------------------------------------------------------------

HAND_ITEMS: dict[str, tuple[str, tuple[str, ...]]] = {
    # column -> (period kind: q = 3 months ending datadate, y = fiscal YTD ending datadate, i = instant), concepts
    "saleq": ("q", ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax", "SalesRevenueNet",
                    "RevenueFromContractWithCustomerIncludingAssessedTax")),
    "niq": ("q", ("NetIncomeLoss", "ProfitLoss")),
    "atq": ("i", ("Assets",)),
    "ltq": ("i", ("Liabilities",)),
    "seqq": ("i", ("StockholdersEquity",)),
    "oancfy": ("y", ("NetCashProvidedByUsedInOperatingActivities",
                     "NetCashProvidedByUsedInOperatingActivitiesContinuingOperations")),
    "epspxq": ("q", ("IncomeLossFromContinuingOperationsPerBasicShare", "EarningsPerShareBasic",
                     "EarningsPerShareBasicAndDiluted")),
    "cshoq": ("c", ("EntityCommonStockSharesOutstanding",)),
}
HAND_N, HAND_SEED = 30, 20261002
_CTX = re.compile(r"<(?:xbrli:)?context\b[^>]*\bid=\"([^\"]+)\"[^>]*>(.*?)</(?:xbrli:)?context>", re.S | re.I)
_NF_OPEN = re.compile(r"<ix:nonFraction\b([^>]*)>", re.I)
_ATTR = re.compile(r"(\w+(?::\w+)?)=\"([^\"]*)\"")


def parse_ixbrl(html: str) -> list[dict[str, Any]]:
    """Numeric facts of an inline XBRL document: concept (local name), start, end / instant, ``dim`` (the context has
    a segment), value (scale and sign applied). Nested ``ix:nonFraction`` tags each yield their own fact."""
    ctx: dict[str, dict[str, Any]] = {}
    for cid, body in _CTX.findall(html):
        s = re.search(r"<(?:xbrli:)?startDate>\s*([\d-]+)", body)
        e = re.search(r"<(?:xbrli:)?endDate>\s*([\d-]+)", body)
        i = re.search(r"<(?:xbrli:)?instant>\s*([\d-]+)", body)
        ctx[cid] = {"start": s.group(1) if s else None, "end": (e or i).group(1) if (e or i) else None,
                    "instant": bool(i), "dim": bool(re.search(r"<(?:xbrli:)?segment|<xbrldi:", body, re.I))}
    facts = []
    for m in _NF_OPEN.finditer(html):
        a = dict(_ATTR.findall(m.group(1)))
        c = ctx.get(a.get("contextRef", ""))
        if c is None or c["end"] is None or a.get("xsi:nil") == "true":
            continue
        stop = html.find("</ix:nonFraction>", m.end())
        text = re.sub(r"<[^>]+>", "", html[m.end():stop if stop >= 0 else m.end()]).strip().replace(",", "")
        if text in ("", "-", "—", "–"):
            val = 0.0
        else:
            try:
                val = float(text)
            except ValueError:
                continue
        val *= 10 ** int(a.get("scale", "0") or 0)
        if a.get("sign") == "-":
            val = -val
        facts.append({"concept": a.get("name", "").split(":")[-1], "start": c["start"], "end": c["end"],
                      "instant": c["instant"], "dim": c["dim"], "value": val, "context": a.get("contextRef")})
    return facts


def hand_value(facts: list[dict[str, Any]], kind: str, concepts: tuple[str, ...], datadate: dt.date,
               fy_start: dt.date | None) -> tuple[float | None, str | None]:
    """The filing's value of the first concept present: ``i`` instant at datadate, ``q`` 3-month duration ending at
    datadate, ``y`` duration from the fiscal-year start to datadate, ``c`` cover instant (dei); non-dimensional
    contexts only."""
    end = datadate.isoformat()
    for c in concepts:
        vals = []
        for f in facts:
            if f["concept"] != c or f["dim"]:
                continue
            if kind == "i" and f["instant"] and f["end"] == end:
                vals.append(f["value"])
            elif kind == "c" and f["instant"]:
                vals.append(f["value"])
            elif kind in ("q", "y") and not f["instant"] and f["end"] == end and f["start"]:
                d = (datadate - dt.date.fromisoformat(f["start"])).days
                if kind == "q" and Q_DUR[0] <= d <= Q_DUR[1]:
                    vals.append(f["value"])
                elif kind == "y" and fy_start is not None and abs((dt.date.fromisoformat(f["start"]) - fy_start).days) <= 7:
                    vals.append(f["value"])
        if vals:
            return vals[0], c
    return None, None


def cover_class_sum(facts: list[dict[str, Any]]) -> tuple[float | None, str | None]:
    """Info for multi-class issuers: dei EntityCommonStockSharesOutstanding summed over the class contexts of the
    latest cover date."""
    dims = [f for f in facts if f["concept"] == "EntityCommonStockSharesOutstanding" and f["dim"] and f["instant"]]
    if not dims:
        return None, None
    last = max(f["end"] for f in dims)
    per_ctx = {f["context"]: f["value"] for f in dims if f["end"] == last}
    return sum(per_ctx.values()), f"sum of {len(per_ctx)} class contexts at {last}"


def hand_derived(facts: list[dict[str, Any]], col: str, datadate: dt.date) -> tuple[float | None, str | None]:
    """Filing-side value under the item's documented fallback rule when its primary concepts are absent: ``saleq``
    banks = interest and dividend income + noninterest income (3 months); ``ltq`` = assets - equity incl. NCI;
    ``seqq`` = equity incl. NCI - minority interest; ``cshoq`` = balance-sheet shares at datadate, else weighted-
    average basic shares of the quarter (non-dimensional facts: multi-class dei counts are per class)."""
    if col == "seqq":
        eq, _ = hand_value(facts, "i", ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",),
                           datadate, None)
        mi, _ = hand_value(facts, "i", ("MinorityInterest",), datadate, None)
        if eq is not None:
            return eq - (mi or 0.0), "derived:EquityInclNCI-MinorityInterest"
    if col == "cshoq":
        v, c = hand_value(facts, "i", ("CommonStockSharesOutstanding",), datadate, None)
        if v is None:
            v, c = hand_value(facts, "q", ("WeightedAverageNumberOfSharesOutstandingBasic",), datadate, None)
        if v is not None:
            return v, f"rule:{c}"
    if col == "saleq":
        a, _ = hand_value(facts, "q", ("InterestAndDividendIncomeOperating",), datadate, None)
        b, _ = hand_value(facts, "q", ("NoninterestIncome",), datadate, None)
        if a is not None and b is not None:
            return a + b, "derived:InterestAndDividendIncomeOperating+NoninterestIncome"
    if col == "ltq":
        at, _ = hand_value(facts, "i", ("Assets",), datadate, None)
        eq, _ = hand_value(facts, "i", ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",),
                           datadate, None)
        if eq is None:
            seq, _ = hand_value(facts, "i", ("StockholdersEquity",), datadate, None)
            mi, _ = hand_value(facts, "i", ("MinorityInterest",), datadate, None)
            eq = None if seq is None else seq + (mi or 0.0)
        if at is not None and eq is not None:
            return at - eq, "derived:Assets-EquityInclNCI"
    return None, None


def handcheck(stage: str = STAGE, n: int = HAND_N, seed: int = HAND_SEED) -> dict[str, Any]:
    """``n`` seeded first-reported 10-Q fundq rows of linked member issuers, datadate 2021-2025: fetch the filing's
    primary document from EDGAR (``sec_http``, <= 5 req/s), read its inline XBRL and compare ``HAND_ITEMS``."""
    from .. import sec_http

    out = C.build_root() / stage
    con = C.connect(memory="300MB", threads=1)
    member_ciks(con, (2021, 2022, 2023, 2024, 2025))
    cols = ", ".join(HAND_ITEMS)
    rows = con.execute(f"""
        SELECT q.cik, q.accession, q.datadate, q.fqtr, q.available_at, {cols}
        FROM read_parquet('{_q(out / 'fundq.parquet')}') q JOIN mem m ON m.cik = q.cik AND m.year = year(q.datadate)
        WHERE q.first_reported AND q.form = '10-Q' AND q.curcd = 'USD' AND year(q.datadate) BETWEEN 2021 AND 2025
        ORDER BY q.cik, q.datadate, q.accession, q.available_at""").fetchall()
    con.close()
    rng = random.Random(seed)
    picks = rng.sample(rows, min(len(rows), n * 2))
    lim = sec_http.default_sec_limiter()
    checks, done = [], 0
    for row in picks:
        if done >= n:
            break
        cik, acc, dd, fqtr, available_at = row[:5]
        ours = dict(zip(HAND_ITEMS, row[5:]))
        base = f"https://www.sec.gov/Archives/edgar/data/{cik}/{acc.replace('-', '')}"
        try:
            hdr = sec_http.sec_get(f"{base}/{acc}-index-headers.html", limiter=lim).decode("utf-8", "replace")
            m = re.search(r"&lt;FILENAME&gt;([^\s&<]+)|<FILENAME>([^\s<]+)", hdr)
            primary = (m.group(1) or m.group(2)) if m else None
            if primary is None:
                idx = json.loads(sec_http.sec_get(f"{base}/index.json", limiter=lim))
                primary = next(i["name"] for i in idx["directory"]["item"] if i["name"].endswith(".htm"))
            html = sec_http.sec_get(f"{base}/{primary}", limiter=lim).decode("utf-8", "replace")
        except Exception as exc:  # noqa: BLE001 - recorded, the pick is skipped
            checks.append({"cik": cik, "accession": acc, "error": str(exc)[:200]})
            continue
        facts = parse_ixbrl(html)
        if not facts:
            checks.append({"cik": cik, "accession": acc, "skipped": "no inline XBRL facts", "primary": primary})
            continue
        fy_start = None
        cfo = [f for f in facts if f["concept"] in HAND_ITEMS["oancfy"][1] and f["end"] == dd.isoformat()
               and f["start"] and not f["dim"]]
        if cfo:
            fy_start = min(dt.date.fromisoformat(f["start"]) for f in cfo)
        items = []
        for col, (kind, concepts) in HAND_ITEMS.items():
            filing, concept = hand_value(facts, kind, concepts, dd, fy_start)
            derived = False
            if filing is None:
                filing, concept = hand_derived(facts, col, dd)
                derived = filing is not None
            ov = ours[col]
            if filing is None and ov is None:
                verdict = "both_absent"
            elif filing is None and col == "saleq" and ov == 0.0:
                verdict = "zero_rule_no_revenue_line"
            elif filing is None or ov is None:
                verdict = "absent_in_filing" if filing is None else "absent_in_fundq"
            else:
                tol = 0.005 * max(abs(filing), 1e-9) if col not in ("epspxq",) else 0.005 + 1e-9
                verdict = ("match_derived" if derived else "match") if abs(filing - ov) <= tol else "MISMATCH"
            row = {"item": col, "fundq": ov, "filing": filing, "concept": concept, "verdict": verdict}
            if col == "cshoq":
                cs_sum, cs_label = cover_class_sum(facts)
                if cs_sum is not None:
                    row["info_cover_class_sum"] = cs_sum
                    row["info_rel_diff_to_class_sum"] = (round((ov - cs_sum) / cs_sum, 5)
                                                         if ov is not None and cs_sum else None)
            items.append(row)
        checks.append({"cik": cik, "accession": acc, "datadate": dd.isoformat(), "available_at": available_at.isoformat(),
                       "fqtr": fqtr, "primary": primary,
                       "items": items})
        done += 1
    summary: dict[str, int] = {}
    for c in checks:
        for it in c.get("items", []):
            summary[it["verdict"]] = summary.get(it["verdict"], 0) + 1
    res = {"rule": handcheck.__doc__.strip(), "issuer_quarters": done, "summary": summary, "checks": checks,
           "mismatches": summary.get("MISMATCH", 0), "pass": done >= n and summary.get("MISMATCH", 0) == 0}
    vpath = out / "validation.json"
    doc = C.read_json(vpath) if vpath.exists() else {}
    doc["handcheck"] = res
    C.write_json_atomic(vpath, doc)
    publish_gate_status(stage)
    return res


# ---------------------------------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--fund-stage", default=DEFAULT_FUND_STAGE)
    b.add_argument("--earn-stage", default=DEFAULT_EARN_STAGE)
    b.add_argument("--buckets", type=int, default=N_BUCKETS)
    for name in ("build", "validate", "cutoff", "handcheck", "gate-status"):
        p = b if name == "build" else sub.add_parser(name)
        p.add_argument("--stage", default=STAGE)
        if name == "cutoff":
            p.add_argument("--n-ciks", type=int, default=CUTOFF_CIKS)
        if name == "handcheck":
            p.add_argument("--n", type=int, default=HAND_N)
        if name == "validate":
            p.add_argument("--checks", nargs="+", default=["coverage", "rdq", "sum4", "balance", "fsds"])
    a = ap.parse_args(argv)
    if a.cmd == "build":
        res = build(a.fund_stage, a.earn_stage, a.stage, a.buckets)
    elif a.cmd == "validate":
        res = validate(a.stage, tuple(a.checks))
    elif a.cmd == "cutoff":
        res = cutoff_check(a.stage, a.n_ciks)
        res = {k: v for k, v in res.items() if k != "examples"} | {"examples": res["examples"][:5]}
    elif a.cmd == "handcheck":
        res = handcheck(a.stage, a.n)
        res = {k: v for k, v in res.items() if k != "checks"}
    else:
        res = publish_gate_status(a.stage)
    print(json.dumps(res, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
