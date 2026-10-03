# Task 14: recovered segments and notes publication

**Current status after Fix round 2: DONE_WITH_CONCERNS; binding gates 0 PASS / 2 FAIL.** Verified reconciliation is **81.00%-84.96%**, below 95%. The source-wide numeric revenue proxy is **88.94%-90.43%**, missing 90% in 2020/2021/2024; its independent all-filer disclosure denominator is additionally uncertified. Earlier registry-proxy PASS and arithmetic-fit figures below are historical and superseded by the latest Fix round 2 section. No fitting was restored to inflate verification.

Status: DONE_WITH_CONCERNS. Recovery performed 2026-10-03 by GPT-6 (runtime identity; no child agents). Both deliverables exist and the successful real build is verified. The reconciliation gate remains a genuine FAIL; thresholds and denominators were not relaxed. This report recovers the missing report from persisted artifacts, rather than claiming another full build.

## Implementation and commits

- `7ec056a3`: segment features and reconciliation search, coverage and manifest gates, fixture tests.
- `146a7766`: decimal lease-rate bound and attributable fixture memory assertion.
- `95a03266`: dataset documentation and registry stub.
- `f77afbc3`: recovery report and reproducible audit script/results/guard receipts.

Owned implementation paths: `atx-db/src/atx_db/alpha_panel/fund_notes.py`, `atx-db/tests/test_alpha_panel_notes.py`, `atx-db/tests/test_alpha_panel_notes_fund.py`, `atx-db/docs/datasets/fundamentals_notes.md`, `atx-db/docs/datasets/fundamentals_notes.registry.json`. Recovery edits only task-specific artifacts in `.superpowers/sdd/tier1-v4/`. Foreign dirty paths read before editing; none edited or staged.

## Published outputs

Stage `data/alpha_panel/v1/fundamentals_notes/`, schema `atx.alpha-panel.fundamentals-notes/v1`:

| File | Rows | Bytes |
|---|---:|---:|
| segment_features.parquet | 162,314 | 8,965,756 |
| segment_recon.parquet | 46,321 | 1,544,567 |
| segments.parquet | 2,483,640 | 21,631,671 |
| notes_wide.parquet | 197,148 | 17,829,282 |
| items/year=*/items.parquet (8 files) | 18,470,845 | 125,791,550 |

Total parquet disk: 175,762,826 bytes. Filing span 2019-01-02 through 2026-08-31; feature period span 2012-10-31 through 2026-07-31. Build receipt `receipts/t14_fund_notes_full_r5.json`: completed, exit 0, 357.4 seconds, native peak 0.5823 GiB, cap 0.8 GiB, no cap hit. No full build repeated. The recovery audit independently hashes all 12 output files: 12/12 size and SHA matches, current fund_notes.py SHA matches manifest. The original build git_head is a shared-tree snapshot; the code-file hash verifies actual implementation.

Features: business-segment count and revenue HHI, foreign-sales share, largest major-customer share, DD1-DD5 and later debt maturities, operating/finance leases and MRC payment ladder, STKCO and unrecognized SBC. Accession vintages preserve earlier rows; missing feature blocks inherit only earlier same-period vintages. Filing acceptance UTC determines availability; missing acceptance uses filed +46h. Documentation contains complete feature column definitions, units, clocks, keys, staleness and as-of SQL; registry stub awaits Task 17. GC3 month-end coverage exists for member_equity, univ_all_common, univ_r3000p, each 2020-2025. No return statistics computed.

## Exact reproducible commands

From `C:/atx/atx-db`, PowerShell setup:

```powershell
$env:PYTHONPATH='C:/atx/atx-db/src'
$env:OPENBLAS_NUM_THREADS='1'
```

Command A (both gates and all panel coverage, actually rerun during recovery):

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_recovery_coverage.json -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes coverage
```

Completed exit 0, 6.0 seconds, native peak 0.3103 GiB, no cap hit. Publishes coverage and gate_status in manifest, overall pass=false.

Command B (hash verification and residual tier/class diagnosis):

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_recovery_audit.json -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-recovery-audit.py
```

Completed exit 0, 1.0 second, peak 0.042 GiB; evidence `t14-recovery-audit.json` includes exact residual SQL.

## Binding gates

Command A measures every row below; years are filing years of original 10-K/10-KT. Revenue denominator is multi-segment filings, not all filers. Multi-segment means at least two tagged reportable-looking business members across retained facts. Sum denominator is those multi-segment filings with at least one segment revenue member. Counts include repeat annual filings by an issuer and exclude amendments. All-filing counts are context only.

