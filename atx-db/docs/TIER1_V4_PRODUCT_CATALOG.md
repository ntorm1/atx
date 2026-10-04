# Tier1 v4 product catalog

Engineering checkpoint 2026-10-04. Full Task17 is **INCOMPLETE**. No actual catalog build, actual stage hash, source/metadata/HTTP job or return evaluation was run by this implementer. The following families are registered through the existing stagelake registry and strict reviewed-contract catalog/verify entrypoints; publication, original source/economic gates and coverage remain independent requirements. Historical reported evidence below is not a refreshed measurement.

Every family targets 2020-2025 coverage on `univ_all_common`, `member_equity` and `univ_r3000p`. Actual refreshed numerator/denominator and published history span are **UNMEASURED/PENDING** in this checkpoint; lookback targets (fundamentals2015+, prices2018+, text/ownership warmup2019+) are requirements, not measured source history. No percentage or PASS is inferred from a registry or NULL exclusion.

| Coverage year | univ_all_common | member_equity | univ_r3000p |
|---|---|---|---|
| 2020 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |
| 2021 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |
| 2022 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |
| 2023 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |
| 2024 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |
| 2025 | UNMEASURED/PENDING | UNMEASURED/PENDING | UNMEASURED/PENDING |

## Security master and identifiers

Stage: `identity/link_table_v3, security_master, share_exchange_history`. Keys: security_id; dated identifier/issuer intervals. Cadence: event and daily. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original listing/identifier availability and dated valid_from/valid_to. Commercial equivalent: CCM, CIQ identifier history, FactSet Symbology.

Known limits: Dated exchange/share-class, FIGI/LEI and link qualification remain original gates.

## Prices and total returns

Stage: `prices, prices_history`. Keys: security_id, session_date. Cadence: daily. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: source-qualified price/action clocks; predictor construction differs from evaluation. Commercial equivalent: CRSP DSF, FactSet prices.

Known limits: Vendor vintage and return/action identity remain independently qualified; no post2022 return evaluation.

## Delisting returns

Stage: `delisting_v2`. Keys: security_id, last_session, candidate_available_at. Cadence: event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original last-traded-session and exact cash/stock consideration availability. Commercial equivalent: CRSP DLRET.

Known limits: Exact proceeds/source gates and complete2267 M&A denominator remain required; imputation is never exact admission.

## Shares, market cap and adjustments

Stage: `market_shares, corporate_actions`. Keys: security_id, session_date/event. Cadence: daily/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: every dated issuer-class constituent visible at formation. Commercial equivalent: CRSP SHROUT/CFACPR, Compustat CSHO.

Known limits: Actual shares, money units, splits and full issuer class membership remain source gates.

## PIT Q/A/TTM fundamentals and vintages

Stage: `fundamentals_release_v1/<generation>/fundamentals_v11`. Keys: cik, accession; item/period vintage. Cadence: filing/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original accepted clock, amendment/restatement/4.02 vintages. Commercial equivalent: Compustat Snapshot/PIT, FactSet FF.

Known limits: Full85 native extract/144 item protocol preserved; no financial unit independently admitted in this checkpoint.

## Standardized fundq and funda

Stage: `compustat_std_v11/<generation>`. Keys: cik, datadate, accession, available_at. Cadence: filing/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: exact source-backed finalfund leaf and original fiscal/calendar/FX clocks. Commercial equivalent: Compustat funda/fundq.

Known limits: All32 native buckets and native gates replay; original30 source handchecks remain FAIL_PENDING.

## Financial ratios

Stage: `ratios_v11/<generation>`. Keys: security_id, session_date; native state cik, available_at. Cadence: monthly and conditional daily. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: native monthend22UTC or prior-session22UTC cells; actual visible finalfund state. Commercial equivalent: WRDS WFRS, FactSet FF ratios.

Known limits: Missing EPS share-basis/indicated dividend/sector primitives remain explicit native NULL reasons and failed gates.

## Earnings calendar and announcement actuals

Stage: `earnings_calendar preserved; earnings_calendar_v2, ea_actuals immutable phase generations`. Keys: cik, accession; later lag/reaction security_id. Cadence: event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original 8K2.02/6K acceptance; lag at later10Q; reaction at third-session close. Commercial equivalent: Compustat RDQ; announcement time-series SUE.

Known limits: Parsed phase cannot serve feature views. Full native parser/cache/calendar/features/audit replay; manual precision remains FAIL_unreviewed.

## Estimates and guidance

Stage: `events_guidance_v2`. Keys: cik, accession, candidate_id, event_kind. Cadence: event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: exact release clock and dated fiscal/accounting/share basis. Commercial equivalent: Free guidance substitute; no I/B/E/S consensus equivalent.

