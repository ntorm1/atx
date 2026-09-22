# Tier-1 parity: stopped status and next-parent goal

**Resume note:** the user subsequently instructed "continue goal of building
Factset/S&P compustat competitor". Work resumed2026-09-22 at21:53UTC. This
file preserves the stop snapshot and prompt; current live state is in
`.superpowers/sdd/tier1-parity/continuation-queue.md`. The stop directive below
is satisfied and no longer prevents authorized continuation.

Written after the user's explicit instruction: **"stop here and write a status
markdown file with goal prompt for next parent agent to pick up"**.
The stop/recovery observations below are UTC on 2026-09-22. This file supersedes
older claims that archive7/archive8 or implementers are still running.

## Stop state

- Branch: `feat/tier1-parity`. Implementation HEAD before this handoff:
  `48be8414`; latest production Python change is VR1 `fd2738c7`.
- All three then-active subagents were interrupted. **Do not resume development
  or launch a loader merely because automatic goal metadata remains active.**
  The next parent should resume when the user invokes the prompt below.
- Archive8 native worker2780 was identity-checked and stopped at
  **2026-09-22T00:43:18.8821192Z**. Session43304 is terminal, exit1. Original
  worker2780, guard8900 and redirectors15480/9116 were independently confirmed
  absent. No other applications were stopped. No loader is left running.
- Guard receipt records `failed` because the user stopped the worker; this was
  **not** a source failure or memory-limit event. Native peak was
  **1.7335357666GiB** under the unchanged2GiB guard.
- The two run ledgers were closed as failed with an explicit intentional-stop
  reason at **00:44:36.104103UTC**. COMMIT and CHECKPOINT passed. The recovery
  process completed, peak0.8838577271GiB. No recovery process remains active.

Actual archive8 dataset UUID, which the next companyfacts resume must use:

```text
17ac14e8-f2f4-4dbf-91c3-fe55be5ab480
```

Do not use the activation run name as a dataset UUID or resume from the older
archive7 UUID `404f66a2-655b-4611-a7a0-b71d4dc6d4d3`.

Measured retained warehouse state after stop:

| Surface | Retained state |
| --- | ---: |
| `sec_company_facts` | 47,006,241 rows |
| `fundamental_points` | 47,006,241 rows |
| `equity_daily_bars` | 31,959,271 rows |
| `custom_features_daily` | 31,934,514 rows |
| Applied migration | 0319 |
| Archive8 committed attempt | 589,436 rows / 229 CIKs |
| Attempt CIK range | 0001442836 through 0001458704 |
| Archive8 source outcomes | 229 loaded, 1,261 empty, 42 unavailable, no recorded source errors |

Attempt rows include replacements, not net warehouse additions. Archive8 first
verified **8,590 retained targets / 37,059,541 rows**, across five lineage runs,
at00:30:44.930UTC. It replayed the retained prefix and began new work by00:37.
Last progress log at00:42:48.421UTC was10,100/20,390 members; the recovery
counts above include commits after that log line. Never use the log count as
the exact durable high-water mark; verified resume derives it from receipts.

Evidence under `.superpowers/sdd/tier1-parity/`:

- `activation-companyfacts-archive8-user-stop.json`
- `activation-companyfacts-archive8-memory.json`, `.log`, `.err`
- `companyfacts-archive8-user-stop-recovery.json`
- `close_companyfacts_archive8_user_stop.py`
- `archive8-user-stop-recovery-memory.json`, `.log`, `.err`

## Objective and process that remain binding

Finish a full production DuckDB US-equity warehouse: PIT standardized
statements, all core ratios/growth/per-share/quality/accrual/leverage/payout
families, daily prices/market cap/EV/multiples/returns/momentum/volatility,
survivorship-safe membership and delistings, deterministic published datasets,
and custom signals supported by proper forward-return decile evidence.

The user's latest development method is: ask what an institutional equity
long/short desk needs, write the SQL expected to answer it, execute it, measure
the gap, and implement a generic repair. Prioritize usable production outputs
over isolated niche work or repeated test loops. Preserve missingness and
lineage rather than fabricating values or statistical significance.

- Codex models only, no Claude work or LLM API spend. The originally requested
  literal commit trailer remains:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Fresh implementer per task, one independent review. Re-review Critical only;
  Important repairs accepted on implementer report. Focused tests once; full
  non-slow suite once at the gate. Whole-branch review uses Codex.