| Year | All 10-Ks in retained-fact frame | Multi-segment | Revenue numerator | Revenue >=90% | Sum numerator | Sum within 1% >=95% |
|---|---:|---:|---:|---|---:|---|
| 2020 | 5,572 | 2,074 | 1,894 | PASS 91.32% | 1,723 / 1,894 | FAIL 90.97% |
| 2021 | 5,877 | 2,019 | 1,846 | PASS 91.43% | 1,690 / 1,846 | FAIL 91.55% |
| 2022 | 6,525 | 2,014 | 1,843 | PASS 91.51% | 1,685 / 1,843 | FAIL 91.43% |
| 2023 | 6,212 | 2,002 | 1,828 | PASS 91.31% | 1,680 / 1,828 | FAIL 91.90% |
| 2024 | 5,790 | 1,943 | 1,754 | PASS 90.27% | 1,621 / 1,754 | FAIL 92.42% |
| 2025 | 5,538 | 1,943 | 1,798 | PASS 92.54% | 1,653 / 1,798 | FAIL 91.94% |

Two binding gate families: 1 PASS / 1 FAIL; yearly observations 6 PASS / 6 FAIL. These are tag-observable multi-segment filers, not an independently hand-identified census of all multi-segment filers. Untagged segment disclosures cannot be certified by this source; single-segment inference from no tagged member must not imply universal coverage.

## Tests and self-review

```powershell
.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/test_alpha_panel_notes.py tests/test_alpha_panel_notes_fund.py -q
```

Actual output: `20 passed in 10.80s`, offline fixtures, both files combined below 60 seconds. Earlier default-xdist invocation also exited 0, but its suppressed summary is not the release evidence. Tests cover DST and conservative missing clock, bounded parsing, malformed archive failure, segment classifications and SQL twin, segment/geography/customer features, lease/debt/SBC amounts, amendment inheritance, PIT cell coverage, thresholds, reconstruction/dedup, industry/custom revenue and zero totals, and universe coverage refresh.

No implementation changes required during recovery; persisted build matches code and docs. Search-based reconciliation is a heuristic: numeric roll-up/duplicate matches can remove economically distinct equal-valued segments or fit an accidental sum. Passing arithmetic is not independently verified disclosure fidelity; no real-filing hand-check gate is invented or claimed. Strict classes remain available for audit. Coverage refresh does not rebuild parquet or rewrite their hashes.

## Residual diagnosis and remaining work

Command B confirms 911 unreconciled revenue filings in 2020-2025: 517 below total, 182 above total, 137 double_count, 70 no_total, 5 scale_or_unit. Tier 1 has 714 misses; tier 2 has 175; extensions have 22. Tier 2 reconciles 72/247 (29.15%), extensions 12/34 (35.29%). These weaker revenue substitutes increase the first gate but retain the second gate failure. They must not be presented as verified consolidated revenue equivalents.

Existing source/implementation diagnosis: partial member revenue tagging and omitted extra-axis contexts; bank composite segment revenue missing consolidated net-interest components; overlapping hierarchies; untagged eliminations. The measured classes support discrepancy direction, but are not proof of each economic cause. Some are source gaps; extra-axis handling and composite industry formulas are unimplemented parser improvements, not intrinsic impossibilities. Resolving them requires additional context rules and real-disclosure validation; no arbitrary arithmetic subset selection or denominator change was introduced during recovery. Genuine residual FAIL is retained per GC9.

Other documented limitations: major-customer percentage facts largely absent from retained source, requiring notes_fetch landing expansion and re-landing ~15.5 GB; geography region-only disclosures lack a home/foreign split; IFRS mapping absent; quarterly features have no annual reconciliation/dedup overlay; filer currency amounts unconverted; some as-filed negative amounts retained. All detailed feature coverage remains in dataset documentation and coverage.json.

## Sources

No new network landing during Task 14 recovery. Reused SEC DERA Financial Statement and Notes datasets: `https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets`, URL pattern `https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/{YYYYqN,YYYY_MM}_notes.zip`. Existing landing: 40 sources, 2019q1-2026_08; quarterly through 2025q2, monthly from 2025_07. Historical downloaded bytes 15,456,189,519; parsed notes ~818 MB, source zips deleted. SEC public data/fair-access terms; existing SEC client 5 requests/second, declared user agent. Receipts `data/raw/sec_notes/receipts.jsonl` and archived receipt copy. These historical landing figures come from task-NOTES-report, not a fresh download. Recovery disk adds only tiny evidence files; no >1GB landing.

