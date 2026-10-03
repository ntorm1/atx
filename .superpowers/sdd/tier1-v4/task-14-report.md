# Task 14: recovered segments and notes publication

Status: DONE_WITH_CONCERNS. Recovery performed 2026-10-03 by GPT-6 (runtime identity; no child agents). Both deliverables exist and the successful real build is verified. The reconciliation gate remains a genuine FAIL; thresholds and denominators were not relaxed. This report recovers the missing report from persisted artifacts, rather than claiming another full build.

## Implementation and commits

- `7ec056a3`: segment features and reconciliation search, coverage and manifest gates, fixture tests.
- `146a7766`: decimal lease-rate bound and attributable fixture memory assertion.
- `95a03266`: dataset documentation and registry stub.
- Recovery commit: this report and reproducible audit script/results/guard receipts (see git log for its SHA; a commit cannot include its own SHA).

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
