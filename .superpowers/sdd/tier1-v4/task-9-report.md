# task-9 report: announcement-clock earnings (calendar v2, EX-99 actuals, SUE family)

Status: **STOPPED (controller / owner request) during orientation.** Nothing built, nothing fetched, no code written.

The statement above is the historical orientation checkpoint. The authoritative
recovery checkpoint is appended below; new code is tested but not materialized.

## Stopped at

**Done (read-only orientation):**
- Read IMPLEMENTER.md, task-9-brief.md, global-constraints.md, gap-analysis §2 item 7 (binding spec),
  tier1-v3 LANE-RULES.md, foreign-dirty-files.txt, tier1-v3 task-EVT-report.md, progress.md, task-3/10/16 briefs.
- State checks (read-only, 2026-10-02):
  - `atx-db/data/alpha_panel/v1/_tmp/events/fetch_fpi.txt` exists (1.0 MB, 12,138 URLs per the EVT report). It was
    never fetched.
  - `atx-db/data/raw/sec_events/fetch-ledger.jsonl` has 17,103 lines. That is the EVT lane's SEC count. The spec
    says 25,762/60,000 used across lanes, and the cap is `ATX_EVT_SEC_CAP`.
  - `data/alpha_panel/v1/earnings_calendar_v2/` does not exist yet (never built).
  - `earnings_calendar_v2.py` (359 lines) and `events_sources.py` (639 lines) are clean (no uncommitted edits).
    `ea_actuals.py` does not exist.

**Not done (whole deliverable):**
1. FPI 6-K fetch (`fetch_fpi.txt`, ≤ 4 req/s), catalog rebuild, calendar v2 parse and build. Gate:
   `member_coverage[year].covered_share` ≥ 0.95 for 2020+, FPIs included.
2. New `alpha_panel/ea_actuals.py`: parse revenue, diluted EPS and net income from the EX-99 corpus
   (`data/raw/sec-earnings-release`, 1.3 GB), with available_at = 8-K acceptance time. Derived columns:
   - `sue_eps_ann`: actual minus the seasonal random walk, scaled by the std of the prior 8 surprises.
   - `sue_rev_ann`.
   - `ea_lag_days`: rdq to the 10-Q.
   - `ea_ret_3d`: a feature only; no IC or return statistic ≥ 2023.
   Also tests, `docs/datasets/ea_actuals.md` and `ea_actuals.registry.json`.
3. Gates not measured:
   - actuals ≥ 97% precise on 200 releases vs the later 10-Q (within 1%). This needs a manual row-by-row read.
   - recall ≥ 85% of 2.02 releases 2020-2025.
   - SUE on ≥ 90% of member cells.
   - calendar covered_share (gate 1 above).
4. Re-measure Task 3's rdq share against calendar v2 (same-ET-date rule), per year 2020-2025. This is reporting
   only; compustat_std.py is not to be edited.

**Uncommitted files:** none (this report is the only file written, and it is committed).

**Running processes I started:** none (no PIDs).

**Exact next command to resume** (from `C:/atx/atx-db`, `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`):
first confirm the shared SEC budget with the controller (other lanes hit SEC). Then run the FPI fetch, which is
resumable and skips ledger URLs:

```
.venv/Scripts/python.exe -m atx_db.alpha_panel.events_sources fetch data/alpha_panel/v1/_tmp/events/fetch_fpi.txt
```

Then follow the EVT-report resume steps 2 and 6:
1. `G ... events_sources catalog`
2. `earnings_calendar_v2 --phase parse`
3. `G ... earnings_calendar_v2 --phase build`

After that, write `ea_actuals.py` (tests first).

## Gates

| criterion | status | measured | command |
|---|---|---|---|
| calendar v2 covered_share ≥ 0.95 / yr 2020+ (FPIs incl.) | NOT RUN | — | — |
| actuals ≥ 97% precise, 200 releases vs 10-Q (1%) | NOT RUN | — | — |
| actuals recall ≥ 85% of 2.02 releases 2020-2025 | NOT RUN | — | — |
| SUE on ≥ 90% of member cells | NOT RUN | — | — |
| Task 3 rdq share re-measured vs calendar v2 | NOT RUN | — | — |

## Tests

None run (no code written).

## Files changed

- `.superpowers/sdd/tier1-v4/task-9-report.md` (this file).

## Sources / disk

None landed. SEC requests used by this task: 0.

## Recovery prebuild checkpoint — 2026-10-03

**Status: PREBUILD_CHECKPOINT / NEEDS_REVIEW.** Controller requested a tested code
commit and independent review before any full corpus job. No complete real
actuals/calendar v2 stage has been published, no return values from real data
were calculated, and no HTTP allocation was granted or consumed. Existing
earnings_calendar outputs/manifests remain read-only. This checkpoint does not
certify data coverage, precision, production readiness or consumer acceptance.
Controller selected the GPT-6.1 Sol implementation lane; no model override or
child agent was created by this agent. The prescribed GC6 legacy co-author
trailer is not a claim about the runtime model.

