"""Stage registry: one entry per published manifest of the lake (data only; see :mod:`atx_db.lake.contract`).

Registering a stage (one entry, data only), either here (lane PLAT) or, without touching ``lake/``, as a module-level
literal in the stage's own module, which :func:`load` discovers by parsing (never importing) the source::

    LAKE_STAGES = [
        {"name": "reference", "lane": "MKT", "schema": "atx.alpha-panel.reference/v1",
         "module": "atx_db.alpha_panel.reference", "args": ["build"], "fetch": ["fetch"], "inputs": [],
         "outputs": [{"glob": "reference/fx_daily.parquet", "view": "reference_fx_daily"}],
         "staleness": "daily series; stale after 5 business days", "vintage": "event", "guard_gb": 0.4},
    ]

``module`` defaults to the declaring module. Output ``clock`` defaults to ``available_at``; ``None`` marks a
clockless (static, audit or look-up) table. ``tests/test_lake_registry.py`` fails with a ready-to-paste entry for
every manifest or Parquet file under the build root that no entry covers.

Seeded 2026-09-29 from ``alpha_panel/build.py`` STAGES and every ``data/alpha_panel/v1/**/*manifest*.json``.
Inputs are the stages whose files a build reads; the panel's scratch sub-stages (``_tmp/panel_core``,
``_tmp/panel_member``: functions of prices and short interest, no manifest of their own) are not stages, so
``identity_table`` and ``delisting``, which read them, list prices and short_interest instead.
"""

from __future__ import annotations

import ast
import sys
from collections.abc import Iterable
from pathlib import Path

from .contract import SRC_ROOT, Output, RegistryError, Stage, validate

M = "atx_db.alpha_panel."
SESSION_MARK = "CAST(session_date AS TIMESTAMP) + INTERVAL 22 HOUR"
LO1_ROLE = "C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-lo1"

