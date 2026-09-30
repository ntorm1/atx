"""Parity catalog (task S0.3): every domain and item of the tier-1 parity contract, with its reference-vendor field
names, the lake stage and column that carry it, the measurement basis, the target and the sprint.

Sources (parsed at generation time; the generated ``parityscore/catalog.csv`` is committed):

* plan section 1.1 domain matrix (``docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md``): one
  ``(domain)`` row per domain;
* the design spec item catalog and derived metric families (``docs/superpowers/specs/2026-09-19-tier1-parity-
  design.md``): one row per item, Compustat mnemonic from the spec, FactSet (and more Compustat) fields from the seed;
* ``seeds/fundamental_items.csv``: every canonical item the spec does not name (vendor fields per vendor, the
  us-gaap tags as the free source);
* CRSP-side fields (plan S2/S3 tasks) and the lake's own fundamentals items that no other source names.

Columns: ``domain, item, compustat_field, crsp_field, factset_field, source, stage, column, basis, target, sprint``
plus ``reference`` (the tier-1 reference text) and ``origin`` (where the row comes from). ``column`` lists lake
columns separated by ``|``; an empty ``stage`` means nothing in the lake carries the item yet.

    python -m atx_db.parityscore.catalog            # rewrite parityscore/catalog.csv
    python -m atx_db.parityscore.catalog --check    # exit 1 when the committed csv is out of date
"""

from __future__ import annotations

import csv
import io
import re
import sys
from collections import OrderedDict
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

PACKAGE_ROOT = Path(__file__).resolve().parents[3]  # .../atx-db
REPO = PACKAGE_ROOT.parent
PLAN = REPO / "docs" / "superpowers" / "plans" / "2026-09-28-tier1-v3-parity-warehouse.md"
SPEC = REPO / "docs" / "superpowers" / "specs" / "2026-09-19-tier1-parity-design.md"
SEED = Path(__file__).resolve().parents[1] / "seeds" / "fundamental_items.csv"
CSV_PATH = Path(__file__).resolve().parent / "catalog.csv"
COLUMNS = ("domain", "item", "compustat_field", "crsp_field", "factset_field", "source", "stage", "column", "basis",
           "target", "sprint", "reference", "origin")

# plan section 1.1 domain number -> (lake stages carrying it, headline columns, measurement basis)
DOMAIN_LAKE: dict[int, tuple[str, str, str]] = {
    1: ("identity_table; security_master", "link_tier",
        "member_equity cells per year, PIT tiers (strict + name) and any tier"),
    2: ("prices; panel", "ret|close|shares_out|me_company", "member_equity cells per year"),
    3: ("corporate_actions", "kind|split_ratio|implied_cash", "member-line cash dividends from 2019"),
    4: ("delisting", "dlret|cause", "M&A delistings from 2018 with exact DLRET"),
    5: ("", "", "public reconstitution lists where available"),
    6: ("fundamentals; export_fundamental_events", "", "items at >= 90% of top-3000 cells, FY2015-2025"),
    7: ("notes", "", "multi-segment 10-K filers"),
    8: ("fundamentals", "fin_template", "linked bank holding companies"),
    9: ("reference", "usd_per_ccy", "non-USD filer events whose currency H.10 covers"),
    10: ("thirteenf", "inst_shares", "member_equity cells per year; 13F SH value per quarter"),
    11: ("insider", "insider_last_code", "member_equity cells per year"),
    12: ("short_interest; short_volume; ftd; regsho_threshold; borrow_proxy", "si_shares|sv_total_volume",
         "member_equity cells per year"),
    13: ("sec_filings; earnings_calendar", "earn_last_utc", "member_equity issuers per year; hand-checked samples"),
    14: ("fundamentals", "grp_ff49", "linked issuers with a SIC"),
    15: ("sec_filings", "", "member_equity cells, PIT"),
    16: ("", "", "license adapter contract tests (mock)"),
    17: ("prices; panel", "iv_atm_21d", "optionable member lines"),
    18: ("reference", "", "series complete 2010+"),
    19: ("characteristics; panel", "", "characteristics with definitions, clocks, staleness"),
    20: ("lake", "", "unattended daily runs, SLO snapshots"),
}
SPEC_SECTIONS = {"Income statement": "income", "Balance sheet": "balance", "Cash flow": "cashflow"}
DOMAIN_OF = {"income": 6, "balance": 6, "cashflow": 6, "bank": 8, "insurance": 8, "reit": 8, "utility": 8,
             "broker_dealer": 8, "derived": 19, "estimate": 16}