## Fix round 1 (historical publication, superseded by round 2)

Review `review-T14-r0-findings.md` read verbatim. Implemented I1-I3 corrections and a bounded, source-supported portion of I4. I4's binding gate remains genuinely FAIL; safe extraction and missing source structure are distinguished below. No subagents, shared contracts, progress files, foreign paths or raw landing changes.

New commits:

- `1fd12bc8` â€” fix(atx-db): preserve segment disclosure provenance and correct notes PIT coverage.
- `91fbb1e4` â€” docs(atx-db): publish corrected notes census gates and coverage evidence.
- This final report-only commit â€” docs(sdd): record Task 14 Fix round 1 verified gate failures (its own SHA is available in git log and controller final response).

### Corrections and behaviour

**I1:** numeric equal-value and subset-sum deletion, keyword exclusion, and mixed-tag fitting remain separate `candidate_only` diagnostics. They never promote `recon_class` or replace economic member values/count/HHI. Only a source-disclosed consolidation component correction may promote a strict miss. `dropped_members` and candidate sums remain explicitly non-economic diagnostics. No structural hierarchy is invented from numeric equality. Adversarial 50/50/40 and 10/20/30 fixtures preserve all three members and their true unmodified HHI; old roll-up fixture likewise stays a strict miss.

**I2:** no singleton default is inferred for an amendment lacking a current-period segment block, even when it repeats consolidated revenue. Its earlier same-period block is inherited; the original remains unchanged. Amendment state placeholders survive into the carry pass even if they have no independent features. `bseg_basis` exposes source-tag versus inferred-singleton provenance. An inherited singleton basis is legitimate earlier knowledge, not a fresh amendment inference.

**I3:** GC3 coverage uses strict month-end session m 22:00 UTC, including same-month-end accepted filings before that clock and excluding equality. Daily serving still requires the daily cutoff. All three panels were refreshed against the final Task 2 universe on 2026-10-03T16:23:48Z. Universe manifest SHA: `38f4028758d90e872cae50826d04fe61bf18014645b032b9030a748ad71586da`; identity manifest SHA `0df3dc103b49f8494002b48983c6895a30f707b4a75eb61f6d6c1f9f169f2e76`; calendar SHA `dd9cb6958d8a4a84de1f6a4ac44d07a8f8e8f9ad8d6ebed477e30d89675fa44e`. Recorded in `t14-fix-r1-evidence.json`, including refreshed member_equity/univ_all_common/univ_r3000p tables. No pending refresh dependency at completion.

**I4 safe extraction:** exact standard timing-at-point versus timing-over-time pairs can be projected to segment revenue only when both non-null facts exist, no other extra axis remains, and no plain total exists for the same fact key. This exhaustive disjoint source partition uses no fitting. Each derived row retains both original DIM strings in `context_provenance`, with `context_basis=timing_transfer_partition`. Recovered 576 segment rows across 101 filings (all available years); incomplete timing partitions remain unprojected. Industry/component/extension concepts cannot borrow unrelated consolidated revenue to claim verification. Named recognized full-revenue concepts are explicit in manifest; unverified comparison totals remain `candidate_total`, with `total_basis=component_or_extension_unverified`, and verified consolidated revenue is NULL. This corrects false equivalence rather than manufacturing an absent industry composite.

### Independent denominator investigation

Previous registry frame omitted original 10-Ks and misclassified tagged multi-segment filers. `segment_census.parquet` now starts from every periodic SUB accession and counts current-period business members across every numeric tag in the raw notes landing, independent of the fund_notes registry. New 10-K reconciliation frame also starts from all original 10-K/10-KT SUB accessions, including no-registry-fact rows. Historical segment-only members from prior periods no longer inflate current-period multi flags.

Independent pre-fix raw NUM/DIM command identified these omissions:

| Year | All original 10-Ks | Current-period all-tag multi | Not old registry-multi | No registry-filing row |
|---|---:|---:|---:|---:|
| 2020 | 5,583 | 2,118 | 54 | 11 |
| 2021 | 5,890 | 2,058 | 46 | 13 |
| 2022 | 6,552 | 2,049 | 43 | 27 |
| 2023 | 6,279 | 2,040 | 42 | 67 |
| 2024 | 5,813 | 1,981 | 48 | 23 |
| 2025 | 5,560 | 2,006 | 73 | 22 |