# shared column documentation (COMMENT ON COLUMN in the catalog); an output's ``columns`` override these
COLUMN_DOCS: dict[str, str] = {
    "security_id": "vendor line key: SpiderRock/ORATS securityID of TickerHistory3 (sid0-bracket repair applied)",
    "session_date": "calendar session (distinct vendor tradingDate with >= 1,000 rows)",
    "cik": "SEC Central Index Key of the issuer",
    "available_at": "clock (UTC): earliest time a point-in-time consumer may use the row; visible at decision "
                    "session d only if available_at < 22:00 UTC of session d-1",
    "available_at_basis": "rule that produced available_at",
    "available_basis": "rule that produced available_at",
    "clock_basis": "rule that produced the clock (fsds_accepted_utc, or fc1 = filed + 46 h floor)",
    "clock_utc": "clock (naive UTC): FSDS accepted_utc of the accession, else filed 00:00 UTC + 46 h (legacy name "
                 "of available_at)",
    "accepted_utc": "EDGAR acceptance time (UTC) of the accession; the row's clock",
    "acceptance_utc": "EDGAR acceptance time (UTC)",
    "acceptance_clock": "basis of the acceptance time (index page, daily index, filing date floor)",
    "vintage_risk": "true when the source re-posts or restates history without a revision trail",
    "accession": "EDGAR accession number",
    "form": "EDGAR form type",
    "filing_date": "EDGAR filing date",
    "filed": "EDGAR filing date",
    "ticker": "vendor ticker on that date",
    "symbol": "source symbol as served",
    "cusip": "9-character CUSIP as served (check digit repaired where flagged)",
    "link_tier": "identity tier of the CIK link: strict (dated evidence), name (FINRA name match) or backfill "
                 "(snapshot ticker, survivorship-tilted)",
    "settlement_date": "settlement date the value refers to",
    "dissemination_date": "FINRA dissemination (publication) date",
    "trade_date": "trade date the value refers to",
    "list_date": "Reg SHO threshold list date",
    "market": "listing market / reporting facility code",
    "map_basis": "rule that mapped the source symbol to security_id",
    "sid_repaired": "true when the vendor sid-0 row was reassigned by the sid0-bracket repair",
    "source_file": "raw landing file the row came from",
    "period_end": "fiscal period end (economic date)",
    "fiscal_period": "fiscal period label (FY, Q1..Q4)",
    "item": "fundamental item code",
    "value": "item value in `currency`",
    "currency": "reporting currency (ISO 4217) of the filing's money facts",
    "valid_from": "first day of the validity interval",
    "valid_to": "last day of the validity interval",
    "start": "first day of the link interval",
    "end_incl": "last day of the link interval (inclusive)",
    "primary": "P = the issuer's primary line that day, J = another line of the issuer",
    "tier": "identity tier (high, medium, backfill, name)",
    "basis": "evidence basis of the link",
    "period_of_report": "13F report period (calendar quarter end)",
    "ret": "vendor total return (close over split/dividend-adjusted prior close, minus 1); NULL when guarded",
    "ret_guarded": "true when the vendor return is non-finite, |ln(1+ret)| > 1.5, or a sentinel price",
    "adj_close": "backward chain of 1 + ret anchored to the line's last raw close",
    "close": "raw traded close",
    "open": "raw traded open", "high": "raw traded high", "low": "raw traded low",
    "volume": "shares traded", "dollar_volume": "close * volume",
    "shares_vendor": "vendor shares outstanding on that date (NOT point in time: cover-date runs)",
    "earn_flag": "vendor earnFlag (N, -1, 0, 1)",
    "gics": "vendor GICS code (static vendor field, not point in time)",
    "return_factor": "vendor price adjustment factor of the day (split, distribution)",
    "member": "scorecard universe research-prior63-usd-adv-topn-v1 (top 3,000 by prior 63-session dollar volume)",
    "member_equity": "member AND is_operating AND NOT is_index (equity coverage universe)",
    "at": "total assets (Compustat at)", "lt": "total liabilities (lt)", "che": "cash and short-term investments (che)",
    "debt": "total debt (dltt + dlc)", "be": "book equity (Fama-French)", "seq": "stockholders' equity (seq)",
    "sale_q": "revenue, fiscal quarter (saleq)", "sale_ttm": "revenue, trailing twelve months",
    "cogs_ttm": "cost of goods sold, TTM", "xsga_ttm": "SG&A, TTM", "gp_ttm": "gross profit, TTM",
    "oi_ttm": "operating income, TTM", "ni_q": "net income, quarter", "ni_ttm": "net income, TTM",
    "cfo_ttm": "operating cash flow, TTM", "capx_ttm": "capital expenditure, TTM", "xrd_ttm": "R&D expense, TTM",
    "dvc_ttm": "common dividends, TTM", "prstkc_ttm": "share repurchases, TTM", "sstk_ttm": "share issuance, TTM",
    "dp_ttm": "depreciation and amortization, TTM", "txt_q": "income tax, quarter",
    "shrs_q": "common shares outstanding at the filing (dei cover, balance sheet or weighted)",
    "noa": "net operating assets", "invt": "inventories (invt)", "rect": "receivables (rect)", "ppe": "net PP&E (ppent)",
    "sue": "standardized unexpected earnings (seasonal random walk)", "fscore": "Piotroski F-score (9 terms)",
}


def _o(glob: str, view: str = "", clock: str | None = "available_at", **kw: object) -> Output:
    return Output(glob=glob, view=view, clock=clock, **kw)  # type: ignore[arg-type]


def _session(glob: str, view: str = "", **kw: object) -> Output:
    return _o(glob, view, clock="session_date", clock_sql=SESSION_MARK, **kw)