# Compustat mnemonic -> lake columns of the fundamentals stage (same names in the panel)
LAKE_FUND = {
    "sale": "sale_q|sale_ttm", "revt": "sale_q|sale_ttm", "cogs": "cogs_q|cogs_ttm", "gp": "gp_q|gp_ttm",
    "xsga": "xsga_q|xsga_ttm", "xrd": "xrd_ttm", "dp": "dp_q|dp_ttm", "oiadp": "oi_q|oi_ttm",
    "oibdp": "ebitda_q|ebitda_ttm", "xint": "xint_q|xint_ttm", "txt": "txt_q", "ni": "ni_q|ni_ttm",
    "dvc": "dvc_ttm", "dvt": "dvt_q|dvt_ttm", "che": "che", "rect": "rect", "invt": "invt", "act": "act",
    "ppegt": "ppegt", "ppent": "ppe", "gdwl": "gdwl", "intan": "intan", "at": "at", "ap": "ap", "lct": "lct",
    "lt": "lt", "mib": "mib", "pstk": "pstk", "seq": "seq", "csho": "shrs_q", "oancf": "cfo_ttm",
    "capx": "capx_ttm", "prstkc": "prstkc_ttm", "sstk": "sstk_ttm",
}
LAKE_SEED = {"total_debt": "debt", "deferred_revenue": "drev"}  # seed canonical code -> lake column
# mega-alpha section 2 targets (linked-USD, non-structural basis), plan section S4 exit
MEGA_ALPHA = {"gp_ttm": 0.90, "oi_ttm": 0.92, "xrd_ttm": 0.95, "capx_ttm": 0.95, "txt_q": 0.95, "sale_ttm": 0.96,
              "shrs_q": 0.97}
FUND_TARGET = ">= 0.90 of top-3000 by market cap, every FY2015-FY2025 (spec gate, S4 exit)"
FUND_BASIS = "member_equity cells linked to a USD filer, ex-structural (metrics); all filing events (fundamentals)"
DERIVED_LAKE = {"market_cap": ("panel", "me_company"), "noa": ("fundamentals", "noa"),
                "piotroski_f": ("fundamentals", "fscore"), "asset_growth": ("characteristics", "asset_growth"),
                "gross_profitability": ("characteristics", "gpa"), "roa": ("characteristics", "roa")}