These are numeric disclosure counts, not a certified census of untagged multi-segment filers. Existing alternate `sec_text` metadata was checked: original annual accession overlaps are 3,066/5,583, 3,234/5,890, 3,419/6,552, 3,377/6,279, 3,242/5,813, 3,156/5,560 for 2020-2025 (373 metadata parts). `filing_text.py` documents member-linked scope and extraction of Business/Risk/MD&A sections, not the Item 8 financial-statement notes; full documents are not retained. This incomplete, section-limited alternate corpus cannot establish all-filer multi-segment disclosure classification. No all-filer certification or proxy waiver is claimed. Manifest gate_status explicitly separates `observable_threshold_pass` and `denominator_certified=false`.

### Latest gates (commands B and C below)

| Year | All original 10-Ks | Source-tagged multi | Revenue count/share, proxy >=90% | Verified sum count/share, >=95% |
|---|---:|---:|---|---|
| 2020 | 5,583 | 2,118 | 1,900/2,118 = 89.71%, FAIL | 1,539/1,900 = 81.00%, FAIL |
| 2021 | 5,890 | 2,058 | 1,852/2,058 = 89.99%, FAIL | 1,513/1,852 = 81.70%, FAIL |
| 2022 | 6,552 | 2,049 | 1,852/2,049 = 90.39%, proxy threshold PASS | 1,525/1,852 = 82.34%, FAIL |
| 2023 | 6,279 | 2,040 | 1,836/2,040 = 90.00%, proxy threshold PASS | 1,522/1,836 = 82.90%, FAIL |
| 2024 | 5,813 | 1,981 | 1,761/1,981 = 88.89%, FAIL | 1,496/1,761 = 84.95%, FAIL |
| 2025 | 5,560 | 2,006 | 1,813/2,006 = 90.38%, proxy threshold PASS | 1,474/1,813 = 81.30%, FAIL |

Binding family status **0 PASS / 2 FAIL**. Numeric proxy yearly threshold observations 3 PASS / 3 FAIL; reconciliation 0 PASS / 6 FAIL. All six certified revenue observations are unverified and cannot be called PASS, even where the numeric proxy meets threshold. Overall manifest pass=false. Count fractions, not rounded shares, establish the exact boundary.

Remaining 2020-2025 observed-revenue reconciliation misses: **1,945** = 445 double_count + 554 no_total + 389 below_total + 551 above_total + 6 scale_or_unit. A further 1,238 source-tagged multi filings have no eligible segment revenue. These directions are measured; they do not independently prove an economic cause. Source-supported remediation already delivered: wider independent numeric denominator, explicit exhaustive timing projection, and rejection of component/full-total mismatches. Unimplemented safe extraction requires source-specific structural evidence for product/entity/geographic hierarchies and mixed concept equivalence. Genuine source gaps include absent consolidated net-interest components for bank composite revenue, industry components not landed without dimensions, untagged eliminations, and missing customer percentages. Existing raw contexts often carry overlapping customer/product/utility categories; simply summing them would violate I1. Obtaining complete hierarchy/disclosure evidence or expanded source facts would be additional work; the current numeric census cannot solve untagged-disclosure identification. No assertion that all misses are intrinsic or all locally recoverable.

### Tests, build, integrity and exact commands

Setup from `C:/atx/atx-db`: `PYTHONPATH=C:/atx/atx-db/src`, `OPENBLAS_NUM_THREADS=1` (PowerShell assignments in initial report). Every real-data job used the guard, one at a time in this lane. Tests are offline fixtures.

```powershell
# Offline tests; UTF-8 transcript t14-fix-r1-tests.txt preserves output.
.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/test_alpha_panel_notes.py tests/test_alpha_panel_notes_fund.py -q
# A: independent context/all-tag denominator audit
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_context_audit_r2.json -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-context-audit.py
# B: corrected full publication (also measures both gates)
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.9 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r1_build.json -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build
# C: final-universe coverage/gate refresh
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.8 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r1_final_coverage.json -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes coverage
# D: final measured docs/registry/evidence and alternate-text metadata coverage
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r1_final_evidence.json -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r1-evidence.py
# E: output hashes/keys/candidate separation/amendment source provenance/census agreement
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/t14_fix_r1_integrity_r2.json -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r1-integrity.py
```