STAGES: tuple[Stage, ...] = (
    Stage("prices", "MKT", "atx.alpha-panel.prices/v1", M + "prices", guard_gb=1.0, vintage="snapshot",
          staleness="daily bars, same-date",
          outputs=(_session("prices/year=*/prices.parquet", "prices",
                            doc="daily vendor bars 2018+ (TickerHistory3), sid0-bracket and factor-break repaired"),
                   _o("calendar.parquet", "calendar", clock=None,
                      doc="session calendar: distinct vendor tradingDate values with >= 1,000 rows")),
          doc="TickerHistory3 (ORATS 2026-09-20 snapshot); no vintage proof"),
    Stage("prices_history", "MKT", "atx.alpha-panel.prices/v1", None, guard_gb=1.0, vintage="snapshot",
          staleness="daily bars, same-date",
          outputs=(_session("prices_history/year=*/prices.parquet", "prices_history",
                            doc="daily vendor bars 2012-03-26..2017 (look-back years; prices/ is canonical 2018+)"),),
          doc="prices.py run with ATX_PRICES_START=2012-03-26 into a separate root; years 2012-2017 copied here"),
    Stage("identity", "ID", None, M + "identity_links", args=(("build",),), guard_gb=0.8, vintage="interval",
          staleness="links hold over [start, end_incl]",
          outputs=(_o("identity/links.parquet", "identity_links_strict",
                      doc="strict CIK links (r4-links-asof-v1) from the identity rehearsal r4 export"),)),
    Stage("identity_names", "ID", None, M + "identity_names", manifest="identity/name_manifest.json",
          inputs=("identity", "short_interest", "prices"), guard_gb=1.0, vintage="daily",
          staleness="a name link holds 45 days after its dissemination",
          outputs=(_o("identity/links_name_days.parquet", "identity_links_name_days",
                      doc="FINRA issue name -> SEC filer name tier (finra-name-match-v1), one row per line-day"),),
          doc="reads _tmp/identity_days_strict_backfill.parquet from identity_backfill's first pass"),
    Stage("identity_backfill", "ID", None, M + "identity_backfill", manifest="identity/backfill_manifest.json",
          inputs=("identity", "identity_names", "prices"), guard_gb=1.0, vintage="interval",
          staleness="links hold over [start, end_incl]",
          outputs=(_o("identity/links_backfill.parquet", "identity_links_backfill",
                      doc="snapshot-run-backfill-v1 tier (survivorship-tilted: lines alive at the snapshot)"),
                   _o("identity/links_combined.parquet", "identity_links_combined",
                      doc="strict + backfill + name links with the re-derived primary line")),
          doc="build.py runs it before identity_names (strict + backfill days) and after (combine)"),
    Stage("identity_table", "ID", "atx.alpha-panel.identity-link-table/v1", M + "identity_table",
          manifest="identity/link_table_manifest.json",
          inputs=("identity_backfill", "identity_names", "short_interest", "prices"), guard_gb=0.8,
          vintage="interval", staleness="links hold over [valid_from, valid_to]",
          outputs=(_o("identity/link_table.parquet", "identity_link_table",
                      doc="dated security_id -> cik link table with link_tier (identity-link-table-v1)"),
                   _o("identity/multi_class_issuers.parquet", "identity_multi_class_issuers", clock=None,
                      doc="issuers with several vendor lines (audit)")),
          doc="also reads the panel core/membership scratch (_tmp/panel_core, _tmp/panel_member)"),
    *(Stage(f"export_identity_bridge_{v}", "ID", "atx.identity-bridge/v1", None, built_by="identity_table",
            manifest=f"export/identity-bridge-v2-{v}/manifest.json", inputs=("identity_table",),
            vintage="interval", staleness="links hold over [valid_from, valid_to]",
            outputs=(_o(f"export/identity-bridge-v2-{v}/links.parquet", f"export_identity_bridge_{v}",
                        doc=f"consumer identity bridge ({v} tiers), atx.identity-bridge/v1"),))
      for v in ("all", "pit", "strict")),
    Stage("short_interest", "OWN", None, M + "short_interest", args=(("build",),), fetch=(("fetch",),),
          guard_gb=0.8, staleness="45 days after settlement",
          outputs=(_o("short_interest/si.parquet", "short_interest", clock="dissemination_date",
                      clock_sql="CAST(dissemination_date AS TIMESTAMP) + INTERVAL 22 HOUR",
                      doc="FINRA consolidated short interest per line and settlement (si-ticker-asof-settlement-v3); "
                          "clock = dissemination date 22:00 UTC (no time-of-day evidence; never earlier than "
                          "publication)"),
                   _o("short_interest/mapping_audit.parquet", "short_interest_mapping_audit", clock=None,
                      doc="every raw FINRA row with its symbol -> security_id mapping outcome (audit)"))),
    Stage("short_volume", "OWN", None, M + "short_volume", args=(("build",),), fetch=(("download",),),
          guard_gb=0.8, staleness="a trade date older than 5 sessions is stale",
          outputs=(_o("short_volume/year=*/short_volume.parquet", "short_volume", clock="trade_date",
                      clock_sql="CAST(trade_date AS TIMESTAMP) + INTERVAL 1 DAY",
                      doc="FINRA CNMS daily short volume per line (sv-ticker-on-trade-date-v2); clock = trade date "
                          "+ 1 day 00:00 UTC (the short_volume_ext rule)"),)),
    Stage("short_volume_ext", "OWN", "atx.alpha-panel.short-volume-ext/v1", M + "short_volume_ext", guard_gb=0.8,
          staleness="a trade date older than 5 sessions is stale", vintage="snapshot",
          outputs=(_o("short_volume_ext/year=*/short_volume_ext.parquet", "short_volume_ext",
                      doc="short volume with exempt volume and reporting facilities"),)),
    Stage("ftd", "OWN", "atx.alpha-panel.ftd/v1", M + "ftd", args=(("build",),), fetch=(("download",),),
          guard_gb=0.8, vintage="snapshot",
          staleness="NaN when the latest published settlement date is > 60 days before the session",
          outputs=(_o("ftd/year=*/ftd.parquet", "ftd", doc="SEC fails-to-deliver per settlement date"),
                   _o("ftd/cusip_map.parquet", "ftd_cusip_map", clock=None,
                      doc="observed CUSIP -> security_id pairs with first/last settlement (not point in time)"))),
    Stage("regsho_threshold", "OWN", "atx.alpha-panel.regsho-threshold/v1", M + "regsho", args=(("build",),),
          fetch=tuple(("download", "--market", m) for m in ("nasdaq", "nyse_combined", "cboe_bzx", "finra_otc")),
          guard_gb=0.8, vintage="snapshot", staleness="NaN when the latest visible list is > 10 days old",
          outputs=(_o("regsho_threshold/lists.parquet", "regsho_threshold_lists",
                      doc="one row per (market, list date) file with its publication clock"),
                   _o("regsho_threshold/year=*/threshold.parquet", "regsho_threshold",
                      doc="Reg SHO threshold list rows mapped to security_id"))),
    Stage("thirteenf", "OWN", "atx.alpha-panel.thirteenf/v1", M + "thirteenf", args=(("build",),),
          fetch=(("fetch",),), inputs=("ftd",), guard_gb=0.8,
          staleness="NaN when the session is > 150 days after period_of_report",
          outputs=(_o("thirteenf/agg_asof45.parquet", "thirteenf_agg_asof45",
                      doc="institutional holdings per security and quarter, filings up to the 45-day deadline"),
                   _o("thirteenf/agg_final.parquet", "thirteenf_agg_final",
                      doc="institutional holdings per security and quarter, every filing (late and amended)"),
                   _o("thirteenf/cusip_map_pit.parquet", "thirteenf_cusip_map_pit", clock=None,
                      doc="CUSIP -> security_id per quarter (13f-cusip-ftd-window-v1)"),
                   _o("thirteenf/filers.parquet", "thirteenf_filers", clock="first_available_at",
                      doc="13F filer per quarter (filer type not classified)"),
                   _o("thirteenf/filing_checks.parquet", "thirteenf_filing_checks", clock=None,
                      doc="value-unit check per filing (audit)"),
                   _o("thirteenf/filings.parquet", "thirteenf_filings", doc="13F-HR filings and amendments"),
                   _o("thirteenf/parts/source=*/filings.parquet", "thirteenf_part_filings",
                      doc="13F filings per source data set"),
                   _o("thirteenf/parts/source=*/holdings.parquet", "thirteenf_holdings",
                      doc="13F information-table rows per source data set"))),
    Stage("sec_filings", "EVT", "atx.alpha-panel.sec-filings/v1", M + "sec_filings", args=(("--phase", "all"),),
          guard_gb=1.0, staleness="event data (filer_regime carries valid_from/valid_to)",
          outputs=(_o("sec_filings/clock_files.parquet", "sec_filings_clock_files", clock=None,
                      doc="acceptance-clock evidence per CIK and source (audit)"),
                   _o("sec_filings/delisting_causes.parquet", "sec_filings_delisting_causes",
                      doc="filer-level delisting cause evidence"),
                   _o("sec_filings/eight_k_items.parquet", "sec_filings_eight_k_items", doc="8-K items per filing"),
                   _o("sec_filings/events.parquet", "sec_filings_events", doc="typed SEC events"),
                   _o("sec_filings/filer_regime.parquet", "sec_filings_filer_regime", vintage="interval",
                      doc="filer regime (domestic, FPI, ...) intervals"),
                   _o("sec_filings/filings.parquet", "sec_filings", doc="every EDGAR filing with its acceptance clock"),
                   _o("sec_filings/issuer_profile.parquet", "sec_filings_issuer_profile", vintage="snapshot",
                      doc="submissions issuer profile (snapshot)"))),
    Stage("earnings_calendar", "EVT", "atx.alpha-panel.earnings-calendar/v1", M + "earnings_calendar",
          inputs=("sec_filings", "prices", "identity_backfill"), guard_gb=0.8, staleness="event data",
          outputs=(_o("earnings_calendar/announcements.parquet", "earnings_calendar",
                      doc="8-K item 2.02 earnings announcements with timing, reaction session and expected next"),)),
    Stage("insider", "OWN", "atx.alpha-panel.insider/v1", M + "insider", args=(), fetch=((),),
          inputs=("sec_filings",), guard_gb=0.8, staleness="event data",
          outputs=(_o("insider/owners/year=*/*.parquet", "insider_owners",
                      doc="Form 3/4/5 reporting owners per accession"),
                   _o("insider/transactions/year=*/*.parquet", "insider_transactions",
                      doc="Form 3/4/5 derivative and non-derivative transactions")),
          doc="one command lands the quarterly data sets and parses them (network)"),
    Stage("fundamentals", "FUND", "atx.alpha-panel.fundamentals/v2", M + "fundamentals",
          args=(("prepare",), ("batches",), ("finalize",), ("validate",)), inputs=("identity",), guard_gb=0.8,
          vintage="vintage",
          staleness="row stale when date - period_end > staleness_days (200, or 400 for annual-only filers)",
          outputs=(_o("fundamentals/events.parquet", "fundamentals_events", clock="clock_utc", keys=("cik",),
                      order=("accession",), doc="issuer knowledge after each filing event (fund-events-pit-v2)"),
                   _o("fundamentals/quarterly_history.parquet", "fundamentals_quarterly_history",
                      keys=("cik", "item", "period_end", "fiscal_period"), order=("accession",),
                      doc="one row per (cik, item, period, accession) with value, currency, clock"),
                   _o("fundamentals/sic_events.parquet", "fundamentals_sic_events", clock="clock_utc",
                      keys=("cik",), order=("accession",), doc="SIC, FF12 and FF49 per filing event"))),
    Stage("security_master", "ID", "atx.alpha-panel.security-master/v1", M + "security_master",
          inputs=("short_interest",), guard_gb=0.8, staleness="FINRA names: 45 days; lines: static",
          outputs=(_o("security_master/finra_names.parquet", "security_master_finra_names",
                      doc="dated FINRA issue names, listing market and security type per line"),
                   _o("security_master/lines.parquet", "security_master_lines", clock=None,
                      doc="one row per vendor line: first/last session, delisting date, directory snapshot"))),
    Stage("corporate_actions", "MKT", "atx.alpha-panel.corporate-actions/v1", M + "corporate_actions",
          inputs=("prices",), guard_gb=0.8, vintage="snapshot", staleness="event data",
          outputs=(_o("corporate_actions/vendor_events.parquet", "corporate_actions",
                      doc="splits and distributions from vendor return factors (vendor-factor-events-v1)"),)),
    Stage("delisting", "MKT", "atx.alpha-panel.delisting/v1", M + "delisting",
          inputs=("security_master", "identity_table", "sec_filings", "prices"), guard_gb=0.8, staleness="event data",
          outputs=(_o("delisting/events.parquet", "delisting",
                      doc="terminal events and imputed delisting returns (delisting-rule-r-v1)"),),
          doc="ever_member reads _tmp/panel_member (panel membership scratch)"),
    Stage("panel", "S0", "atx.alpha-panel.panel/v2", M + "panel",
          args=(("--stages", "core,membership"), ("--stages", "assemble")),
          inputs=("prices", "identity_backfill", "identity_table", "fundamentals", "short_interest", "short_volume",
                  "security_master", "earnings_calendar", "sec_filings", "ftd", "insider", "short_volume_ext",
                  "regsho_threshold", "thirteenf"), guard_gb=1.0, vintage="daily",
          staleness="issuer items stale after 400 days (sue 200, SIC 550, SI 45)",
          outputs=(_session("panel/year=*/*.parquet", "panel",
                            doc="point-in-time daily panel, scorecard field names (atx.alpha-panel.panel/v2)"),)),
    Stage("borrow_proxy", "S0", "atx.alpha-panel.borrow-proxy/v1", M + "borrow_proxy",
          inputs=("short_interest", "thirteenf", "ftd", "regsho_threshold", "prices", "panel"), guard_gb=0.8,
          vintage="daily", planned=True, staleness="SI 45 d, 13F 150 d, FTD 60 d, threshold lists 10 d",
          outputs=(_session("borrow_proxy/year=*/borrow_proxy.parquet", "borrow_proxy",
                            doc="public short-constraint PROXY inputs (not a borrow fee)"),)),
    Stage("metrics", "S0", "atx.alpha-panel.metrics/v1", M + "metrics", inputs=("panel", "fundamentals",
          "identity_table"), guard_gb=1.0, planned=True, staleness="per panel build",
          outputs=(_o("metrics/coverage.parquet", "metrics_coverage", clock=None,
                      doc="finite share per panel field and window over member_equity cells and linked bases"),
                   _o("metrics/distribution.parquet", "metrics_distribution", clock=None,
                      doc="quantiles per panel field and window (no return statistic on 2023+)"))),
    Stage("export_fundamental_events", "FUND", "atx.fundamental-events/v1", M + "fund_export",
          args=(("build",), ("verify",)), manifest="export/fundamental-events-v1/manifest.json",
          inputs=("fundamentals",), guard_gb=0.8, vintage="vintage", staleness="consumer contract rule",
          outputs=(_o("export/fundamental-events-v1/fundamental_events.parquet", "export_fundamental_events",
                      clock="accepted_utc", keys=("cik",), order=("accession",),
                      doc="atx.fundamental-events/v1 consumer export"),
                   _o("export/fundamental-events-v1/sic_events.parquet", "export_fundamental_sic_events",
                      clock="accepted_utc", keys=("cik",), order=("accession",),
                      doc="atx.fundamental-events/v1 SIC events"))),
    Stage("export_acceptance_bridge_pit_me", "S0", "atx.research-role-fields/v1", None,
          manifest="export/acceptance/bridge-pit-me/manifest.json", inputs=("panel",), guard_gb=1.0,
          vintage="daily", doc="atx-impl research-role fields (f64 arrays, no Parquet) for the consumer acceptance "
                              "load, written by export_impl on the acceptance role's axes"),
    Stage("export_lo1", "S0", "atx.research-role-fields/v1", M + "export_impl",
          args=(("align", "--role", LO1_ROLE, "--out", "data/alpha_panel/v1/export/lo1-fields-v2"),),
          manifest="export/lo1-fields-v2/manifest.json", inputs=("panel",), guard_gb=1.0, vintage="daily",
          planned=True, doc="lo1 acceptance role fields (f64 arrays, no Parquet)"),
    Stage("characteristics", "CHAR", None, M + "characteristics", inputs=("panel",), guard_gb=1.0, vintage="daily",
          planned=True, staleness="per panel build",
          outputs=(_session("characteristics/year=*/characteristics.parquet", "characteristics",
                            doc="raw daily characteristic values"),)),
)

