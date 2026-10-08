# Dataset `fundamentals_notes`: segments and notes, point in time

Corrected Task 14 Fix round 5 publication (2026-10-03). Code `src/atx_db/alpha_panel/fund_notes.py`,
schema `atx.alpha-panel.fundamentals-notes/v1`, clock `notes-accepted-v1`.
Source: existing SEC DERA Financial Statement and Notes landing, 2019q1-2026_08; bounded primary-disclosure pilot supplies structural evidence, not new numeric facts.

**Current binding gates: 0 PASS / 2 FAIL.** Source-wide numeric revenue proxy misses >=90% in 2020, 2021,
2024, and the all-filer multi-segment disclosure denominator remains uncertified. Verified reconciliation is
81.05%-84.96%, below 95%. Earlier registry-proxy PASS and arithmetic-fitting figures are superseded.

## Outputs

| File | Rows | Bytes |
|---|---:|---:|
| `items/year=2019/items.parquet` | 2,436,878 | 16,676,874 |
| `items/year=2020/items.parquet` | 2,414,313 | 16,450,628 |
| `items/year=2021/items.parquet` | 2,411,732 | 16,482,233 |
| `items/year=2022/items.parquet` | 2,453,801 | 16,689,594 |
| `items/year=2023/items.parquet` | 2,404,029 | 16,251,825 |
| `items/year=2024/items.parquet` | 2,317,184 | 15,575,591 |
| `items/year=2025/items.parquet` | 2,328,449 | 15,593,265 |
| `items/year=2026/items.parquet` | 1,704,459 | 12,071,540 |
| `notes_wide.parquet` | 197,148 | 17,829,282 |
| `segment_census.parquet` | 201,137 | 2,194,723 |
| `segment_features.parquet` | 164,704 | 9,114,529 |
| `segment_recon.parquet` | 46,513 | 1,724,181 |
| `segment_structure.parquet` | 14 | 11,859 |
| `segments.parquet` | 2,484,216 | 23,990,714 |

Parquet total 180,656,838 bytes. Features filed 2019-01-02 through 2026-08-31, available_at 2019-01-02 21:13:00+00:00 through 2026-08-31 21:01:00+00:00, periods 2011-09-30 through 2026-07-31.

Keys: segment_features `(cik,period,adsh)` (vintage `(cik,period)`); segment_recon and segment_census `adsh`;
notes_wide `adsh`; items `(adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg)`;
segments `(adsh,metric,axis_kind,member,tag,ddate,qtrs,segments,uom)`.
`segment_census.parquet` covers every periodic SUB filing, with `cik,adsh,form,period,filed,available_at,n_members,multi_segment`.
Its raw `n_members_source_tagged` count uses all retained numeric tags on the current-period business axis, independent of the feature registry. `n_members` uses a source-corroborated reportable cut for the 14 exact retained filings documented in `segment_structure`; otherwise it remains the tagged proxy. `member_basis` distinguishes these cases.
The advertised item key is not currently unique: 36 existing US-GAAP/IFRS namespace/version collisions remain an explicit integrity failure; six current tax_dtl notes-wide source choices are ambiguous. See the detailed source diagnosis below.
Underlying long-item and notes-wide family definitions are in `docs/ALPHA_PANEL_NOTES.md`.

Additional diagnostic columns (source NUM/DIM and accession clock; same annual staleness as source filing):

| File | Column | Type/unit | Definition |
|---|---|---|---|
| segment_census | n_members | BIGINT/count | distinct current-period reportable-looking business members across all retained numeric tags |
| segment_census | multi_segment | BOOLEAN | n_members >=2; tag-observable proxy, not an independent untagged-disclosure classification |
| segments | context_basis | VARCHAR/provenance | plain or timing_transfer_partition |
| segments | context_provenance | VARCHAR[]/DIM strings | original source contexts behind the value |
| segment_recon | total_basis | VARCHAR/provenance | full_revenue_concept or component_or_extension_unverified |
| segment_recon | candidate_total | DOUBLE/filer currency | nonverified comparison total retained for diagnosis |
| segment_recon | candidate_only | BOOLEAN | candidate arithmetic fit cannot promote verification or alter economic members |
| segment_recon | recon_rule | VARCHAR | disclosed-component correction or candidate fitting rule |
| segment_recon | dropped_members | VARCHAR[]/names | when candidate_only=true, suggested diagnostic exclusions; for source_role_structure, source-verified nonselected hierarchy levels excluded from the economic count |
| segment_recon | seg_sum_reconciled | DOUBLE/filer currency | candidate sum, verified only when candidate_only=false |
| segment_recon | reconciling_used | DOUBLE/filer currency | disclosed component used in arithmetic comparison |
| segment_recon | seg_members | STRUCT(member VARCHAR,value DOUBLE)[] | candidate/verified member set, separated by candidate_only |

