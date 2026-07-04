# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Integration branch: `feat/warehouse-parity` / local `main`

PF2-S1 through PF2-S9 are implemented. PF2-S9 lives on branch
`feat/pf2-s9` as implementation commit
`4fad387eb2da9ad268da4ecc800e2cdf8e74324d`.

## Completed

- PF2-S1: schema-as-contract complete on branch `feat/pf2-s1`, base `636e82b`.
  Migrations consumed: `0097-0099`.
- PF2-S2: migration governance + backup/checkpoint/DR complete on branch
  `feat/pf2-s2` as `ceec446f2db24aee3bc721ebc136692c98a7b86f`. Migrations
  consumed: `0100-0101` (`0102` remains reserved headroom). Local-main DB
  testing-speed improvements were merged into the worktree before verification.
- PF2-S3: standardization engine complete on branch `feat/pf2-s3` as
  `8a78117f5f35d1445a6e8aaf83c1a7c6ebf7a263`. Migrations consumed:
  `0103-0106`.
- PF2-S4: PIT month snapshot + ratio-vintage history complete on branch
  `feat/pf2-s4` as `6a20a31a23bebc660a196c10a25f275b56e98882`. Migrations
  consumed: `0107-0109`.
- PF2-S5: industry-specialized templates complete on branch `feat/pf2-s5` as
  `90af61f9b03ed5d1ab7bd09c28f913c3357adb9d`. Migrations consumed:
  `0110-0113`.
- PF2-S6: calendarization + fiscal normalization + quarterly-TTM complete on
  branch `feat/pf2-s6` as `4b8cd5a66b6ddf76bed98ec23b8ede7f6aa0e05c`.
  Migrations consumed: `0114-0116`.
- PF2-S7: segment data + footnote sub-ledgers complete on branch
  `feat/pf2-s7` as `9544e25d385017b72787984e703b34d5a4cd562b`.
  Migrations consumed: `0117-0120`.
- PF2-S8: press-release preliminary actuals + estimate basis complete on
  branch `feat/pf2-s8` as
  `1e5ddce014fa3546c721baca437fd4a1d1ece4e1`. Migrations consumed:
  `0121-0123`.
- PF2-S9: populated valuation overlap provenance + cross-vendor standardized
  fact reconciliation + wider SQL-native DQC subset complete on branch
  `feat/pf2-s9` as
  `4fad387eb2da9ad268da4ecc800e2cdf8e74324d`. Migrations consumed:
  `0124-0127`.

## S6 Implemented Surfaces

- New `db/calendarization.py` with pure FYR/period-length helpers, materialized
  `fundamental_calendar_map`, `fundamental_calendar_ttm`, and
  `calendarization_coverage` refreshes.
- Migrations `0114-0116` add/catalog the calendar map, calendar-aligned TTM
  surface, coverage table, and lookup indexes with schema-contract refreshes.
- `refresh_fundamental_ttm_points` now tags annual-minus-9mo Q4 stitch paths as
  `stitched_quarterly_ttm` while preserving the existing 400-day self-join guard.
- Critical quality checks cover exactly-one calendar map labels, 52/53-week
  flags, calendar-TTM duplicate windows, stitched-TTM duplicate windows, and
  aggregate coverage status.
- Offline fixtures prove FYR boundary handling, a 371-day 53-week period, offset
  fiscal-year-end calendar TTM, partial coverage flags, and thin-cache no-op.

## S7 Implemented Surfaces

- New `db/segments.py` mines dimensional inline-XBRL facts into `segment_dim`
  and `segment_fact` for business/geographic/product/customer segments, with
  deterministic IDs, PIT lineage, revision flags, and tolerance-banded
  reconciliation to `fundamental_standardized` / `fundamental_xbrl_metric`.
- New `db/footnotes.py` normalizes dimensional pension/OPEB, deferred-tax,
  lease, and SBC facts into `footnote_pension`, `footnote_deferred_tax`,
  `footnote_lease`, and `footnote_sbc`; SBC remains aggregate/award-type only,
  not Execucomp grant detail.
- Migrations `0117-0120` add/catalog segment tables, footnote tables, indexes,
  reconciliation support, and `segment_footnote_coverage` with schema-contract
  refreshes.
- Critical quality checks cover bad segment rows, duplicate segment revision
  keys, tolerance-banded segment reconciliation warnings, bad footnote
  sub-ledger rows, and coverage-count validity.