- One heavy workload. Production DuckDB1GB, one thread, Windows process-tree
  guard2GiB. Do not raise limits. Preflight physical and commit free must each
  exceed cap+2GiB; runtime stops its own job below1.5GiB physical/3GiB commit.
  Preserve backups (`--backup-keep 100`). No other-app kills, pagefile changes,
  full-table pandas, or unbounded Python collections.
- Shared tree: no stash/checkout--/reset/restore/clean. Pathspec-only commits;
  registry/jobs/activation serialized. Include owned migration bodies with
  their imports. Do not touch the other session's atx-engine/atx-impl work.
- SEC User-Agent is only `atx-db/0.1 atx-research@example.com`. Never send the
  user's email externally; do not dump old dataset `params_json` (it may
  contain old contact data). Bulk archives/cache before individual requests.
- Snapshot stays **2026-09-20**, cutoff **22:00UTC**. Do not silently advance
  dates to today's date.
- Ask before merging main. No merge, first release, measured gate flip, or
  alpha claim has happened. **Remind user about `stash@{0}`**, their other
  session's work; do not apply/drop it.
- Graph discovery tools were unavailable; `rg` fallback was used. Windows
  PowerShell5.1; interpreter `C:/atx/atx-db/.venv/Scripts/python.exe`.

## Completed since the previous handoff

Original five in-flight handoff groups, S3T8 retirement, S4T10 retirement and
S4T11 docs/CI repairs were completed earlier. Historical details and commit
IDs are in `program.md`, the sprint ledgers and `continuation-queue.md`.

- VR1 `fd2738c7`: verified candidate-only reuse recycles every100 members;
  raw/empty committed replacements remain every10. Ownership, source identity,
  raw fingerprints, resume proofs and transactions unchanged. Five initial
  focused selectors passed; the sixth passed after correcting only fixture
  replacement-count expectations. Scoped Ruff passes. Four strict-mypy errors
  exactly match isolated baseline. Original Critical review finding is closed.
- `216fd7dd`: CVX source/warehouse gap audit.
- `f62727d2`: executed EPS SQL/evidence, corrected production briefs and
  archive8 start checkpoint.
- `48be8414`: quant-desk acceptance documents and readouts. One static review,
  two Important reporting repairs accepted on root's fix report, no Critical.
  Original executed SQL/result frozen; successor/readout not yet executed.
- Earlier production capacity, availability and annual-path repairs through
  `cd841f26`/0319 are committed: bounded statement/standardization/derived/
  market/forward publication, annual-only fallback, 201 metric definitions,
  conservative SEC filing-date clock. Full-universe runtime remains to prove
  their capacity and coverage; focused tests are not that proof.

## First desk acceptance: CVX quarterly EPS growth

[Public case and executed-query evidence](../../../atx-db/docs/CVX_EPS_ACCEPTANCE.md)
and [desk query index](../../../atx-db/docs/QUANT_DESK_ACCEPTANCE.md).

| Quarter | GAAP diluted EPS | Prior-year quarter | YoY |
| --- | ---: | ---: | ---: |
| Q2 2026 | 6.11 | 1.45 | +321.3793103448% |
| Q1 2026 | 1.11 | 2.00 | -44.5000000000% |
| Q4 2025 | 1.39 | 1.84 | -24.4565217391% |

The read-only query at00:18:09.931497UTC found Q1/Q2 raw inputs and computed
the first two figures, but **all three stored derived results were NULL**.
Neither direct Q4 CompanyFacts EPS input exists. Last measured entire
statement-points, standardized and derived tables were zero; their activation
has not run since that observation. The unresolved nonempty CIK owner does
not itself prevent materialization.

Correct metric: `eps_diluted_q_growth_yoy` (`q`), item1035/`eps_diluted`.
`eps_diluted_growth_yoy` is TTM. FY2025 EPS6.63 minus9M5.27 gives1.36, which
is not reported Q4 EPS1.39. Existing per-share residual prohibition is correct.

The source owner for CVX is
`SEC-COMPANYFACTS-UNRESOLVED-CIK-0000093410`; its3,621 price rows through
Sep18 use `SEC-CIK-0000093410`. Current identifier history beginsSep20; do
not backdate it to earlier filings or certify historical security attachment.
A current ticker-to-issuer lookup can legitimately select historical issuer
content with a separate content cutoff.