## PIT and amendments

Every row carries UTC `available_at`: DST-aware SUB acceptance time, otherwise filed +46 hours.
Daily consumption requires available_at < previous decision session 22:00 UTC.
Month-end coverage requires available_at < that month-end session 22:00 UTC (strict exclusion at equality).
Partial amendments containing consolidated revenue but no segment block inherit the earlier same-period
count/HHI; no singleton is inferred for an amendment. Earlier accession rows remain unchanged.
Original-filing singleton defaults are marked `bseg_basis=inferred_singleton`; this is not certified disclosure.
Annual coverage requires period within 548 days and selects latest period then latest visible vintage.

```sql
CREATE OR REPLACE MACRO segment_features_asof(ts) AS TABLE
 SELECT * FROM read_parquet('<root>/fundamentals_notes/segment_features.parquet')
 WHERE available_at < CAST(ts AS TIMESTAMPTZ)
 QUALIFY row_number() OVER(PARTITION BY cik,period ORDER BY available_at DESC,adsh DESC)=1;
```

## `segment_features` columns

One row per periodic filing (10-K, 10-Q, 10-KT, 10-QT, 20-F, 40-F and amendments) with at least one feature or an amendment state placeholder that can inherit an earlier feature block.
Context: the filing's current period (`ddate = period`), its own flow duration (`qtrs = flow_qtrs`; a 10-Q falls
back to the discrete quarter `qtrs = 1` when it tags no year-to-date segment rows), the filer context (no
co-registrant) and the filer currency (the filing's most frequent monetary unit). Source of every value: SEC DERA
Financial Statement and Notes data set, NUM/DIM rows of the accession. Clock: `available_at` (above). Staleness:
annual blocks describe the fiscal year ending at `period` and stay current until the next 10-K (coverage counts
them for 548 days); flows in a 10-Q are year-to-date.

| name | type | unit | definition |
|---|---|---|---|
| cik | BIGINT | id | SEC CIK of the filer |
| adsh | VARCHAR | id | EDGAR accession |
| form | VARCHAR | code | EDGAR form |
| period | DATE | date | SEC balance-sheet date (month-end rounded) = fiscal period end |
| fy, fp | BIGINT, VARCHAR | label | SEC fiscal year / period label (FY, Q1..Q3, H1, M9) |
| filed | DATE | date | EDGAR filing date |
| available_at | TIMESTAMPTZ | UTC | acceptance clock (PIT rule above) |
| clock_basis | VARCHAR | rule | `accepted_utc` or `filed_plus_46h` |
| currency | VARCHAR | ISO 4217 | filer currency of the money values |
| flow_qtrs | INTEGER | quarters | duration of the filing's own flows (4 annual, 1/2/3 year-to-date) |
| is_annual | BOOLEAN | flag | `flow_qtrs = 4` |
| is_amendment | BOOLEAN | flag | form ends in `/A` |
| countryba | VARCHAR | ISO 3166 | SUB business-address country (blank read as US); the home market of `foreign_sales_share` |
| rev_consolidated | DOUBLE | currency | consolidated revenue of the context: first explicitly recognized full-revenue concept with a non-zero total |
| bseg_n_tagged | BIGINT | count | current-period reportable-looking business members across every numeric tag in the raw notes landing; no arithmetic deletion |
| bseg_basis | VARCHAR | provenance | `current_period_numeric_tags` or `inferred_singleton`; carried blocks retain source basis |
| bseg_n | BIGINT | count | business segments: `bseg_n_tagged`, or inferred 1 for an original annual filing with consolidated revenue and no tagged member; partial amendments inherit earlier blocks |
| bseg_rev_n | BIGINT | count | reportable segments with positive revenue in the context |
| bseg_rev_tag | VARCHAR | tag | revenue tag of the segment rows (source concept, never replaced by an arithmetic candidate) |
| bseg_rev_rule | VARCHAR | rule | disclosed-component rule of verified reconciliation; arithmetic candidates never replace members |
| bseg_rev_hhi | DOUBLE | 0..1 | Herfindahl of segment revenue shares: sum (r_i / sum r)^2 over positive reportable segment revenues (>= 2 needed when >= 2 members are tagged); 1.0 for a single-segment annual filing with positive consolidated revenue |
| bseg_recon_class | VARCHAR | class | annual: the `segment_recon` class (`direct`, `with_reconciling`, `reconciled_search`, or a miss class) |
| geo_rev_n | BIGINT | count | geographic members (`srt:StatementGeographicalAxis`) with revenue in the context |
| foreign_sales_share | DOUBLE | 0..1 | 1 - domestic / consolidated revenue; domestic = member equal to `countryba`, US synonyms for a US filer, or `Domestic`; else the aggregate foreign member (`Foreign`, `International`, ...; `NonUs` / outside-US for a US filer) / consolidated revenue; NULL outside [-1%, 101%], else clipped to [0, 1] |
| foreign_basis | VARCHAR | rule | `domestic_member`, `foreign_member`, `out_of_range` |
| major_cust_n | BIGINT | count | single customers with revenue on `srt:MajorCustomersAxis` (alone or with a customer-concentration `ConcentrationRiskBy*` context; customer classes such as Residential, Commercial, Government-wide aggregates excluded) |
| major_cust_share | DOUBLE | 0..1 | largest single-customer revenue / consolidated revenue, in (0, 1] |
| dd1 .. dd5 | DOUBLE | currency | Compustat DD1-DD5: long-term debt principal due in year 1 .. 5 (`LongTermDebtMaturitiesRepaymentsOfPrincipal*`, fiscal or rolling variant) |
| dd_after5 | DOUBLE | currency | principal due after year 5 |
| dd_rem_fy | DOUBLE | currency | principal due in the remainder of the fiscal year (10-Q ladders) |
| dd_n_buckets | INTEGER | count | tagged buckets among dd1..dd5, dd_after5 |
| dd_sched_total | DOUBLE | currency | sum of the tagged buckets (missing = 0), plus `dd_rem_fy` when `dd1` is absent |
| dd1_share | DOUBLE | 0..1 | dd1 / (dd1 + .. + dd5 + dd_after5) |
| oplease_liab | DOUBLE | currency | operating lease liability (ASC 842), else current + noncurrent |
| oplease_rou | DOUBLE | currency | operating lease right-of-use asset |
| oplease_cost | DOUBLE | currency | operating lease cost of the period (flow, year-to-date in a 10-Q) |
| oplease_rate | DOUBLE | decimal | weighted-average operating lease discount rate |
| finlease_liab | DOUBLE | currency | finance lease liability, else current + noncurrent |
| mrc1 .. mrc5, mrcta | DOUBLE | currency | Compustat MRC1-MRC5 / MRCTA analog: undiscounted operating lease payments due in year 1 .. 5 / thereafter (ASC 842) |
| stkco | DOUBLE | currency | Compustat STKCO: share-based compensation (cash-flow add-back, else expense tags; year-to-date in a 10-Q) |
| sbc_unrecognized | DOUBLE | currency | unrecognized compensation cost of nonvested awards |
| carry_n | INTEGER | count | feature blocks inherited from an earlier vintage of the same (cik, period) |

## Reconciliation and context provenance

Original 10-K/10-KT filing frame starts from all SUB filings, including filings with no registry facts.
Multi-segment denominator uses current-period business-axis members across every numeric tag retained by notes_fetch.
Source-wide tagging is broader than the feature registry, but still cannot identify an untagged textual disclosure.

Verified reconciliation uses source member values and known full-revenue concepts. Industry/component/extension
revenue tags may supply segment measures, but cannot borrow an unrelated consolidated total to claim verification.
`total_basis` describes the concept class, `candidate_total` preserves the old candidate comparison,
`consolidated_revenue` is NULL when equivalence is unverified. Bank gross interest income, insurer premiums,
and lessor income are components, not automatically the company's revenue total; missing net-interest or
investment-income components are not fabricated. Named full-revenue tags are enumerated in manifest verification.

`recon_class_strict` preserves strict arithmetic classes. A correction using a disclosed consolidation component
can be `reconciled_search`. Numerically deleting equal-valued segments, arbitrary subset roll-ups, keyword-member
exclusions and mixed-tag fitting are `candidate_only=true`: diagnostics (`recon_rule,dropped_members,
seg_sum_reconciled,reconciling_used,seg_members`) never alter verified class, count, or HHI.
No numerical coincidence establishes an economic hierarchy.

## Source structure and downstream quality (Fix round 5)

Reportable identity requires a syntactically bounded declared list, exact complete-item aliases, qualifiers/years over all contributing blocks, exact current annual inline contexts and currency definitions, and a sound matching presentation role. Adjacent product prose cannot complete unmapped names. Historical/conditional/forward segment qualifiers refuse certification. Genuine pure multiline lists and conjunctions within complete names remain supported.

The retained 100-case pilot supplies 14 source cuts (nine curated plus five automatic ADES/Evoqua). All original retained bytes were reprocessed, including nine receipt/SHA/role/member equalities and exact citation sets in `t14-fix-r5-reopened-proof.json`. Reportable-count identity alone does not establish the revenue measure's meaning or company-sum reconciliation.

`bseg_rev_measure_basis` (VARCHAR, source/PIT clock and annual staleness as business block) separately states source_declared_external_revenue, source_reported_segment_measure, tagged_unverified_segment_measure or inferred_consolidated_singleton. It carries together with the preceding visible business block. `revenue_measure_basis` is the corresponding recon diagnostic; `revenue_tag_strict` (VARCHAR) preserves the original selected concept. Source-declared external measure selection requires the exact customer/external section, annual member facts and same-section annual company-total context, defined same currency, sound role, exact NUM contexts and unique own total. Gross and customer/external facts remain distinct; no contract component, equity-method earnings or intersegment amount is fitted into company revenue. Source offsets, table rows, member and company context IDs and unit refs travel in proof JSON. HHI uses original source-selected values; partition quality remains reportable identity only.

Signed bridges require the same revenue row, selected context family, annual period and unit. A separate intersegment-sales row cannot be subtracted from external sales. Source-declared reportable Corporate/All Other remains economic; explicitly nonreportable rows may bridge only with source proof. Switching revenue concept cannot carry the original gross-measure bridge.

Current cuts reconcile 11/14; remaining score-window misses are 1,934 revenue-bearing and 1,235 missing revenue. Unknown cases remain parser/landing/source-semantic questions, not intrinsic absences. All output/module hashes and six derived public keys match; quality/PIT provenance checks pass. Whole-stage integrity remains failed:

**Whole-stage public item-key integrity FAIL.** `t14_fix_r5_final_audit.json` exits 1 and `t14-fix-r5-evidence.json` preserves 36 duplicate advertised long-item key groups/72 rows; no narrowed passing audit replaces this result. Of these, 21 groups have different values. Existing extraction deduplicates with `x.version` but projects neither namespace nor version to the advertised `(adsh,tag,is_custom,segments,ddate,qtrs,uom,coreg)` key. The retained NUM rows contain same-local-name US-GAAP and IFRS concepts (2018-2024); exact landing file SHA, taxonomy/version, values and published rows are in `t14-fix-r5-item-collisions.json`. Both extraction and `wide_sql` are unchanged in the `74320900..c7ff21df` diff. Collision families are assets (7 groups), long_lived_assets (5), tax_dtl (21), goodwill (3). 12 current-period tax/goodwill groups win notes tag precedence ({"goodwill": 1, "tax_dtl": 11}); all 6 distinct-value source ties affect `notes_wide.tax_dtl`. The three goodwill keys include one current-period group; all duplicated values agree. Wide ordering also lacks namespace/version. Exact accession, preferred tag, alternative values and published wide value are retained per key. All collision rows are nondimensional, with zero revenue tags and zero current business-axis rows. The computed intersection of affected notes fields with `FEATURE_COLS` is empty; census reads raw current business-axis facts and reconciliation/HHI read revenue. Thus the collision defect affects advertised long-item uniqueness and some tax_dtl wide values, with zero impact on segment features, count, HHI or business gates. Controller owns unchanged-code whole-branch/Task17 namespace triage; this round does not waive or hide it.

Independent text evidence has 665 named spans/483 positive accessions. All old 698 spans/497 accessions were reopened; 423 remain positive, 74 are refused, and 60 additional pure named cases are accepted. This candidate-restricted positive frame leaves 35,194 annual filings unclassified, so the all-filer denominator is uncertified. One optional cached section partition remains corrupt. No new HTTP attempt or landing occurred.

The one final controller-released GC3 refresh completed at `2026-10-03T19:26:17+00:00` against universe manifest SHA256 `90e9ca70cec64838a96b5f7ffc0de88a28e1faf529b48b480c56f4b9d85d0c0e` after R9 release. All nine manifest outputs, including six required 2020-2025 files, matched manifest bytes and SHA before the refresh; full before/after file and manifest hashes remained identical. Final panels member_equity, univ_all_common and univ_r3000p are in `t14-fix-r5-final-coverage.json`. Earlier overlap-era universe measurements are superseded. This is consumer coverage, not Task2 universe parity certification; controller reports its French 10/60 gate still FAIL.

## Gates

Both binding gates remain FAIL in manifest. Revenue's source-tagged proxy threshold result is separate
(`observable_threshold_pass`); certified PASS additionally requires an independent all-filer multi-segment
disclosure denominator, which is unavailable in the retained landing (`denominator_certified=false`).
Sum denominator is the multi-segment filings with observed segment revenue; threshold is >=95% within 1%.
Exact command from atx-db with PYTHONPATH=src and OPENBLAS_NUM_THREADS=1:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r5_build_final_r2.json -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build --years 2025 --defer-universe-coverage
```

| Year | All 10-Ks | Multi-segment numeric | Revenue | Revenue proxy >=90% | Reconciled | Sum >=95% |
|---|---:|---:|---:|---|---:|---|
| 2020 | 5,583 | 2,118 | 1,900 | FAIL 0.8971 | 1,540/1,900 | FAIL 0.8105 |
| 2021 | 5,890 | 2,058 | 1,852 | FAIL 0.8999 | 1,515/1,852 | FAIL 0.8180 |
| 2022 | 6,552 | 2,049 | 1,852 | PASS 0.9039 | 1,528/1,852 | FAIL 0.8251 |
| 2023 | 6,279 | 2,040 | 1,837 | PASS 0.9005 | 1,524/1,837 | FAIL 0.8296 |
| 2024 | 5,813 | 1,981 | 1,762 | FAIL 0.8894 | 1,497/1,762 | FAIL 0.8496 |
| 2025 | 5,560 | 2,006 | 1,814 | PASS 0.9043 | 1,479/1,814 | FAIL 0.8153 |

Proxy observations above never constitute certified all-filer PASS. All diagnostics are value/coverage measurements; no return statistic is computed.

## Measured month-end coverage

member_equity: strict month-end 22:00 UTC cutoff, period within548days; latest visible period/vintage.

| Year | cells | linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---|---|---|---|---|---|---|---|---|---|
| 2020 | 27558 | 0.9816 | 0.8961 | 0.8518 | 0.7874 | 0.3564 | 0.0532 | 0.4263 | 0.6594 | 0.8427 |
| 2021 | 29100 | 0.9825 | 0.8811 | 0.8327 | 0.775 | 0.3439 | 0.0488 | 0.4146 | 0.7717 | 0.8196 |
| 2022 | 27861 | 0.9904 | 0.9021 | 0.8622 | 0.8066 | 0.3613 | 0.0485 | 0.4474 | 0.8125 | 0.8483 |
| 2023 | 27601 | 0.9945 | 0.9117 | 0.8708 | 0.8153 | 0.3648 | 0.0482 | 0.453 | 0.8505 | 0.8614 |
| 2024 | 27284 | 0.9954 | 0.9061 | 0.8652 | 0.806 | 0.3617 | 0.0491 | 0.4567 | 0.8533 | 0.8612 |
| 2025 | 26563 | 0.9945 | 0.9026 | 0.8702 | 0.8064 | 0.3703 | 0.0513 | 0.4475 | 0.8541 | 0.8587 |

univ_all_common: strict month-end 22:00 UTC cutoff, period within548days; latest visible period/vintage.

| Year | cells | linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---|---|---|---|---|---|---|---|---|---|
| 2020 | 39774 | 1.0 | 0.9717 | 0.8669 | 0.794 | 0.325 | 0.0554 | 0.3597 | 0.6587 | 0.9061 |
| 2021 | 44351 | 1.0 | 0.9469 | 0.8276 | 0.7631 | 0.3007 | 0.0528 | 0.348 | 0.7815 | 0.8704 |
| 2022 | 44633 | 1.0 | 0.9669 | 0.8485 | 0.7893 | 0.3107 | 0.0505 | 0.3591 | 0.8054 | 0.8995 |
| 2023 | 42676 | 1.0 | 0.9804 | 0.8661 | 0.8066 | 0.3268 | 0.0512 | 0.3688 | 0.8781 | 0.9215 |
| 2024 | 41991 | 1.0 | 0.9819 | 0.868 | 0.8045 | 0.3286 | 0.0535 | 0.3722 | 0.8958 | 0.9193 |
| 2025 | 41015 | 1.0 | 0.9849 | 0.8951 | 0.8167 | 0.34 | 0.0567 | 0.3757 | 0.905 | 0.9252 |

univ_r3000p: strict month-end 22:00 UTC cutoff, period within548days; latest visible period/vintage.

| Year | cells | linked | annual_row | bseg_n | bseg_rev_hhi | foreign_sales_share | major_cust_share | dd1 | oplease_liab | stkco |
|---|---|---|---|---|---|---|---|---|---|---|
| 2020 | 35889 | 0.9554 | 0.9221 | 0.8446 | 0.7711 | 0.3262 | 0.0483 | 0.4043 | 0.657 | 0.8598 |
| 2021 | 36073 | 0.965 | 0.9334 | 0.8494 | 0.7817 | 0.3294 | 0.0484 | 0.4142 | 0.802 | 0.8713 |
| 2022 | 35895 | 0.9778 | 0.9424 | 0.8649 | 0.8045 | 0.3374 | 0.0452 | 0.4255 | 0.8233 | 0.8852 |
| 2023 | 35839 | 0.98 | 0.9448 | 0.8679 | 0.8071 | 0.3471 | 0.0448 | 0.4275 | 0.8645 | 0.8936 |
| 2024 | 35703 | 0.9819 | 0.9483 | 0.8673 | 0.8034 | 0.3458 | 0.0471 | 0.4308 | 0.8809 | 0.8985 |
| 2025 | 35862 | 0.981 | 0.9491 | 0.8824 | 0.8083 | 0.3519 | 0.048 | 0.4325 | 0.8876 | 0.8984 |

## Limits

The independent all-tag census diagnoses registry omissions but cannot certify untagged multi-segment filers.
All-tag current-period fact coverage and arithmetic reconciliation are not hand-verified filing fidelity.
Industry composites lacking consolidated component facts need expanded landing/disclosure validation.
Other extra-axis hierarchies need source structure rather than numeric fitting.
Major-customer percentage disclosures are largely absent from retained source; this task does not re-land them.
Region-only geography cannot identify domestic/foreign split. IFRS axes are unmapped. Amounts remain filer currency.
As-filed negative amounts are retained; operating lease rates outside [0,1] are NULL.
Daily users must continue using previous-session cutoff even though monthly coverage now uses month-end cutoff.