Actual final tests: **24 passed in 27.43s**, both files combined below 60s. Cover equal-valued distinct segments, accidental subset sum, earlier original preservation, total-only partial amendment, month-end acceptance and equality boundary, independent unknown-tag denominator, exact timing provenance and incomplete partition refusal, plus all previous notes families/clock/parser fixtures. Earlier intermediate 22/24-test runs passed; final transcript is authoritative. `git diff --check` clean for implementation/tests/docs.

| Command | Exit/status | Runtime | Peak GiB | Cap GiB |
|---|---|---:|---:|---:|
| A | 0 completed | 11.0s | 0.1246 | 0.8 |
| B | 0 completed | 277.2s | 0.5664 | 0.9 |
| C | 0 completed | 6.0s | 0.3105 | 0.8 |
| D | 0 completed | 2.0s | 0.1040 | 0.6 |
| E | 0 completed | 1.0s | 0.0607 | 0.6 |

All cap_hit=false. Full build queued 69.2s; waited normally. Initial audit A attempted a full-year DIM hash join and hit DuckDB's 300MB allocation limit (exit 1, 114.1s, native peak 0.471 GiB); fixed by compact business-only DIM tables one source at a time, not by increasing host cap. Initial integrity check mistakenly treated 884 amendment rows carrying an earlier original's inferred-singleton provenance as fresh inferences; the corrected ancestry check confirms every such source block comes from an earlier visible vintage. Both failed receipts and outputs retained transparently. No publication change was required for that diagnostic correction.

Final integrity: **13/13 parquet SHA/size matches**, code SHA matches manifest, 0 duplicate feature keys, 0 candidate promotions, 0 unsupported amendment singleton inferences, 0 census/recon mismatches. Evidence `t14-fix-r1-integrity-r2.json` and final guard receipt. Original recovery integrity evidence remains historical, not overwritten.

### Current output and disk/source status

Corrected outputs: features 164,704 rows / 9,027,384 bytes; reconciliation 46,513 / 1,641,601; source-wide census 201,137 / 2,124,326; segments 2,484,216 / 23,990,714; wide 197,148 / 17,829,282; long items unchanged 18,470,845 / 125,791,550. Total parquet **180,404,857 bytes**. Features filed 2019-01-02..2026-08-31, periods 2011-09-30..2026-07-31, availability 2019-01-02 21:13..2026-08-31 21:01 UTC. New census includes all periodic source filings; features may contain amendment placeholders. Docs/registry describe new provenance and census columns/keys and the separate gate-certification limitation. Shared stage registry still untouched; Task 17 owns integration.

No new URLs fetched or raw files changed; existing source landing and terms/cadence figures remain those in initial Sources. No >1GB landing, no held-out return statistic, no threshold waiver. This round increases parquet disk by 4,642,031 bytes over the recovery publication and adds small task-specific evidence files. Stage publishes with genuine gate_status FAIL under GC9; it is not declared fully parity-certified.

## Fix round 2 (authoritative current publication)

Review `review-T14-r1-findings.md` read verbatim. N1 addressed; retained I4 and independent denominator certification remain unmet. Current binding status **0 PASS / 2 FAIL**, with lower verified figures preserved. No numerical hierarchy inference, proxy waiver, threshold relaxation, foreign edits, progress edits or subagents.

Implementation commits **1b1856c2** and **1fae76ef** (accession-scope correction) preserves the census for every republished existing partition when item extraction is restricted. Offline two-year build then one-year rebuild fixture preserves omitted census, count, HHI and gate denominator. Targeted regression red: 1 failed in 3.02s; green: 1 passed in 2.28s. Final covering command `.venv/Scripts/python.exe -m pytest -o addopts='' --run-slow tests/test_alpha_panel_notes.py tests/test_alpha_panel_notes_fund.py -q`: **26 passed in 34.72s**, exit 0, final completed session 93481. `t14-fix-r2-tests.txt` records actual final tool output.

Accession-scoped primary-evidenced ReportableLegalEntities and ReportableGeographicalComponents exceptions supplement generally admissible OperatingSegments. Primary disclosures corroborate Forian's three reportable segments, Avantor's identical operating/reportable geographic segments, and Smith Douglas's Southeast/Central reportable segments. Members remain unchanged; this is container identity, not an arithmetic-fitted hierarchy. Fixture tests assert direct count/HHI for both identities. Detailed source URLs, response SHA256 and checked raw receipts appear in `t14-fix-r2-pilot-evidence.json`.