CRSP_ROWS = [  # (domain, item, crsp field, stage, column, target, sprint, source)
    (1, "permno", "PERMNO", "prices", "security_id", "one key per listing line, delisted included", "S2",
     "TickerHistory3 securityID (sid0-bracket repair)"),
    (1, "ncusip", "NCUSIP", "", "", ">= 99.5% of 13F SH value maps; ISIN check digits valid 100%", "S2",
     "13F, FTD, N-PORT, 13D/G CUSIPs"),
    (1, "shrcd", "SHRCD", "security_master", "finra_type", "share-code-style history per line", "S2",
     "FINRA issue names, cover-page dei"),
    (1, "exchcd", "EXCHCD", "security_master", "exchange", "exchange history per line", "S2",
     "FINRA market class, dei SecurityExchangeName"),
    (1, "ticker", "TICKER", "prices", "ticker", "dated ticker per line", "S2", "TickerHistory3"),
    (2, "prc", "PRC", "prices", "close", "validated closes 2012-03-26+", "S3", "TickerHistory3"),
    (2, "ret", "RET", "prices", "ret", ">= 99% of sampled daily returns within 1 bp", "S3", "TickerHistory3"),
    (2, "vol", "VOL", "prices", "volume", "member_equity cells", "S3", "TickerHistory3"),
    (2, "shrout", "SHROUT", "panel", "shares_out", ">= 99% of member_equity cells; two sources within 5% on >= 95%",
     "S3", "dei cover counts, balance sheet, vendor"),
    (2, "mktcap", "SHROUT*PRC", "panel", "me_company", ">= 99% of member_equity cells", "S3", "shares x close"),
    (3, "cfacpr", "CFACPR", "corporate_actions", "return_factor", "ex-date factors", "S3", "vendor return factors"),
    (3, "divamt", "DIVAMT", "corporate_actions", "implied_cash",
     "declare date for >= 90% of member-line cash dividends from 2019", "S3", "XBRL dividends declared, EX-99"),
    (4, "dlret", "DLRET", "delisting", "dlret", "exact DLRET for >= 80% of M&A delistings from 2018", "S3",
     "8-K 1.01/2.01, DEFM14A, SC TO-T, Form 25"),
    (5, "ewretd", "EWRETD", "panel", "mkt_ret", "CRSP-style EW/VW market returns; French Mkt rho >= 0.99 (<= 2022)",
     "S3", "computed from prices (panel mkt_ret: equal-weight member return)"),
    (5, "vwretd", "VWRETD", "", "", "French Mkt-RF rho >= 0.99 on monthly windows ending <= 2022-12", "S3",
     "computed from prices and shares"),
    (14, "siccd", "SICCD", "fundamentals", "sic", "SIC for 100% of linked issuers", "S7", "FSDS SUB SIC per filing"),
]
PANEL_ROWS = [  # (domain, item, factset/other reference, stage, column, target, sprint, source)
    (10, "inst_shares", "FactSet Ownership: institutional shares", "thirteenf", "inst_shares",
     "fund ownership for >= 95% of member_equity cells from 2020", "S5", "13F information tables"),
    (10, "filer_type", "FactSet Ownership: holder type", "thirteenf", "filer_type",
     "a type for >= 90% of 13F SH value per quarter", "S5", "Form ADV bulk data"),
    (11, "insider_net_buying", "FactSet Insiders", "insider", "insider_last_code",
     "144 notices with available_at; nets per issuer and month", "S5", "Forms 3/4/5, 144"),
    (12, "short_interest", "FINRA / Markit short interest", "short_interest", "si_shares",
     "member_equity cells per year", "S5", "FINRA consolidated short interest"),
    (12, "short_volume", "FINRA short volume", "short_volume", "sv_total_volume", "member_equity cells per year", "S5",
     "FINRA CNMS daily short volume"),
    (12, "fails_to_deliver", "SEC FTD", "ftd", "ftd_quantity", "member_equity cells per year", "S5", "SEC FTD files"),
    (12, "threshold_list", "Reg SHO threshold lists", "regsho_threshold", "regsho_last_list_date",
     "threshold lists for all 5 listing markets 2018+", "S5", "exchange and FINRA threshold files"),
    (13, "earnings_announcement", "FactSet Events / StreetAccount", "earnings_calendar", "earn_last_utc",
     ">= 95% of member_equity issuers covered, FPIs included", "S6", "8-K item 2.02, 6-K EX-99"),
    (17, "iv_atm", "OptionMetrics implied volatility", "prices", "iv_atm_21d",
     ">= 95% of optionable member lines", "S8", "vendor ATM IV term"),
]
LAKE_ONLY = [("be", "book equity (Fama-French)"), ("sue", "standardized unexpected earnings"),
             ("buyback_authorized", "buyback program authorized amount"),
             ("buyback_remaining", "buyback program remaining amount")]
MARKET_SPEC = [  # design spec "Market data from tbltickerhistory" and "Quality gates" named quantities
    (2, "adj_close", "prices", "adj_close", "S3"), (2, "total_return", "prices", "ret", "S3"),
    (3, "split_ratio", "corporate_actions", "split_ratio", "S3"),
    (3, "dividend_cash", "corporate_actions", "implied_cash", "S3"),
    (2, "shares_outstanding_daily", "panel", "shares_out", "S3"), (4, "delisting_return", "delisting", "dlret", "S3"),
    (1, "universe_us_listed", "panel", "member_equity", "S2"),
    (6, "identity_assets_eq_liabilities_plus_equity", "", "", "S4"),
    (6, "identity_gross_profit", "", "", "S4"), (6, "identity_cash_flow", "", "", "S4"),
    (2, "shares_cross_source_check", "", "", "S3"), (6, "fsds_benchmark", "", "", "S4"),
]


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _table(lines: list[str], start: int) -> list[list[str]]:
    rows: list[list[str]] = []
    i = start
    while i < len(lines) and not lines[i].startswith("|"):
        i += 1
    while i < len(lines) and lines[i].startswith("|"):
        cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
        if not all(re.fullmatch(r":?-+:?", c) for c in cells):
            rows.append(cells)
        i += 1
    return rows[1:]  # header dropped