- Frozen `quarterly-eps-acceptance.sql` SHA256:
  `fe886f41543f7bf42261dec61096a6935f2c608ee7e447e1ca6379b90c7af262`.
  Result: `quarterly-eps-acceptance-result.json`; guard peak0.466GiB.
- `quarterly-eps-acceptance-v2.sql` fixes the publication-status label so a
  valid release-sourced quarter need not have a raw CompanyFacts counterpart.
  Prepared, **not executed**; retain old SQL/result and use fresh output paths.
- Root additionally located the actual SEC Q4 exhibit:
  [accession0000093410-26-000019, EX99.1](https://www.sec.gov/Archives/edgar/data/93410/000009341026000019/a12312025ex9918-k.htm).
  Attachment1 has explicit three-month vs annual columns and diluted1.39/1.84.
  Summary also has prior-quarter and adjusted EPS columns: a useful real
  generic-parser case, not permission for issuer-specific extraction.

## Preserved draft work: do not treat as integrated

All paths below are under `.superpowers/sdd/tier1-parity/`. None of these
drafts were applied to production source, tested, imported, or run on the DB.
Interrupted agents must not be assumed to have finished their current edits.

### Issuer query surface

- Agent `issuer_content_query_implementation`; draft
  `issuer-content-query-draft/`, original `integration.patch` and report.
- New issuer API/catalog/asof helpers: separate lookup/content clocks, actual
  source-owner discovery, direct CIK reads, preserve derived NULL states.
- `issuer-content-query-review.md` has **one Critical**: a reused owner ID can
  expose another CIK's derived rows because derived has no CIK column. Prove
  each owner belongs exclusively to the requested visible CIK, or exclude it
  and return `ambiguous_owner_cik_collision`.
- Important: explicit unqualified market-association metadata; multiple
  directory candidates must not choose an arbitrary security; first_reported
  shares contract; focused tests for distinct table/clock paths. Minor: uniform
  empty envelope and immutable lineage fields. Reject malformed CIKs rather
  than letting `lpad` truncate them into another identifier.
- Original implementer was **interrupted during fixes**. No
  `integration-final.patch` or fix report was present at stop. Original patch
  is not approved; inspect partial mirrored files, finish, then re-review only
  the Critical issue. Accept Important repairs on report and run focused tests
  once under the guard. Do not assume the old patch includes partial repairs.
- Known separate producer gap: derived groups by security_id; the same CIK
  changing owner across quarters can split growth histories. Measure affected
  CIKs after materialization; the query adapter does not fix that.

### Reported-quarter EPS source (0320)

- Agent `reported_quarter_eps_implementation` completed isolated draft
  `reported-quarter-eps-draft/`: integration.patch, implementation-report.md,
  source-contract.md, source/CLI/migration/tests.
- Source rows: `SEC 8-K Item 2.02 reported earnings release`, exact CIK,
  `GAAP` / `EPS_DILUTED` / `USD_PER_SHARE`. Raw source owner currently uses
  `cik_security_id`, which differs from CompanyFacts' unresolved owner;
  downstream exact-CIK alignment is necessary. Source ownership is not
  historical market linkage.
- Candidate discovery from bulk submissions; bounded SEC index/EX99 adapter,
  cache and receipt evidence; explicit month/week duration needed. Preserve
  prior-quarter comparatives, exact period boundaries and all source vintages.
- 0320 reserved to this source task: additive receipt table plus original
  acceptance timestamp string on future submissions loads. Existing completed
  submissions use conservative filing-date+46h; no forced metadata reload.
- Daily clock is max(filing-date+46h, later qualified availability), explicitly
  conservative. Never invent exact UTC from naive timestamps or equate
  acceptance with dissemination. No two-year production ceiling or LLM parsing.
- Fresh reviewer `review_reported_eps_source` was **interrupted before a final
  review report existed**. Restart a fresh static review before integration.
  Check real multilevel tables (including the SEC exhibit above), all quarter
  columns, byte/memory/request bounds, immutable receipt/resume/transaction
  behavior, bootstrap/catalog/pins and source-bulk-resume compatibility.
- Report lists only standalone CLI: governed activation/jobs integration is
  not proven complete. Resolve that explicitly; do not mistake source-only
  facts for a completed production EPS metric.

### Reported EPS core bridge (0321)

- Agent `reported_eps_core_bridge` was **interrupted during implementation**.
  Partial mirrors in `reported-eps-core-bridge-draft/atx-db/src/atx_db/`:
  `reported_eps_core.py`, `fundamental_statements.py`,
  `_standardization_set_based.py`, `_derived_pit.py`, `derived_metrics.py`,
  `migrations/bodies_0321.py`. No final patch/report/review/tests yet.
- Naive source union is insufficient: statement points rebuild from fact
  revisions and standardization handles source vintages separately. Build
  coherent direct-quarter EPS states with both source lineages and clocks.
  Earlier release is visible first; later equal direct filing may supersede;
  visible disagreement over0.005 must produce conflict/unavailable, not a
  silent winner or average. Do not fabricate FY-minus-YTD EPS.
- **Root authorized0321**: minimally allow nullable
  `fundamental_standardized.value` and matching fresh-bootstrap/catalog/schema
  pins, with existing lineage/exception evidence if sufficient. Existing NOT
  NULL constraint cannot represent a later conflict; exceptions alone leave
  stale valid EPS in derived metrics. Prefer the existing table/engine over a
  parallel state store. Extra physical status columns need a checked positional
  INSERT/consumer compatibility plan; not yet approved as necessary.
- Ownership includes narrowly needed `_derived_pit.py`/`derived_metrics.py`
  changes. No rawCompanyFacts/fingerprint mutation. **0320 source task retains
  registry ownership first;0321 only body draft until serialized registration.**
- Check all readers rank NULL/unavailable states before valid filtering and
  coverage measures usable values. Root static observation: item_coverage
  already filters `value IS NOT NULL` after revision rank; provider_coverage
  needs inspection for standardized-specific row counts. Avoid broad refactors.
- Finish implementation, fresh review, focused integrated fixture proving
  validEPS -> conflictNULL -> derivedgrowthNULL, plus ordinary CF behavior,
  exact-CIK isolation, release timing, fiscal durations and repeat cleanup.

## Production sequence on explicit resume

The source load remains critical path. Draft repairs can proceed in parallel
only on disjoint files while the single guarded loader runs. No tests or DB
probes concurrently with it. Do not change imported production source in flight.

From `C:/atx/atx-db`, after checking no writer and fresh filenames:

```powershell
& C:/atx/atx-db/.venv/Scripts/python.exe C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py `
  --job-gb 2 `
  --receipt C:/atx/.superpowers/sdd/tier1-parity/activation-companyfacts-archive9-memory.json `
  --stdout C:/atx/.superpowers/sdd/tier1-parity/activation-companyfacts-archive9.log `
  --stderr C:/atx/.superpowers/sdd/tier1-parity/activation-companyfacts-archive9.err `
  -- C:/atx/atx-db/.venv/Scripts/python.exe scripts/warehouse_activate.py `
  --db-path data/warehouse.duckdb --as-of-date 2026-09-20 `
  --only companyfacts_load --companyfacts-symbol-source archive_members `
  --companyfacts-replace-existing `
  --companyfacts-resume-from-run-id 17ac14e8-f2f4-4dbf-91c3-fe55be5ab480 `
  --memory-limit 1GB --threads 1 --backup-keep 100 --force `
  --run-id activation-companyfacts-archive9 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

Then:

1. Complete all20,390 companyfacts members, inspecting actual receipts, errors
   and terminal status. Obtain the new actual dataset UUID if interrupted.
2. Resume all-form submissions from failed dataset UUID
   `04cf947d-53bb-49b7-a276-b3c74a2a52c8`, batch50, same pinned archive, all CIKs
   and history. Do not add `sec_bulk_download`. Complete that source.
3. Integrate reviewed drafts serially, focused verification/commits and
   governed migrations with backups. Source/bridge availability may require an
   explicit earnings-source invocation before the statement-points suffix;
   do not skip materializing existing useful fundamentals while waiting for
   theoretical source completeness. Record actual source coverage/gaps.
4. Full activation-run5 from `--start-stage statement_points --force`,
   `--shards 16` (sequential partitions), 1GB/one thread under2GiB guard.
   Watch logs/activation_stage_runs; actual failures become bounded tasks.
5. Evaluate the existing CF1 build `custom-features-build1` after forward labels
   exist. Primary horizon21, secondary5/63. First planned evaluation ID
   `custom-features-evaluation1`; snapshotSep20, run-atSep20T22UTC. The evaluator
   is not an activation stage. No evaluation/statistical alpha has run yet.
6. Run v2 EPS acceptance and
   `atx-db/sql/research/custom-feature-decile-acceptance.sql` (all8 hypotheses
   across3 splits), with fresh immutable outputs. Readout is reviewed/fixed,
   prepared but not executed. Do not select only winning hypotheses.
7. Measure item/provider/all-quality checks, publish real numbers, regenerate
   DATA_DICTIONARY after schema/catalog settle. Flip conditions only on measured
   thresholds. Assess gates before `publish_release`; CLI does not enforce all
   quality eligibility automatically and has no `--created-at` flag.
8. Publish first real release, verify manifests/hashes; full non-slow suite once,
   Codex whole-branch review, then ask before main merge; remind stash@{0}.

Detailed command templates are in
`.superpowers/sdd/tier1-parity/production-resume-sequence-2026-09-21.md`;
its archive5/6 live-state paragraph is historical and superseded here.

Updated price source is already published: staged
`atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet`,
SHA256 `0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`,
prices throughSep18. Do not redo old June data. CF1 has31.9M causal feature
rows but no forward-return evaluation. Historical US-common membership,
class/identifier history, exact delivery vintages, adjustment economics and
complete terminal-event coverage remain unqualified. No new local
TickerDefinitionHist source was found; do not invent it or repeatedly ask
the already-pending source question without new need.

Four old content-empty/EOL-only tracked changes remain unrelated:
`migrations/_runner.py`, `migrations/bodies_0280.py`,
`tests/test_migration_governance.py`, `tests/test_migrations.py`. Preserve them.
Other-session atx-engine/atx-impl work and stash@{0} remain untouched.

## Copy/paste goal prompt for the next parent

```text
/goal

Continue the atx-db Tier-1 parity program on feat/tier1-parity. Read
docs/superpowers/handoffs/2026-09-22-tier1-parity-status-and-next-goal.md first;
then the original 2026-09-20-tier1-parity-handoff.md, .superpowers/sdd/tier1-parity/program.md
and its S3/S4 ledgers. The latest handoff supersedes stale live-run notes.

Objective unchanged: full production PIT DuckDB for the entire listed US equity
universe, standardized fundamentals, all core ratios/growth/per-share/quality/
accrual/leverage/payout families, daily valuation/returns/risk, survivorship and
delistings, deterministic code and published datasets. Build useful proprietary
signals with measured forward-return decile evidence, without LLM API spend.

Use desk questions -> executable SQL -> measured gaps -> generic fixes. CVX
quarterly reported diluted-EPS YoY is the first acceptance case: Q2 2026
+321.3793103%, Q1 -44.5%, Q4 2025 -24.4565217%. Raw Q1/Q2 exist, raw direct Q4
does not, and core materializers have not run. Never subtract FY/YTD EPS.

Resume companyfacts from actual stopped archive8 dataset UUID
17ac14e8-f2f4-4dbf-91c3-fe55be5ab480 using fresh archive9 receipts. Archive8
was intentionally user-stopped; session43304 is terminal, ledgers recovered
and checkpointed, 47,006,241 facts retained. No writer or subagent was left
running. Preserve verified resume source/ownership/fingerprint contracts.

Finish preserved issuer-query Critical/Important repairs, fresh EPS-source
review/repairs (0320), and the interrupted core EPS bridge (0321 nullable
standardized invalidation state). These are isolated drafts, not applied or
runtime-tested. Registry/jobs/activation edits are serialized;0320 before0321.
Fresh Codex implementers/reviewers only; no Claude work. One review, Critical
re-review only, Important accepted on fix report. Focused tests only until gate.

Protect memory: one heavy workload, production DuckDB1GB/one thread inside
the existing2GiB Windows job guard. Do not increase limits, kill other apps or
change pagefile settings. Draft work can run in parallel on disjoint files;
tests/DB probes wait until the writer is terminal. No live imported-code edits.
Bulk inputs/cache first, only dummy SEC contact atx-research@example.com;
never expose or send the user's email.

Complete source loads, all-form submissions verified resume, full activation-run5
from statement_points --force with16 sequential shards, CF1 evaluation, actual
item/provider/quality measurements, dictionary, first publish_release and
manifest verification. Snapshot2026-09-20/cutoff22UTC remains explicit. No
coverage-condition flips, historical eligibility or alpha claims without evidence.

Shared tree: no git stash/checkout--/reset/restore/clean; pathspec-only commits
with trailer Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>. Preserve
other-session work and backups. Full non-slow suite once at sprint gate, Codex
whole-branch review, then ask me before merging main. Remind me about stash@{0}.
Otherwise continue autonomously without confirmation loops.
```
