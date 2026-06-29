from __future__ import annotations

import json
from dataclasses import dataclass

from .connection import DuckDBStore


@dataclass(frozen=True)
class ProviderParityRow:
    provider: str
    provider_domain: str
    warehouse_domain: str
    reference_tables: tuple[str, ...]
    institutional_grain: str
    institutional_keys: tuple[str, ...]
    pit_fields: tuple[str, ...]
    factors_or_fields: tuple[str, ...]
    open_substitute: str
    warehouse_tables: tuple[str, ...]
    parity_status: str
    limitations: str
    next_gap: str
    source_urls: tuple[str, ...]


PROVIDER_PARITY_ROWS: tuple[ProviderParityRow, ...] = (
    ProviderParityRow(
        provider="FactSet",
        provider_domain="Symbology and entity master",
        warehouse_domain="security_master",
        reference_tables=(
            "Symbology API",
            "ID Lookup API",
            "Concordance API",
            "FactSet native security/entity identifiers",
        ),
        institutional_grain="Identifier, security, listing, and entity mapping rows.",
        institutional_keys=("fsymId", "entityId", "ticker", "CUSIP", "ISIN", "SEDOL", "exchange/local identifiers"),
        pit_fields=("effective/as-of mapping dates where licensed products expose them", "source_loaded_at"),
        factors_or_fields=("entity reference fields", "security identifiers", "listing identifiers", "cross-reference mappings"),
        open_substitute=(
            "SEC company_tickers, Nasdaq Trader symbol directory and add/delete events, local ticker archive, "
            "13F CUSIP evidence, and audited identifier_resolution_* decisions."
        ),
        warehouse_tables=(
            "securities",
            "security_identifier_history",
            "exchange_listings",
            "nasdaq_listing_events",
            "listing_status_intervals",
            "identifier_resolution_candidates",
            "identifier_resolution_decisions",
        ),
        parity_status="partial",
        limitations="FactSet identifiers and concordance history are proprietary; open substitute is evidence-based.",
        next_gap="Add FIGI, LEI, ISIN, and longer historical listing-status backfill while preserving security_id stability.",
        source_urls=(
            "https://developer.factset.com/api-catalog/symbology-api",
            "https://developer.factset.com/api-catalog/id-lookup-api",
            "https://developer.factset.com/api-catalog/factset-concordance-api",
            "https://www.sec.gov/search-filings/edgar-application-programming-interfaces",
            "https://www.nasdaqtrader.com/trader.aspx?id=symboldirdefs",
        ),
    ),
    ProviderParityRow(
        provider="FactSet",
        provider_domain="Prices, returns, and corporate actions",
        warehouse_domain="prices",
        reference_tables=("FactSet Prices API", "FactSet Global Prices API", "corporate actions endpoints"),
        institutional_grain="Security-date bars and security-event corporate action rows.",
        institutional_keys=("fsymId", "price date", "corporate action event id/date"),
        pit_fields=("trade_date", "event/ex_date", "pricing availability timestamp", "source_loaded_at"),
        factors_or_fields=("OHLCV", "returns", "shares", "splits", "dividends", "event-based corporate actions"),
        open_substitute=(
            "Local tbltickerhistory daily bars plus inferred split/dividend corporate actions and "
            "SEC XBRL-derived PIT share-count history."
        ),
        warehouse_tables=(
            "tbltickerhistory_daily",
            "equity_daily_bars",
            "corporate_actions",
            "shares_outstanding_history",
            "v_equity_daily_returns",
        ),
        parity_status="partial",
        limitations="Local archive does not provide the full licensed FactSet adjustment chain or global coverage.",
        next_gap="Add delisting-return proxies plus split/dividend adjustment audit trails with separate price/share factors.",
        source_urls=(
            "https://developer.factset.com/api-catalog/factset-prices-api",
            "https://developer.factset.com/api-catalog/factset-global-prices-api",
        ),
    ),
    ProviderParityRow(
        provider="FactSet",
        provider_domain="Formula and derived feature access",
        warehouse_domain="feature_store",
        reference_tables=("Formula API", "FactSet Fundamentals API", "FactSet Prices API"),
        institutional_grain="Identifier-date formula output and cross-sectional/range requests.",
        institutional_keys=("identifier", "formula/expression", "as-of date", "calendar"),
        pit_fields=("as-of date", "calendar", "source data availability", "run/input lineage"),
        factors_or_fields=("price factors", "fundamental factors", "business logic formulas", "cross-sectional outputs"),
        open_substitute="feature_definitions, feature_values, feature_set_catalog, feature_dependency_edges, feature_build_manifests, alpha expression/signal/backtest tables, and as-of-safe SQL helpers.",
        warehouse_tables=(
            "feature_definitions",
            "feature_values",
            "feature_set_catalog",
            "feature_dependency_edges",
            "feature_build_manifests",
            "alpha_expression_catalog",
            "alpha_signal_values",
            "alpha_backtest_manifests",
            "v_alpha_daily_panel",
        ),
        parity_status="partial",
        limitations="FactSet formula library and most vendor data packages are proprietary.",
        next_gap="Add reusable factor family APIs, richer expression parsing, and cross-sectional formula request helpers.",
        source_urls=(
            "https://developer.factset.com/api-catalog/formula-api",
            "https://developer.factset.com/api-catalog/factset-fundamentals-api",
            "https://developer.factset.com/api-catalog/factset-prices-api",
        ),
    ),
    ProviderParityRow(
        provider="S&P Global Compustat",
        provider_domain="Standardized fundamentals",
        warehouse_domain="fundamentals",
        reference_tables=("Compustat Financials", "Xpressfeed item-level transaction files"),
        institutional_grain="Company/security fiscal period statement points, annual and interim.",
        institutional_keys=("company/security identifier", "fiscal year", "fiscal period", "statement item", "currency/unit"),
        pit_fields=("point-in-time snapshot", "filing/availability date", "load timestamp"),
        factors_or_fields=("standardized income statement", "balance sheet", "cash flow", "ratios", "multiples"),
        open_substitute=(
            "SEC companyfacts XBRL facts normalized into fundamental_points with explicit-symbol, universe, "
            "or SEC ticker-map coverage controls; accession revision chains, "
            "canonical income/balance/cash-flow statement points, fiscal-period windows, TTM values, "
            "observed SEC frames, public FASB/XBRL presentation/calculation/dimension relationships, "
            "issuer filing-instance fact/context/dimension-member extraction, "
            "and sec_fundamentals_v1 accounting, valuation, cash-flow, shareholder-yield, growth, and revision features."
        ),
        warehouse_tables=(
            "sec_company_facts",
            "xbrl_concept_catalog",
            "xbrl_taxonomy_relationships",
            "xbrl_dimension_edges",
            "xbrl_fact_frames",
            "xbrl_filing_contexts",
            "xbrl_filing_dimensions",
            "xbrl_filing_facts",
            "fundamental_fact_revisions",
            "fundamental_statement_map",
            "fundamental_statement_points",
            "fundamental_periods",
            "fundamental_ttm_points",
            "fundamental_points",
            "feature_values",
            "v_fundamental_ttm_latest",
            "v_fundamental_statement_latest",
            "v_fundamental_points_latest",
        ),
        parity_status="partial",
        limitations="Compustat standardized taxonomy, history, and point-in-time snapshots are proprietary.",
        next_gap="S4a/S4b concept dictionary now covers 137 authorized cross-industry item_ids plus 37 bank/insurance/REIT overlay item_ids; rdq date capture (S4c), DQC validation (S4d), and broader issuer backfill remain pending.",
        source_urls=(
            "https://www.marketplace.spglobal.com/en/datasets/compustat-financials-%288%29",
            "https://www.marketplace.spglobal.com/en/solutions/xpressfeed-%28b73250d6-a15c-4243-9016-3e5bf6300e43%29",
            "https://www.spglobal.com/market-intelligence/en/solutions/products/fundamental-data",
            "https://www.sec.gov/search-filings/edgar-application-programming-interfaces",
        ),
    ),
    ProviderParityRow(
        provider="S&P Capital IQ",
        provider_domain="As-reported fundamentals, estimates, and ratios",
        warehouse_domain="fundamentals_features",
        reference_tables=("S&P Capital IQ Financials", "As Reported Financials", "Estimates Snapshot"),
        institutional_grain="Company fiscal period facts, estimates, ratios, and revision snapshots.",
        institutional_keys=("company identifier", "fiscal period", "data item", "currency/unit", "revision timestamp"),
        pit_fields=("filing date", "availability timestamp", "estimate snapshot timestamp", "source_loaded_at"),
        factors_or_fields=("as-reported statements", "standardized values", "ratios", "valuation multiples", "estimates"),
        open_substitute="SEC companyfacts/submissions for as-reported fundamentals with explicit-symbol, universe, or SEC ticker-map coverage controls; public XBRL taxonomy relationships, filing-instance fact/context/dimension extraction, revision chains, normalized statement/period/TTM points, and feature_values for derived ratios, growth, and revision signals.",
        warehouse_tables=(
            "sec_submissions",
            "sec_company_facts",
            "xbrl_taxonomy_relationships",
            "xbrl_dimension_edges",
            "xbrl_fact_frames",
            "xbrl_filing_contexts",
            "xbrl_filing_dimensions",
            "xbrl_filing_facts",
            "fundamental_fact_revisions",
            "fundamental_statement_points",
            "fundamental_periods",
            "fundamental_ttm_points",
            "fundamental_points",
            "feature_values",
        ),
        parity_status="partial",
        limitations="Capital IQ estimates, proprietary standardization, and linkages are not open data.",
        next_gap="Add estimates placeholders/catalog entries, scale filing-instance segment history beyond the current five-issuer batch, and enrich inline-XBRL transforms.",
        source_urls=(
            "https://www.spglobal.com/market-intelligence/en/solutions/products/fundamental-data",
            "https://www.marketplace.spglobal.com/en/solutions/xpressfeed-%28b73250d6-a15c-4243-9016-3e5bf6300e43%29",
        ),
    ),
    ProviderParityRow(
        provider="CRSP",
        provider_domain="Security master and identifiers",
        warehouse_domain="security_master",
        reference_tables=("CRSP US Stock Databases", "PERMNO/PERMCO security and issuer identifiers"),
        institutional_grain="Security and company identifier histories across listings and active/inactive securities.",
        institutional_keys=("PERMNO", "PERMCO", "NCUSIP/CUSIP", "ticker", "exchange", "date ranges"),
        pit_fields=("name/listing date ranges", "event dates", "release/load timestamp"),
        factors_or_fields=("permanent identifiers", "names", "exchanges", "share classes", "active/inactive coverage"),
        open_substitute="Stable warehouse security_id plus PIT identifier/listing history from SEC, Nasdaq add/delete events, and local archive evidence.",
        warehouse_tables=(
            "securities",
            "security_identifier_history",
            "exchange_listings",
            "nasdaq_listing_events",
            "listing_status_intervals",
            "v_security_master_current",
        ),
        parity_status="partial",
        limitations="CRSP PERMNO/PERMCO and full historical event processing are proprietary.",
        next_gap="Backfill deeper historical listing checkpoints and add stronger share-class issuer modeling.",
        source_urls=(
            "https://www.crsp.org/research/crsp-us-stock-databases/",
            "https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_%26_Indexes_Database_Data_Descriptions_Guide.pdf",
        ),
    ),
    ProviderParityRow(
        provider="CRSP",
        provider_domain="Daily stock file, returns, and distributions",
        warehouse_domain="prices",
        reference_tables=("CRSP daily stock file", "distribution events", "delisting returns"),
        institutional_grain="Security-date price/return rows and security-event distribution/delisting rows.",
        institutional_keys=("PERMNO", "trade date", "distribution event", "delisting event"),
        pit_fields=("trade date", "ex/distribution dates", "delisting date", "release/load timestamp"),
        factors_or_fields=("returns", "prices", "volume", "shares", "distributions", "delisting returns"),
        open_substitute=(
            "equity_daily_bars, v_equity_daily_returns, corporate_actions, SEC XBRL share-count history, "
            "Nasdaq add/delete events, and listing-status intervals from local/public evidence."
        ),
        warehouse_tables=(
            "equity_daily_bars",
            "corporate_actions",
            "shares_outstanding_history",
            "nasdaq_listing_events",
            "listing_status_intervals",
            "v_equity_daily_returns",
        ),
        parity_status="partial",
        limitations="Open substitute lacks official CRSP delisting returns and survivorship-bias-free full history.",
        next_gap="Add delisting-return estimates, adjustment factor history, and return-adjustment diagnostics.",
        source_urls=(
            "https://www.crsp.org/research/crsp-us-stock-databases/",
            "https://www.crsp.org/crsp_pdf/crsp-us-stock-indexes-databases-guide-flat-file-format-1-0/",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR and FASB",
        provider_domain="Companyfacts, submissions, and XBRL taxonomy",
        warehouse_domain="fundamentals",
        reference_tables=("SEC submissions JSON", "SEC companyfacts JSON", "US GAAP and SEC Reporting Taxonomies"),
        institutional_grain="CIK/accession filing metadata and XBRL concept/unit/period fact rows.",
        institutional_keys=("CIK", "accession_number", "taxonomy", "concept", "unit", "period_start/end", "filed_date"),
        pit_fields=("filed_date", "acceptance_datetime", "available_at", "source_loaded_at"),
        factors_or_fields=("as-reported XBRL facts", "forms", "accessions", "restatements", "taxonomy concepts"),
        open_substitute=(
            "Direct SEC APIs and FASB taxonomy documentation loaded into sec_company_facts "
            "with configurable explicit-symbol, universe, or SEC ticker-map coverage; "
            "xbrl_taxonomy_relationships, xbrl_dimension_edges, xbrl_fact_frames, "
            "xbrl_filing_contexts, xbrl_filing_dimensions, xbrl_filing_facts, "
            "fundamental_fact_revisions, fundamental_statement_points, and fundamental_points."
        ),
        warehouse_tables=(
            "sec_submissions",
            "xbrl_concept_catalog",
            "xbrl_taxonomy_packages",
            "xbrl_taxonomy_relationships",
            "xbrl_dimension_edges",
            "xbrl_fact_frames",
            "xbrl_filing_contexts",
            "xbrl_filing_dimensions",
            "xbrl_filing_facts",
            "sec_company_facts",
            "fundamental_fact_revisions",
            "fundamental_statement_map",
            "fundamental_statement_points",
            "fundamental_points",
            "feature_values",
        ),
        parity_status="partial",
        limitations="Filing-instance fact/context extraction is loaded for a bounded balanced issuer batch; inline transform coverage is conservative and issuer breadth depends on configured batch limits.",
        next_gap="Scale filing-instance extraction across the research universe and add richer inline-XBRL transform/unit normalization.",
        source_urls=(
            "https://www.sec.gov/search-filings/edgar-application-programming-interfaces",
            "https://data.sec.gov/",
            "https://www.fasb.org/projects/fasb-taxonomies",
            "https://xbrl.fasb.org/us-gaap/2026/us-gaap-2026.zip",
            "https://xbrl.fasb.org/srt/2026/srt-2026.zip",
            "https://xbrl.fasb.org/resources/annualrelease/2026/SEC_Reporting_Taxonomy_Technical_Guide.pdf",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR",
        provider_domain="Form 13F institutional ownership",
        warehouse_domain="ownership",
        reference_tables=("Form 13F bulk data sets", "cover page", "summary page", "information table"),
        institutional_grain="Manager filing, report period, and holding rows keyed by accession and information-table row.",
        institutional_keys=("accession_number", "CIK", "report_period", "CUSIP", "infotable_sk"),
        pit_fields=("filing_date", "report_period", "source_period", "source_loaded_at"),
        factors_or_fields=("manager metadata", "CUSIP holdings", "shares/value", "discretion", "put/call", "voting authority"),
        open_substitute="SEC bulk Form 13F ZIP files, identifier-resolution evidence, and PIT derived manager/security ownership feature tables.",
        warehouse_tables=(
            "thirteenf_submissions",
            "thirteenf_cover_pages",
            "thirteenf_summary_pages",
            "thirteenf_holdings",
            "thirteenf_managers",
            "thirteenf_manager_reports",
            "thirteenf_security_positions",
            "thirteenf_security_ownership",
            "feature_values",
            "v_thirteenf_positioning_by_security",
        ),
        parity_status="partial",
        limitations="13F is quarterly, delayed, long-only for reportable securities, and current loaded holdings breadth may be filtered by bootstrap CUSIP options.",
        next_gap="Run full-holdings multi-period refreshes, broaden CUSIP/security reconciliation, and add manager-level style/concentration history.",
        source_urls=(
            "https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets",
            "https://www.sec.gov/files/form_13f_readme.pdf",
            "https://www.sec.gov/rules-regulations/staff-guidance/division-investment-management-frequently-asked-questions/frequently-asked-questions-about-form-13f",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR / FactSet Ownership / S&P Capital IQ Pro",
        provider_domain="Section 16 insider ownership",
        warehouse_domain="ownership",
        reference_tables=(
            "SEC Forms 3/4/5 ownership XML",
            "FactSet Ownership insider transactions",
            "S&P Capital IQ ciqInsider / ciqInsiderTransaction",
        ),
        institutional_grain="Reporting owner, issuer, filing accession, transaction row, and disclosed holding row.",
        institutional_keys=(
            "accession_number",
            "reporting_owner_cik",
            "issuer_cik",
            "transaction_ordinal",
            "transaction_code",
        ),
        pit_fields=("period_of_report", "transaction_date", "filing_date", "acceptance_datetime", "available_at"),
        factors_or_fields=(
            "director/officer/10-percent-owner role flags",
            "28-code Section 16 transaction taxonomy",
            "direct/indirect ownership",
            "shares/price/holdings",
            "derivative rows",
            "Rule 10b5-1 indicator and adoption date",
        ),
        open_substitute=(
            "SEC ownership XML normalized into insider, filing_form4, insider_relationship, "
            "insider_transaction, insider_holding, and tradingplan_10b5_1 tables."
        ),
        warehouse_tables=(
            "insider",
            "filing_form4",
            "insider_relationship",
            "insider_transaction",
            "insider_holding",
            "tradingplan_10b5_1",
        ),
        parity_status="partial",
        limitations=(
            "Open SEC XML covers Section 16 filings but lacks vendor backfilled role normalization, "
            "pre-2023 10b5-1 plan inference, insider scoring, and cross-person household/entity rollups."
        ),
        next_gap="Add 13D/G parser, Form 144 reconciliation, N-PORT fund holdings, and richer insider role/cluster scoring.",
        source_urls=(
            "https://www.sec.gov/edgar/searchedgar/ownershipformcodes.html",
            "https://www.sec.gov/page/edgar-ownership-xml-tech-spec",
            "https://data.sec.gov/",
            "https://www.factset.com/marketplace/catalog/product/factset-ownership",
            "https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR / FactSet Ownership / S&P Capital IQ Pro / WhaleWisdom",
        provider_domain="Schedule 13D/G blockholder ownership",
        warehouse_domain="ownership",
        reference_tables=(
            "SEC Schedule 13D/G filings",
            "FactSet Ownership beneficial owners",
            "S&P Capital IQ activism and ownership",
            "WhaleWisdom Schedule 13D/G",
        ),
        institutional_grain="Issuer, filing accession, reporting person, beneficial ownership amount, and percent of class.",
        institutional_keys=("accession_number", "issuer_cik", "CUSIP", "reporting_person_seq"),
        pit_fields=("event_date", "filing_date", "available_at", "source_loaded_at"),
        factors_or_fields=(
            "5%+ beneficial-owner identity",
            "Schedule 13D vs 13G type",
            "voting/dispositive power",
            "aggregate beneficial ownership",
            "percent of class",
            "purpose-of-transaction text",
        ),
        open_substitute=(
            "SEC structured Schedule 13D/G XML normalized into blockholder_filing and "
            "blockholder_reporting_person landing tables with PIT availability."
        ),
        warehouse_tables=("blockholder_filing", "blockholder_reporting_person"),
        parity_status="partial",
        limitations=(
            "Structured XML is only dependable for the post-2024 modernization window; "
            "pre-XML HTML/text backfile parsing, activism campaign classification, and "
            "reporting-person rollups remain vendor gaps."
        ),
        next_gap="Add pre-2024 13D/G HTML parser, amendment-chain reconstruction, and activism campaign/topic classifiers.",
        source_urls=(
            "https://www.sec.gov/edgar/search/",
            "https://data.sec.gov/",
            "https://www.factset.com/marketplace/catalog/product/factset-ownership",
            "https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro",
            "https://whalewisdom.com/schedule13d",
        ),
    ),
    ProviderParityRow(
        provider="FINRA",
        provider_domain="Consolidated short interest",
        warehouse_domain="short_interest",
        reference_tables=("FINRA Consolidated Short Interest dataset", "FINRA Query API"),
        institutional_grain="Symbol and settlement-date short-interest rows.",
        institutional_keys=("symbol", "settlement_date", "market_class_code"),
        pit_fields=("settlement_date", "publication/available_at", "source_loaded_at"),
        factors_or_fields=("current short position", "previous short position", "average daily volume", "days to cover", "short-pressure features", "cross-sectional ranks"),
        open_substitute="FINRA public/API consolidated short interest normalized into finra_short_interest plus compact all-symbol settlement-date backfill manifests and PIT feature-store rows for short pressure, changes, and market-wide ranks.",
        warehouse_tables=("finra_short_interest", "finra_short_interest_backfill_manifests", "feature_values", "feature_definitions", "v_finra_short_interest_latest"),
        parity_status="partial",
        limitations="Warehouse now supports symbol loads plus compact all-symbol settlement-date refresh/backfill; full multi-year market history still depends on FINRA pagination/time budget and broader security reconciliation.",
        next_gap="Add rolling all-symbol incremental scheduling, multi-year backfill manifests, and exchange/security reconciliation breadth for OTC/listed variants.",
        source_urls=(
            "https://developer.finra.org/docs",
            "https://developer.finra.org/catalog",
            "https://developer.finra.org/products/query-api",
        ),
    ),
    ProviderParityRow(
        provider="Nasdaq Trader",
        provider_domain="Listed and other-listed symbol directory",
        warehouse_domain="security_master",
        reference_tables=("nasdaqlisted.txt", "otherlisted.txt", "TradingSystemAddsDeletes.txt"),
        institutional_grain="Directory snapshot and add/delete event rows by symbol, listing venue, effective date, and as-of date.",
        institutional_keys=("directory", "symbol", "as_of_date", "effective_date", "event_id"),
        pit_fields=("as_of_date", "effective_date", "source_file_created_at", "source_loaded_at"),
        factors_or_fields=("security name", "market category", "ETF/test flags", "round lot", "listing venue fields", "add/delete actions"),
        open_substitute="Direct Nasdaq Trader text files loaded into nasdaq_symbol_directory, nasdaq_listing_events, exchange_listings, and listing_status_intervals.",
        warehouse_tables=("nasdaq_symbol_directory", "nasdaq_listing_events", "listing_status_intervals", "exchange_listings", "securities"),
        parity_status="partial",
        limitations="Current Nasdaq files are not a complete CRSP-style history by themselves; long history requires repeated checkpoints or archival source evidence.",
        next_gap="Backfill longer historical snapshots and reconcile snapshot/event precedence rules.",
        source_urls=(
            "https://www.nasdaqtrader.com/trader.aspx?id=symboldirdefs",
            "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt",
        ),
    ),
    ProviderParityRow(
        provider="FRED and ALFRED",
        provider_domain="Macro observations and vintages",
        warehouse_domain="macro",
        reference_tables=("FRED series metadata", "FRED observations", "ALFRED/vintage real-time periods"),
        institutional_grain="Series observation rows by observation date and real-time/vintage date.",
        institutional_keys=("series_id", "observation_date", "realtime_start", "realtime_end", "vintage_date"),
        pit_fields=("observation_date", "as_of_date/realtime_start", "available_at", "last_updated"),
        factors_or_fields=("rates", "inflation", "labor", "volatility proxies", "vintage revisions"),
        open_substitute="FRED graph CSV/API latest-revision observations in macro_observations; schema leaves as_of_date for vintages.",
        warehouse_tables=("macro_series", "macro_observations", "v_macro_latest"),
        parity_status="partial",
        limitations="Current graph CSV loader is latest-revision; true macro revision PIT requires ALFRED/FRED API vintages.",
        next_gap="Add API-key-aware vintage loader using realtime_start/realtime_end or vintage_dates.",
        source_urls=(
            "https://fred.stlouisfed.org/docs/api/fred/series.html",
            "https://fred.stlouisfed.org/docs/api/fred/series_observations.html",
            "https://fred.stlouisfed.org/docs/api/fred/realtime_period.html",
        ),
    ),
    ProviderParityRow(
        provider="WorldQuant BRAIN",
        provider_domain="Alpha expression research workflow",
        warehouse_domain="feature_store",
        reference_tables=("BRAIN datasets", "operators", "alpha simulation workflow"),
        institutional_grain="Dataset field, operator, alpha expression, simulation, and performance-result objects.",
        institutional_keys=("dataset/field id", "operator", "alpha expression", "simulation id", "universe/date settings"),
        pit_fields=("simulation settings date range", "data availability", "build manifest timestamp"),
        factors_or_fields=("alpha features", "operators", "neutralization/grouping metadata", "simulation metrics"),
        open_substitute="feature definitions, values, feature-set catalog, dependency edges, build manifests, alpha expression catalog, PIT signal values, backtest manifests, quality checks, and lake manifests.",
        warehouse_tables=(
            "feature_definitions",
            "feature_values",
            "feature_set_catalog",
            "feature_dependency_edges",
            "feature_build_manifests",
            "alpha_expression_catalog",
            "alpha_signal_values",
            "alpha_backtest_manifests",
            "data_quality_checks",
            "lake_export_files",
        ),
        parity_status="partial",
        limitations="BRAIN platform data fields, operators, and simulation service are proprietary or access-controlled.",
        next_gap="Add richer operator parsing, neutralization/grouping metadata, and simulation result drill-down tables.",
        source_urls=(
            "https://www.worldquant.com/brain/",
            "https://worldquantbrain.com/consultant",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR / OMB / BLS",
        provider_domain="Industry classification (SIC, GICS, NAICS, ICB)",
        warehouse_domain="reference_classifications",
        reference_tables=("SEC SIC codes", "BLS NAICS 2022 hierarchy", "Fama-French 12/48/49 industry groupings"),
        institutional_grain="Taxonomy hierarchy rows (division, sector, group, industry) and PIT entity-level classification rows.",
        institutional_keys=("taxonomy_id", "node_code", "security_id", "valid_from", "valid_to"),
        pit_fields=("valid_from", "valid_to", "as_of_date", "available_at", "source_loaded_at"),
        factors_or_fields=("SIC division/group/industry", "Fama-French 12 industry", "NAICS 2-digit sector", "primary vs. derived classification flag"),
        open_substitute=(
            "SEC EDGAR submissions SIC codes loaded into taxonomy + taxonomy_node hierarchy and PIT entity_classification rows; "
            "Fama-French 12 SIC-range crosswalk embedded in warehouse ETL; "
            "NAICS 2022 20-sector spine seeded statically from BLS/Census public files."
        ),
        warehouse_tables=(
            "taxonomy",
            "taxonomy_node",
            "entity_classification",
            "taxonomy_mapping",
        ),
        parity_status="partial",
        limitations="GICS (MSCI/S&P) and ICB (FTSE) are licensed taxonomies not available as open data; 6-digit NAICS crosswalk and Fama-French 48/49 groupings not yet seeded.",
        next_gap="GICS/ICB licensing; full 6-digit NAICS crosswalk; FF48/49",
        source_urls=(
            "https://www.sec.gov/info/edgar/siccodes.htm",
            "https://www.census.gov/naics/",
            "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/det_12_ind_port.html",
        ),
    ),
    # ── S2: estimates ─────────────────────────────────────────────────────────
    ProviderParityRow(
        provider="FactSet Estimates / IBES / Zacks",
        provider_domain="Reported actuals and earnings surprise",
        warehouse_domain="estimates_actuals",
        reference_tables=(
            "FactSet Estimates API reported actuals",
            "IBES actuals file",
            "Refinitiv actuals",
            "Zacks actuals feed",
        ),
        institutional_grain="Security fiscal-period reported actuals with PIT availability timestamps.",
        institutional_keys=("security_id", "measure_code", "fiscal_year", "fiscal_period", "accession_number"),
        pit_fields=("available_at", "announce_date", "source_loaded_at"),
        factors_or_fields=("EPS diluted/basic", "revenue", "net income", "operating income", "SUE signal"),
        open_substitute=(
            "SEC XBRL companyfacts (sec_company_facts) mapped to measure codes via est_measure, "
            "loaded into est_actual with PIT available_at carried from filing acceptance; "
            "est_surprise computes Standardized Unexpected Earnings (SUE) via the "
            "seasonal-random-walk-with-drift model (Foster-Olsen-Shevlin 1984), "
            "the documented basis of the Post-Earnings Announcement Drift (PEAD) factor."
        ),
        warehouse_tables=("est_measure", "est_actual", "est_surprise"),
        parity_status="implemented",
        limitations=(
            "Open XBRL covers US-GAAP concepts only; no non-GAAP adjustments, "
            "no broker estimate revisions, no street-consensus actuals."
        ),
        next_gap="Add TTM/LTM actuals; enrich XBRL unit normalization for non-USD reporters.",
        source_urls=(
            "https://developer.factset.com/api-catalog/factset-estimates-api",
            "https://www.sec.gov/search-filings/edgar-application-programming-interfaces",
            "https://data.sec.gov/api/xbrl/companyfacts/",
        ),
    ),
    ProviderParityRow(
        provider="FactSet Estimates / IBES / Refinitiv / Zacks",
        provider_domain="Consensus estimates",
        warehouse_domain="estimates_consensus",
        reference_tables=(
            "FactSet Estimates consensus endpoint",
            "IBES consensus summary file",
            "Refinitiv consensus file",
            "Zacks consensus feed",
        ),
        institutional_grain="Security-measure-period consensus rows with mean/median/stdev/count by as-of date.",
        institutional_keys=("security_id", "measure_code", "fiscal_year", "fiscal_period", "consensus_date"),
        pit_fields=("consensus_date", "available_at", "source_loaded_at"),
        factors_or_fields=("mean estimate", "median estimate", "high/low estimate", "stdev", "num_estimates", "revisions"),
        open_substitute="est_consensus table with injectable provider connector; default-empty.",
        warehouse_tables=("est_consensus", "est_detail", "est_broker", "est_analyst"),
        parity_status="partial",
        limitations=(
            "licensed: IBES/FactSet Estimates/Zacks; schema + injectable loader; no open-data source"
        ),
        next_gap="Wire licensed IBES/FactSet Estimates feed via injectable provider.",
        source_urls=(
            "https://developer.factset.com/api-catalog/factset-estimates-api",
            "https://www.lseg.com/en/data-analytics/financial-data/estimates",
        ),
    ),
    ProviderParityRow(
        provider="FactSet Estimates / IBES / Refinitiv / Zacks",
        provider_domain="Individual broker estimates and recommendations",
        warehouse_domain="estimates_detail",
        reference_tables=(
            "FactSet Estimates detail endpoint",
            "IBES detail file",
            "Zacks detail feed",
        ),
        institutional_grain="Broker-analyst-measure-period estimate rows and recommendation change events.",
        institutional_keys=("security_id", "broker_id", "analyst_id", "measure_code", "fiscal_year", "fiscal_period", "estimate_date"),
        pit_fields=("estimate_date", "rating_date", "available_at", "source_loaded_at"),
        factors_or_fields=("individual estimate", "broker rating", "rating direction", "price target"),
        open_substitute="est_detail, est_recommendation, est_broker, est_analyst tables with injectable provider connectors; default-empty.",
        warehouse_tables=("est_detail", "est_broker", "est_analyst", "est_recommendation"),
        parity_status="partial",
        limitations=(
            "licensed: IBES/FactSet Estimates/Zacks; schema + injectable loader; no open-data source"
        ),
        next_gap="Wire licensed IBES/FactSet Estimates detail feed via injectable provider.",
        source_urls=(
            "https://developer.factset.com/api-catalog/factset-estimates-api",
        ),
    ),
    ProviderParityRow(
        provider="SEC EDGAR / FactSet Estimates",
        provider_domain="Management guidance",
        warehouse_domain="estimates_guidance",
        reference_tables=(
            "SEC 8-K Item 2.02 and 7.01 filings",
            "FactSet Estimates guidance endpoint",
        ),
        institutional_grain="Security-measure-period management guidance ranges (low/high/mid) by guidance date.",
        institutional_keys=("security_id", "measure_code", "fiscal_year", "fiscal_period", "guidance_date"),
        pit_fields=("guidance_date", "available_at", "source_loaded_at"),
        factors_or_fields=("guidance low/high/mid", "GAAP vs non-GAAP basis", "form", "accession_number"),
        open_substitute="est_guidance table with injectable connector; default-empty. Real extraction requires SEC 8-K Item 2.02/7.01 free-text NER (documented TODO).",
        warehouse_tables=("est_guidance",),
        parity_status="partial",
        limitations="SEC 8-K Item 2.02/7.01 free-text extraction; injectable connector",
        next_gap="Implement SEC 8-K NER pipeline to extract guidance ranges from Item 2.02/7.01 text.",
        source_urls=(
            "https://www.sec.gov/cgi-bin/browse-edgar?action=getcompany&type=8-K&dateb=&owner=include&count=40",
            "https://developer.factset.com/api-catalog/factset-estimates-api",
        ),
    ),
    ProviderParityRow(
        provider="Renaissance Technologies",
        provider_domain="Quant research data and compute infrastructure",
        warehouse_domain="operations",
        reference_tables=("public firm descriptions of data-driven quantitative research infrastructure",),
        institutional_grain="Research datasets, feature builds, compute jobs, model inputs, and operational lineage.",
        institutional_keys=("dataset", "feature set", "run id", "input hash", "artifact manifest", "quality check"),
        pit_fields=("available_at", "source_loaded_at", "run timestamps", "manifest exported_at"),
        factors_or_fields=("broad data ingestion", "statistical feature generation", "governance", "research operations"),
        open_substitute="DuckDB warehouse, Parquet lake, ETL job manager, watermarks, feature/alpha manifests, catalogs, and QA checks.",
        warehouse_tables=(
            "dataset_catalog",
            "table_catalog",
            "field_catalog",
            "etl_job_runs",
            "feature_set_catalog",
            "feature_dependency_edges",
            "feature_build_manifests",
            "alpha_expression_catalog",
            "alpha_signal_values",
            "alpha_backtest_manifests",
            "lake_export_files",
            "data_quality_checks",
        ),
        parity_status="research_only",
        limitations="Renaissance internals are proprietary; public pages only support broad infrastructure targets.",
        next_gap="Add artifact dependency graphs beyond features, plus distributed execution records and richer backtest result history.",
        source_urls=(
            "https://www.rentec.com/Home.action?about=true",
            "https://www.rentec.com/Home.action?index=true",
        ),
    ),
)


def _json_tuple(values: tuple[str, ...]) -> str:
    return json.dumps(list(values), sort_keys=True)


def seed_provider_parity_matrix(store: DuckDBStore) -> None:
    """Seed auditable open-data parity targets from public provider documentation."""

    for row in PROVIDER_PARITY_ROWS:
        store.con.execute(
            """
            INSERT OR REPLACE INTO provider_parity_matrix (
                provider,
                provider_domain,
                warehouse_domain,
                reference_tables_json,
                institutional_grain,
                institutional_keys_json,
                pit_fields_json,
                factors_or_fields_json,
                open_substitute,
                warehouse_tables_json,
                parity_status,
                limitations,
                next_gap,
                source_urls_json,
                updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now())
            """,
            [
                row.provider,
                row.provider_domain,
                row.warehouse_domain,
                _json_tuple(row.reference_tables),
                row.institutional_grain,
                _json_tuple(row.institutional_keys),
                _json_tuple(row.pit_fields),
                _json_tuple(row.factors_or_fields),
                row.open_substitute,
                _json_tuple(row.warehouse_tables),
                row.parity_status,
                row.limitations,
                row.next_gap,
                _json_tuple(row.source_urls),
            ],
        )