def plan_domains(path: Path = PLAN) -> list[dict[str, str]]:
    """Rows of the plan section 1.1 domain matrix."""
    lines = path.read_text(encoding="utf-8").splitlines()
    start = next(i for i, ln in enumerate(lines) if ln.startswith("### 1.1 Domain matrix"))
    out = []
    for cells in _table(lines, start):
        n, domain, ref, source, today, target, sprint = cells[:7]
        out.append({"n": n, "domain": domain, "reference": ref, "source": source, "today": today, "target": target,
                    "sprint": sprint})
    return out


def spec_items(path: Path = SPEC) -> list[dict[str, str]]:
    """Items of the design spec: statement tables, supplemental list, derived families."""
    lines = path.read_text(encoding="utf-8").splitlines()
    out: list[dict[str, str]] = []
    for i, ln in enumerate(lines):
        if ln.startswith("### "):
            title = ln[4:].strip()
            key = next((v for k, v in SPEC_SECTIONS.items() if title.startswith(k)), None)
            if key:
                for cells in _table(lines, i + 1):
                    out.append({"section": key, "item": cells[0], "cs": cells[1] if len(cells) > 1 else "",
                                "notes": cells[2] if len(cells) > 2 else ""})
            elif title.startswith("Supplemental"):
                for cells in _table(lines, i + 1):
                    applies = cells[1]
                    sec = {"banks": "bank", "insurers": "insurance", "REITs": "reit"}.get(applies, "supplemental")
                    for item in cells[0].split(","):
                        out.append({"section": sec, "item": item.strip(), "cs": "", "notes": applies})
    start = next(i for i, ln in enumerate(lines) if ln.startswith("## Derived metric catalog"))
    end = next(i for i in range(start + 1, len(lines)) if lines[i].startswith("## "))
    text = "\n".join(lines[start:end])
    for m in re.finditer(r"^- \*\*(?P<fam>[^*]+)\*\*(?P<body>.*?)(?=^- \*\*|\Z)", text, re.S | re.M):
        fam = m.group("fam").strip()
        body = " ".join(m.group("body").split())
        if fam.startswith("TTM"):
            items = ["flow_ttm", "balance_avg2"]
        else:
            body = body.split(":", 1)[1] if ":" in body else body
            body = re.sub(r"\([^)]*\)", "", body)
            items = []
            for tok in body.split(","):
                ident = re.search(r"[a-z][a-z0-9_]*", tok.split("=")[0])
                if ident:
                    items.append(ident.group(0))
            if fam.startswith("Growth"):
                items = [f"growth_{x}" for x in items]
        for item in items:
            out.append({"section": "derived", "item": item, "cs": "", "notes": fam})
    return out


@dataclass
class SeedItem:
    item_id: str
    statement: str
    definition: str
    vendors: dict[str, list[str]] = field(default_factory=dict)
    tags: list[str] = field(default_factory=list)


def seed_items(path: Path = SEED) -> OrderedDict[str, SeedItem]:
    """{canonical_code: vendor fields per vendor and us-gaap alias tags} from the seed csv."""
    out: OrderedDict[str, SeedItem] = OrderedDict()
    with path.open(encoding="utf-8", newline="") as fh:
        for r in csv.DictReader(fh):
            d = out.setdefault(r["canonical_code"], SeedItem(r["item_id"], r["statement"], r["definition"]))
            if r["vendor"]:
                fields = d.vendors.setdefault(r["vendor"], [])
                if r["vendor_field"] not in fields:
                    fields.append(r["vendor_field"])
            if r["alias_code"] and r["alias_code"] not in d.tags:
                d.tags.append(r["alias_code"])
    return out