### Retained work and source accounting

Retained work was the AttemptLimiter addition in events_sources.py, its offline
budget tests and Task9 inventory/probe/plan scratch artifacts. ea_actuals.py was
absent; earnings_calendar_v2.py was initially clean. Existing inventory reported
52,397 successful EX-99 units, 17,103 sec_events ledger lines and 12,138 planned
FPI URLs, all initially uncached by URL. Those are historical inventory leads,
not a new complete body-integrity certificate.

Conservative shared accounting: **25,762 baseline + 2,998 Task2 + 398 Task14 +
1,127 Task4 upper + 65 Task3 nominal + 2 Task5 actual = 30,352**, before a
separate 10-attempt Task5 reservation. The original 60,000 limit was scoped to
the events ledger, not all historical SEC requests; the earnings-release ledger
has 108,494 older entries. Historical retries/redirects remain uninstrumented.
The later shared all-attempt ceiling cannot be certified from ledger counts;
its uncertainty remains an explicit unresolved dependency. No proposed lane
allocation increases the global ceiling.

Fresh guarded cross-store receipt audit: **0 matching FPI accessions, 0 retained
full-body objects** across scanned SEC receipt/ledger stores. sec_text retains
only 10-K/20-F sections and diagnostics, not original full bodies. Submission and
header metadata cannot substitute for release bytes. This is receipt/cache
evidence, not proof that no unindexed cache exists anywhere on disk.

Fresh cached probe: **20 documents, 0 formally qualified monetary tables**.
Selection: original 8-K documents with filing dates 2020-2025, ordered by
DuckDB hash(accession || 't9-probe-v1'), first 20 catalog rows. It was not the
200-release precision sample and did not restrict item 2.02; several source
documents concern other events. Evidence retains accession, document SHA,
acceptance class, extraction result, opening text and table context in
`t9-recovery-probe.json`. The low initial yield identifies parser qualification
limitations (currency declarations, mixed quarter/YTD geometry and source
selection); it is not an estimate of true source absence. This initial probe
predates later source-period and detailed rejection-reason improvements.

### Implemented behavior

* New ea_actuals parser reads cached EX-99 objects only, verifies their content
  hashes and emits a missing-aware row for every original 2.02 filing since 2015.
  Explicit quarter/date column geometry, monetary scale/currency, GAAP labels
  and conflict checks qualify cells. Unknowns remain NULL with reasons.
  Source period extraction is independent of actual-value qualification.
* Revenue SUE uses the seasonal random walk and sample standard deviation of
  eight prior visible surprises, excluding the current surprise. Latest-visible
  vintages are selected at each row's own cutoff. EPS is EPSFI-style total diluted
  EPS (`epsfiq`), not continuing-operations EPSFX. A current comparative does not
  qualify the share basis of eight older EPS surprises: EPS SUE stays NULL with
  `eight_prior_eps_share_bases_unqualified` until a source-backed split/share
  bridge is supplied. This is an implementation/source dependency, not a waiver
  of the >=90% SUE gate.
* New calendar v2 preserves v1 physically. An 8-K's fiscal period comes from
  release evidence, never a later 10-Q. Latest fiscal-year-end profiles cannot
  resolve historical fiscal labels. Unknown acceptance clocks cannot promote
  primary announcements, timing or expected dates. FPI document conflicts stay
  missing and each examined exhibit retains hash/classification evidence.
* ea_lag_days is in a separately clocked filing_lags product, visible only at the
  later matching original 10-Q acceptance. ea_ret_3d is separate, mapped using
  identity at reaction session, and visible at the third session at 22:00 UTC.
  All source price partitions and rows end before 2023 before any arithmetic;
  post-2022 or incomplete windows remain NULL with a holdout/missing reason.
* A stable releases_manifest.json prevents calendar -> actuals feature manifest
  cycles. Every producer input is checked for complete status and every required
  output SHA/size. Final audit refuses stale v10 standardized inputs; it requires
  fundamentals_v11 plus final compustat/universe publications. It retains Task3's
  original first-reported member 10-Q denominator and reports same-ET-date and
  strict timestamp shares using source-backed primary 8-K 2.02 only.
* Per-document parse checkpoints are keyed by exact source SHA, source metadata,
  code hashes and source manifest/catalog identity. Complete JSON units publish
  atomically; incomplete units are never reused. Bounded --limit-new-documents
  passes do not publish incomplete stages and can resume without reparsing
  complete units. Whole-corpus publication/memory behavior still needs a guarded
  real-data measurement; no full parse has been launched.
* Frozen 200-release selection is lowest SHA256 of
  cik/accession/t9-precision-200-v1 across original 2.02 releases 2020-2025,
  independent of extraction yield. Missing candidates remain in the sample.
  Manual original-release/later-10-Q review is **0/200 completed**.