# alpha_panel/build.py STAGES name -> registry stage (None: not a lake stage)
BUILD_STEPS: dict[str, str | None] = {
    "prices": "prices", "identity": "identity", "backfill": "identity_backfill",
    "fundamentals_prepare": "fundamentals", "fundamentals_batches": "fundamentals",
    "fundamentals_finalize": "fundamentals", "fundamentals_validate": "fundamentals",
    "short_interest_fetch": "short_interest", "short_interest": "short_interest",
    "short_volume_download": "short_volume", "short_volume": "short_volume", "names": "identity_names",
    "combine": "identity_backfill", "panel_core": "panel", "sec_filings": "sec_filings",
    "earnings_calendar": "earnings_calendar", "insider": "insider", "ftd_download": "ftd", "ftd": "ftd",
    **{f"regsho_download_{m}": "regsho_threshold" for m in ("nasdaq", "nyse_combined", "cboe_bzx", "finra_otc")},
    "regsho": "regsho_threshold", "short_volume_ext": "short_volume_ext", "thirteenf_fetch": "thirteenf",
    "thirteenf": "thirteenf", "identity_table": "identity_table", "security_master": "security_master",
    "corporate_actions": "corporate_actions", "delisting": "delisting", "panel": "panel",
    "borrow_proxy": "borrow_proxy", "metrics": "metrics", "fund_export": "export_fundamental_events",
    "fund_export_verify": "export_fundamental_events", "export_lo1": "export_lo1",
    "characteristics": "characteristics",
    "coverage": None,  # report: writes coverage.json and docs, no manifest
}