def _join(xs: Iterable[str]) -> str:
    seen: list[str] = []
    for x in xs:
        if x and x not in seen:
            seen.append(x)
    return "|".join(seen)


def _fund_target(column: str) -> str:
    specific = [f"{c} >= {MEGA_ALPHA[c]:.2f} (mega-alpha section 2, linked-USD non-structural)"
                for c in column.split("|") if c in MEGA_ALPHA]
    return "; ".join([*specific, FUND_TARGET])


def build_rows(plan: Path = PLAN, spec: Path = SPEC, seed: Path = SEED) -> list[dict[str, str]]:
    domains = plan_domains(plan)
    dname = {int(d["n"]): f"{int(d['n']):02d} {d['domain']}" for d in domains}
    dsprint = {int(d["n"]): d["sprint"] for d in domains}
    rows: list[dict[str, str]] = []
    for d in domains:
        n = int(d["n"])
        ref = d["reference"]
        stage, column, basis = DOMAIN_LAKE[n]
        rows.append({
            "domain": dname[n], "item": "(domain)",
            "compustat_field": _join(m.strip() for m in re.findall(r"Compustat ([^,;]+)", ref)),
            "crsp_field": _join(m.strip() for m in re.findall(r"CRSP ([^,;]+)", ref)),
            "factset_field": _join(m.strip() for m in re.findall(r"FactSet ([^,;]+)", ref)),
            "source": d["source"], "stage": stage, "column": column, "basis": basis, "target": d["target"],
            "sprint": d["sprint"], "reference": ref, "origin": f"plan 1.1 #{n}"})
    seeds = seed_items(seed)
    by_cs: dict[str, list[str]] = {}
    for code, si in seeds.items():
        for f in si.vendors.get("compustat", []):
            by_cs.setdefault(f.lower(), []).append(code)
    used: set[str] = set()
    for it in spec_items(spec):
        sec = it["section"]
        cs = [c.strip().lower() for c in it["cs"].split("/") if c.strip()]
        codes = [c for m in cs for c in by_cs.get(m, []) + by_cs.get(m + "q", [])]
        # the seed's own name for the item (``gross_profit__1004``), the derived ones included
        allowed = {"derived"} if sec == "derived" else set(DOMAIN_OF) - {"derived", "estimate"}
        codes += [c for c in seeds if re.sub(r"__\d+$", "", c) == it["item"] and seeds[c].statement in allowed]
        codes = list(dict.fromkeys(codes))
        used.update(codes)
        comp = _join([*cs, *(f for c in codes for f in seeds[c].vendors.get("compustat", []))])
        fs = _join(f for c in codes for f in seeds[c].vendors.get("factset", []))
        tags = [t for c in codes for t in seeds[c].tags][:3]
        if sec == "derived":
            stage, column = DERIVED_LAKE.get(it["item"], ("", ""))
            n, target, sprint = 19, ">= 250 characteristics; OSAP median rho >= 0.90 on >= 80% (S9)", "S9"
            source = f"computed ({it['notes']})"
            basis = "member_equity cells per year"
        else:
            column = _join(LAKE_FUND[m] for m in cs if m in LAKE_FUND)
            stage = "fundamentals" if column else ""
            n = 8 if sec in ("bank", "insurance", "reit") else 6
            target = _fund_target(column) if n == 6 else \
                "bank loans, deposits, NII, provisions, CET1 for >= 95% of linked BHCs (S4.5)"
            sprint = "S4"
            source = "SEC Company Facts / FSDS" + (f" (us-gaap {', '.join(tags)})" if tags else "")
            basis = FUND_BASIS if n == 6 else "linked issuers of the template"
        rows.append({"domain": dname[n], "item": it["item"], "compustat_field": comp, "crsp_field": "",
                     "factset_field": fs, "source": source, "stage": stage, "column": column, "basis": basis,
                     "target": target, "sprint": sprint, "reference": it["notes"], "origin": f"spec {sec}"})
    for code, si in seeds.items():
        if code in used:
            continue
        vend = si.vendors
        n = DOMAIN_OF.get(si.statement, 6)
        comp = _join(vend.get("compustat", []))
        column = LAKE_SEED.get(code) or _join(LAKE_FUND[m.lower()] for m in vend.get("compustat", [])
                                               if m.lower() in LAKE_FUND)
        tags = si.tags[:3]
        if n == 16:
            source, target, sprint = ("license (D3: I/B/E/S, FactSet Estimates); free substitute: EX-99 guidance "
                                      "(S6.4)", "adapter contract tests pass on mocks (S8.1)", "S8")
            basis = "license adapter"
        elif n == 19:
            source, target, sprint = "computed", ">= 250 characteristics (S9)", "S9"
            basis = "member_equity cells per year"
        else:
            source = "SEC Company Facts / FSDS" + (f" (us-gaap {', '.join(tags)})" if tags else "")
            target = _fund_target(column) if n == 6 else "industry template items (S4.5)"
            sprint, basis = "S4", FUND_BASIS if n == 6 else "linked issuers of the template"
        others = {k: v for k, v in vend.items() if k not in ("compustat", "factset")}
        rows.append({"domain": dname[n], "item": code, "compustat_field": comp, "crsp_field": "",
                     "factset_field": _join(vend.get("factset", [])), "source": source,
                     "stage": "fundamentals" if column else "", "column": column, "basis": basis, "target": target,
                     "sprint": sprint, "reference": "; ".join(f"{k} {'|'.join(v)}" for k, v in sorted(others.items())),
                     "origin": f"seed {si.item_id}"})
    for n, item, crsp, stage, column, target, sprint, source in CRSP_ROWS:
        rows.append({"domain": dname[n], "item": item, "compustat_field": "", "crsp_field": crsp, "factset_field": "",
                     "source": source, "stage": stage, "column": column, "basis": "member_equity cells per year",
                     "target": target, "sprint": sprint, "reference": "CRSP", "origin": "plan S2/S3 (CRSP)"})
    for n, item, ref, stage, column, target, sprint, source in PANEL_ROWS:
        rows.append({"domain": dname[n], "item": item, "compustat_field": "", "crsp_field": "",
                     "factset_field": ref if ref.startswith("FactSet") else "", "source": source, "stage": stage,
                     "column": column, "basis": "member_equity cells per year", "target": target, "sprint": sprint,
                     "reference": ref, "origin": f"plan {dsprint[n]}"})
    for n, item, stage, column, sprint in MARKET_SPEC:
        rows.append({"domain": dname[n], "item": item, "compustat_field": "", "crsp_field": "", "factset_field": "",
                     "source": "TickerHistory3 / SEC" if stage else "quality gate", "stage": stage, "column": column,
                     "basis": "member_equity cells per year" if stage else "issuer-periods FY2015+",
                     "target": "design spec market data / quality gates", "sprint": sprint, "reference": "spec",
                     "origin": "spec market/quality"})
    for item, doc in LAKE_ONLY:
        rows.append({"domain": dname[6], "item": item, "compustat_field": "", "crsp_field": "", "factset_field": "",
                     "source": "SEC Company Facts / FSDS", "stage": "fundamentals", "column": item,
                     "basis": FUND_BASIS, "target": FUND_TARGET, "sprint": "S4", "reference": doc,
                     "origin": "lake fundamentals v9"})
    return rows


def to_csv(rows: list[dict[str, str]]) -> str:
    buf = io.StringIO()
    w = csv.DictWriter(buf, fieldnames=COLUMNS, lineterminator="\n")
    w.writeheader()
    w.writerows(rows)
    return buf.getvalue()


def load(path: Path = CSV_PATH) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    text = to_csv(build_rows())
    if "--check" in args:
        current = CSV_PATH.read_text(encoding="utf-8") if CSV_PATH.exists() else ""
        print("up to date" if current == text else "parityscore/catalog.csv is out of date: python -m atx_db.parityscore.catalog")
        return 0 if current == text else 1
    CSV_PATH.write_text(text, encoding="utf-8", newline="\n")
    print(f"{CSV_PATH}: {text.count(chr(10)) - 1} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