### Causal audit and unresolved implementation/source split

Baseline all **3,183** keys audited: **1,945** verified reconciliation misses and **1,238** missing-revenue filings. Compact reproducible index `t14-fix-r2-case-keys.jsonl`; scripts retain detailed raw NUM/DIM evidence locally. Diagnostic classes below describe required evidence, not certified economic causality:

| Diagnostic evidence class | Cases |
|---|---:|
| no_eligible_annual_revenue_fact_requires_expanded_source_or_disclosure | 818 |
| unknown_discrepancy_requires_primary_disclosure | 237 |
| unknown_extra_axis_structure_requires_disclosure | 815 |
| component_equivalence_or_consolidated_total_requires_source_evidence | 554 |
| numeric_candidate_without_hierarchy_provenance_requires_disclosure | 730 |
| local_context_or_concept_review_needed | 6 |
| local_alternate_full_concept_with_all_numeric_members | 23 |

The 23 alternate recognized-concept cases include 10 numerical fits, none accepted merely for fitting. Cowen `0001466538-20-000026` explicitly reconciles Economic Income segment revenue to US GAAP with funds/consolidation adjustments; this disproves automatic equivalence. Three of the six context-review cases support the container correction. The other three contain only a ConsolidatedEntities business aggregate; no artificial segment split is inferred.

Martin Marietta `0000950170-23-004361` independently discloses East/West nested under Total Building Materials, plus Magnesia Specialties. This proves at least one source-supported hierarchy exists, but a general filing-role-aware presentation/definition parser and full production structural landing remain **unimplemented**. It is not labeled intrinsic data failure. Exact role relationships must be preserved before member/count correction; a subtotal keyword or subset sum is insufficient. The compact key index identifies this case separately. The 818 cases with no eligible annual revenue fact in retained known concepts need expanded source or definition-equivalence evidence; it is not proven that every one lacks revenue in the filing. Other 3,178 keys beyond the three container cases, one hierarchy case and one non-equivalence case remain **unknown local versus new-source causality**; diagnostic categories do not certify that split. No pilot extrapolation is claimed.

### Authorized primary-disclosure pilot and minimal next acquisition

Deterministic selection: all 29 local-review cases plus balanced discrepancy/SIC strata, seed `t14-r2-pilot-1`; **100/100 processed**, **100 successful primary filings**, **398 total HTTP attempts** including retries/redirects, **589,808,378 response bytes**, **41,607,091 on-disk bytes**. Enforced <=1 attempt/s, <=600 attempts, <=750 MiB response landing, sec_http approved User-Agent/shared limiter, <=0.8 GiB Job Object. Raw receipts `data/raw/sec_notes_t14_pilot/receipts.jsonl`; no raw data committed. Pilot selection, summary and raw SHA checks are persisted. Existing Business cache was consulted first; its corrupt/oversized parts are reported in pilot cache_errors rather than silently treated as complete.

Primary path: SEC Archives accession primary document for Item 8 segment notes, accession index.json, relevant definition/presentation/calculation linkbases. All 3,183 residual primary URLs are identified; expanded residual acquisition needs 3,183 primary +3,183 index +3,183..9,549 structure GETs before retries, estimated **10,472,668,404 uncompressed primary bytes** using historical median. Production untagged denominator requires independent classification of **35,677** original annual filings; 35,676 primary URLs known, one index resolution needed, median-size estimate **117,384,037,276 bytes** before compression. Existing 28 historical Notes ZIPs total **12,205,444,547 bytes**, but TAG/PRE/roles were not retained. Minimal next step is bounded role-aware extraction/validation from this pilot, then approval of a scoped full structural/source landing; full corpus was not launched. Preserve C:>=40GB before any >1GB landing. Estimates are cost estimates, not evidence that missing data will resolve every gate.

### Latest measured gates and restricted-build integrity

| Year | Multi | Revenue proxy count/share | Verified reconciliation count/share |
|---|---:|---|---|
| 2020 | 2118 | 1900/2118 = 89.7100% | 1539/1900 = 81.0000% |
| 2021 | 2058 | 1852/2058 = 89.9900% | 1513/1852 = 81.7000% |
| 2022 | 2049 | 1852/2049 = 90.3900% | 1525/1852 = 82.3400% |
| 2023 | 2040 | 1837/2040 = 90.0500% | 1523/1837 = 82.9100% |
| 2024 | 1981 | 1762/1981 = 88.9400% | 1497/1762 = 84.9600% |
| 2025 | 2006 | 1814/2006 = 90.4300% | 1475/1814 = 81.3100% |

