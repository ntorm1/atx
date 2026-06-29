# ats-eqt Parity Gap Matrix

**Purpose:** Table-by-table, domain-by-domain gap analysis driving the multi-sprint build toward FactSet / S&P-Compustat / Refinitiv parity on equity fundamentals + ownership + supply-chain.
**Target (blueprint):** `C:\atx\archive\research\` — `schemas/data_models_and_methodology.md` (consolidated DDL), `schemas/cross_vendor_field_map.md` (137 cross-industry + 37 overlay canonical fundamentals fields), `datasets/*.md` (per-dataset DDL), `sources/public_data_sources.md` (44+ upstream sources).
**Current (built):** `C:\atx\atx-impl\db\schema.py` (60 `CREATE TABLE`, 9 views), loader modules in `db/`, parity ledger seed `db/parity.py` (14 provider rows).
**Author:** read-only analysis, 2026-06-28. No code modified.

Fill-level legend: **Built** = table exists + loader + non-trivial rows; **Partial** = table exists but missing key columns/sub-tables or thin coverage; **Stub** = schema present, ~0 rows / no real loader; **Missing** = no table at all.

---

## 1. Executive Summary

| # | Domain | Target tables (n) | Built | Partial | Missing | Current loader(s) | Sprint |
|---|--------|------:|-------|---------|---------|-------------------|:------:|
| 1 | Fundamentals (XBRL/us-gaap) | ~14 | sec_company_facts, fundamental_points, fundamental_statement_points, fundamental_ttm_points, fundamental_periods, fundamental_fact_revisions, xbrl_* + xbrl_validation_results | S4a/S4b statement map, rdq/four-date period fields, and SQL calc-linkbase validation are built; full Arelle/DQC sidecar and vendor reconciliation remain | full DQC 150+ execution, cross-vendor `fact_disagreement`, richer as-reported press-release extraction | fundamentals.py, fundamental_statements.py, xbrl_taxonomy.py, xbrl_filing_contexts.py, xbrl_validation.py | 8 (polish) |
| 2 | Estimates (IBES/consensus) | ~11 | est_measure, est_actual, est_surprise, est_broker, est_broker_alias, est_analyst, est_analyst_alias, est_period_dim, est_detail, est_consensus, est_guidance, est_recommendation, est_recommendation_summary, est_security_link | SEC-derived actuals/SUE, local SEC 8-K guidance text extraction, injectable detail/consensus/recommendation landing surfaces, and PIT-safe vendor-security-id links | default DB has no licensed broker-detail/consensus/recommendation rows, accepted estimate crosswalks, or loaded SEC guidance corpus | build_estimates.py, estimates.py, estimate_security_links.py | 2 (extend) |
| 3 | Ownership-13F | 5 | thirteenf_managers, thirteenf_manager_reports, thirteenf_security_positions, thirteenf_security_ownership, filer_13f_cik_alias | S6a filer CIK-alias / entity-resolution spine built (SELF + name-history + same-name candidates + injectable continuity seed); S6b 2023-01-03 value-unit cutover normalized; FIGI mostly null; auto cross-CIK rollups stay advisory | curated subadvisor/M&A continuity seed, N-PORT cross-link | thirteenf.py, filer_alias.py, ownership.py | 6 (extend) |
| 4 | Insider ownership (3/4/5, 13D/G) | ~15 | — | — | **ALL**: insider, insider_relationship, insider_transaction, filing_form4, blockholder_filing, blockholder_reporting_person, fund, fund_class, filing_nport, fund_holding, form144_intent, tradingplan_10b5_1, proxy_vote, congressional_disclosure | none | **3** |
| 5 | Corporate actions | ~10 | corporate_actions (142 rows), corp_action_type_dim, adjustment_factor_history, delist_code_dim, delisting_events, delisting_return_observations | div/split factors plus public delisting evidence; injectable DLRET-like surface exists but default DB has no observed terminal returns | populated observed terminal returns, CRSP DLSTCD reconciliation, name_history, ticker_history, spinoff_basis_allocation, offering, trading_halt | corporate_actions.py, adjustment_factors.py, delisting.py | 5 |
| 6 | Pricing / market data | 6 | equity_daily_bars (9.8k rows), shares_outstanding_history (342 rows), adjustment_factor_history, daily_adjustment_factors, delisting_events, delisting_return_observations | OHLCV plus PIT SEC XBRL share counts; event-level/daily split-total-return factors; public delisting proxy rows; injectable observed-return table | quote_eod, bar_intraday, 3-close semantics, populated observed dlret, float/treasury shares, full spinoff/merger factor policy | ticker_history.py, shares_outstanding.py, adjustment_factors.py, daily_adjustments.py, delisting.py | 5 |
| 7 | ESG / sustainability | 5 | — | — | **ALL**: esg_metric_dim, esg_metric, esg_score, esg_controversy, ghg_emission | none | 7 |
| 8 | Reference classifications | ~8 | taxonomy (3), taxonomy_node (125), taxonomy_mapping (256), entity_classification | S1 SIC/Fama-French/NAICS taxonomy trees + crosswalks populated offline on the live DB; entity_classification now offline-injectable via CIK→SIC CSV (S8a) | populated entity_classification for the full universe (needs SIC source), isic/nace dims, RBICS-style multi-tag | reference_classifications.py | **2** |
| 9 | Off-exchange / short-interest | ~6 | finra_short_interest (66k rows), offexchange_venue, offexchange_volume, offexchange_security_period | short interest populated; S7a ATS/OTC transparency built as injectable surface (default empty) | live FINRA OTC API connector, daily short-volume flow, borrow-fee | finra.py, short_interest_features.py, offexchange.py | 6 |
| 10 | Supply-chain + Macro | sc_node/sc_edge + macro (~5) | macro_series, macro_observations (40k rows) | macro built; supply-chain absent | sc_node, sc_edge, sc_node_observation, customs/CBP-AMS, Exhibit-21 subsidiary graph, GLEIF/LEI loader | macro.py | 9 (last) |

**One-line read:** Fundamentals, 13F, FINRA short interest, macro, reference classifications, insider/blockholder landing tables, and the estimates schema are real. Corp-actions/pricing now include adjustment and delisting evidence surfaces. Remaining large gaps are populated licensed/public estimate feeds, ESG, deeper off-exchange data, and the strategic supply-chain wedge.

---

## 2. Per-Domain Detail

### Domain 1 — Fundamentals (XBRL / us-gaap)  ·  Status: BUILT (polish remaining)

**Target tables** (`datasets/fundamentals_us_equities.md`, `data_models_and_methodology.md` Part A/G.2):
- `fund_fact` / long-form EAV — `(entity, security, period_id, item_id, value)` bitemporal core.
- `fund_item` — canonical item dictionary (us-gaap tag → ats-eqt item_id, parent, calc_formula).
- `fund_period` — period dim with the four-date PIT model: `datadate`, `rdq` (8-K Item 2.02 release), `fdate` (10-K/Q filed), `ldate` (last vintage).
- `xbrl_concept_map` — us-gaap/IFRS concept → canonical metric, with extension classification.
- `entity.industry_template` — gating field (INDL vs FS) routing bank/insurance/REIT/Nareit-FFO overlays.

**Current state — mostly BUILT:**
- `sec_company_facts` (companyfacts.json facts) — `fundamentals.py`, ~12k+ facts.
- `fundamental_points` (12,318) / `fundamental_statement_points` (12,315) / `fundamental_ttm_points` (7,377) / `fundamental_periods` (3,206) / `fundamental_fact_revisions` (12,318) — bitemporal restatement chain with `as_of_date`/`available_at`. This already implements the "as-first-reported → restated" two-tier + revision audit (`is_latest_revision`, `previous_value`, `value_delta`).
- `fundamental_statement_map` S4a cross-industry seed (137 authorized item_ids; multiple rows where concepts coalesce) = the `xbrl_concept_map` equivalent — backed by `fundamental_statements.py` seed.
- Full XBRL substrate: `xbrl_concept_catalog`, `xbrl_taxonomy_packages/roles/relationships`, `xbrl_dimension_edges`, `xbrl_fact_frames` (1,204), `xbrl_filing_contexts` (6,878), `xbrl_filing_dimensions` (14k), `xbrl_filing_facts` (25k) — `xbrl_taxonomy.py`, `xbrl_filing_contexts.py`.

**Gap:**
- `fundamental_statement_map` S4a scope is exactly **137** authorized cross-industry item_ids from `cross_vendor_field_map.md` sections 2.1-2.4; S4b adds 37 bank/insurance/REIT overlay item_ids from sections 2.5-2.7.
- No **industry-template overlay**: zero bank (`tdsa`, `nim`, `pln`, `alll`), insurance (`losres`, `pncia`), or REIT/Nareit-FFO concepts. `securities.asset_class` exists but no `industry_template` routing.
- Four-date PIT is implicit (`as_of_date` + `available_at`) but `rdq` (press-release date from 8-K Item 2.02) is **not separately captured** — needed for contemporaneous earnings-reaction studies.
- **Missing quality checks:** calculation-linkbase summation validation, DQC 150+ rules, cross-vendor reconciliation table (`fact_disagreement`).
- **Connectors present** (SEC companyfacts.json, Frames, taxonomy packages). **Missing:** DERA Financial Statement Data Sets bulk (SUB/NUM/TAG/PRE) for pre-API backfill; Sharadar SF1 / SimFin as reconciliation baselines.

**S4 implementation update:** S4a/S4b/S4c/S4d now cover exact 137 cross-industry item_ids, 37 bank/insurance/REIT overlays, `fundamental_periods.datadate/rdq/pdate/fdate/ldate`, and SQL calculation-linkbase validation rows in `xbrl_validation_results`. Remaining fundamentals gaps are full Arelle/DQC 150+ rule execution, cross-vendor `fact_disagreement`, and richer EX-99 press-release actual extraction.

**S9a implementation update — calculated items (Compustat/FactSet ratio parity):** `fundamental_ratios` (migration `0032`, `db/fundamental_ratios.py`) is the derived "calculated items" surface — a long-format PIT fact (one row per `security_id,ratio_code,basis,period_end`) computed from the bitemporal TTM flows (`fundamental_ttm_points`) over the instant balances (`fundamental_statement_points`). v1 ships 15 ratios across profitability (net/operating margin, ROA, ROE), leverage (assets/equity, liabilities/assets, liabilities/equity), cash-flow (free_cash_flow level, fcf_margin, ocf/net_income, capex/revenue), payout (dividend payout, buyback/NI, total payout), and per-share (book_value_per_share). Each ratio carries its **own** `available_at` = max availability of its specific inputs (true per-fact PIT), a `numerator_value`/`denominator_value` lineage pair so `value` is reproducible, and an `is_meaningful` flag (false when a denominator is non-positive, e.g. ROE on negative equity). As-of reader `fundamental_ratios_asof` + `query_asof --view fundamental-ratios`; depends on `sec_company_facts` in the DAG. **S9b** expands coverage to **23 ratios** (no schema change, same engine), adding an `efficiency`/activity family (`asset_turnover`, `equity_turnover`) plus cash-flow-coverage and reinvestment ratios (`operating_return_on_assets`, `operating_cash_flow_margin`, `operating_cash_flow_to_assets`, `operating_cash_flow_to_liabilities`, `capex_to_operating_cash_flow`, `retention_ratio`) — all computable from the existing 15 canonical metrics, so they materialize real rows immediately. **S9c** adds a `growth` family (5 YoY ratios — `revenue_growth_yoy`, `net_income_growth_yoy`, `operating_income_growth_yoy`, `operating_cash_flow_growth_yoy`, `assets_growth_yoy`) computed by a pure-pandas prior-year pairing (`_attach_prior_year`, 350–380-day window) over the same TTM/instant inputs; `ratio_kind='growth'` (value = (current − prior)/|prior|, flagged not-meaningful off a non-positive base). **S9d** reuses the prior-year pairing to add average-balance returns (`average_return_on_assets`, `average_return_on_equity`, `operating_cash_flow_to_average_assets` — denominator = mean of ending and prior-year balance, the Compustat return-on-capital convention) plus `equity_growth_yoy`. Live: 7,728 rows / **32 ratio codes** over 5 securities (AAPL TTM @2026-03-28 net_profit_margin=0.2715, ROE=1.151, average_return_on_equity=1.4147, asset_turnover=1.2166, revenue_growth_yoy=0.1276, free_cash_flow=129.174B). **Gap / next:** v1 stores only the latest-revision input vintage (one ratio per period) — a restatement-vintage ratio history is the planned non-breaking refinement (bitemporal columns already present); price-based valuation multiples (P/E, P/B, EV/EBITDA) are blocked on the live DB because the cached `equity_daily_bars` sample (2012–2013) does not temporally overlap the fundamentals (2017–2026); current/quick/cash/coverage/gross-margin/inventory-turnover ratios and EV/EBITDA await current-asset/current-liability/COGS/inventory/debt/interest concept coverage (the cached `sec_company_facts` carries only the 16 fetched concepts — widening needs a SEC companyfacts re-fetch with an expanded `DEFAULT_CONCEPTS`, out of the test path); average-balance ROA/ROE needs a prior-period-balance loader join.

### Domain 2 — Estimates (analyst / IBES / consensus)  ·  Status: PARTIAL

**Target tables** (`datasets/estimates.md` §9, `data_models_and_methodology.md` Part B):
- `est_broker` + `est_broker_alias` — estimator registry with vendor-ID vintage tracking (IBES reshuffle).
- `est_analyst` + `est_analyst_alias` — analyst registry, masked/named, vintage dates.
- `est_measure_dim` — EPS/SALES/EBITDA/CFPS/DPS/BPS/TGT/REC canonical codes.
- `est_period_dim` — FY/FQ/CY/LT/NT with FPI encoding (`fpi_ibes` 0..9/A/B/Y), `fpedats` anchor, expected/confirmed release dates.
- `est_detail` (a.k.a. `est_fact`) — long-format triple `(broker_id, analyst_id, measure_id, fpi, fpedats, value)` + `anndats`/`actdats`/`revdats` + `basis` (P/D) + `is_gaap` + `estimate_type` + bitemporal.
- `est_consensus` — mean/median/high/low/stdev/N + revision counts, snapshotted at `statpers`/`consensus_date`.
- `est_actual` — as-reported actuals per period.
- `est_recommendation` — 1=StrongBuy..5=Sell (canonical IBES scale) + price target.
- `est_guidance` — 8-K Item 2.02/7.01 NER-extracted company guidance (no us-gaap XBRL element exists).

**Current state — PARTIAL BUILT:**
- `est_measure`, `est_actual`, and `est_surprise` are real: SEC companyfacts actuals map into canonical estimate measures and SUE is computed PIT-safely from originally reported actuals.
- `est_detail` now has an injectable CSV loader for IBES-like or normalized detail rows, with `anndats`/`actdats`/`revdats`/`stop_date`, source-file hashes, broker/analyst alias dimensions, `est_period_dim`, watermarks, lake exports, quality checks, and `est_detail_asof`.
- `est_consensus` now has an injectable CSV loader for IBES `statsumu`-style or normalized summary rows, with `statpers`/`fpedats` aliases, stable snapshot ids, source-file hashes, `est_period_dim` updates, 105-day stale windows, watermarks, quality checks, and latest-snapshot `est_consensus_asof`.
- `est_recommendation` now has an injectable CSV loader for IBES `recddet`/`ptgdet`-style or normalized recommendation and price-target rows, with canonical IBES rating scale, broker/analyst aliases, source-file hashes, active-window as-of semantics, watermarks, and quality checks.
- `est_recommendation_summary` now has an injectable CSV loader for IBES `recdsum`/`ptgsum`-style or normalized recommendation-distribution and price-target summary snapshots, with canonical IBES-direction rating means, provider scale-direction metadata, source-file hashes, watermarks, quality checks, and `est_recommendation_summary_asof`.
- `est_security_link` now reconciles estimate vendor security identifiers to warehouse `security_id` using PIT-visible `security_identifier_history` and accepted `identifier_resolution_decisions`, records accepted/conflict candidate evidence, promotes accepted vendor-id links back into identifier history when enabled, and lets `est_detail_asof`, `est_consensus_asof`, `est_recommendation_asof`, and `est_recommendation_summary_asof` resolve `security_id` only after the link's own `available_at`.
- `est_guidance` now supports both injectable normalized rows and local SEC 8-K Item 2.02/7.01 text corpora. The deterministic extractor captures conservative revenue/EPS/operating-income/net-income ranges or point values, records `guidance_type`, unit/currency scale, `source_item`, extraction confidence, evidence text, source-file hashes, and PIT `available_at` inherited from acceptance time or local row metadata.

**Gap:**
- Populated licensed/public estimate feeds, accepted estimate crosswalk rows, and SEC guidance text corpora are not in the default DB. Full parity still needs real IBES/FactSet/CIQ/Zacks detail/summary/recommendation/price-target files, a scaled 8-K exhibit backfill, and parser extensions for open-ended guidance, non-calendar fiscal years, and more measures.
- **Connectors:** primary public path is SEC EDGAR 8-K Item 2.02 (results) + 7.01 (Reg-FD guidance) prose extraction; secondary public aggregators (Zacks/Yahoo/Nasdaq — license-murky, sourced from IBES). Vendor truth (IBES via WRDS `ibes.detu_epsus`/`statsumu_epsus`/`recddet`, FactSet Estimates API, Bloomberg BEst, CIQ `ciqEstimateNumericData`) is licensed.
- **Quality gotchas to encode:** IBES broker-ID reshuffle (2018: 13.8% brokers / 30.7% analysts reassigned — never join on numeric broker_id across vintages); `revdats`-bumped FPI gotcha (filter `anndats ≤ D ≤ revdats`, join on `fpedats` not `fpi`); **Bloomberg recommendation scale inverts IBES** (5=Buy vs 1=Buy); P-vs-D EPS mixing (~5–8% silent error); stopped-estimate filter (`estimate_type != 'S'`); 105-day stale scrub on consensus.

### Domain 3 — Ownership-13F  ·  Status: BUILT (entity-resolution extension remaining)

**Target tables** (`datasets/13f_holdings.md`): `filer_13f`, `filer_13f_cik_alias`, `filing_13f`, `holding_13f`.

**Current state — BUILT:**
- `thirteenf_managers` (10,672) ← `filer_13f`; `thirteenf_manager_reports` (11,761) ← `filing_13f`; `thirteenf_security_positions` (10,564) ← `holding_13f`; `thirteenf_security_ownership` (54) = security-side QoQ rollup (a bonus aggregate). Loaders: `thirteenf.py`, `ownership.py`.
- Schema already handles the hard parts: `put_call`, `investment_discretion`, `voting_auth_sole/shared/none`, `share_quantity_type`, `is_amendment`/`amendment_type`, `is_confidential_omitted`, `portfolio_weight`, `available_at` bitemporal.

**Gap:**
- **`filer_13f_cik_alias` BUILT (S6a)** — bitemporal CIK alias / entity-resolution spine in `filer_alias.py`: SELF identity per filer CIK, intra-CIK `NAME_HISTORY`, low-confidence cross-CIK `NAME_MATCH_CANDIDATE` links, and an injectable curated `SUBADVISOR`/`MA_CONTINUITY`/`MANUAL` seed. `resolve_primary_cik` defaults to authoritative-only (confidence ≥ 1.0) so same-name candidates never silently merge. Live build: 10,672 SELF + 99 candidate rows (e.g. Dimensional Fund Advisors across 3 CIKs, D.E. Shaw, Citigroup). Remaining: load a curated continuity seed for true subadvisor/M&A rollups; auto-derived cross-CIK links stay advisory.
- **2023-01-03 value-unit cutover RESOLVED (S6b)** — `thirteenf.value_unit_multiplier` / `apply_value_unit_cutover` scale `value_usd` to whole dollars at holdings load (×1000 for `period_of_report ≤ 2022-12-31`, via an accession→period map). No live change (all loaded periods are post-2023, factor = 1); the fix protects future pre-2023 backfills from a 1000× SUM understatement. Regression-tested in `test_thirteenf_value_cutover.py`.
- `figi` column exists but is largely null — CUSIP→FIGI OpenFIGI resolution incomplete (`identifier_resolution_*` only has 1 row).
- **Missing:** N-PORT cross-link (`fund.adviser_cik → filer_13f.primary_cik`) — depends on Domain 4; Form SHO 13f-2 short table (defer to ~2027, first filings 2028-02-14).

### Domain 4 — Insider ownership (Form 3/4/5, 13D/G, N-PORT, 144)  ·  Status: MISSING

**Target tables** (`datasets/insider_ownership.md` Part I):
- `insider`, `insider_relationship` — person dim + (insider×issuer×role) bitemporal.
- `insider_transaction` — Form 4/5 fact with the **28-letter `transaction_code`** (P=highest-signal buy; S split by `rule_10b5_1_indicator`; F/M cashless-exercise netting; J/K footnote-NER), `is_derivative`, `acquired_disposed`, `shares_owned_following`, `direct_indirect`.
- `filing_form4` — metadata + `footnotes` JSON (load-bearing for 10b5-1, family-trust, vesting).
- `blockholder_filing` + `blockholder_reporting_person` — Schedule 13D/G, group-filing flag, `type_of_reporting_person` (BD/BK/IC/IA/...), voting/dispositive power, `purpose_text` (Item 4 activist intent).
- `fund`, `fund_class`, `filing_nport`, `fund_holding` — N-PORT fund-level holdings (units NS/PA/NC/OU, asset_cat, payoff_profile, securities-lending).
- `form144_intent` + `form144_to_form4_link` — intent-vs-executed reconciliation.
- `tradingplan_10b5_1` — plan registry + `cooling_off_compliant`.
- `proxy_vote` (N-PX), `congressional_disclosure` (STOCK Act PTR).

**Current state:** None. `sec_submissions` (505 rows) captures filing metadata generically but no Form-4 parsing.

**Gap (entire domain):**
- **Connectors:** SEC EDGAR Ownership XML (Form 3/4/5 mandatory XML since 2003, ~250k/yr; spec v5.1); Schedule 13D/G **structured XML mandate 2024-12-18** (pre-date needs HTML/ASCII parser — two pipelines); N-PORT CSV bulk (`sec.gov/data-research/.../form-n-port-data-sets`); Form 144 XML (mandatory 2023-04-13); N-PX XML (2024-08-31); STOCK Act via House Clerk / Senate Ethics + Senate/House Stock Watcher JSON. Reuse: `edgartools`.
- **Quality gotchas:** transaction_code enumeration is the load-bearing field; 13D deadline compressed to 5 business days (2024-02-05); 10b5-1 cover-page indicator from 2023-04-01 + 90/120-day cooling-off; N-PORT monthly-public timing is a moving target (delayed to 2027-11-17 / 2028-05-18, Feb-2026 proposal may scale back); CUSIP→FIGI on 13D/G.

### Domain 5 — Corporate actions  ·  Status: PARTIAL

**Target tables** (`datasets/corporate_actions.md`): `corp_action`, `corp_action_type_dim`, `adjustment_factor`, `delisting`, `delist_code_dim`, `name_history`, `ticker_history`, `spinoff_basis_allocation`, `offering`, `trading_halt`.

**Current state — PARTIAL:**
- `corporate_actions` (142 rows) — single flat table: `action_type`, `ex_date`, `declaration_date`, `record_date`, `payable_date`, `cash_amount`, `split_from`/`split_to`, `adjustment_factor`. Loader `corporate_actions.py` (also writes `tbltickerhistory_daily`).
- `security_identifier_history` + `exchange_listings` partially serve `name_history`/`ticker_history` (bitemporal `valid_from`/`valid_to`).
- `delist_code_dim` + `delisting_events` materialize PIT public delisting evidence from `listing_status_intervals`, with nullable `delisting_return`, explicit `is_return_imputed`, `return_policy`, and source-status lineage.
- `delisting_return_observations` is an injectable observed-return surface for CRSP-like `DLRET`/`DLRETX`/`DLAMT`/`DLPRC` rows; `delisting_events_asof` enriches events only with observations visible at the query timestamp.

**Gap:**
- `delist_code_dim` is a public proxy dimension, not a licensed CRSP DLSTCD map; `delisting_events.delisting_return` is null by default unless an explicit imputation policy is requested.
- No populated observed **`dlret`** terminal-return evidence in the default DB; Shumway-Warther-style -30% unresolved-delete imputation exists only as an opt-in research policy, not as a claimed source value.
- No **`spinoff_basis_allocation`** (Form 8937 / IRC §358), no `offering` (IPO/secondary), no `trading_halt`.
- **Connectors:** present — inferred from local archive. Missing: CRSP `dsedist`/`dsedelist` enumerations (academic gold), DTCC CA 20022, EDGAR Form 8937 (only public spinoff-basis path), Form 25/15 for delistings.

### Domain 6 — Pricing / market data  ·  Status: PARTIAL

**Target tables** (`datasets/pricing_market_data.md`): `bar_daily`, `bar_intraday`, `quote_eod`, `shares_outstanding_history`, `short_interest`, `adjustment_factor_history`.

**Current state — PARTIAL:**
- `equity_daily_bars` (9,834) ← `bar_daily`: `open/high/low/close/adjusted_close/volume/vwap`, `dividend_amount`, `split_factor`, `is_adjusted`, `available_at`. Loader `ticker_history.py`. View `v_equity_daily_returns`.
- `shares_outstanding_history` (342) ← public SEC XBRL `fundamental_statement_points`: `shares_outstanding`, `shares_basic_avg`, `shares_diluted_avg` with `effective_date`, `as_of_date`, `available_at`, accession, revision metadata, as-of reader, watermarks, and quality checks. Loader `shares_outstanding.py`.
- `adjustment_factor_history` + `daily_adjustment_factors` derive event-level and daily PIT split/total-return factors from normalized `corporate_actions`, with classifier reasons, as-of readers, watermarks, quality checks, and lake export coverage.
- `delisting_events` carries public delisting evidence and optional imputation-policy metadata; `delisting_return_observations` can inject observed terminal-return rows, but the default DB has none loaded.
- `short_interest` need served by `finra_short_interest` (Domain 9).

**Gap:**
- **Single source `adjusted_close` remains on `equity_daily_bars`** — split-only and split+dividend total-return close now live in `daily_adjustment_factors`, and `daily_panel_asof` exposes those explicit adjusted-close and adjusted-return semantics. The base `v_equity_daily_returns` view remains raw-close-only for backward compatibility.
- No **`quote_eod`** (bid/ask/spread, close_type A/T/B), no three-close model (official auction vs consolidated tape vs composite).
- **Share-count limits:** `shares_outstanding_history` now covers public XBRL current/basic/diluted share counts, but it is not yet a CRSP daily `SHROUT` equivalent and does not cover float, treasury shares, exchange-sourced daily shares, or split-factor-integrated daily market-cap restatements.
- No **`bar_intraday`**; corporate-action factors are still local/public-evidence derived and do not yet include full spinoff, merger, or observed delisting-return policies.
- **Connectors:** present — local archive only. Missing: CRSP DSF/MSF (delisting-correct), Polygon/Tiingo/Alpaca (modern forward bars), Compustat `co_secd`. PIT integrity: free APIs restate adj_close without snapshot retention — must compute own from raw events.

### Domain 7 — ESG / sustainability  ·  Status: MISSING

**Target tables** (`datasets/esg_sustainability.md`): `esg_metric_dim`, `esg_metric`, `esg_score`, `esg_controversy`, `ghg_emission`.

**Current state:** None.

**Gap (entire domain):**
- `esg_metric_dim` (~1,500 metrics; ESRS/SASB/GRI/TCFD/PAI/GHG-Protocol crosswalk with `pillar` E/S/G).
- `esg_metric` (long-format entity×metric×period, polymorphic value, `source_type` CSRD_ESRS/SEC_10K/CDP/CA_SB253/VENDOR, assurance flags, bitemporal).
- `esg_score` (vendor scores MSCI_IVA/SUSTAINALYTICS_RISK/etc., **`redistributable=FALSE`** — never computed by ats-eqt).
- `ghg_emission` (Scope 1/2/3, per-gas, scope-2 location/market, scope-3 category 1–15, `gwp_basis` AR5/AR6, assurance).
- `esg_controversy` (event-shaped, severity 1–5, monetary impact).
- **Connectors:** SEC 10-K Item 1 (human capital) / Item 1A (climate risk) / Exhibit 21 / Form SD (conflict minerals) / DEF 14A; EU CSRD/ESRS iXBRL (1,144 datapoints, `filings.xbrl.org`); SFDR 18 PAI; CDP; GHG Protocol; California SB 253 (first Scope 1+2 deadline 2026-08-10 = operative US anchor); UK Gender Pay Gap CSV.
- **Gotcha:** vendor ESG ratings are functionally uncorrelated (0.38–0.71) — wedge is **raw disclosed metrics + bitemporal evidence trail**, not another score. SEC Climate Rule is federally dead (pivot to CA SB 253 + 10-K NLP).

### Domain 8 — Reference classifications (GICS/ICB/NAICS/SIC)  ·  Status: BUILT (taxonomy populated; entity coverage pending SIC source)

**S1/S8a update:** `taxonomy`/`taxonomy_node`/`taxonomy_mapping` are built and now populated offline on the live DB (3 taxonomies — SIC, FAMA_FRENCH_12, NAICS_2022; 125 nodes; 256 SIC↔NAICS / SIC-range↔FF12 crosswalk rows). `entity_classification` is built with PIT primary-SIC + derived FF12/NAICS rows and is now populatable **without network** via two injectable offline sources — a CIK→SIC CSV (`sic_file` / `--sic-file`, S8a) and the SEC bulk `submissions.zip` with lazy per-CIK lookup (`submissions_zip` / `--submissions-zip`, S8b) — in addition to the live SEC submissions fetcher. The SEC bulk archive is a one-time operator download (never hit in tests); fixtures cover both offline paths. Default-DB entity_classification remains empty until one of those SIC sources is supplied for the universe. Remaining gaps below.


**Target tables** (`datasets/reference_classifications.md`): `taxonomy`, `taxonomy_node`, `entity_classification`, `taxonomy_mapping`, `sic_code_dim`, `naics_code_dim`, `isic_code_dim`, `nace_code_dim`.

**Current state:** None. SIC codes ride along in EDGAR/XBRL filings but are not extracted into a dim table; `sec_company_tickers` is CIK↔ticker only.

**Gap (entire domain):**
- `taxonomy` (registry with `licensed_flag`/`redistribution_ok`), `taxonomy_node` (hierarchical, bitemporal), `entity_classification` (many-to-many, **`weight` column for RBICS-style multi-tag**, `primary_flag`, source/confidence), `taxonomy_mapping` (many-to-many crosswalks, `mapping_strength`).
- `sic_code_dim` keyed **`(sic_code, authority)`** — SEC fork (8888 Foreign Govts, 9995 Nonclassifiable, 6770 Blank-Check SPAC) vs BLS-1987; CRSP↔Compustat disagree ~36%.
- `naics_code_dim` (public domain, `(naics_code, vintage)`), `isic_code_dim`, `nace_code_dim`.
- **Connectors (all free/redistributable):** NAICS (Census), SIC (BLS-1987 + SEC fork, from EDGAR assigned-sic), ISIC (UN), NACE (Eurostat), Fama-French 12/17/30/48/49 (Tuck/Ken French, SIC-derived), **TRBC via PermID API (the one open index-grade hook)**, Wikidata P452. Closed (alias-only, ACL-gated): GICS, ICB, BICS, RBICS.
- **Gotcha:** all crosswalks many-to-many (GICS-163 → NAICS-6: ~6.5 fan-out); taxonomy revision risk (GICS 2016 RE carve-out, 2018 Comm Services) demands bitemporal storage.

### Domain 9 — Off-exchange / short-interest  ·  Status: PARTIAL

**Target tables** (`datasets/off_exchange_transparency.md` + pricing `short_interest`): `offexchange_volume`, `offexchange_venue`, `offexchange_security_period`, `offexchange_quality_report`, `offexchange_security_period_v`; plus `short_interest`.

**Current state — PARTIAL (short-interest + injectable ATS/OTC transparency):**
- `finra_short_interest` (66,266 rows) + `finra_short_interest_backfill_manifests` (2). Loaders `finra.py`, `short_interest_features.py`. Strong: bi-monthly settle-date short interest with feature pipeline.
- **S7a FINRA OTC Transparency BUILT (injectable):** `offexchange_venue` (MPID dim, ATS vs non-ATS), `offexchange_volume` (per security×venue×period fact with bitemporal availability + FINRA restatement handling via `is_latest`/`restatement_seq`), and `offexchange_security_period` (derived ATS-share rollup with `ats_share_pct`, `ats_venue_count`, `restatement_detected`) plus `v_offexchange_security_period`. Loader `db/offexchange.py` ingests a FINRA `weeklySummary`-shaped or normalized CSV; materializer computes the rollup. End-to-end smoke reproduces the blueprint AAPL fixture exactly (total 66,925,813; ATS 21,246,912; non-ATS 45,678,901; ats_share_pct 31.7470%; 2 ATS venues). Default DB tables are empty until a FINRA file is injected.

**Gap:**
- **Live FINRA connector not yet wired** — the FINRA OTC Transparency API (`POST /data/group/otcMarket/name/{weeklySummary|monthlySummary|blockSummary|weeklySummaryHistoric}`, OAuth2 client-credentials, 10 GB/mo cap, Tier-1 ~14d / Tier-2 ~28d delay) is the production source; ingestion is currently injectable-CSV only (no network in tests). Daily short-volume flow (FINRA) for high-freq proxy and a dedicated `offexchange_quality_report` table remain; per-run QA currently rides `data_quality_checks`. Borrow-fee/utilization is commercial (S3/Markit) — skip.
- **Gotcha:** restatement semantics (NEW vs AMEND vs METADATA_AMEND) — handled via `is_latest`/`restatement_seq`/`finra_last_update_date`; manager-level shorts (Form SHO 13f-2) not until 2028-02-14.

### Domain 10 — Supply-chain + Macro  ·  Status: MACRO BUILT / SUPPLY-CHAIN MISSING

**Target tables** (`data_models_and_methodology.md` Part C/G.4 + `sources/public_data_sources.md`): `sc_node`, `sc_edge`, `sc_node_observation` (entity-res audit); macro via `macro_series`/`macro_observations`.

**Current state:**
- Macro **BUILT**: `macro_series` (6), `macro_observations` (40,589) with `as_of_date`/`available_at` vintaging. Loader `macro.py` (FRED/ALFRED).
- Supply-chain **MISSING**: no `sc_node`/`sc_edge`.

**Gap:**
- `sc_node` (company/subsidiary/plant/port/vessel/mine/material/product/person + geo) and `sc_edge` (supplies_to/customer_of/ships_to/owns, bitemporal, `disclosure_type` D/R/I, `tier`, `confidence`, `share_of_revenue`, `share_of_cogs`, `weight_usd`). `sc_path` as a view, not stored.
- **Connectors (the strategic wedge — vendors overlap only ~43%):** EDGAR Exhibit 21 (subsidiary graph, NLP), 10-K Item 1 customer-concentration (formulaic NER), US CBP AMS bills-of-lading, US Census Intl Trade API (free, HS-10), UN Comtrade v2 (free), Eurostat Comext, OECD TiVA, AIS (NOAA Marine Cadastre, public domain), GLEIF LEI Golden Copy (free, redistributable), OpenFIGI (MIT), Wikidata (CC0). Avoid OpenCorporates (£12k+/yr) as the public spine.
- **Macro gap (minor):** add World Bank (CC-BY) + IMF SDMX alongside FRED.
- **Gotcha:** HS code is NLP-inferred (not in public AMS) — better imputation is a v1 differentiator; keep disclosed edges separate from inferred shipment edges.

---

## 3. Prioritized Sprint Backlog

Ordering rationale: maximize (parity impact × inverse effort). Estimates and reference-classifications are high-impact / low-effort (pure SEC-EDGAR prose + free public taxonomies, no licensing). Insider is high-impact / medium-effort (clean Form-4 XML). Corp-actions and pricing depth are foundational for any return study. ESG and off-exchange are medium. Supply-chain is the highest-effort wedge and goes last. Fundamentals concept-coverage polish is interleaved.

| Sprint | Domain | Headline gap | One-line acceptance criterion |
|:------:|--------|--------------|-------------------------------|
| **1** | Estimates | Populate and extend injectable estimates store | Real licensed/manual estimate detail and consensus files load into `est_detail`/`est_consensus`; recommendation/price-target normalization is covered; accepted vendor crosswalk rows populate `est_security_link` with PIT-safe as-of resolution; 8-K Item 2.02/7.01 NER populates `est_guidance` for >=500 issuers; row counts appear in `lake/est_*/_manifest.json`. |
| **2** | Reference classifications | No taxonomy tables; SIC/NAICS unextracted | `taxonomy`/`taxonomy_node`/`entity_classification`/`sic_code_dim`(authority-keyed)/`naics_code_dim` exist; SEC-assigned SIC + NAICS + Fama-French + TRBC-via-PermID loaded for the full security universe with bitemporal `valid_from`. |
| **3** | Insider ownership | No Form 3/4/5 or 13D/G parsing | `insider`/`insider_transaction`/`filing_form4` populated from EDGAR Ownership XML with full 28-letter `transaction_code` + `rule_10b5_1_indicator`; `blockholder_filing` handles post-2024-12-18 XML; ≥1 quarter of Form-4 backfill loaded. |
| **4** | Fundamentals (concept coverage) | S4a cross-industry dictionary corrected to 137 item_ids; S4b overlays seeded | `fundamental_statement_map` covers exactly the 137 authorized cross-industry item_ids from sections 2.1-2.4 plus 37 bank/insurance/REIT-FFO overlay item_ids from sections 2.5-2.7; `rdq` (8-K 2.02 date) captured; calc-linkbase + DQC validation writes to `data_quality_checks`. |
| **5** | Corp actions + Pricing depth | Public delisting proxy plus injectable observed-return surface exists; no populated DLRET | Load real CRSP/Form 25/15/OTC terminal-return evidence into `delisting_return_observations`, reconcile CRSP DLSTCD, then add quote/close-type depth and spinoff `Form 8937` loader stub. |
| **6** | 13F extension + Off-exchange ATS | No filer entity-resolution; no ATS volume | `filer_13f_cik_alias` (subadvisor/M&A rollup) populated; 2023-01-03 ×1000 cutover verified; `offexchange_volume`/`offexchange_venue` loaded from FINRA weeklySummary API with ATS % rollup. |
| **7** | ESG / sustainability | No ESG store | `esg_metric_dim`/`esg_metric`/`ghg_emission`/`esg_score`(redistributable=FALSE)/`esg_controversy` exist; 10-K Item 1/1A + Form SD + CA SB 253 + CDP loaders populate raw disclosed metrics with bitemporal evidence trail. |
| **8** | Fundamentals PIT/QA hardening | No cross-vendor reconciliation | `fact_disagreement` audit table compares XBRL vs Sharadar/SimFin; DERA bulk backfill (SUB/NUM/TAG/PRE) extends pre-API history; >99% agreement target tracked in `data_quality_checks`. |
| **9** | Supply-chain graph | `sc_node`/`sc_edge` absent (strategic wedge) | `sc_node`/`sc_edge` exist; Exhibit-21 subsidiary graph + 10-K Item 1 customer-concentration loaded with `disclosure_type`/`confidence`; CBP-AMS or Census-trade ingest produces tier-1 inferred edges keyed on GLEIF/FIGI. |

**Parity-ledger upkeep (cross-cutting):** every sprint above must add/flip the matching `db/parity.py` `ProviderParityRow` from `partial`/absent to reflect new `warehouse_tables` — currently the ledger claims partial parity for estimates (via `fundamentals_features`) and corp-actions that the schema does not yet substantiate.