def discover(src_root: Path = SRC_ROOT, package: str = "atx_db", errors: list[str] | None = None) -> list[Stage]:
    """``LAKE_STAGES`` literals declared in modules under ``src_root/package`` (parsed, never imported). With
    ``errors`` given, an unparsable module or a bad entry is reported there and skipped instead of raising."""
    found: list[Stage] = []
    base = src_root / package
    for path in sorted(base.rglob("*.py")):
        if "lake" in path.relative_to(base).parts[:1]:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "LAKE_STAGES" not in text:
            continue
        dotted = ".".join(path.relative_to(src_root).with_suffix("").parts)
        try:
            for node in ast.parse(text, filename=str(path)).body:
                if not isinstance(node, ast.Assign | ast.AnnAssign) or node.value is None:
                    continue
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                if not any(isinstance(t, ast.Name) and t.id == "LAKE_STAGES" for t in targets):
                    continue
                try:
                    entries = ast.literal_eval(node.value)
                except ValueError as exc:
                    raise RegistryError(f"{path}: LAKE_STAGES must be a literal list of dicts ({exc})") from exc
                for e in entries:
                    found.append(Stage.from_dict({"module": dotted, **e}))
        except (SyntaxError, RegistryError, TypeError) as exc:
            if errors is None:
                raise
            errors.append(f"{dotted}: {exc}")
    return found