* Prepared, unrun FPI pilot has at most 100 deterministic URLs, a 250-attempt
  ceiling, 128 MiB cumulative decoded-body reservation and 64 MiB cumulative
  gzip reservation. Bodies are capped at 512 KiB; reservations include one
  64 KiB read overshoot and gzip worst-case overhead. Explicit redirects count
  separately; automatic retries/redirects are disabled. Reservations persist
  before issuance and interruptions overcount safely. Every response gets a
  receipt and hash. No wholesale 14,000-attempt acquisition is authorized.

### Evidence and exact commands

All commands below ran from C:/atx/atx-db with
PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1.

```powershell
.venv/Scripts/python.exe -m pytest tests/test_alpha_panel_ea_actuals.py tests/test_alpha_panel_ea_actuals_pipeline.py tests/test_alpha_panel_events_sources_budget.py -q -n 0
```

Final result: **23 passed**, approximately 9 seconds total. One inherited
fetch_record_batch deprecation warning, no failures. Tests cover losses/scales,
period and currency ambiguity, non-GAAP/conflicting rows, unknown clocks,
seasonal surprise cutoff invariance, durable attempt/byte bounds across a
redirect and resume, complete unit reuse/input changes/incomplete rejection,
partial publication refusal, and the end-to-end offline fixture pipeline.

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t9-recovery-cache-audit.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t9-recovery-cache-audit.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t9-recovery-cache-audit.err.log --quiet -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t9-recovery-cache-audit.py
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --receipt ../.superpowers/sdd/tier1-v4/logs/t9-recovery-probe.receipt.json --stdout ../.superpowers/sdd/tier1-v4/logs/t9-recovery-probe.out.log --stderr ../.superpowers/sdd/tier1-v4/logs/t9-recovery-probe.err.log --quiet -- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/t9-recovery-probe.py
```

Both returned 0, cap_hit=false. Cache audit: 16.0 seconds, 0.0382 GiB native peak;
probe: 4.0 seconds, 0.0951 GiB native peak. Only Task9 scratch/receipt files were
written; its controller-granted brief data slot was released immediately after.

| Binding criterion | Status | Current evidence | Required measurement |
|---|---|---|---|
| Calendar >=95% member issuers/year 2020+, FPIs included | FAIL / unmeasured | 0 new complete publications | New calendar build and final audit |
| Actuals >=97% precise, full 200 releases vs later 10-Q within 1% | FAIL / unreviewed | 0/200 manual reviews | Frozen precision_sample.parquet and independent row-by-row source audit |
| Recall >=85% of 2.02 releases 2020-2025 | FAIL / unmeasured | No full releases publication | ea_actuals --phase build |
| SUE >=90% member cells | FAIL / unmeasured | EPS historical share-basis qualification unresolved | ea_actuals --phase audit on final universe |
| Task3 rdq >=95% same ET date, strict share alongside | FAIL / stale producer dependency | Final v11 not yet available | ea_actuals --phase audit after final Task5 |

**Binding gates: 0 PASS / 5 FAIL or unmeasured**, without threshold or denominator
waivers. Offline checks are engineering evidence, not substitutions for these
real-data gates. Source budget accounting is still unverifiable historically.

### Next concrete commands and dependencies

Wait for independent prebuild review and the controller's next exclusive data
slot. Then begin a bounded cached-unit pass, not an untested whole-corpus job:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ea_actuals --phase parse --limit-new-documents 100
```

Use unique receipt/stdout/stderr names for every subsequent run. Cache units
invalidate if code or exact input identities change; preserve older units and
failed receipts. After bounded real-data resource/resume evidence, finish parse,
build calendar v2, then actuals features. Final coverage and Task3 rdq audit must
wait for quiescent final Task2 universe and final Task5 fundamentals_v11 plus
compustat publication, with all required output hashes checked. Task5 initially
continues to consume the unchanged earnings_calendar v1. New calendar rdq refresh
is a separate explicitly bound final measurement, not an in-place calendar swap.
No HTTP pilot may run without a controller-issued allocation and disposition of
the unresolved shared historical accounting.

### Owned changes, limits and self-review

Production: alpha_panel/{ea_actuals,earnings_calendar_v2,events_sources}.py.
Tests: test_alpha_panel_{ea_actuals,ea_actuals_pipeline,events_sources_budget}.py.
Docs/stubs: datasets/{ea_actuals,earnings_calendar_v2}.{md,registry.json}.
Task9-only scratch producers: t9-recovery-cache-audit.py and t9-recovery-probe.py.
Evidence retained in Task9 scratch/logs; no raw data is committed. No foreign
paths, shared controller ledger/HANDOFF, registries, event writer from another
task or existing earnings_calendar publication was edited. No owned process is
running at checkpoint. Network/raw landing: 0 attempts, 0 response/landing bytes.

Self-review caught and fixed a DuckDB reserved alias in reaction SQL, EPSFX vs
EPSFI naming, an unknown-clock filter with nullable vintage risk, the parse/feature
manifest dependency cycle, whole-corpus retry limitations and missing historical
EPS share-basis qualification. Remaining review risks: strict parser recall,
mixed table geometry/source basis, full-build bounded memory/publication resume,
frozen manual precision and final-producer audit SQL on real schemas. Do not
certify these from offline fixtures alone.