- Jobs registry exports `segments` and `footnotes` downstream of
  `xbrl_filing_contexts`; `segments` also depends on `fundamental_xbrl_metric`
  for consolidated reconciliation.

## S8 Implemented Surfaces

- New `db/press_release.py` ingests injectable 8-K Item 2.02 / EX-99 text or
  normalized rows into `press_release_facts`, extracting preliminary revenue,
  EPS, operating-income, and net-income figures with confidence, evidence text,
  source-file hashes, release-time `available_at`, and PIT as-of reads.
- `press_release_reconciliation` ties preliminary rows to later final
  `est_actual` rows while retaining both vintages, and updates
  `fundamental_periods.pdate` / `rdq` to the earlier flash date through an
  additive reconciliation write.
- `est_actual.basis` is stamped `GAAP` for SEC companyfacts actuals, and
  `est_surprise` now carries `actual_basis`, `consensus_basis`, and
  `basis_mismatch`; `surprise_pct` is suppressed when tagged bases differ while
  untagged legacy consensus remains backward-compatible.
- Migrations `0121-0123` add/catalog press-release fact and reconciliation
  tables plus estimate-basis columns with schema-contract refreshes. Migration
  `0123` defensively creates missing estimate base tables for legacy-forward DB
  fixtures and avoids same-transaction secondary-index recreation around
  DuckDB's `est_surprise` ALTER dependency guard.
- Critical quality checks cover malformed press-release rows, no-lookahead
  release timestamps, retained preliminary vintages, missing EPS actual basis,
  and mismatch rows with populated `surprise_pct`.
- Jobs registry exports `press_release_facts` downstream of `est_actual`.

## S9 Implemented Surfaces

- `valuation_multiples` now persists each refresh's proof-slice coverage into
  `valuation_overlap_slice`, including numerator/denominator security counts,
  overlap date spans, stale price/fundamental gaps, details JSON, and PIT
  markers. `ValuationMultiplesDataset.load` writes both the existing quality
  rows and the durable overlap provenance row.
- New `db/fact_disagreement.py` normalizes injectable Sharadar/SimFin-style
  CSV/JSON/DataFrame/callable baseline facts into `vendor_baseline_facts`,
  compares latest visible vintages against `fundamental_standardized` on
  `(security_id,item_id,period_end,basis)`, and writes `fact_disagreement`
  rows with `agrees` / `disagrees` / `missing_warehouse` statuses.
- The >99% cross-vendor agreement gate is first-class in both
  `FactDisagreementDataset.load` and warehouse quality checks. Bad-row checks
  cover the new overlap and disagreement surfaces.
- `xbrl_validation.py` keeps the existing calculation-linkbase tolerance at
  `1.0` and appends additive DQC rows for SQL-expressible subsets:
  DQC_0004, DQC_0080, DQC_0018, and DQC_0041, alongside the existing
  DQC_0015/DQC_0053 subset. Migration `0126` catalogs ported rules and
  explicitly skipped full-processor rules (`DQC_0118`, `DQC_0135`).
- Jobs registry now exposes `fundamental_standardized` and
  `fact_disagreement`, with `fact_disagreement` downstream of the standardized
  fact surface.

## Verification