Known limits: Original300 precision population and native fiscal/share/basis/actual resolution gates remain required; no licensed consensus asserted.

## Corporate actions, distributions and buybacks

Stage: `corporate_actions, distributions, buyback, mna_v2`. Keys: native event_id/accession/source_id. Cadence: event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: qualified declaration/acceptance and complete exchange calendar; no guessed early clock. Commercial equivalent: CRSP actions, FactSet corporate actions.

Known limits: Task12/13 source contracts, dividend cash checks and200 buyback handchecks remain required.

## Short side

Stage: `short_interest, short_volume_ext, ftd, regsho_threshold, borrow_proxy`. Keys: security_id/session; report/settlement event. Cadence: daily/report. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: source publication/settlement clocks and original conservative floors. Commercial equivalent: FactSet SI; partial free lending proxy.

Known limits: Fee/utilization remains licensed; no source completeness inferred from feature fill.

## 13F institutional ownership

Stage: `thirteenf_quality, thirteenf_cohorts, thirteenf_native, thirteenf_features`. Keys: accession, position_index; report quarter/CIK/security vintages. Cadence: quarterly filing/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: actual public acceptance, amendments and dated CUSIP/share-class maps. Commercial equivalent: FactSet Ownership.

Known limits: Expected public accessions -> retained bronze -> parsed totals -> PIT links -> gold must all reconcile; current incomplete cohorts are not known zero.

## Insider transactions

Stage: `insider, insider_ext`. Keys: issuer_cik, month; transaction accession/line. Cadence: event/monthly. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original filing acceptance and source-qualified price sanity. Commercial equivalent: FactSet Insiders.

Known limits: Bad Form4 prices, amended lines and unexplained value totals remain original gates.

## 13D/G stakes and activism

Stage: `existing thirteenf/ownership event products`. Keys: accession, holder/issuer event. Cadence: event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: actual acceptance; later amendments are new vintages. Commercial equivalent: FactSet stakes/activism.

Known limits: Existing parse/name/activism failures remain visible; Task17 does not repair foreign ingestion.

## N-PORT fund ownership and flows

Stage: `nport, nport_vintages`. Keys: fund_key, report_date, available_at. Cadence: monthly/quarterly filing. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: original public filing availability; correct fund-series/class and amendment chain. Commercial equivalent: FactSet fund ownership/flows.

Known limits: Retained originals, flow cash arithmetic and economic denominator remain unqualified until native admission.

## Indexes, universe and liquidity

Stage: `indexes, universe, market_index, market_liquidity`. Keys: security_id, session_date; index universe/weighting/period. Cadence: daily/formation. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: formation flags from exact completed input snapshots; previous-session issuer clocks. Commercial equivalent: CRSP index/universe and market liquidity.

Known limits: Interrupted or changed partitions fail; provisional GC3 evidence must refresh against completed universe.

## Characteristic library and replication

Stage: `char_monthly, char_replication`. Keys: security_id, monthend; characteristic and holding month. Cadence: monthly. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: strict monthend22UTC with original lookbacks and finalfund pins. Commercial equivalent: JKP/OSAP style characteristics.

Known limits: All original153, full overlap/pair inventory and source gates remain required; no source gap solved by NULL exclusions.

## Factors and risk inputs

Stage: `factors, risk_inputs`. Keys: freq/session; security_id, session_date. Cadence: daily/monthly. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: independent RF, original construction-asof/PIT input contract. Commercial equivalent: Fama-French/HXZ style factors and risk predictors.

Known limits: Historical predictor construction allowed; all correlation/IC/portfolio evidence wholly pre2023 and complete-window guarded.

## Segments and footnotes

Stage: `fundamentals_notes seven products`. Keys: original adsh/tag/context/period keys; segment feature cik, period vintage. Cadence: filing/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: exact original filing/source SHA/context and bridge provenance. Commercial equivalent: Compustat Segments and notes.

Known limits: R12 findingsI4-I7 and invalid namespace/version item keys remain unresolved; no arbitrary dedup or synthetic key.

## Bank, insurer and REIT formats

Stage: `fundamental standardized/native primitive templates`. Keys: cik, accession, datadate vintage. Cadence: filing/event. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: native source clock, units, structural applicability. Commercial equivalent: Compustat Bank/FS, FactSet FF.

Known limits: Missing specialized primitives/FFO/FFIEC/Y9C remain gaps; structural NULL is not source completeness.

## Text, filing NLP and TNIC

Stage: `text_native/text/features; classification_tnic/{pairs,firms,years}`. Keys: cik/accession; year/cik/peer_cik. Cadence: filing and annual corpus vintage. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: max original filing/section, current/prior full corpus, historical SIC and completeness clocks. Commercial equivalent: FactSet filing NLP; reproduced TNIC-style.

