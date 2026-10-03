# Historical issuer class status

`issuer_classes/` adds issuer-bound REIT observations from original periodic disclosures. Dated financial SIC selects a broad inspection cohort and never assigns REIT status. The fixed cohort contains all 285 admitted NYSE financial issuers at the 60 benchmark month ends from 2018 through 2022; its complete real-estate SIC subset contains 41 issuers. Missing SIC and other industries remain an explicitly unassessed population.

Original annual disclosures are selected for the financial cohort in 2021/2022, and also before first eligibility and in 2018–2020 for the real-estate subset. A separately seeded deterministic sample examines plausible classification, foreign, duplicate-class, recent-issuer and link risks; overlap between its strata is disclosed. Original pre-sample annual filings are preferred, with original corporate disclosure fallbacks where no annual exists. This audit is diagnostic and is not a population precision estimate or a fitted count screen.

A positive event requires a direct current issuer assertion or qualifying tax-election statement. Named issuer assertions are bound to the matching original header company. Generic Company/we assertions additionally require a single matching document registrant CIK or an explicit named Company definition. Subsidiary-only, conditional, prospective, investment-adviser, conflicting and nonasserting text stays unknown. Evidence spans and original raw hashes are preserved. Explicit current termination is a false event and can override stale inherited REIT/SIC evidence. Absence of a statement never creates a false event.

Availability follows the original header's conservative acceptance/filing/change-date clock defined in [issuer_history.md](issuer_history.md). Universe chooses the latest event strictly before the cutoff; unknown assessments emit no status assertion and preserve the existing fallback. Neither source state nor later status is applied backward. Sparse source years do not establish uninterrupted status or absence of unobserved conversions.

| Output | Fields and purpose |
| --- | --- |
| reit_status.parquet | cik, accession, accepted_at, available_at, report_period_end, reit_status, primary_url, primary_sha256, header_sha256, evidence |
| incorporation.parquet | Verified issuer header incorporation facts with the same contract as issuer_history |
| assessment.json | Every planned observation, including explicit unknown/unfetched/conflicting reasons and evidence |
| plan.json | Fixed cohort, accession selection, missing annual boundaries and source hashes |
| fetch_status.json | Combined attempt count, cap and remaining observations |
| manifest.json | Publish-last code/input/output hashes and measured source coverage |

The two new source stages share the content-addressed raw ledger and persisted attempt budget. At most one HTTP attempt per second and at most 3,000 attempts are allowed this round, including retry and redirect attempts. Existing SEC user agent and host limiter remain mandatory. Acquisition is resumable; loading verifies raw SHA-256 and performs no network requests. SEC EDGAR is public and subject to fair-access rules; periodic filing cadence supplies observations. All real preparation, fetching and loading runs use a guard at no more than 1 GiB. Shared registries and foreign panel files are untouched.

```text
python -m atx_db.alpha_panel.issuer_classes prepare --cohort ../.superpowers/sdd/tier1-v4/t2r2-class-cohort.json --audit ../.superpowers/sdd/tier1-v4/t2r2-independent-audit-sample.json --status-followup
python -m atx_db.alpha_panel.issuer_classes fetch --cap 3000
python -m atx_db.alpha_panel.issuer_classes load
```

Measured status events, unresolved observations, rows/bytes and actual request counts are reported in Task 2 Fix round 2 after acquisition. Independent class evidence is distinct from matching French constituent identities; the aggregate benchmark cannot identify a particular false inclusion by itself. June 2020's vendor gap remains separate. Existing Databento metadata covers a later period and supplies no authorized existing June 2020 replacement; vendor-derived history is not an independent price source. No prices are synthesized or backfilled from neighboring sessions.

Initial cohort preparation omits `--status-followup`; after loading bounded original observations, that flag adds one uniform latest-2022-periodic observation for every positive-source issuer. Reprepare if the verified positive cohort expands, retaining all successful receipts and the same cumulative counter. The final 704-row plan contains the closed 11-issuer follow-up cohort. Commands describe the authorized historical run; additional unplanned acquisition requires its own scope and budget, and the existing counter must never be reset.

Primary acquisition streams bounded response chunks and stores compressed, content-addressed originals. The initial 10 MB ceiling produced oversized receipts; the authorized 64 MB ceiling permits one recorded retry of those receipts, without deleting their history or resetting the combined attempt counter. Successful receipts are reused. Any remaining oversized, truncated, mismatched, conflicting or unreadable observation is unknown. No document tree is materialized during fetching. At least 40 GiB of free C: space is checked before a tranche capable of landing more than 1 GiB.

