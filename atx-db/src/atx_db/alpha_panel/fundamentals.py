"""Stage F: issuer filing events (fundamentals), SIC industry events and the quarterly history.

Outputs under ``<build root>/fundamentals``:

* ``events.parquet``: one row per ``(cik, accession)`` 10-K/10-Q/10-KT/10-QT/20-F/40-F (and ``/A``)
  filing with clock at or after 2010-01-01; the issuer's latest-known state of every item after that filing
  (``fund_items.ALL_ITEMS``) plus ``currency``, ``fin_template``, ``staleness_days``, ``zero_filled``,
  ``xrd_reported_zero`` and the fallback source codes ``sale_src``, ``gp_src``, ``oi_src``, ``shrs_src``.
* ``quarterly_history.parquet``: one row per ``(cik, item, period_end, fiscal_period, accession)``: the
  discrete-quarter flow or period-end stock of that fiscal period as known right after that filing
  (``available_at`` = the filing's clock), written whenever a filing first reports or changes it.
* ``sic_events.parquet``: ``cik, clock_utc, sic, sic2, ff12, ff49, accession, sic_basis``.
* ``manifest.json`` (published last): sources, rule text, counts, per-year coverage, timings.

Sources (read-only): the Company Facts extraction ``fundamentals/_work/cf`` (``fund_extract.py``, archive
snapshot 2026-09-20: us-gaap, ifrs-full, dei), FSDS v2 ``sub/`` (accession acceptance clock and SIC),
``num/`` (class-of-stock share sums) and ``pre/`` (which lines each filing's statements present).

Clock per accession: FSDS ``accepted_utc`` (naive UTC); an accession missing from FSDS gets
``filed 00:00 UTC + 46h`` (``clock_basis = fc1_filed_plus_46h``).

The build is resumable: each extract batch writes ``_work/parts/{events,sic,history}-NNNN.parquet`` and a
receipt; a batch whose receipt matches the source identity and code version is skipped.

Usage (under the memory guard, cap <= 0.8 GiB)::

    python -m atx_db.alpha_panel.fund_extract run        # Company Facts -> _work/cf (once per archive)
    python -m atx_db.alpha_panel.fundamentals prepare
    python -m atx_db.alpha_panel.fundamentals batches [--only 0-9]
    python -m atx_db.alpha_panel.fundamentals finalize
    python -m atx_db.alpha_panel.fundamentals validate
    python -m atx_db.alpha_panel.fundamentals validate-cutoff
    python -m atx_db.alpha_panel.fundamentals attach-receipt --receipt R.json [--key batches]
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import math
import sys
import time
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from . import common
from . import fund_catalog as fcat
from . import fund_extract as fx
from . import fund_fx as ffx
from . import fund_items as fi

CODE_VERSION = "fundamentals-v10"
RULE = "fund-events-pit-v3"
SCHEMA = "atx.alpha-panel.fundamentals/v2"
EMIT_FROM = dt.datetime(2010, 1, 1)
SCOPE_LINK_END_MIN = dt.date(2017, 1, 1)
FC1_HOURS = 46
FORMS = tuple(sorted(fx.FORMS))
COVERAGE_YEARS = tuple(range(2016, 2027))
MODULES = ("fundamentals", "fund_items", "fund_extract", "fund_fx", "fund_catalog", "common")

RULE_TEXT = (
    "fund-events-pit-v3: one event per (cik, accession) of forms 10-K/10-Q/10-KT/10-QT/20-F/40-F (+/A) "
    "whose Company Facts rows carry us-gaap or ifrs-full money/share/per-share facts or dei "
    "EntityCommonStockSharesOutstanding, for every CIK of the Company Facts archive (snapshot 2026-09-20). Clock = "
    "FSDS sub accepted_utc (naive UTC), else filed 00:00 UTC + 46h (fc1). Facts with end > filed or start > end are "
    "dropped. Filings are applied to the issuer knowledge in (clock, accession) order: for every (concept, start, end) "
    "the latest-clock value wins (restatements enter on their own clock). Event period_end = max(latest "
    "period end of the filing's main statements, the issuer's previous period_end); items are computed at "
    "period_end from the knowledge after the event (same-clock filings applied together). Quarterly flows: "
    "discrete-quarter (80-120 day) facts, else YTD differences within one concept (Q4 = FY - 9M), else (S4.1) across "
    "the tier's concepts: a longer period of one concept minus the YTD of another concept from the same start, else "
    "minus the discrete quarters chaining back to that start, else two contiguous stub periods (predecessor/"
    "successor) spanning a quarter. TTM = fiscal-year (350-380 day) fact at period_end, else four chained single-"
    "concept quarters, else YTD + prior FY - prior YTD within one concept (the v9 paths), else four chained quarters "
    "admitting cross-concept quarters, else the YTD path across concepts; sale/gp/oi finally chain four item-level "
    "quarters of any fallback level (src quarters_mixed). Every component is in the knowledge, so a derived value "
    "exists from the latest component's clock (quarterly_history recomputes the quarter ends a filing's periods can "
    "feed). Year-ago = period end within 20 days of end - 365. Every lookup runs on the v1 concept tier first and on "
    "the extension tier (us-gaap fallbacks, ifrs-full, FSDS product/service sums, FSDS PRE label lines) only when "
    "the v1 tier has no value. Non-USD filings in an H.10 currency are converted (fx_rule); others keep USD facts "
    "only. Catalog items (catalog.parquet) per fund_catalog. is_amendment = form /A; is_restated = the filing "
    "changed a previously reported value of a watched chain by more than 0.5%. Emitted events: clock >= 2010-01-01; "
    "knowledge is built from every filing since 2009."
)
STALENESS_RULE = (
    "staleness_days = 200 if the CIK has a 10-Q/10-QT (or /A) accession with clock in (clock - 400 d, clock], "
    "else 400 (annual-only filer, e.g. 20-F/40-F); consumer rule (atx.fundamental-events/v1): the whole row is "
    "stale, every item NaN, when date(d) - period_end > staleness_days")
CURRENCY_RULE = (
    "Event money columns are USD. A filing whose reporting currency (most frequent money unit of its facts) is covered "
    "by reference/fx_daily.parquet (FRED H.10) takes its money items from the reporting-currency knowledge converted "
    "per fx_rule (fx_converted = true, fx_rate / fx_rate_avg_q / fx_rate_avg_ttm, available_at = max(filing clock, "
    "rate clocks)); a filing in an uncovered currency keeps the v9 rule (USD facts only, usually NaN). `currency` is "
    "the reporting currency. Currency-invariant items (sue, f_* terms, fscore, fscore_n, fscore_partial) of non-USD "
    "filings come from the reporting-currency knowledge, and quarterly_history.parquet keeps every value in its own "
    "`currency` (the reporting-currency values of converted events).")

WORK = "_work"


def fund_dir() -> Path:
    """The stage directory (env ``ATX_FUND_STAGE``, default ``fundamentals``; the v10 build uses ``fundamentals_v10``)."""
    return common.stage_dir(fx.stage_name())


def work_dir() -> Path:
    path = fund_dir() / WORK
    path.mkdir(parents=True, exist_ok=True)
    return path


def parts_dir() -> Path:
    path = work_dir() / "parts"
    path.mkdir(parents=True, exist_ok=True)
    return path


def sub_clock_path() -> Path:
    return work_dir() / "sub_clock.parquet"


def scope_path() -> Path:
    return work_dir() / "scope_ciks.parquet"


def class_shares_path() -> Path:
    return work_dir() / "class_shares.parquet"


def pre_flags_path() -> Path:
    return work_dir() / "pre_flags.parquet"


def pos_sums_path() -> Path:
    return work_dir() / "pos_sums.parquet"


def label_lines_path() -> Path:
    return work_dir() / "label_lines.parquet"


def links_path() -> Path:
    return common.build_root() / "identity" / "links.parquet"


def cf_batches() -> list[Path]:
    return fx.batch_files()


def eight_k_path() -> Path:
    return common.build_root() / "sec_filings" / "eight_k_items.parquet"


NONRELIANCE_DAYS = 365
NONRELIANCE_RULE = (
    f"nonreliance_402_at = available_at of the issuer's latest 8-K (or 8-K/A) Item 4.02 (non-reliance on previously "
    f"issued financial statements) from sec_filings/eight_k_items.parquet with available_at <= the event's "
    f"available_at and within {NONRELIANCE_DAYS} days before it, else NULL")


def input_manifests() -> dict[str, Any]:
    """SHA-256 of every input stage manifest the build reads (plan invariant 5)."""
    out: dict[str, Any] = {}
    for rel in ("identity/manifest.json", "sec_filings/manifest.json", "reference/manifest.json"):
        path = common.build_root() / rel
        out[rel] = common.sha256_file(path) if path.exists() else None
    return out


def catalog_manifest() -> dict[str, Any]:
    """The S4.2 catalog definition as published (per item: Compustat mnemonic, seed item, kind, chain, rules)."""
    return {"rule": fcat.__doc__, "label_rule": LABEL_RULE, "pre_rule": CATALOG_PRE_RULE,
            "structural_by_template": {k: sorted(v) for k, v in fi.CAT_STRUCTURAL.items()},
            "items": [{"col": it.col, "mnemonic": it.mnemonic, "seed_item_id": it.seed, "kind": it.kind,
                       "chain": list(it.chain), "zero_statement": it.stmt, "zero_line_pattern": it.line,
                       "knowledge_zero": it.knowledge_zero, "sic_ranges": [list(r) for r in it.sic],
                       "note": it.note} for it in fcat.CATALOG]}


# ---------------------------------------------------------------------------
# prepare: FSDS accession clocks, class-of-stock share sums, statement-line flags, the CIK scope
# ---------------------------------------------------------------------------

# FSDS NUM rows with exactly one ClassOfStock member (no other axis, no co-registrant) are summed per
# (accession, tag, ddate, qtrs). Members naming preferred stock, ADRs/depositary shares, class
# equivalents (Berkshire's "EquivalentClassA/B" restate one total), treasury stock, warrants or units are
# dropped; when a generic member (CommonStock, OrdinaryShares, ...) equals the sum of the others within
# 1% it is the total and the others are not added.
CLASS_TAGS = ("CommonStockSharesOutstanding", "WeightedAverageNumberOfSharesOutstandingBasic",
              "WeightedAverageNumberOfDilutedSharesOutstanding")
CLASS_EXCLUDE = "Preferred|Adr|Depositary|Treasury|Warrant|Unit|Right"
CLASS_EQUIVALENT = "Equivalent"
CLASS_GENERIC = "^(CommonStock|CommonStocks|CommonShares|OrdinaryShares|CommonStockMember)$"
CLASS_RULE = (
    "cls-sum-v1: FSDS NUM (uom shares, us-gaap version, no coreg) facts of CommonStockSharesOutstanding, "
    "WeightedAverageNumberOfSharesOutstandingBasic/Diluted whose segments are exactly one ClassOfStock member; "
    f"members matching /{CLASS_EXCLUDE}/ dropped; per (accession, tag, ddate, qtrs) the sum over members, "
    "or the generic member (CommonStock, CommonShares, OrdinaryShares) alone when it equals the sum of the "
    "others within 1%; class-equivalent members (Berkshire's EquivalentClassA/B, each the same total in another "
    "unit) are never summed: used only when they are the only members, as their largest value. Enters the issuer knowledge at the accession's clock; used for shrs_q only when dei, "
    "non-dimensional balance-sheet and weighted-average shares are all missing (ddate is month-end rounded: "
    "matched within 16 days)")
POS_RULE = (
    "pos-sum-v1: FSDS NUM (currency uom, us-gaap version, no coreg) duration facts of CostOfGoodsAndServicesSold, "
    "CostOfRevenue, CostOfGoodsSold, CostOfServices whose segments are exactly one ProductOrService member, summed "
    "per (accession, tag, ddate, qtrs, uom); mapped onto the accession's own Company Facts period (a duration of the "
    "same accession ending within 16 days of ddate, length within 20 days of 91.3 x qtrs); the last tier of the "
    "cost-of-revenue chain (filers that tag cost of revenue only by product and service line)")
# FSDS PRE (not parenthetical) statement lines of each accession, case-insensitive tag patterns.
PRE_PATTERNS = {
    "is_rd": r"researchanddevelopment|researchdevelopment",
    "is_rev": r"revenue|sales|rental|leaseincome|royalt|premium|commission|feeincome|fees|licens|tuition|contractincome",
    "is_tax": r"incometax|taxexpense|taxbenefit|taxesonincome|provisionforincome",
    "is_int": r"interestexpense|interestanddebtexpense|financecost|interestincomeexpense|interestcost",
    "cf_capx": (r"paymentstoacquire(propertyplant|productive|machinery|otherproperty|oilandgas|mining|equipment"
                r"|realestate|capital)|paymentsforcapital|capitalexpenditure|paymentstodevelop|paymentsforconstruction"
                r"|purchaseofpropertyplant|additionstopropertyplant|purchaseofproperty|paymentsforproperty"
                r"|purchasesofpropertyplant|purchaseoffixedassets|paymentsforpropertyplant"),
}
PRE_REV_EXCLUDE = r"costof|^cost|marketing|gain|loss|proceeds|payable|receivable|deferred|unearned|salesof"
PRE_RULE = (
    "FSDS PRE lines (inpth = false) per accession: has_is = any IS or CI line, has_cf = any CF line; "
    "is_* = an IS/CI line whose tag matches (case-insensitive) " + "; ".join(
        f"{k}: /{v}/" for k, v in PRE_PATTERNS.items() if k != "cf_capx")
    + f" (is_rev excludes tags matching /{PRE_REV_EXCLUDE}/); cf_capx = a CF line matching /{PRE_PATTERNS['cf_capx']}/")


def class_sum_sql(num_glob: str) -> str:
    """cls-sum-v1 over FSDS NUM parquet files (see ``CLASS_RULE``)."""
    tags = ", ".join(f"'{t}'" for t in CLASS_TAGS)
    return f"""
        WITH r AS (
            SELECT adsh, tag, ddate, qtrs, regexp_extract(segments, 'ClassOfStock=([^;]*);', 1) AS m,
                   max(CAST(value AS DOUBLE)) AS v
            FROM read_parquet('{num_glob}')
            WHERE tag IN ({tags}) AND uom = 'shares' AND version NOT LIKE '0%'
              AND (coreg IS NULL OR coreg = '') AND regexp_matches(segments, '^ClassOfStock=[^;]*;$')
              AND value IS NOT NULL
            GROUP BY ALL
        ), k AS (
            SELECT *, regexp_matches(m, '{CLASS_EQUIVALENT}') AS eq FROM r WHERE NOT regexp_matches(m, '{CLASS_EXCLUDE}')
        ), g AS (
            SELECT adsh, tag, ddate, qtrs, count(*) FILTER (WHERE NOT eq) AS n_members, count(*) AS n_all,
                   sum(v) FILTER (WHERE NOT eq AND NOT regexp_matches(m, '{CLASS_GENERIC}')) AS v_specific,
                   max(v) FILTER (WHERE NOT eq AND regexp_matches(m, '{CLASS_GENERIC}')) AS v_generic,
                   sum(v) FILTER (WHERE NOT eq) AS v_all, max(v) FILTER (WHERE eq) AS v_equiv
            FROM k GROUP BY ALL
        )
        SELECT adsh, tag, ddate, qtrs, CASE WHEN n_members > 0 THEN n_members ELSE n_all END AS n_members,
               CASE WHEN n_members = 0 THEN v_equiv
                    WHEN v_generic IS NOT NULL AND v_specific IS NOT NULL
                         AND abs(v_generic - v_specific) <= 0.01 * greatest(v_generic, 1) THEN v_generic
                    ELSE v_all END AS value
        FROM g WHERE (CASE WHEN n_members = 0 THEN v_equiv ELSE v_all END) > 0
    """


def pos_sum_sql(num_glob: str) -> str:
    """pos-sum-v1 over FSDS NUM parquet files (see ``POS_RULE``)."""
    tags = ", ".join(f"'{t}'" for t in fi.POS_TAGS)
    return f"""
        SELECT adsh, tag, ddate, qtrs, uom, count(*) AS n_members, sum(v) AS value
        FROM (
            SELECT adsh, tag, ddate, qtrs, uom, segments, max(CAST(value AS DOUBLE)) AS v
            FROM read_parquet('{num_glob}')
            WHERE tag IN ({tags}) AND regexp_matches(uom, '^[A-Z]{{3}}$') AND version NOT LIKE '0%'
              AND (coreg IS NULL OR coreg = '') AND regexp_matches(segments, '^ProductOrService=[^;]*;$')
              AND value IS NOT NULL AND qtrs > 0
            GROUP BY ALL
        )
        GROUP BY ALL
    """


def pre_flags_sql(pre_glob: str) -> str:
    """Statement-line flags per accession over FSDS PRE parquet files (see ``PRE_RULE`` and ``CATALOG_PRE_RULE``)."""
    is_cols = ",\n".join(
        f"bool_or(stmt IN ('IS', 'CI') AND regexp_matches(lower(tag), '{pat}')"
        + (f" AND NOT regexp_matches(lower(tag), '{PRE_REV_EXCLUDE}')" if name == "is_rev" else "")
        + f") AS {name}"
        for name, pat in PRE_PATTERNS.items() if name != "cf_capx")
    cat_cols = "".join(
        f",\n               bool_or(stmt IN ({', '.join(repr(x) for x in fcat.PRE_STMTS[it.stmt])}) AND "
        f"regexp_matches(lower(tag) || ' ' || lower(coalesce(plabel, '')), '{it.line}')) AS \"{fi.CAT}{it.col}\""
        for it in fcat.CATALOG if it.stmt)
    return f"""
        SELECT adsh, bool_or(stmt IN ('IS', 'CI')) AS has_is, bool_or(stmt = 'CF') AS has_cf,
               {is_cols},
               bool_or(stmt = 'CF' AND regexp_matches(lower(tag), '{PRE_PATTERNS["cf_capx"]}')) AS cf_capx,
               bool_or(stmt = 'BS') AS has_bs{cat_cols}
        FROM read_parquet('{pre_glob}') WHERE NOT coalesce(inpth, false)
        GROUP BY adsh
    """


# S4.2(c) FSDS PRE label fallback: custom-tag (version = adsh) income-statement lines identified by their label.
LABEL_CONCEPTS = {
    "GrossProfit": r"^(total )?gross (profit|margin|income)( \(loss\))?(, net)?$|^gross (loss|profit \(loss\))$",
    "CostOfRevenue": (r"^(total )?costs? of (net )?(revenues?|sales|goods sold|goods and services sold|products sold"
                      r"|products and services|products|services|merchandise sold|goods|sales and services)"
                      r"( \((exclusive|excluding|excludes|exclusive of)[^)]*\))?$"),
    "OperatingIncomeLoss": (r"^(total )?(operating (income|profit|earnings|loss)( \(loss\))?|(income|earnings|profit)"
                            r"( \(loss\))? from operations|operating income \(loss\)|loss from operations"
                            r"|income \(loss\) from operations|operating \(loss\) income|\(loss\) income from operations)$"),
    "Revenues": r"^(total )?(net )?(operating )?(revenues?|sales)(, net)?$",
}
LABEL_RULE = (
    "lbl-label-v1 (S4.2c): FSDS PRE income-statement lines (IS/CI, not parenthetical) whose tag is custom (version = "
    "adsh) and whose lower-case label matches: " + "; ".join(f"{k}: /{v}/" for k, v in LABEL_CONCEPTS.items())
    + ". Value = the FSDS NUM fact of that custom tag (no segments, no co-registrant, currency uom, qtrs > 0); per "
    "(accession, concept, ddate, qtrs, uom) the single matching line, or the single 'total ...' line when several "
    "match (else none); mapped onto the accession's own Company Facts duration like pos-sum-v1; the last tier of the "
    "revenue, cost-of-revenue, gross-profit and operating-income chains (pseudo taxonomy lbl)")
CATALOG_PRE_RULE = (
    "catalog statement-line flags: has_bs = any BS line; c_<item> = a line of the item's statement (BS; IS or CI; "
    "CF) whose 'lower(tag) lower(plabel)' matches the item's fund_catalog line pattern")


def label_lines_sql(pre_glob: str, num_glob: str) -> str:
    """lbl-label-v1 over FSDS PRE and NUM parquet files (see ``LABEL_RULE``)."""
    cases = " ".join(f"WHEN regexp_matches(lab, '{pat}') THEN '{name}'" for name, pat in LABEL_CONCEPTS.items())
    return f"""
        WITH m AS (
            SELECT DISTINCT adsh, tag, CASE {cases} END AS concept, starts_with(lab, 'total') AS is_total
            FROM (SELECT adsh, tag, lower(trim(plabel)) AS lab FROM read_parquet('{pre_glob}')
                  WHERE NOT coalesce(inpth, false) AND stmt IN ('IS', 'CI') AND version = adsh AND plabel IS NOT NULL)
        ), m2 AS (SELECT * FROM m WHERE concept IS NOT NULL),
        n AS (
            SELECT adsh, tag, ddate, qtrs, uom, max(CAST(value AS DOUBLE)) AS v
            FROM read_parquet('{num_glob}')
            WHERE version = adsh AND segments IS NULL AND (coreg IS NULL OR coreg = '') AND qtrs > 0
              AND regexp_matches(uom, '^[A-Z]{{3}}$') AND value IS NOT NULL
              AND adsh IN (SELECT adsh FROM m2)
            GROUP BY ALL
        )
        SELECT * FROM (
            SELECT m2.adsh, m2.concept, n.ddate, n.qtrs, n.uom, count(*) AS n_lines,
                   count(*) FILTER (WHERE is_total) AS n_total,
                   CASE WHEN count(*) FILTER (WHERE is_total) = 1 THEN max(n.v) FILTER (WHERE is_total)
                        WHEN count(*) = 1 THEN max(n.v) END AS value
            FROM m2 JOIN n USING (adsh, tag)
            GROUP BY m2.adsh, m2.concept, n.ddate, n.qtrs, n.uom
        ) WHERE value IS NOT NULL
    """


def prepare() -> dict[str, Any]:
    receipt: dict[str, Any] = {}
    con = common.connect(memory="450MB", threads=2)
    try:
        with common.timed(receipt, "sub_clock"):
            sub_glob = (common.FSDS_DIR / "sub" / "*.parquet").as_posix()
            dest = sub_clock_path()
            n = common.copy_to_parquet(
                con,
                f"""
                SELECT adsh, TRY_CAST(cik AS BIGINT) AS sub_cik, TRY_CAST(sic AS INTEGER) AS sic, form AS sub_form,
                       fy AS sub_fy, fp AS sub_fp, filed AS sub_filed,
                       CAST(accepted_utc AS TIMESTAMP) AS accepted_utc, quarter AS sub_quarter
                FROM read_parquet('{sub_glob}')
                """,
                dest,
            )
            dup = con.execute(
                f"SELECT count(*) - count(DISTINCT adsh) FROM read_parquet('{dest.as_posix()}')"
            ).fetchone()[0]
            if dup:
                raise AssertionError(f"FSDS sub has {dup} duplicate accessions")
            receipt["sub_rows"] = n
            receipt["sub_accepted_null"] = con.execute(
                f"SELECT count(*) FROM read_parquet('{dest.as_posix()}') WHERE accepted_utc IS NULL"
            ).fetchone()[0]
        with common.timed(receipt, "class_shares"):
            num_glob = (common.FSDS_DIR / "num" / "*.parquet").as_posix()
            receipt["class_share_rows"] = common.copy_to_parquet(con, class_sum_sql(num_glob), class_shares_path())
        with common.timed(receipt, "pos_sums"):
            receipt["pos_sum_rows"] = common.copy_to_parquet(con, pos_sum_sql(num_glob), pos_sums_path())
        with common.timed(receipt, "pre_flags"):
            pre_glob = (common.FSDS_DIR / "pre" / "*.parquet").as_posix()
            receipt["pre_flag_rows"] = common.copy_to_parquet(con, pre_flags_sql(pre_glob), pre_flags_path(),
                                                              row_group_size=32768)
        with common.timed(receipt, "label_lines"):
            receipt["label_line_rows"] = common.copy_to_parquet(con, label_lines_sql(pre_glob, num_glob),
                                                                label_lines_path())
            receipt["label_lines_by_concept"] = dict(con.execute(
                f"SELECT concept, count(*) FROM read_parquet('{label_lines_path().as_posix()}') GROUP BY 1").fetchall())
        with common.timed(receipt, "scope"):
            n = common.copy_to_parquet(
                con,
                f"""
                SELECT DISTINCT cik FROM read_parquet('{links_path().as_posix()}')
                WHERE end_incl >= DATE '{SCOPE_LINK_END_MIN.isoformat()}'
                """,
                scope_path(),
            )
            receipt["scope_ciks"] = n
    finally:
        con.close()
    receipt["class_rule"] = CLASS_RULE
    receipt["pre_rule"] = PRE_RULE
    receipt["pos_rule"] = POS_RULE
    receipt["label_rule"] = LABEL_RULE
    receipt["catalog_pre_rule"] = CATALOG_PRE_RULE
    common.write_json_atomic(work_dir() / "prepare.json", receipt)
    return receipt


# ---------------------------------------------------------------------------
# batches
# ---------------------------------------------------------------------------

EVENT_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("accession", pa.string()), ("form", pa.string()), ("filed", pa.date32()),
     ("clock_utc", pa.timestamp("us")), ("clock_basis", pa.string()), ("period_end", pa.date32()),
     ("fiscal_year", pa.int32()), ("fiscal_period", pa.string())]
    + [(c, pa.float64()) for c in fi.ITEM_COLUMNS]
    + [(c, pa.float64()) for c in fi.ITEM_COLUMNS_V2]
    + [("currency", pa.string()), ("fin_template", pa.string()), ("sic_in_force", pa.int32()),
       ("staleness_days", pa.int32()), ("xrd_reported_zero", pa.bool_()), ("zero_filled", pa.string())]
    + [(c, pa.string()) for c in fi.SRC_COLUMNS]
    + [("fx_converted", pa.bool_()), ("fx_rate", pa.float64()), ("fx_rate_avg_q", pa.float64()),
       ("fx_rate_avg_ttm", pa.float64()), ("available_at", pa.timestamp("us")),
       ("is_amendment", pa.bool_()), ("is_restated", pa.bool_()), ("restated_items", pa.string())]
)
CATALOG_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("accession", pa.string()), ("clock_utc", pa.timestamp("us")),
     ("available_at", pa.timestamp("us")), ("period_end", pa.date32()), ("fiscal_year", pa.int32()),
     ("fiscal_period", pa.string()), ("form", pa.string()), ("currency", pa.string()), ("fin_template", pa.string()),
     ("sic_in_force", pa.int32())]
    + [(c, pa.float64()) for c in fcat.CAT_COLUMNS] + [("catalog_zero_filled", pa.string())]
)
SIC_PART_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("clock_utc", pa.timestamp("us")), ("accession", pa.string()), ("sic", pa.int32()),
     ("sic_basis", pa.string())]
)
HISTORY_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("item", pa.string()), ("period_end", pa.date32()), ("fiscal_period", pa.string()),
     ("accession", pa.string()), ("value", pa.float64()), ("currency", pa.string()),
     ("available_at", pa.timestamp("us")), ("clock_basis", pa.string()), ("zero_filled", pa.bool_())]
)


def _register_static(con) -> None:
    concepts = pa.Table.from_pylist(
        [{"cid": cid, "taxonomy": tax, "concept": c, "unit": unit, "kind": kind}
         for cid, tax, c, unit, kind, _ in fi.CONCEPTS]
    )
    con.register("concepts_arrow", concepts)
    con.execute("CREATE OR REPLACE TEMP TABLE concepts AS SELECT * FROM concepts_arrow")
    forms = ", ".join(f"'{f}'" for f in FORMS)
    con.execute(f"CREATE OR REPLACE TEMP TABLE forms AS SELECT unnest([{forms}]) AS form")


def _batch_source_identity(batch: Path) -> dict[str, Any]:
    ident = common.file_identity(batch)
    receipt_json = batch.with_suffix(".json")
    if receipt_json.exists():
        meta = common.read_json(receipt_json)
        ident["extract_sha256"] = meta.get("parquet_sha256")
        ident["extract_rows"] = meta.get("rows")
        ident["extract_code_version"] = meta.get("code_version")
    return ident


def _fx_identity(fx: ffx.FxTable | None) -> dict[str, Any] | None:
    return None if fx is None else {k: fx.source.get(k) for k in ("path", "bytes", "sha256", "rows")}


def _batch_done(batch: Path, receipt_path: Path, fx: ffx.FxTable | None = None) -> bool:
    if not receipt_path.exists():
        return False
    r = common.read_json(receipt_path)
    ident = _batch_source_identity(batch)
    return (r.get("code_version") == CODE_VERSION and r.get("source", {}).get("bytes") == ident["bytes"]
            and r.get("source", {}).get("mtime_ns") == ident["mtime_ns"]
            and r.get("fx_source") == _fx_identity(fx)
            and all((parts_dir() / r[k]).exists() for k in ("events_file", "sic_file", "history_file", "catalog_file")))


def _base_sql(src: str, cik_filter: str = "") -> str:
    """CTEs ``f`` (Company Facts rows of stage concepts), ``acc``/``clk`` (accession clocks), ``cls``
    (class-of-stock sums of those accessions) and ``facts`` (f + cls)."""
    sub = sub_clock_path().as_posix()
    cls_path = class_shares_path().as_posix()
    pos_path = pos_sums_path().as_posix()
    lbl_path = label_lines_path().as_posix()
    return f"""
        WITH f AS (
            SELECT CAST(b.cik AS BIGINT) AS cik, b.accession_number AS accn, c.cid, c.kind,
                   b.period_start, b.period_end, b.value, b.unit, b.form, b.filed_date, b.fiscal_year,
                   b.fiscal_period
            FROM read_parquet('{src}') b
            JOIN concepts c ON b.taxonomy = c.taxonomy AND b.concept = c.concept
                 AND ((c.unit = 'shares') = (b.unit = 'shares'))
                 AND ((c.unit = 'per_share') = (b.unit LIKE '%/shares'))
            WHERE b.form IN (SELECT form FROM forms) {cik_filter}
        ),
        acc AS (
            SELECT f.cik, f.accn, min(f.filed_date) AS filed, min(f.form) AS form, s.accepted_utc, s.sub_cik, s.sic
            FROM f LEFT JOIN read_parquet('{sub}') s ON s.adsh = f.accn
            GROUP BY f.cik, f.accn, s.accepted_utc, s.sub_cik, s.sic
        ),
        clk AS (
            SELECT cik, accn, filed, form, sub_cik, sic,
                   coalesce(accepted_utc, CAST(filed AS TIMESTAMP) + INTERVAL {FC1_HOURS} HOUR) AS clock,
                   CASE WHEN accepted_utc IS NULL THEN 'fc1_filed_plus_46h' ELSE 'fsds_accepted_utc' END AS basis
            FROM acc
        ),
        cls AS (
            SELECT a.cik, x.adsh AS accn, c.cid, c.kind,
                   CASE WHEN x.qtrs = 0 THEN NULL
                        ELSE x.ddate - CAST(round(x.qtrs * 365.25 / 4) AS INTEGER) + 1 END AS period_start,
                   x.ddate AS period_end, x.value, 'shares' AS unit, a.form, a.filed AS filed_date,
                   CAST(NULL AS INTEGER) AS fiscal_year, CAST(NULL AS VARCHAR) AS fiscal_period
            FROM read_parquet('{cls_path}') x
            JOIN clk a ON a.accn = x.adsh
            JOIN concepts c ON c.taxonomy = '{fi.CLS}' AND c.concept = x.tag
            WHERE (x.qtrs = 0) = (c.kind = 'instant')
        ),
        pos AS (
            SELECT DISTINCT a.cik, x.adsh AS accn, c.cid, c.kind, d.period_start, d.period_end, x.value, x.uom AS unit,
                   a.form, a.filed AS filed_date, CAST(NULL AS INTEGER) AS fiscal_year,
                   CAST(NULL AS VARCHAR) AS fiscal_period
            FROM read_parquet('{pos_path}') x
            JOIN clk a ON a.accn = x.adsh
            JOIN (SELECT DISTINCT accn, period_start, period_end FROM f WHERE period_start IS NOT NULL) d
                 ON d.accn = x.adsh AND abs(datediff('day', x.ddate, d.period_end)) <= 16
                 AND abs(datediff('day', d.period_start, d.period_end) + 1 - x.qtrs * 91.3) <= 20
            JOIN concepts c ON c.taxonomy = '{fi.POS}' AND c.concept = x.tag
        ),
        lbl AS (
            SELECT DISTINCT a.cik, x.adsh AS accn, c.cid, c.kind, d.period_start, d.period_end, x.value, x.uom AS unit,
                   a.form, a.filed AS filed_date, CAST(NULL AS INTEGER) AS fiscal_year,
                   CAST(NULL AS VARCHAR) AS fiscal_period
            FROM read_parquet('{lbl_path}') x
            JOIN clk a ON a.accn = x.adsh
            JOIN (SELECT DISTINCT accn, period_start, period_end FROM f WHERE period_start IS NOT NULL) d
                 ON d.accn = x.adsh AND abs(datediff('day', x.ddate, d.period_end)) <= 16
                 AND abs(datediff('day', d.period_start, d.period_end) + 1 - x.qtrs * 91.3) <= 20
            JOIN concepts c ON c.taxonomy = '{fi.LBL}' AND c.concept = x.concept
        ),
        facts AS (SELECT * FROM f UNION ALL SELECT * FROM cls UNION ALL SELECT * FROM pos UNION ALL SELECT * FROM lbl)
    """


FACT_FILTER = """
    x.value IS NOT NULL AND isfinite(x.value) AND x.period_end IS NOT NULL
    AND x.filed_date IS NOT NULL AND x.period_end <= x.filed_date
    AND (x.kind = 'instant') = (x.period_start IS NULL)
    AND (x.period_start IS NULL OR x.period_start <= x.period_end)