Known limits: R24 original prior-year DF/SIC3 strict method required; nearest-five/all-word positive cosine is not qualification or official TNIC.

## Options implied measures

Stage: `preserved options/free IV source adapter`. Keys: security_id, observation/availability. Cadence: daily. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: native observation availability and conservative lag. Commercial equivalent: Partial OptionMetrics style free IV.

Known limits: Skew/volume/OI licensed or missing; source vintage proof remains required.

## Data quality and operational references

Stage: `metrics, lake verify, catalog.duckdb proof`. Keys: stage/output manifest pin; source observation identity. Cadence: per complete quiescent publication. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: actual full output SHA/size and selected partition inventory before/after. Commercial equivalent: Vendor QA/reference reconciliation.

Known limits: Integrity is separate from economic/source quality. Every missing stage, changed partition, skipped view, invalid note key fails strict verification.

## Golden research panels and delivery

Stage: `golden_panel, golden_labels, golden_evaluation, golden_export`. Keys: session_date, security_id; horizon/candidate/endpoint/bundle/field. Cadence: daily/monthly and conditional evaluation. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: all source vintages strictly before exact exchange-session formation; labels separate. Commercial equivalent: Vendor-agnostic research delivery.

Known limits: All5/21/63 labels/evaluation must use full admitted return/action/cash/event evidence wholly pre2023; actual products and source coverage pending.

## Macro, rates, FX and reference series

Stage: `reference`. Keys: series/currency, observation_date, available_at. Cadence: daily/observation. History: actual independently admitted published span pending; coverage2020-2025 remains unmeasured in all three universes.

PIT rule: actual release/observation availability and original FX currency provenance. Commercial equivalent: FactSet macro/rates/FX.

Known limits: Missing RF/FX values never borrowed from future releases or asserted money units.

## Source-to-gold completeness

| Chain | Independent/source coverage disposition |
|---|---|
| Company Facts extraction -> native financial -> standardized -> ratios -> characteristics/gold | Root independently admitted all85 extracts. Native financial/standardized/ratio/characteristic/gold actual admission and original gates remain pending. Extraction admission is not financial-unit admission. |
| 13F expected public accessions -> retained bronze -> parsed holdings -> dated links -> gold | Complete-cohort expected accession inventory, no unexplained missing bronze, SH/PRN/put/call units, manager/amendment accounting, totals, linkage and full3-population coverage remain REQUIRED/FAIL_PENDING. |
| Profitability/growth primitives -> native formulas -> gold | Complete matrix retained from char_monthly.requirements.json in tier1_source_to_gold_completeness.json. Unsupported/missing source primitives remain explicit; no NULL exemption constitutes completion. |
| Notes original filing -> exact proof/context -> segment fields -> gold | Only independently pinned nine curated reportable identity proofs may be considered; five automatic cuts/external selections withheld. External-sales/geographic basis needs separate independent admission. |
| Text original sections -> full corpus/SIC clocks -> strict reproduced TNIC -> gold | Full native original-byte reader and R24 method required; actual plan/original source admissions and yearly gates remain pending. |

R12 remains **0/2 segment gates**:1934 revenue-bearing misses,1235 missing-revenue filings,35194 unclassified annual filings. The483 positive disclosures are a nonprobability subset. I4 denominator/revenue completeness, I5 multiline list/partition false certification, I6 competing external-revenue headings and I7 incompatible context/bridge switching remain unresolved. Whole long-item integrity is **FAIL36 groups/72 rows**,21 unequal-value groups and six ambiguous `tax_dtl` choices; goodwill duplicates agree. Raw parser quality labels do not override independently admitted proof inventory. Strict catalog verification recomputes collisions using the advertised original keys and fails rather than inventing a new key or dropping rows.

## Existing catalog and verify interfaces

See [native reader contract](datasets/tier1_native_readers.md) and [catalog contract](datasets/tier1_catalog.md). `registry.load()` now includes all static family stubs. The existing `python -m atx_db.stagelake catalog --tier1-contract ...` uses full actual-byte snapshot admission, all fixed native readers, immutable generation/phase resolution and before/after checks. `python -m atx_db.stagelake verify --tier1-contract ... --catalog ...` verifies actual catalog bytes, source snapshot, registry views/macros and full native readers. Legacy exploratory mode cannot satisfy Task17.

Catalog integrity does not certify source economics. The strict actual catalog remains blocked until required publications/source admissions exist and notes key integrity is resolved or explicitly dispositioned without unconditional whole-stage certification. Root alone owns guarded actual execution after independent review. Previous complete generations and failed attempts remain retained. Generic legacy producers can still replace multiple partition files before their manifest; R9 quiescent actual byte checks detect that limitation rather than granting publication atomicity.