- `python -m py_compile db\calendarization.py db\fundamental_statements.py db\migrations.py db\quality.py db\__init__.py db\tests\test_calendarization.py`
- `python -m pytest db\tests\test_calendarization.py -q -n0`
- `python -m pytest db\tests\test_fundamental_period_dates.py db\tests\test_restatement_lineage.py db\tests\test_schema_contract.py db\tests\test_schema_contract_quality_checks.py db\tests\test_import.py -q -n0`
- `python -m pytest db\tests -q -n0`
- `python -m py_compile db\segments.py db\footnotes.py db\migrations.py db\quality.py db\__init__.py db\jobs.py db\tests\test_segments.py db\tests\test_footnotes.py`
- `python -m pytest db\tests\test_segments.py db\tests\test_footnotes.py -q -n0`
- `python -m pytest db\tests\test_import.py db\tests\test_jobs_dag.py db\tests\test_schema_contract.py db\tests\test_schema_contract_quality_checks.py db\tests\test_segments.py db\tests\test_footnotes.py -q -n0`
- `python -m pytest db\tests -q -n0`
- `python -m py_compile db\press_release.py db\estimates.py db\migrations.py db\quality.py db\jobs.py db\__init__.py db\tests\test_press_release.py`
- `python -m pytest db\tests\test_press_release.py -q -n0`
- `python -m pytest db\tests\test_estimates.py db\tests\test_press_release.py -q -n0`
- `python -m pytest db\tests\test_import.py db\tests\test_jobs_dag.py db\tests\test_schema_contract.py db\tests\test_schema_contract_quality_checks.py db\tests\test_press_release.py -q -n0`
- `python -m pytest db\tests` (xdist default): 955 passed in 188.32s
- `python -m py_compile db\valuation_multiples.py db\fact_disagreement.py db\xbrl_validation.py db\migrations.py db\quality.py db\jobs.py db\__init__.py db\tests\test_valuation_multiples.py db\tests\test_fact_disagreement.py db\tests\test_xbrl_validation.py db\tests\test_jobs_dag.py`
- `python -m pytest db\tests\test_valuation_multiples.py db\tests\test_fact_disagreement.py db\tests\test_xbrl_validation.py db\tests\test_jobs_dag.py -q -n0`
- `python -m pytest db\tests\test_migrations.py db\tests\test_migration_governance.py db\tests\test_schema_contract.py db\tests\test_schema_contract_quality_checks.py db\tests\test_import.py -q -n0`
- `python -m pytest db\tests`: 964 passed in 168.68s

## Live DB Smoke

Operator-pending for S1-S9. No live 14 GB shared-DB migration/apply/rebuild was
run from the PF2 worktrees. S6 live proof-slice counts for
`fundamental_calendar_map`, `fundamental_calendar_ttm`, non-Dec-FYE relabels,
53-week flags, stitched-TTM windows, and `run_id` remain pending until an
approved backed-up live run. S7 live proof-slice counts for `segment_dim`,
`segment_fact`, footnote sub-ledgers, reconciliation pass/flag/no-consolidated
split, coverage row, and `run_id` also remain pending. S8 live proof-slice
counts for `press_release_facts`, `press_release_reconciliation`, periods with
pre-10-Q preliminary capture, EPS basis coverage, and `run_id` also remain
pending. S9 live proof-slice counts for `valuation_overlap_slice`,
`valuation_multiples`, `vendor_baseline_facts`, `fact_disagreement`,
agreement ratio, DQC rows by rule code, and `run_id` also remain pending.

## Known Caveats

- S1's PIT-column-presence gate intentionally exposes known pre-existing PIT
  gaps on older fact tables, especially missing `is_latest_revision`; PF2-S10
  owns halt/exemption/backfill policy.
- S4 ratio-vintage history is opt-in and currently lifts TTM accession vintages.
  The default ratio rebuild remains latest-revision mode for compatibility.
- S5 industry routing is SIC-derived. Unclassified entities deterministically
  fall back to `ALL`; richer vendor/security-master profile routing is future
  work.
- S6 derives FYR from issuer annual periods when available, with period-end month
  fallback for thin fixtures; broader issuer fiscal-calendar metadata is future
  input coverage.
- S6 live proof slice is not run; docs do not claim live row counts.
- S7 segment sums use a default 2% relative reconciliation tolerance and flag
  divergence as a warning, not a load failure; ASC 280 segment disclosures are
  expected to be noisy.
- S7 live proof slice is not run; docs do not claim segment or footnote live row
  counts.
- S8 extraction is intentionally conservative and offline/injectable; the
  default DB has no loaded press-release corpus until an operator supplies one.
- S8 live proof slice is not run; docs do not claim preliminary-capture or basis
  coverage counts.
- S9 cross-vendor reconciliation is an offline/injectable surface. The default
  DB has no Sharadar/SimFin baseline rows until an operator supplies a file or
  loader; tests prove comparison and quality gating on fixtures only.
- S9's DQC work is still a SQL-native subset, not the full XBRL-US plugin. The
  full Arelle/XULE-dependent rules remain explicitly catalogued as skipped.
- `0102` remains unused reserved headroom after S2.

## Resume Point

1. Merge `feat/pf2-s9` into local `main` / `feat/warehouse-parity`.
2. Continue ROADMAP sequencing with PF2-S10.
3. Track progress in `.superpowers/sdd/progress.md`; append or update sprint
   closeout rows only when the sprint lands.

## Dirty Worktree Notes

Do not run `git add -A`. Preserve unrelated dirty/untracked files unless the
user explicitly asks to clean them up.