"""


def _issuer_extras(con, base: str) -> tuple[dict, dict, dict, list]:
    """Per CIK: {accn: sic} (FSDS SUB SIC of the CIK's own filings), {accn: PRE flags}; {accn: clock basis};
    and the (cik, clock, accn, sub_cik, sic) rows for the SIC events."""
    sic_rows = con.execute(base + " SELECT cik, clock, accn, sub_cik, sic, basis FROM clk ORDER BY cik, clock, accn"
                           ).fetchall()
    sic_by: dict[int, dict[str, int]] = collections.defaultdict(dict)
    basis_by: dict[str, str] = {}
    for cik, _clock, accn, sub_cik, sic, basis in sic_rows:
        basis_by[accn] = basis
        if sic is not None and sub_cik == cik and sic > 0:
            sic_by[cik][accn] = sic
    flags = ", ".join(f"p.{c}" for c in fi.PRE_FLAGS)
    pre_by: dict[int, dict[str, dict[str, bool]]] = collections.defaultdict(dict)
    for row in con.execute(
            base + f" SELECT k.cik, k.accn, {flags} FROM clk k JOIN read_parquet('{pre_flags_path().as_posix()}') p "
                   "ON p.adsh = k.accn").fetchall():
        pre_by[row[0]][row[1]] = {c: bool(v) for c, v in zip(fi.PRE_FLAGS, row[2:])}
    return sic_by, pre_by, basis_by, [r[:5] for r in sic_rows]


def stream_issuers(con, base: str):
    """Yield (cik, rows) with rows (clock, accn, cid, start, end, value, unit, form, filed, fy, fp, basis)."""
    cur = con.execute(
        base + f"""
        SELECT x.cik, k.clock, x.accn, x.cid, x.period_start, x.period_end, x.value, x.unit, x.form, k.filed,
               x.fiscal_year, x.fiscal_period, k.basis
        FROM facts x JOIN clk k ON k.cik = x.cik AND k.accn = x.accn
        WHERE {FACT_FILTER}
        ORDER BY x.cik, k.clock, x.accn, x.cid, x.period_start NULLS FIRST, x.period_end, x.value
        """
    )
    pending_cik, pending = None, []
    while True:
        chunk = cur.fetchmany(50_000)
        if not chunk:
            break
        for row in chunk:
            if row[0] != pending_cik:
                if pending_cik is not None:
                    yield pending_cik, pending
                pending_cik, pending = row[0], []
            pending.append(row[1:])
    if pending_cik is not None:
        yield pending_cik, pending


class _HistoryWriter:
    def __init__(self, path: Path) -> None:
        self.tmp = path.with_name(path.name + ".partial")
        self.path = path
        self.w = pq.ParquetWriter(self.tmp, HISTORY_SCHEMA, compression="zstd")
        self.buf: list[dict[str, Any]] = []
        self.rows = 0

    def add(self, cik: int, rows: list[tuple], basis_by: dict[str, str]) -> None:
        for item, pe, fp, accn, value, clock, zero, cur in rows:
            self.buf.append({"cik": cik, "item": item, "period_end": pe, "fiscal_period": fp, "accession": accn,
                             "value": value, "currency": cur, "available_at": clock,
                             "clock_basis": basis_by.get(accn), "zero_filled": zero})
        if len(self.buf) >= 100_000:
            self.flush()

    def flush(self) -> None:
        if self.buf:
            self.w.write_table(pa.Table.from_pylist(self.buf, schema=HISTORY_SCHEMA))
            self.rows += len(self.buf)
            self.buf = []

    def close(self) -> int:
        self.flush()
        self.w.close()
        self.tmp.replace(self.path)
        return self.rows


def process_batch(con, batch: Path, fx_table: ffx.FxTable | None = None) -> dict[str, Any]:
    t0 = time.perf_counter()
    bid = batch.stem.split("-")[1]
    ev_path = parts_dir() / f"events-{bid}.parquet"
    sic_path = parts_dir() / f"sic-{bid}.parquet"
    hist_path = parts_dir() / f"history-{bid}.parquet"
    cat_path = parts_dir() / f"catalog-{bid}.parquet"
    base = _base_sql(batch.as_posix())
    counts = dict(con.execute(
        base + """
        SELECT 'rows_in_scope', count(*) FROM f
        UNION ALL SELECT 'drop_null_or_nonfinite', count(*) FROM f WHERE value IS NULL OR NOT isfinite(value)
        UNION ALL SELECT 'drop_end_after_filed', count(*) FROM f WHERE period_end > filed_date
        UNION ALL SELECT 'drop_kind_mismatch', count(*) FROM f
            WHERE (kind = 'instant') <> (period_start IS NULL)
        UNION ALL SELECT 'drop_start_after_end', count(*) FROM f WHERE period_start > period_end
        UNION ALL SELECT 'class_share_rows', count(*) FROM cls
        UNION ALL SELECT 'pos_cogs_rows', count(*) FROM pos
        UNION ALL SELECT 'label_fallback_rows', count(*) FROM lbl
        UNION ALL SELECT 'rows_non_usd_money', count(*) FROM f
            WHERE unit NOT IN ('USD', 'shares') AND unit NOT LIKE '%/shares'
        """
    ).fetchall())
    sic_by, pre_by, basis_by, sic_rows = _issuer_extras(con, base)
    counters: dict[str, int] = {}
    events: list[dict[str, Any]] = []
    hist = _HistoryWriter(hist_path)
    n_rows = ciks_seen = 0
    try:
        for cik, rows in stream_issuers(con, base):
            ciks_seen += 1
            n_rows += len(rows)
            h: list[tuple] = []
            events.extend(fi.issuer_events(cik, rows, EMIT_FROM, counters, sic_by.get(cik), pre_by.get(cik), h,
                                           fx=fx_table))
            hist.add(cik, h, basis_by)
        n_hist = hist.close()
    except BaseException:
        hist.w.close()
        raise

    emitted = {(e["cik"], e["accession"]) for e in events}
    sic_out: list[dict[str, Any]] = []
    last: dict[int, int] = {}
    for cik, clock, accn, sub_cik, sic in sic_rows:
        basis = None
        if sic is not None and sub_cik == cik and sic > 0:
            last[cik] = sic
            basis = "fsds_sub"
        elif cik in last:
            basis = "carried"
        if basis and (cik, accn) in emitted:
            sic_out.append({"cik": cik, "clock_utc": clock, "accession": accn, "sic": last[cik], "sic_basis": basis})

    tmp = ev_path.with_name(ev_path.name + ".partial")
    pq.write_table(pa.Table.from_pylist(events, schema=EVENT_SCHEMA), tmp, compression="zstd")
    tmp.replace(ev_path)
    tmp = sic_path.with_name(sic_path.name + ".partial")
    pq.write_table(pa.Table.from_pylist(sic_out, schema=SIC_PART_SCHEMA), tmp, compression="zstd")
    tmp.replace(sic_path)
    tmp = cat_path.with_name(cat_path.name + ".partial")
    pq.write_table(pa.Table.from_pylist(events, schema=CATALOG_SCHEMA), tmp, compression="zstd")
    tmp.replace(cat_path)
    receipt = {
        "code_version": CODE_VERSION,
        "source": _batch_source_identity(batch),
        "fx_source": _fx_identity(fx_table),
        "events_file": ev_path.name,
        "sic_file": sic_path.name,
        "history_file": hist_path.name,
        "catalog_file": cat_path.name,
        "fact_rows_used": n_rows,
        "ciks": ciks_seen,
        "events": len(events),
        "sic_events": len(sic_out),
        "history_rows": n_hist,
        "filter_counts": {k: int(v) for k, v in counts.items()},
        "counters": counters,
        "seconds": round(time.perf_counter() - t0, 2),
        "created_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    }
    common.write_json_atomic(parts_dir() / f"batch-{bid}.json", receipt)
    return receipt


def run_batches(only: tuple[int, int] | None = None) -> None:
    if not fx.complete():
        raise SystemExit("Company Facts extract incomplete: run `python -m atx_db.alpha_panel.fund_extract run`")
    batches = cf_batches()
    fx_table = ffx.load_optional()
    print(f"fx: {_fx_identity(fx_table)}", flush=True)
    con = common.connect(memory="300MB", threads=2)
    try:
        _register_static(con)
        for batch in batches:
            bid = int(batch.stem.split("-")[1])
            if only and not (only[0] <= bid <= only[1]):
                continue
            receipt_path = parts_dir() / f"batch-{bid:04d}.json"
            if _batch_done(batch, receipt_path, fx_table):
                continue
            r = process_batch(con, batch, fx_table)
            print(f"batch {bid:04d}: ciks={r['ciks']} rows={r['fact_rows_used']} events={r['events']} "
                  f"history={r['history_rows']} {r['seconds']}s", flush=True)
    finally:
        con.close()


# ---------------------------------------------------------------------------
# finalize
# ---------------------------------------------------------------------------

def _ff_lookup(sics: list[int]) -> pa.Table:
    from atx_db.reference_classifications import fama_french_12_for_sic, fama_french_49_for_sic

    rows = [{"sic": s, "sic2": s // 100, "ff12": fama_french_12_for_sic(s), "ff49": fama_french_49_for_sic(s)}
            for s in sics]
    return pa.Table.from_pylist(rows, schema=pa.schema([("sic", pa.int32()), ("sic2", pa.int32()),
                                                        ("ff12", pa.string()), ("ff49", pa.string())]))


def structural_na() -> dict[str, list[str]]:
    """{item: [fin_template values where the item is structurally absent]} (metrics readers)."""
    out: dict[str, list[str]] = {}
    for tmpl in fi.TEMPLATES:
        for item in sorted(fi.STRUCTURAL[tmpl]):
            out.setdefault(item, []).append(tmpl)
    return dict(sorted(out.items()))


def coverage_by_year(con, ev: Path, items: tuple[str, ...] = fi.ALL_ITEMS,
                     years: tuple[int, ...] = COVERAGE_YEARS, where: str = "") -> dict[str, Any]:
    """Per item per clock year over events (all CIKs): n, finite share, structural-NaN share, and finite
    share over the non-structural events (structural = NaN and the event's fin_template is structural for
    the item)."""
    e = ev.as_posix()
    snan = structural_na()
    cols = []
    for c in items:
        tmpl = snan.get(c)
        s_expr = (f"sum((\"{c}\" IS NULL AND fin_template IN ({', '.join(repr(t) for t in tmpl)}))::INT)"
                  if tmpl else "0")
        cols.append(f'sum(("{c}" IS NOT NULL AND isfinite("{c}"))::INT) AS "{c}__f", {s_expr} AS "{c}__s"')
    rows = con.execute(
        f"SELECT year(clock_utc) AS y, count(*) AS n, {', '.join(cols)} FROM read_parquet('{e}') "
        f"WHERE year(clock_utc) BETWEEN {years[0]} AND {years[-1]} {where} GROUP BY 1 ORDER BY 1").fetchall()
    out: dict[str, Any] = {"years": {}, "rule": "finite = non-null finite value; structural = null and "
                                             "fin_template in structural_na[item]; finite_ex_structural = "
                                             "finite / (n - structural)"}
    for row in rows:
        y, n = row[0], row[1]
        vals = row[2:]
        per: dict[str, Any] = {}
        for i, c in enumerate(items):
            f, s = vals[2 * i], vals[2 * i + 1]
            per[c] = {"finite": round(f / n, 4), "structural": round(s / n, 4),
                      "finite_ex_structural": round(f / (n - s), 4) if n > s else None}
        out["years"][str(y)] = {"events": n, "items": per}
    return out


def finalize() -> dict[str, Any]:
    t0 = time.perf_counter()
    batches = cf_batches()
    fx_table = ffx.load_optional()
    missing = [b.name for b in batches
               if not _batch_done(b, parts_dir() / f"batch-{b.stem.split('-')[1]}.json", fx_table)]
    if missing:
        raise SystemExit(f"{len(missing)} batches not complete: {missing[:5]}")
    receipts = [common.read_json(parts_dir() / f"batch-{b.stem.split('-')[1]}.json") for b in batches]
    out_dir = fund_dir()
    ev_dest, sic_dest = out_dir / "events.parquet", out_dir / "sic_events.parquet"
    hist_dest = out_dir / "quarterly_history.parquet"
    cat_dest = out_dir / "catalog.parquet"
    manifest: dict[str, Any] = {"rule": RULE, "code_version": CODE_VERSION, "rule_text": RULE_TEXT,
                                "staleness_rule": STALENESS_RULE, "currency_rule": CURRENCY_RULE, "fx_rule": ffx.RULE,
                                "template_rule": fi.TEMPLATE_RULE, "structural_na": structural_na(),
                                "class_share_rule": CLASS_RULE, "pre_flag_rule": PRE_RULE, "pos_cogs_rule": POS_RULE,
                                "nil_zero_rule": fi.nil_zeros.__doc__.split("\n\n")[0].replace("\n    ", " "),
                                "item_rules": ITEM_RULES, "emit_from_clock": EMIT_FROM.isoformat(),
                                "items_v1": list(fi.ITEM_COLUMNS), "items_v2": list(fi.ITEM_COLUMNS_V2),
                                "history_items": list(fi.HISTORY_ITEMS)}
    old = common.read_json(out_dir / "manifest.json") if (out_dir / "manifest.json").exists() else {}
    stale = out_dir / "manifest.json"
    if stale.exists():
        stale.unlink()  # publish-last: no manifest while the outputs are being replaced
    con = common.connect(memory="450MB", threads=2)
    try:
        parts = (parts_dir() / "events-*.parquet").as_posix()
        with common.timed(manifest, "events"):
            ek = eight_k_path()
            if ek.exists():
                src = f"""
                    SELECT e.*, CASE WHEN k.t402 > e.available_at - INTERVAL {NONRELIANCE_DAYS} DAY THEN k.t402 END
                                AS nonreliance_402_at
                    FROM read_parquet('{parts}') e
                    ASOF LEFT JOIN (SELECT cik, available_at AS t402 FROM read_parquet('{ek.as_posix()}')
                                    WHERE item = '4.02' AND available_at IS NOT NULL) k
                      ON e.cik = k.cik AND e.available_at >= k.t402"""
            else:
                src = f"SELECT *, CAST(NULL AS TIMESTAMP) AS nonreliance_402_at FROM read_parquet('{parts}')"
            n_ev = common.copy_to_parquet(con, f"SELECT * FROM ({src}) ORDER BY cik, clock_utc, accession", ev_dest,
                                          row_group_size=32768)
            manifest["nonreliance_rule"] = NONRELIANCE_RULE
            dup = con.execute(
                f"SELECT count(*) - count(DISTINCT (cik, accession)) FROM read_parquet('{ev_dest.as_posix()}')"
            ).fetchone()[0]
            if dup:
                raise AssertionError(f"{dup} duplicate (cik, accession) events")
        with common.timed(manifest, "sic_events"):
            sparts = (parts_dir() / "sic-*.parquet").as_posix()
            sics = [r[0] for r in con.execute(
                f"SELECT DISTINCT sic FROM read_parquet('{sparts}') WHERE sic IS NOT NULL ORDER BY 1").fetchall()]
            con.register("ff_arrow", _ff_lookup(sics))
            n_sic = common.copy_to_parquet(
                con,
                f"""
                SELECT s.cik, s.clock_utc, s.sic, f.sic2, f.ff12, f.ff49, s.accession, s.sic_basis
                FROM read_parquet('{sparts}') s LEFT JOIN ff_arrow f USING (sic)
                ORDER BY s.cik, s.clock_utc, s.accession
                """,
                sic_dest,
            )
        with common.timed(manifest, "quarterly_history"):
            hparts = (parts_dir() / "history-*.parquet").as_posix()
            n_hist = common.copy_to_parquet(
                con, f"SELECT * FROM read_parquet('{hparts}') ORDER BY cik, item, period_end, available_at, accession",
                hist_dest)
            dup = con.execute(
                f"SELECT count(*) - count(DISTINCT (cik, item, period_end, fiscal_period, accession)) "
                f"FROM read_parquet('{hist_dest.as_posix()}')").fetchone()[0]
            if dup:
                raise AssertionError(f"{dup} duplicate quarterly_history keys")
        with common.timed(manifest, "catalog"):
            cparts = (parts_dir() / "catalog-*.parquet").as_posix()
            n_cat = common.copy_to_parquet(
                con, f"SELECT * FROM read_parquet('{cparts}') ORDER BY cik, clock_utc, accession", cat_dest,
                row_group_size=32768)
            manifest["catalog"] = catalog_manifest()
        with common.timed(manifest, "stats"):
            manifest["stats"] = _stats(con, ev_dest, sic_dest, hist_dest)
            manifest["coverage_by_year"] = coverage_by_year(con, ev_dest)
            manifest["coverage_by_year_10k_10q"] = coverage_by_year(
                con, ev_dest, fi.ITEM_COLUMNS + ("fscore_partial",), where="AND form IN ('10-Q', '10-K')")
    finally:
        con.close()
    agg: dict[str, int] = collections.Counter()
    filt: dict[str, int] = collections.Counter()
    for r in receipts:
        agg.update(r["counters"])
        filt.update(r["filter_counts"])
    manifest["counters"] = dict(sorted(agg.items()))
    manifest["filter_counts"] = dict(sorted(filt.items()))
    manifest["batch_seconds_total"] = round(sum(r["seconds"] for r in receipts), 1)
    manifest["fact_rows_used"] = sum(r["fact_rows_used"] for r in receipts)
    manifest["sources"] = _sources(batches)
    manifest["sources"]["fx_daily"] = _fx_identity(fx_table)
    manifest["input_manifests_sha256"] = input_manifests()
    manifest["outputs"] = {"events.parquet": {"rows": n_ev}, "sic_events.parquet": {"rows": n_sic},
                           "quarterly_history.parquet": {"rows": n_hist}, "catalog.parquet": {"rows": n_cat}}
    if "guard" in old:
        manifest["guard_previous"] = old["guard"]
    manifest["timings_s"]["total_finalize"] = round(time.perf_counter() - t0, 3)
    manifest["created_utc"] = dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
    common.write_stage_manifest(fx.stage_name(), SCHEMA, MODULES, manifest, pattern="*.parquet")
    return manifest


def _stats(con, ev: Path, sic: Path, hist: Path) -> dict[str, Any]:
    e = ev.as_posix()
    stats: dict[str, Any] = {}
    stats["rows"], stats["ciks"] = con.execute(f"SELECT count(*), count(DISTINCT cik) FROM read_parquet('{e}')").fetchone()
    stats["events_by_year"] = {str(y): n for y, n in con.execute(
        f"SELECT year(clock_utc), count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 1").fetchall()}
    stats["ciks_by_year"] = {str(y): n for y, n in con.execute(
        f"SELECT year(clock_utc), count(DISTINCT cik) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 1").fetchall()}
    lk = links_path().as_posix()
    stats["strict_link_coverage"] = dict(zip(
        ("event_ciks_in_strict_links", "strict_link_ciks_end_ge_2017", "strict_link_ciks_with_events"),
        con.execute(
            f"""
            SELECT (SELECT count(DISTINCT cik) FROM read_parquet('{e}')
                    WHERE cik IN (SELECT cik FROM read_parquet('{lk}'))),
                   (SELECT count(DISTINCT cik) FROM read_parquet('{lk}') WHERE end_incl >= DATE '2017-01-01'),
                   (SELECT count(DISTINCT cik) FROM read_parquet('{lk}')
                    WHERE end_incl >= DATE '2017-01-01' AND cik IN (SELECT cik FROM read_parquet('{e}')))
            """).fetchone()))
    stats["clock_basis"] = dict(con.execute(
        f"SELECT clock_basis, count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 1").fetchall())
    stats["forms"] = dict(con.execute(
        f"SELECT form, count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["currency"] = dict(con.execute(
        f"SELECT coalesce(currency, 'none'), count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["currency_ciks_2020_plus"] = dict(con.execute(
        f"SELECT coalesce(currency, 'none'), count(DISTINCT cik) FROM read_parquet('{e}') "
        f"WHERE clock_utc >= TIMESTAMP '2020-01-01' GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["fin_template"] = dict(con.execute(
        f"SELECT fin_template, count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["staleness_days"] = {str(k): v for k, v in con.execute(
        f"SELECT staleness_days, count(*) FROM read_parquet('{e}') GROUP BY 1 ORDER BY 1").fetchall()}
    for col in fi.SRC_COLUMNS:
        stats[col] = dict(con.execute(
            f"SELECT coalesce({col}, 'none'), count(*) FROM read_parquet('{e}') "
            f"WHERE clock_utc >= TIMESTAMP '2016-01-01' GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["zero_filled_2016_plus"] = dict(con.execute(
        f"SELECT z, count(*) FROM (SELECT unnest(string_split(zero_filled, ',')) AS z FROM read_parquet('{e}') "
        f"WHERE zero_filled <> '' AND clock_utc >= TIMESTAMP '2016-01-01') GROUP BY 1 ORDER BY 2 DESC").fetchall())
    stats["period_end_age_days_p50_p95"] = con.execute(
        f"SELECT quantile_cont(CAST(clock_utc AS DATE) - period_end, [0.5, 0.95]) FROM read_parquet('{e}')"
    ).fetchone()[0]
    s = sic.as_posix()
    stats["sic_events"] = dict(zip(
        ("rows", "ciks", "fsds_sub", "carried", "ff49_null"),
        con.execute(
            f"SELECT count(*), count(DISTINCT cik), sum((sic_basis = 'fsds_sub')::INT), "
            f"sum((sic_basis = 'carried')::INT), sum((ff49 IS NULL)::INT) FROM read_parquet('{s}')").fetchone()))
    h = hist.as_posix()
    stats["quarterly_history"] = {
        "rows_ciks_accessions": con.execute(
            f"SELECT count(*), count(DISTINCT cik), count(DISTINCT accession) FROM read_parquet('{h}')").fetchone(),
        "by_item": dict(con.execute(
            f"SELECT item, count(*) FROM read_parquet('{h}') GROUP BY 1 ORDER BY 1").fetchall()),
        "by_available_year": {str(y): n for y, n in con.execute(
            f"SELECT year(available_at), count(*) FROM read_parquet('{h}') GROUP BY 1 ORDER BY 1").fetchall()},
        "later_vintages": con.execute(
            f"SELECT count(*) - count(DISTINCT (cik, item, period_end)) FROM read_parquet('{h}')").fetchone()[0],
        "currency": dict(con.execute(
            f"SELECT currency, count(*) FROM read_parquet('{h}') GROUP BY 1 ORDER BY 2 DESC LIMIT 20").fetchall()),
    }
    return stats


def _sources(batches: list[Path]) -> dict[str, Any]:
    fsds_manifest = common.FSDS_DIR / "fsds-staging-manifest.json"
    subs = sorted((common.FSDS_DIR / "sub").glob("*.parquet"))
    plan = fx.out_dir() / "plan.json"
    arch = fx.archive_identity()
    return {
        "companyfacts_archive": {**arch, "sha256": common.sha256_file(fx.ARCHIVE)},
        "companyfacts_extract_plan": {**common.file_identity(plan), "sha256": common.sha256_file(plan)},
        "companyfacts_batches": {b.name: _batch_source_identity(b) for b in batches},
        "fsds_staging_manifest": {**common.file_identity(fsds_manifest), "sha256": common.sha256_file(fsds_manifest)},
        "fsds_sub_files": {p.name: {"bytes": p.stat().st_size, "sha256": common.sha256_file(p)} for p in subs},
        "fsds_num_files": {p.name: {"bytes": p.stat().st_size, "sha256": common.sha256_file(p)}
                           for p in sorted((common.FSDS_DIR / "num").glob("*.parquet"))},
        "fsds_pre_files": {p.name: {"bytes": p.stat().st_size, "sha256": common.sha256_file(p)}
                           for p in sorted((common.FSDS_DIR / "pre").glob("*.parquet"))},
        "derived_class_shares": {**common.file_identity(class_shares_path()),
                                 "sha256": common.sha256_file(class_shares_path())},
        "derived_pre_flags": {**common.file_identity(pre_flags_path()), "sha256": common.sha256_file(pre_flags_path())},
        "derived_pos_sums": {**common.file_identity(pos_sums_path()), "sha256": common.sha256_file(pos_sums_path())},
        "identity_links": {**common.file_identity(links_path()), "sha256": common.sha256_file(links_path())},
        "statement_map_seed": _seed_identity(),
    }


def _seed_identity() -> dict[str, Any]:
    from atx_db.statement_map_seed import STATEMENT_MAP_SEED_PATH

    return {**common.file_identity(STATEMENT_MAP_SEED_PATH), "sha256": common.sha256_file(STATEMENT_MAP_SEED_PATH)}


ITEM_RULES = {
    "chains_v1": {k: list(v) for k, v in fi.CHAINS.items()},
    "chains_v2_extension": {k: list(v) for k, v in fi.CHAINS_EXT.items()},
    "tiers": "every lookup (value at an end, discrete quarter, TTM, year-ago search) is completed on the v1 chain "
             "first; the v2 extension chain of the same key is read only when the v1 chain has no value",
    "at": "Assets at period_end (IFRS Assets)",
    "lt": "Liabilities; else at - StockholdersEquityIncludingNCI; else at - StockholdersEquity - minority interest",
    "che": "CashCashEquivalentsAndShortTermInvestments; else cash chain + short-term investment chain (missing part 0)",
    "debt": "st + ltd_nc; st = DebtCurrent else current LTD chain + short-term borrowing chain; ltd_nc = long-term "
            "debt chain else LongTermDebt - current LTD; components zero-filled when at exists",
    "seq": "StockholdersEquity; else StockholdersEquityIncludingNCI - MinorityInterest; else at - Liabilities",
    "be": "seq + txditc - pstk (txditc: DeferredIncomeTaxLiabilitiesNet else DeferredTaxLiabilitiesNoncurrent; "
          "pstk: redemption, liquidation, carrying value; each carried from the latest end within 400 days, else 0)",
    "noa": "(at - che) - (at - debt - mib - pstk - (seq - pstk)); mib = MinorityInterest else SEQ_NCI - SEQ else 0",
    "flows": "quarter = 80-120 day fact else YTD difference within one concept (Q4 = FY - 9M); ttm = FY fact at period_end, else four "
             "chained quarters, else YTD + prior FY - prior YTD; concept priority applied per period",
    "sale_ttm/sale_q": "v1 revenue chain; else bank fallback InterestAndDividendIncomeOperating + NoninterestIncome; "
                       "else v2 extension chain (sale_src chain_ext); else 0 (sale_src zero_no_revenue_line, listed in "
                       "zero_filled) when the filing's own income statement (FSDS PRE) has no revenue/sales line and "
                       "the revenue chains have no fact ending within 460 days, not for bank/insurer/reit templates",
    "xsga_ttm": "SG&A chain; else GeneralAndAdministrativeExpense + selling/marketing (0 if missing)",
    "gp_ttm": "GrossProfit TTM (IFRS GrossProfit) else sale_ttm - cogs_ttm (v1 COGS chain, then the v2 extension: "
              "cost of services ex D&A, real-estate and lessor direct costs, hotels, IFRS CostOfSales, FSDS "
              "ProductOrService member sums (pos-sum-v1)); else (Compustat-style cost of revenue) sale_ttm - (total "
              "CostsAndExpenses / OperatingCostsAndExpenses - SG&A - R&D - D&A, missing deductions 0, only when >= 0); "
              "gp_src GrossProfit / sale_minus_cogs / sale_minus_opcost_ex_sga_rd_dp",
    "oi_ttm": "OperatingIncomeLoss (IFRS ProfitLossFromOperatingActivities); else, except SIC 6000-6199, "
              "IncomeLossFromContinuingOperationsBeforeInterestExpenseInterestIncomeIncomeTaxesExtraordinaryItemsNoncontrollingInterestsNet; "
              "else pre-tax income from continuing operations (IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest, "
              "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments, "
              "IFRS ProfitLossBeforeTax) + interest expense (xint chain: InterestExpense, InterestExpenseNonoperating, "
              "InterestAndDebtExpense, InterestExpenseDebt, InterestExpenseBorrowings, InterestExpenseOperating, "
              "IFRS InterestExpense, FinanceCosts; 0 under the xint zero rule); oi_src",
    "xrd_ttm": "R&D chain TTM; else 0 with xrd_reported_zero = true when the chain has no fact ending within 460 days "
               "and the filing's own income statement exists without an R&D line (FSDS PRE), or, for a period "
               "without PRE flags, ni_ttm exists; NaN otherwise (statement missing or R&D facts present but no TTM)",
    "capx_ttm": "capex chain (v1, then oil & gas, mining, construction in progress, real-estate development, IFRS "
                "PurchaseOfPropertyPlantAndEquipment*); else 0 (zero_filled) when the chain has no fact within 460 days and "
                "the cash-flow statement exists without a capex line (FSDS PRE) or, without PRE flags, cfo_ttm exists",
    "txt_q": "income-tax chain (v1, IFRS IncomeTaxExpenseContinuingOperations) discrete quarter; else 0 (zero_filled) "
             "when that quarter's income statement has no income-tax line (PRE) and the chain has no fact ending at "
             "period_end, or, without PRE flags, ni_q exists and the chain has no fact within 460 days; txt_q_lag4 "
             "the same at the year-ago quarter",
    "nil_as_zero": "nil-as-zero-v1 (capx, txt, xrd, dvc, prstkc, sstk, xint, dvt): a filing that reports the flow for "
                   "the year-ago comparative but for no current period (Company Facts drops blank/nil values) enters 0 "
                   "for its own current period of the same duration at its clock, unless a shorter period of the same "
                   "chain from the same start is non-zero; counter nil_zero_facts",
    "capx_dvc_prstkc": "keep the reported positive outflow sign (the seed's -1 cash-flow multiplier is not applied)",
    "dvc_prstkc_sstk": "TTM of the chain; 0 when cfo_ttm exists and the chain has no fact ending within 460 days "
                       "of period_end; outflows keep the reported positive sign",
    "shrs_q": "dei EntityCommonStockSharesOutstanding with the latest cover date in [end-15d, end+120d]; else "
              "us-gaap CommonStockSharesOutstanding at end (IFRS NumberOfSharesOutstanding); else weighted-average "
              "basic (diluted) shares of the period ending at end; else (v2) the FSDS class-of-stock sum of "
              "balance-sheet shares, else of weighted-average shares (cls-sum-v1, within 16 days); shrs_src",
    "shrs_q_lag4": "same source as shrs_q (dei: earliest cover of the year-ago period) at the year-ago balance-sheet "
                   "period end (NULL when that source is missing), converted to the current share basis "
                   "by the split ledger: a filing that restates share-count keys (weighted-average or balance-sheet "
                   "shares) by a common ratio r (|ln r| >= ln 1.09) evidences a split between the last old-basis "
                   "report and that filing; windows with the same ratio merge; a split is accepted when a first-"
                   "reported share series jumps by r inside (15%) or across (35%) the window (pure 10^3/10^6 "
                   "factors need dei or balance-sheet evidence), else when r is split-like; the lag is multiplied "
                   "by every accepted split reported after it. NULL when |log10(shrs_q/lag)| >= 2",
    "lags": "at_lag4/noa_lag4: balance sheet at the Assets/SEQ end within 20d of end-365; be_lag1q: within 25d of "
            "end-91; be_lag1q_lag4: within 20d of that date - 365; ni/txt/sale_q_lag4: quarter ending within 20d "
            "of end-365; all as known at the event",
    "sue": "(niq_first(t) - niq_first(t-4)) / stdev of the previous up to 8 such differences (quarters ending "
           "60..760 days before t), min 4; niq_first = quarterly net income as first derivable at the filing that "
           "first reported that quarter",
    "fscore": "sum of 9 Piotroski signals, NULL unless all 9 are computable: ni_ttm>0; cfo_ttm>0; "
              "ni_ttm/at_lag4 > ni_ttm(t-4)/at(t-8); cfo_ttm>ni_ttm; ltd_nc/at fell vs t-4; act/lct rose vs t-4; "
              "sstk_ttm<=0; gp/sale TTM rose vs t-4; sale_ttm/at_lag4 rose vs t-4",
    "f_terms": "f_roa, f_cfo, f_droa, f_accrual, f_dlever, f_dliquid, f_eq_offer, f_dmargin, f_dturn: the nine "
               "fscore signals in that order (1/0, NULL when an input is missing); fscore_n = number available; "
               "fscore_partial = their sum when fscore_n >= 6, else NULL; fscore unchanged (all nine required)",
    "d6_flows": "cogs_q, xsga_q, gp_q, oi_q (fallbacks as the TTM items), xint_q/xint_ttm (xint chain; 0 when the "
                "chain has no fact within 460 days and the income statement has no interest line (PRE) or, without "
                "PRE flags, debt = 0), dp_q (dp chain), ebitda_q/ebitda_ttm = oi + dp, dvt_q/dvt_ttm (dividends "
                "declared: DividendsCommonStock, DividendsCommonStockCash, Dividends, DividendsCash, IFRS "
                "DividendsRecognisedAsDistributionsToOwnersOfParent; 0 when dvc_ttm = 0 and no fact within 460 days)",
    "d6_stocks": "act, lct (current assets/liabilities), ap (AccountsPayableCurrent, ...Trade, ...AndAccruedLiabilities, "
                 "IFRS TradeAndOtherCurrentPayables), drev (ContractWithCustomerLiability / DeferredRevenue total, else "
                 "current + noncurrent), ppegt (PropertyPlantAndEquipmentGross), gdwl (Goodwill; 0 when at exists and no "
                 "goodwill fact within 460 days), intan (IntangibleAssetsNetExcludingGoodwill, else finite + indefinite, "
                 "else incl. goodwill - goodwill; 0 like gdwl), mib (be's minority interest; 0 when at exists and no "
                 "MinorityInterest / equity-incl.-NCI fact within 460 days), pstk (be's preferred stock, 0 when at exists)",
    "buyback": "buyback_authorized = StockRepurchaseProgramAuthorizedAmount1 (else ...AuthorizedAmount), "
               "buyback_remaining = StockRepurchaseProgramRemainingAuthorizedRepurchaseAmount1 (else ...Amount): the "
               "non-dimensional value at period_end, else the latest earlier value within 400 days; NULL otherwise "
               "(per-program dimensional facts are not in Company Facts)",
    "zero_filled": "comma-separated sorted names of the items whose value at this event is a zero fill",
}


def attach_receipt(receipt_path: Path, key: str) -> None:
    manifest_path = fund_dir() / "manifest.json"
    manifest = common.read_json(manifest_path) if manifest_path.exists() else {}
    guard = common.read_json(receipt_path)
    manifest.setdefault("guard", {})[key] = {
        "receipt": str(receipt_path),
        "job_limit_gb": guard.get("job_limit_gb"),
        "native_peak_job_memory_gb": guard.get("native_peak_job_memory_gb"),
        "status": guard.get("status"),
    }
    common.write_json_atomic(manifest_path, manifest)


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------

SPOT_CIKS = {"AAPL": 320193, "MSFT": 789019, "JPM": 19617, "NVDA": 1045810, "COST": 909832, "TSM": 1046179,
             "ASML": 937966, "GOOGL": 1652044, "META": 1326801, "BRK": 1067983, "BABA": 1577552, "SHEL": 1306965}
SPOT_COLUMNS = ("form", "clock_utc", "clock_basis", "period_end", "fiscal_year", "fiscal_period", "currency",
                "fin_template", "staleness_days", "at", "be", "sale_q", "sale_ttm", "gp_ttm", "gp_src", "oi_ttm",
                "oi_src", "ni_q", "ni_ttm", "cfo_ttm", "capx_ttm", "xrd_ttm", "xrd_reported_zero", "txt_q", "shrs_q",
                "shrs_src", "shrs_q_lag4", "xint_ttm", "ebitda_ttm", "gdwl", "intan", "ap", "drev", "sue", "fscore",
                "fscore_n", "fscore_partial", "buyback_authorized", "buyback_remaining", "zero_filled")


def validate(out: Path | None) -> dict[str, Any]:
    """Cross-check events.parquet and quarterly_history.parquet against raw Company Facts; ``validation.json``."""
    t0 = time.perf_counter()
    ev = (fund_dir() / "events.parquet").as_posix()
    hist = (fund_dir() / "quarterly_history.parquet").as_posix()
    cf_glob = (fx.out_dir() / "batch-[0-9][0-9][0-9][0-9].parquet").as_posix()
    forms = ", ".join(f"'{f}'" for f in FORMS)
    res: dict[str, Any] = {}
    con = common.connect(memory="450MB", threads=2)
    try:
        con.execute(
            f"""
            CREATE TEMP TABLE raw AS
            SELECT cik, accession_number AS accn, taxonomy, concept, unit, period_start, period_end,
                   max(value) AS value, min(value) AS value_min
            FROM read_parquet('{cf_glob}')
            WHERE form IN ({forms})
              AND ((taxonomy = 'us-gaap' AND concept IN ('Assets', 'NetIncomeLoss', 'StockholdersEquity'))
                   OR (taxonomy = 'ifrs-full' AND concept IN ('Assets', 'EquityAttributableToOwnersOfParent')))
            GROUP BY ALL
            """
        )
        con.execute(
            f"""
            CREATE TEMP TABLE rawdei AS
            SELECT cik, accession_number AS accn, period_end, max(value) AS value
            FROM read_parquet('{cf_glob}')
            WHERE taxonomy = 'dei' AND concept = 'EntityCommonStockSharesOutstanding' AND form IN ({forms})
            GROUP BY ALL
            """
        )

        def check(name: str, sql: str) -> None:
            n, ok, bad = con.execute(sql).fetchone()
            res.setdefault("own_filing_checks", {})[name] = {
                "compared": int(n), "match": int(ok or 0), "mismatch": int(bad or 0),
                "match_share": round((ok or 0) / n, 5) if n else None}

        tol = "abs(e.{c} - r.value) <= 1e-6 * greatest(1, abs(r.value))"
        base = f"FROM read_parquet('{ev}') e JOIN raw r ON r.cik = e.cik AND r.accn = e.accession " \
               f"AND r.period_end = e.period_end WHERE e.clock_utc >= TIMESTAMP '2020-01-01' AND r.value = r.value_min " \
               f"AND r.unit = 'USD'"
        check("at_vs_own_Assets",
              f"SELECT count(*), sum(({tol.format(c='at')})::INT), sum((NOT {tol.format(c='at')})::INT) "
              f"{base} AND r.taxonomy = 'us-gaap' AND r.concept = 'Assets' AND r.period_start IS NULL")
        check("seq_vs_own_StockholdersEquity",
              f"SELECT count(*), sum(({tol.format(c='seq')})::INT), sum((NOT {tol.format(c='seq')})::INT) "
              f"{base} AND r.taxonomy = 'us-gaap' AND r.concept = 'StockholdersEquity' AND r.period_start IS NULL")
        check("ni_ttm_vs_own_FY_NetIncomeLoss",
              f"SELECT count(*), sum(({tol.format(c='ni_ttm')})::INT), sum((NOT {tol.format(c='ni_ttm')})::INT) "
              f"{base} AND r.taxonomy = 'us-gaap' AND r.concept = 'NetIncomeLoss' "
              f"AND r.period_end - r.period_start + 1 BETWEEN 350 AND 380")
        check("ni_q_vs_own_3M_NetIncomeLoss",
              f"SELECT count(*), sum(({tol.format(c='ni_q')})::INT), sum((NOT {tol.format(c='ni_q')})::INT) "
              f"{base} AND r.taxonomy = 'us-gaap' AND r.concept = 'NetIncomeLoss' "
              f"AND r.period_end - r.period_start + 1 BETWEEN 80 AND 100")
        check("ifrs_at_vs_own_Assets_usd",
              f"SELECT count(*), sum(({tol.format(c='at')})::INT), sum((NOT {tol.format(c='at')})::INT) "
              f"{base} AND r.taxonomy = 'ifrs-full' AND r.concept = 'Assets' AND r.period_start IS NULL")
        # quarterly history against the raw fact of the same accession (every currency)
        hbase = (f"FROM read_parquet('{hist}') e JOIN raw r ON r.cik = e.cik AND r.accn = e.accession "
                 f"AND r.period_end = e.period_end AND r.unit = e.currency AND r.period_start IS NULL "
                 f"WHERE r.value = r.value_min")
        htol = "abs(e.value - r.value) <= 1e-6 * greatest(1, abs(r.value))"
        for taxo, concept, item in (("us-gaap", "Assets", "at"), ("ifrs-full", "Assets", "at"),
                                    ("us-gaap", "StockholdersEquity", "seq"),
                                    ("ifrs-full", "EquityAttributableToOwnersOfParent", "seq")):
            check(f"history_{item}_vs_own_{taxo}_{concept}",
                  f"SELECT count(*), sum(({htol})::INT), sum((NOT {htol})::INT) {hbase} AND e.item = '{item}' "
                  f"AND r.taxonomy = '{taxo}' AND r.concept = '{concept}'")
        n, ok, bad = con.execute(
            f"""
            SELECT count(*), sum((abs(e.shrs_q - d.value) <= 1e-6 * d.value)::INT),
                   sum((abs(e.shrs_q - d.value) > 1e-6 * d.value)::INT)
            FROM read_parquet('{ev}') e JOIN rawdei d ON d.cik = e.cik AND d.accn = e.accession
            WHERE e.clock_utc >= TIMESTAMP '2020-01-01' AND d.period_end >= e.period_end - 15
              AND d.period_end <= e.period_end + 120
            """
        ).fetchone()
        res["own_filing_checks"]["shrs_q_vs_own_dei_cover"] = {
            "compared": int(n), "match": int(ok or 0), "mismatch": int(bad or 0),
            "match_share": round((ok or 0) / n, 5) if n else None}
        # point-in-time sanity
        res["pit"] = dict(zip(
            ("events", "clock_before_filed", "clock_after_filed_plus_4d", "period_end_after_clock"),
            con.execute(
                f"""
                SELECT count(*), sum((clock_utc < CAST(filed AS TIMESTAMP))::INT),
                       sum((clock_utc > CAST(filed AS TIMESTAMP) + INTERVAL 4 DAY)::INT),
                       sum((period_end > CAST(clock_utc AS DATE))::INT)
                FROM read_parquet('{ev}')
                """).fetchone()))
        res["pit_history"] = dict(zip(
            ("rows", "period_end_after_available_at", "available_at_differs_from_accession_event_clock"),
            con.execute(
                f"""
                SELECT count(*), sum((h.period_end > CAST(h.available_at AS DATE))::INT),
                       sum((e.clock_utc IS NOT NULL AND e.clock_utc <> h.available_at)::INT)
                FROM read_parquet('{hist}') h
                LEFT JOIN read_parquet('{ev}') e ON e.cik = h.cik AND e.accession = h.accession
                """).fetchone()))
        # split consistency of the share pair
        res["shares_pair"] = dict(zip(
            ("pairs", "abs_log_ratio_gt_ln1p8", "abs_log_ratio_gt_ln3", "p01", "p50", "p99"),
            con.execute(
                f"""
                SELECT count(*), sum((abs(ln(shrs_q / shrs_q_lag4)) > ln(1.8))::INT),
                       sum((abs(ln(shrs_q / shrs_q_lag4)) > ln(3))::INT),
                       quantile_cont(shrs_q / shrs_q_lag4, 0.01), quantile_cont(shrs_q / shrs_q_lag4, 0.5),
                       quantile_cont(shrs_q / shrs_q_lag4, 0.99)
                FROM read_parquet('{ev}')
                WHERE clock_utc >= TIMESTAMP '2020-01-01' AND shrs_q > 0 AND shrs_q_lag4 > 0
                """).fetchone()))
        res["shares_pair_by_src"] = {r[0]: {"pairs": r[1], "abs_log_ratio_gt_ln1p8": r[2], "p50": r[3]}
                                     for r in con.execute(
            f"""
            SELECT shrs_src, count(*), sum((abs(ln(shrs_q / shrs_q_lag4)) > ln(1.8))::INT),
                   quantile_cont(shrs_q / shrs_q_lag4, 0.5)
            FROM read_parquet('{ev}') WHERE clock_utc >= TIMESTAMP '2020-01-01' AND shrs_q > 0 AND shrs_q_lag4 > 0
            GROUP BY 1 ORDER BY 2 DESC
            """).fetchall()}
        # amendments that changed the issuer state
        res["amendments"] = dict(zip(
            ("amendment_events", "changed_at", "changed_ni_ttm", "changed_be"),
            con.execute(
                f"""
                WITH x AS (
                    SELECT *, lag("at") OVER w AS p_at, lag(ni_ttm) OVER w AS p_ni, lag(be) OVER w AS p_be,
                           lag(period_end) OVER w AS p_pe
                    FROM read_parquet('{ev}') WINDOW w AS (PARTITION BY cik ORDER BY clock_utc, accession))
                SELECT count(*), sum(("at" IS DISTINCT FROM p_at)::INT), sum((ni_ttm IS DISTINCT FROM p_ni)::INT),
                       sum((be IS DISTINCT FROM p_be)::INT)
                FROM x WHERE form LIKE '%/A' AND period_end = p_pe
                """).fetchone()))
        # spot rows
        cols = ", ".join(f'"{c}"' for c in SPOT_COLUMNS)
        res["spot"] = {}
        for name, cik in SPOT_CIKS.items():
            rows = con.execute(
                f"SELECT accession, {cols} FROM read_parquet('{ev}') WHERE cik = {cik} "
                f"AND clock_utc >= TIMESTAMP '2024-01-01' AND clock_utc < TIMESTAMP '2025-09-01' ORDER BY clock_utc"
            ).fetchall()
            res["spot"][name] = [dict(zip(("accession",) + SPOT_COLUMNS, r)) for r in rows]
        res["spot_history"] = {}
        for name in ("TSM", "ASML", "AAPL"):
            rows = con.execute(
                f"SELECT item, period_end, fiscal_period, accession, value, currency, available_at "
                f"FROM read_parquet('{hist}') WHERE cik = {SPOT_CIKS[name]} AND item IN ('sale_q', 'ni_q', 'at') "
                f"AND period_end >= DATE '2023-01-01' AND period_end < DATE '2025-01-01' "
                f"ORDER BY item, period_end, available_at").fetchall()
            res["spot_history"][name] = [dict(zip(("item", "period_end", "fiscal_period", "accession", "value",
                                                   "currency", "available_at"), r)) for r in rows]
        split = con.execute(
            f"SELECT accession, period_end, shrs_q, shrs_q_lag4, shrs_q / shrs_q_lag4 FROM read_parquet('{ev}') "
            f"WHERE cik = 1045810 AND period_end BETWEEN DATE '2024-01-01' AND DATE '2025-12-31' ORDER BY clock_utc"
        ).fetchall()
        res["nvda_split_2024"] = [dict(zip(("accession", "period_end", "shrs_q", "shrs_q_lag4", "ratio"), r))
                                  for r in split]
    finally:
        con.close()
    res["seconds"] = round(time.perf_counter() - t0, 2)
    dest = out or (fund_dir() / "validation.json")
    common.write_json_atomic(dest, res)
    print({k: v for k, v in res.items() if k in ("own_filing_checks", "pit", "pit_history", "shares_pair",
                                                 "amendments")})
    return res


CUTOFF_CIKS = (320193, 789019, 19617, 1045810, 909832, 2186, 1748790, 1046179, 937966, 1652044, 1326801, 1067983,
               1577552, 1764046, 1306965, 72971, 1403161, 1800, 21344, 1090872)
CUTOFFS = (dt.datetime(2012, 6, 30), dt.datetime(2019, 6, 30), dt.datetime(2022, 3, 1), dt.datetime(2024, 8, 15))


def _eq(a: Any, b: Any) -> bool:
    if isinstance(a, float) and isinstance(b, float):
        return a == b or (math.isnan(a) and math.isnan(b))
    return a == b


def validate_cutoff(out: Path | None, ciks: tuple[int, ...] = CUTOFF_CIKS) -> dict[str, Any]:
    """No-leak rebuild: rebuild the issuers from facts whose filing clock precedes each cutoff and require every
    earlier event row and quarterly-history row of the full build to be reproduced exactly."""
    t0 = time.perf_counter()
    cik_list = ", ".join(str(c) for c in ciks)
    ev = (fund_dir() / "events.parquet").as_posix()
    hist = (fund_dir() / "quarterly_history.parquet").as_posix()
    cf_glob = (fx.out_dir() / "batch-[0-9][0-9][0-9][0-9].parquet").as_posix()
    res: dict[str, Any] = {"ciks": list(ciks), "cutoffs": [c.isoformat() for c in CUTOFFS], "by_cutoff": {}}
    con = common.connect(memory="450MB", threads=2)
    try:
        _register_static(con)
        base = _base_sql(cf_glob, f"AND CAST(b.cik AS BIGINT) IN ({cik_list})")
        sic_by, pre_by, _, _ = _issuer_extras(con, base)
        full_rows = {cik: rows for cik, rows in stream_issuers(con, base)}
        ev_cols = [f.name for f in EVENT_SCHEMA]
        full_ev = con.execute(
            f"SELECT {', '.join(chr(34) + c + chr(34) for c in ev_cols)} FROM read_parquet('{ev}') "
            f"WHERE cik IN ({cik_list}) ORDER BY cik, clock_utc, accession").fetchall()
        full_h = con.execute(
            f"SELECT cik, item, period_end, fiscal_period, accession, value, currency, available_at, zero_filled "
            f"FROM read_parquet('{hist}') WHERE cik IN ({cik_list})").fetchall()
    finally:
        con.close()
    ev_idx = {name: i for i, name in enumerate(ev_cols)}
    for cutoff in CUTOFFS:
        rebuilt_ev: dict[tuple, dict[str, Any]] = {}
        rebuilt_h: set[tuple] = set()
        for cik, rows in full_rows.items():
            part = [r for r in rows if r[0] < cutoff]
            h: list[tuple] = []
            counters: dict[str, int] = {}
            for e in fi.issuer_events(cik, part, EMIT_FROM, counters, sic_by.get(cik), pre_by.get(cik), h):
                rebuilt_ev[(cik, e["accession"])] = e
            for item, pe, fp, accn, value, clock, zero, cur in h:
                rebuilt_h.add((cik, item, pe, fp, accn, value, cur, clock, zero))
        compared = mismatched = missing = 0
        examples: list[dict[str, Any]] = []
        for row in full_ev:
            if row[ev_idx["clock_utc"]] >= cutoff:
                continue
            compared += 1
            e = rebuilt_ev.get((row[0], row[1]))
            if e is None:
                missing += 1
                continue
            diff = []
            for name in ev_cols:
                a = row[ev_idx[name]]
                b = e.get(name)
                if EVENT_SCHEMA.field(name).type == pa.float64() and b is not None:
                    b = float(b)
                if not _eq(a, b):
                    diff.append(name)
            if diff:
                mismatched += 1
                if len(examples) < 5:
                    examples.append({"cik": row[0], "accession": row[1], "columns": diff[:10]})
        h_prior = {r for r in full_h if r[7] < cutoff}
        res["by_cutoff"][cutoff.date().isoformat()] = {
            "events_compared": compared, "events_missing": missing, "events_mismatched": mismatched,
            "examples": examples, "history_rows_before_cutoff": len(h_prior),
            "history_rows_not_reproduced": len(h_prior - rebuilt_h),
            "history_rows_extra_in_rebuild": len(rebuilt_h - h_prior)}
    res["seconds"] = round(time.perf_counter() - t0, 2)
    dest = out or (fund_dir() / "validation_cutoff.json")
    common.write_json_atomic(dest, res)
    print(res["by_cutoff"])
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("prepare")
    b = sub.add_parser("batches")
    b.add_argument("--only", help="inclusive batch id range a-b")
    sub.add_parser("finalize")
    v = sub.add_parser("validate")
    v.add_argument("--out", type=Path)
    vc = sub.add_parser("validate-cutoff")
    vc.add_argument("--out", type=Path)
    a = sub.add_parser("attach-receipt")
    a.add_argument("--receipt", required=True, type=Path)
    a.add_argument("--key", default="finalize")
    args = ap.parse_args(argv)
    if args.cmd == "prepare":
        print(prepare())
    elif args.cmd == "batches":
        only = tuple(int(x) for x in args.only.split("-")) if args.only else None
        run_batches(only)  # type: ignore[arg-type]
    elif args.cmd == "finalize":
        m = finalize()
        print({k: m["stats"][k] for k in ("rows", "ciks", "clock_basis")})
    elif args.cmd == "validate":
        validate(args.out)
    elif args.cmd == "validate-cutoff":
        validate_cutoff(args.out)
    else:
        attach_receipt(args.receipt, args.key)
    return 0


if __name__ == "__main__":
    sys.exit(main())