def load(src_root: Path = SRC_ROOT, extra: Iterable[Stage] = (), include_discovered: bool = True,
         strict: bool = True) -> list[Stage]:
    """Every registered stage, validated, in topological order (inputs first).

    ``strict=False`` (the CLIs): a discovered entry that does not validate (bad field, unknown input, clash) is
    dropped with a warning on stderr, so one lane's broken declaration cannot stop verify, catalog or plan."""
    stages = list(STAGES) + list(extra)
    if not include_discovered:
        return validate(stages)
    if strict:
        return validate(stages + discover(src_root))
    errors: list[str] = []
    pending = discover(src_root, errors=errors)
    progress = True
    while pending and progress:
        progress = False
        for s in list(pending):
            try:
                validate([*stages, s])
            except RegistryError:
                continue
            stages.append(s)
            pending.remove(s)
            progress = True
    for s in pending:
        try:
            validate([*stages, s])
        except RegistryError as exc:
            errors.append(f"{s.module}: stage {s.name!r} dropped: {exc}")
    for e in errors:
        print(f"lake registry WARNING: {e}", file=sys.stderr)
    return validate(stages)


def get(name: str, stages: Iterable[Stage] | None = None) -> Stage:
    for s in stages if stages is not None else load():
        if s.name == name:
            return s
    raise KeyError(f"stage {name!r} is not registered")
