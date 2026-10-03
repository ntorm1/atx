# Task 14 round 4 scoped review

### Spec Compliance — ❌ Needs fixes

Reviewed supplied commits `74320900`, `45a46ceb`, reviewer/resume contracts, task brief, binding gap-analysis §2 item 12, GC1–GC9, round 3 findings and authoritative round 4 report. Scope: retained I4, independent denominator certification and new fix breakage. I1–I3/N1 remain settled. Graph tools unavailable; targeted source reads and one focused offline reproduction were used. No full suite, build, network, real-data query or git reconstruction was run; only this findings artifact was written.

I4 remains **NOT ADDRESSED**: source-cut evidence records 14 cuts, six reconciliations and eight failures; residual 1,939 revenue-bearing misses and 1,235 missing-revenue filings. Yearly verified reconciliation is 1,539/1,900; 1,513/1,852; 1,526/1,852; 1,524/1,837; 1,497/1,762; 1,479/1,814 (81.00%–84.96%). Revenue proxy is 89.71%, 89.99%, 90.39%, 90.05%, 88.94%, 90.43%; it misses 90% in 2020/2021/2024 and has no certified all-filer denominator. Binding gates remain **0 PASS / 2 FAIL**. Honest FAIL publication satisfies GC9 reporting, not the binding thresholds.

Independent denominator remains **NOT ADDRESSED**, with useful partial progress: 698 spans across 497 positive accessions (447 multi, 50 single), 15 multi outside numeric proxy, zero count conflicts, 35,180 unclassified annual filings. The classifier depends on textual claims rather than revenue arithmetic; the summary explicitly calls its candidate-restricted frame nonprobability and uncertified. Corrupt optional partition is disclosed.

Nine earlier source proofs now have persisted reopened-byte evidence: nine receipt/primary SHA equalities, nine nonempty citation sets, compressed hashes, exact decoded source offsets and raw-span hashes. Five automatic cuts supplement those nine; production effects preserve eight failed sums rather than fit values. Republic's +331m bridge supplies the sixth reconciliation. Signed bridges select exact table/row, annual contexts and selected source unit/context family; retained negative tests reject a separate intersegment-sales row. Source-declared Corporate/Other names now survive generic filters in economic census and HHI. **M1 ADDRESSED:** `fundamentals_notes.md:51` distinguishes candidate suggestions from verified exclusions of nonselected levels.

**⚠ Cannot verify from supplied evidence:** reviewers did not independently decompress production primary sources or recompute parquet outputs. Reopen/hash/integrity receipts are implementation evidence, not an independent full-filer semantic census. Final Task 2 GC3 refresh remains pending. Controller additionally reports its intermediate universe writer stopped before manifest finalization with partial year writes: a stable universe manifest hash alone cannot certify a consistent input-file snapshot during concurrent writing. Treat recorded all-common/R3000 coverage as provisional until a completed final universe publication and controller-coordinated integrity/refresh.

### Strengths

Reports preserve the genuine binding failures and original numeric proxy, expose exact source citations, and separate partition identity from reconciliation. Recorded integrity has 14/14 output hashes and six module hashes matching, with zero duplicate/provenance/candidate-promotion/census errors. Guard receipts report caps ≤0.9 GiB and successful exits. Fixture transcript reports 46 passed in 60.38 seconds across three files, no warning text; combined duration does not demonstrate a GC4 per-file violation. No return computation or shared registry mutation is introduced.

### Issues — Critical / Important / Minor

Counts: **Critical 0; Important 2 (one retained plan-mandated, one new); Minor 0**. Independent denominator is part of unresolved I4 verification.

**I4 — Important, retained, plan-mandated:** `task-14-report.md:313–327`. Neither ≥95% verified reconciliation nor independently certified ≥90% revenue coverage is met. Complete filing-aware source extraction and concept/bridge validation, independently classify the disclosure denominator, and remeasure both gates; otherwise retain explicit unmet requirements for owner disposition. Do not infer intrinsic absence from unexamined cases.

**I5 — Important, NEW: unrelated or historical prose can certify a wrong economic partition.** `atx-db/src/atx_db/alpha_panel/fund_notes_semantic.py:98–109`. `reportable_lists` searches every contiguous token span after the claim within the block/extended blocks, rather than parsing the declared list. Qualifier/year checks inspect only the original claim block and preceding block, excluding following list paragraphs. A sound role and annual revenue facts for the wrongly matched names then promote `verified_structure`, changing production economic count/HHI/source quality via `cached_structure` and `recon_overlay`.

Focused offline reproduction used `runpy.run_path('tests/test_alpha_panel_notes_semantic.py')`, then `cut(document(statement))`, with unchanged `roles()`/current-year USD facts. Both exact statements returned `verified_structure`, members `['Alpha','Beta']`:

1. `We have two reportable segments: Gamma and Delta. Our products Alpha and Beta generate revenue.`
2. `We have two reportable segments:</p><p>Previously Alpha and Beta.`

The first explicitly names Gamma/Delta as reportables, but product prose supplies Alpha/Beta token matches. The second puts the historical qualifier in a following list paragraph outside the checked scope. Command: from `C:/atx/atx-db`, set `PYTHONPATH=C:/atx/atx-db/src`, pipe the above Python reproduction to `.venv/Scripts/python.exe -`; completed exit 0. This is a concrete named-risk fixture check, not a suite rerun or data command.

Fix: bound extraction to a syntactically identified declared list and reject descriptive/unmapped items; inspect qualifier/year semantics over every contributing list block. Keep unmatched declarations unresolved rather than filling the declared count from nearby known DIM aliases. Add both regressions through production `semantic_cut`; revalidate the five automatic cuts after correction.

### Assessment — Task quality: Needs fixes

Round 4 improves bounded provenance and honest denominator evidence, but retained binding requirements are incomplete and new I5 permits false source certification. I1–I3/N1 remain settled; M1 is closed. GC3 final universe publication and refresh remain coordinated follow-up work.