Certified all-filer revenue gate remains FAIL with denominator_certified=false; numeric proxy observations are not certified PASSs. Restricted-year real rebuild recomputed every census row; all-year census digest and omitted 2020-2022 feature/reconciliation digests match baseline: {'census': True, 'omitted_2020_2022_features': True, 'omitted_2020_2022_recon': True}; omitted gate counts preserved=True. Updated parquet total **180,404,910 bytes**; full row/size table and GC3 year panels in dataset docs and `t14-fix-r2-evidence.json`. GC3 refreshed at 2026-10-03T17:06:42+00:00; actual current input SHA256 {'universe_manifest': '33a15585807fa242fc4e7eb38ec3c1902bf043ba442f57b2e2a8f45dbad7071d', 'identity_manifest': '0df3dc103b49f8494002b48983c6895a30f707b4a75eb61f6d6c1f9f169f2e76', 'calendar': 'dd9cb6958d8a4a84de1f6a4ac44d07a8f8e8f9ad8d6ebed477e30d89675fa44e'}. No returns computed.

### Exact guarded commands and receipts

From C:/atx/atx-db, PYTHONPATH=C:/atx/atx-db/src, OPENBLAS_NUM_THREADS=1. Each command below used `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb CAP --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/receipts/RECEIPT -- COMMAND`. One data job at a time in this lane.

| Receipt | Cap | Command after -- | Exit | Seconds | Peak GiB |
|---|---:|---|---:|---:|---:|
| t14_fix_r2_causal_audit_r2.json | 0.8 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-causal-audit.py` | 0 | 22.0 | 0.6393 |
| t14_fix_r2_acquisition_audit_r2.json | 0.8 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-acquisition-audit.py` | 0 | 3.0 | 0.1619 |
| t14_fix_r2_pilot_r3.json | 0.8 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-pilot.py` | 0 | 427.3 | 0.2759 |
| t14_fix_r2_preservation_before.json | 0.6 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation.py before` | 0 | 2.0 | 0.126 |
| t14_fix_r2_build.json | 0.9 | `.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build --years 2025` | 0 | 112.1 | 0.5399 |
| t14_fix_r2_preservation_after.json | 0.6 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-preservation.py after` | 0 | 1.0 | 0.1266 |
| t14_fix_r2_coverage.json | 0.8 | `.venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes coverage` | 0 | 5.0 | 0.3106 |
| t14_fix_r2_evidence.json | 0.6 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-evidence.py` | 0 | 1.0 | 0.1039 |
| t14_fix_r2_integrity.json | 0.6 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-integrity.py` | 0 | 1.0 | 0.0604 |
| t14_fix_r2_pilot_evidence.json | 0.6 | `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t14-fix-r2-pilot-evidence.py` | 0 | 1.0 | 0.084 |

All successful receipts cap_hit=false. Initial causal SQL alias failed before publication; initial acquisition diagnostic exceeded its 0.6 GiB guard and was replaced by a filtered bounded scan at 0.8 GiB. Initial pilot cache scan hit DuckDB allocation limit; second hit a corrupt cached parquet. Both stopped before HTTP, so consumed no SEC attempts. Third reads bounded partitions and records unavailable cache parts. Failed receipts retained. Final output integrity verifies all parquet hashes, code hash, duplicate keys, candidate separation, amendment ancestry and census/recon agreement.

Round2 commits: implementation **1b1856c2** and **1fae76ef**; evidence/docs **f986d6fe**; final report commit identifies itself via git log. Prior Task14 SHAs remain 7ec056a3,146a7766,95a03266,f77afbc3,1fd12bc8,91fbb1e4,6bcce194.

Post-fix measured effect: exactly three formerly missing-revenue filings recovered and directly reconciled (one each 2023/2024/2025), with source-wide denominator counts unchanged. Remaining baseline verified misses: 1,945; missing revenue now 1,235. All 2019-2026 census rows preserve the baseline digest. Report-generation command also ran under 0.6 GiB guard, receipt t14_fix_r2_report.json: exit 0, 1.0s, peak 0.0352 GiB, cap_hit=false.