The status table key is `(cik, accession)`. `cik` is int64; `accession`, source URLs, hashes and JSON `evidence` are strings; `reit_status` is Boolean; `accepted_at` and `available_at` are UTC timestamp microseconds. `report_period_end` is a nullable DATE from the original header's conformed report period, in calendar-date units. It supplies period provenance and never sets availability. Status has no numeric unit; it describes the latest observed issuer assertion at the disclosure's availability, not its entire asserted historical tax period. `accepted_at` preserves original source ordering and `available_at` supplies the conservative as-of boundary. Missing assertions emit no row. The incorporation table uses the identical types, units, source and clock contract as `issuer_history/incorporation.parquet`.

Rule `issuer-bound-original-reit-status-v2` separates incorporation coverage from class-source identity and availability: a missing state literal remains unknown incorporation, while an exact target corporate CIK/form/accession and verified clock can date an independently bound primary class assertion. Conflicting issuer states, mismatched identity and noncorporate filings still reject. The complete positive cohort receives one uniform latest-2022 periodic follow-up; current termination statements emit false at original availability. Conditional or prospective terminations and proposed conversion plans remain unknown. A later disclosure of an earlier tax effective date is never assigned backward.

The acquired class-observation horizon stops in 2022. Full 2018–latest universe output uses the latest observed assertion and existing fallback; this is not certification of uninterrupted REIT status or coverage of 2023–2026 conversions/terminations. Later status refresh is a concrete remaining source obligation. Unknown later filings do not establish that the last known status remained unchanged. This bounded-source limitation is separate from the pre-2023 French count gate and from the dated hand audit at its stated sample dates.

Rule `issuer-bound-original-reit-status-v3` checks the complete punctuation-preserving assertion sentence for conditions on either side, including long preceding assumptions, following `only if` clauses, pending revocations and proposed elections. Semicolons do not erase a condition; common issuer abbreviations and dotted acronyms do not create false sentence boundaries. Conditional positives and terminations remain unknown, emit no event and cause no membership transition. An unrelated condition in a separate sentence does not cancel an unqualified current assertion. Preparation retains all previously selected annual, audit and status-follow-up accessions with the same immutable-identity checks as issuer-history v2; extending any selection path cannot erase an earlier vintage.

A completed current election followed by a separate intention to maintain or continue the already asserted status remains a current assertion. Conditional language anywhere in the full sentence still rejects it. Retained Seritage originals supply regression examples for both completed-election/continuance clauses; this general rule avoids turning a future maintenance intention into a pending initial election.

The final round-2 publication assesses all 704 planned observations: 49 explicit status events for 11 CIKs (48 true, one false), 653 usable incorporation observations, and 655 unknown class observations. All planned header/primary receipts are acquired; 40 missing annual boundaries in accession discovery remain listed in the plan. Unknown includes 650 nonasserting disclosures and five issuer-binding failures; it never means non-REIT. The status availability horizon is 2017-02-22 through 2022-11-15 UTC. All 11 issuers with positive source assertions received the same latest-2022-periodic follow-up selection.

Seritage's November 2022 quarterly disclosure establishes current termination. Although it describes a tax revocation effective earlier in 2022, the first acquired explicit false is available on 2022-11-10 04:59:59 UTC, and is applied only after that clock. Earlier termination availability remains an unfilled source gap; the parser does not assign the asserted tax effective date backward. Complete later-source refresh must cover status changes and explicit unknowns beyond this bounded horizon.

| Availability year | Explicit status observations | Distinct status CIKs | Incorporation observations |
|---|---:|---:|---:|
| 2017 | 3 | 3 | 22 |
| 2018 | 3 | 3 | 39 |
| 2019 | 4 | 4 | 39 |
| 2020 | 9 | 9 | 36 |
| 2021 | 10 | 10 | 256 |
| 2022 | 20 | 11 | 261 |

The status output is 10,324 bytes and incorporation output 49,812 bytes; the latter covers 286 CIKs because original headers from the separately fixed independent audit also contribute. Both outputs have unique keys and zero clocks earlier than original acceptance. Completed planned acquisition is 704 successful primary and matching header receipts; unknown class assertions and missing annual boundaries are retained. The shared raw landing contains 2,159 successful target-header candidates and 704 successful primary URLs, with 4,420,767,098 decompressed source bytes and 285,017,073 bytes on disk. Historical 58 oversized receipts remain preserved; the larger authorized retry succeeded for those URLs. These are receipt/source counts, not independent class accuracy. Final acquisition across both source stages and separate hand receipts totals 2,998 attempts, with minimum recorded spacing 1.00004 seconds.
