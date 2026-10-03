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

## Prebuild fix round 1/5 — scoped review checkpoint

**NEEDS_REVIEW; I1-I5 and M1 addressed in code/fixtures, subject to independent
rereview.** No HTTP, real-data job, actual corpus publication or real return
calculation ran in this round. Binding gates remain **0 PASS / 5 FAIL or
unmeasured**, manual precision remains **0/200**, and historical all-attempt
accounting/allocation remains unresolved. Existing earnings_calendar v1 is
unchanged. M2 resource evidence remains a required controller-coordinated guarded
100-document experiment; these fixtures do not establish a full-corpus bound.

| Finding | Correction | Focused evidence |
|---|---|---|
| I1 mixed metric currencies | Enforce one currency across all accepted current/comparative candidates; mixed currencies NULL all monetary values and currency with reason | Two separate tables: USD sales only, CAD income/EPS only; formerly mislabeled case now fails closed |
| I2 arbitrary eight older surprises | Require explicit source quarter/year and fiscal-year-end declaration, compatible quarter-end month, exactly the preceding eight quarter identities, no missing surprise, consistent calendar and 75-105-day consecutive end spacing | Quarterly positive control; annual-only March history rejected; missing quarter and changed calendar rejected; same-clock exclusion and cutoff invariance retained |
| I3 first-exhibit comparative wins | Order-independent value/basis equivalence, including every seasonal comparative and fiscal/EPS qualification; compatible evidence union sorted; conflicts NULL affected inputs with reasons; conflicting comparative cannot fall back silently | Both exhibit orders yield identical output; identical actual/different comparative keeps current value but NULLs comparative; incompatible basis fails closed |
| I4 stale input identities overwrite fresh ones | Current feature producer identities remain current; immutable parse lineage and original parse inputs are separate; changed SEC snapshots and calendar/parse mismatch reject build | Changed price producer feature rebuild publishes its new hash and unchanged parse lineage; changed SEC snapshot rejects before computation |
| I5 stale phase/in-place rebuild | Unique immutable generation directories; phase-specific explicit product sets; atomic manifest activation; consumers reject incompatible phase/product; pending failures never replace a complete generation; rollback checks old feature and parse outputs before activation | Parse-after-build advertises only releases/sample and rejects feature lookup; injected interruption after private writes leaves old manifest byte-identical; old outputs retain hashes; explicit rollback restores old complete generation and pinned parse alias |
| M1 stale FPI scratch | Complete receipt pins classifier, code LF hashes, catalog hash, output SHA/size and exhibit evidence; checked before row admission and before activation | Obsolete classifier receipt rejects calendar build despite readable scratch parquet |
| M3 deprecated iterator | v2 uses to_arrow_reader | Final fixture suite has no warnings |

Parser version is now ex99-qualified-quarter-v2. Quarter identity is never
manufactured from price/future-filing dates or the latest fiscal profile. An
explicit quarter label without a qualified historical calendar stays unknown.
The eight-quarter rule can lower measured SUE coverage; the acceptance threshold
is unchanged and any resulting coverage failure remains a failure. Historical
EPS share-basis qualification is still unresolved and is not inferred here.

Publication helper: `alpha_panel/ea_publication.py`. Products live under
`<stage>/generations/<id>/`. Root manifest.json activates one complete generation
and carries explicit products/phase mappings. releases_manifest.json separately
activates an immutable parse generation. Feature manifests pin that generation's
immutable manifest path/hash and retain original parse inputs separately from
current feature inputs. Validation retains its own audit lineage without
relabelling the copied feature outputs' original input lineage. Every generation
is retained; incomplete `_pending` directories are never admitted. No cleanup or
deletion of prior generations was introduced.

Paths now resolve with `ea_publication.file(stage, product, manifest_name)` and
phase checks via `ea_actuals.verify_input`. Task17 integration must bind the
resolved product path and honor required_phases in the updated registration
stubs; it must not use a root-level parquet path or glob all generations.
Rollback is `ea_publication.activate(stage, 'generations/<id>')`, under the guard
and controller slot for real products. It restores the complete immutable
generation and its pinned parse alias without rewriting old data. Producer
changes during feature/calendar builds are checked again before activation.

Exact covering command, from C:/atx/atx-db with the established environment:

```powershell
.venv/Scripts/python.exe -m pytest tests/test_alpha_panel_ea_actuals.py tests/test_alpha_panel_ea_actuals_pipeline.py tests/test_alpha_panel_events_sources_budget.py -q -n 0
```

Final output: **27 passed**, no warnings, approximately 4 seconds. These are
offline fixtures only. Initial covering run exposed old test assumptions about
root parquet paths and unqualified quarter identity; those expectations were
updated to exercise the declared generation/quarter contracts, including a
positive quarterly control. The final end-to-end fixture includes the changed
producer, interruption, stale phase, rollback, incompatible SEC input and stale
FPI receipt cases. No full suite or external data job was run.

Owned changed paths: ea_actuals.py, new ea_publication.py,
earnings_calendar_v2.py, the two ea_actuals fixture files, dataset docs/stubs and
this report. events_sources.py remains at the prebuild checkpoint. No foreign
files, shared controller files/registries, existing calendar publication or other
event writer changed. No owned process is running at this checkpoint.

Remaining evidence: M2 real peak/resume measurement, frozen 200-release manual
source audit, full actuals recall/calendar/SUE measurements, source-qualified EPS
history bridge, quiescent final Task2/Task5 producers and Task3 rdq refresh.
Independent review of these fixes is required before bounded materialization;
this implementer does not waive any of those requirements or grant acquisition.
### Guarded bounded cache experiment — 2026-10-03

### Fix round 2/5 — source-backed quarter geometry, pending independent review

The additional controller-granted cache-only context read was bounded to five originals from the completed 100 units, selected as the first five sorted cache units with mixed-duration or missing-currency table rejections. It inspected seven exact tables, retaining physical colspans/rowspans, preceding captions, headline, accession, raw SHA and canonical source URL constructed from accession identity (landed_v2 catalog URL is NULL; URL was not requested). Evidence: `t9-r2-context.py`, `t9-r2-context.json`, and `logs/t9-r2-context-20261003.{receipt.json,out.log,err.log}`. The 0.3 GiB native guard completed exit 0 in 5.1 seconds, peak 0.1164 GiB, cap_hit false. Slot released before code work.

Exact examples: AAR accession `0001104659-18-018959`, EX99 `a18-8706_1ex99d1.htm`, raw SHA `86d00c784be3dcbde44a79227554a1bd5abf808e1cb99c35d1b23445e3531c98`, table 0, shows a five-column Three Months Ended February 28 span above separate 2018/2017 two-column year bands, alongside an independent Nine Months span. AAR `0001104659-20-136867`, `tm2038572d1_ex99-1.htm`, table 8, has six-column quarter versus six-month spans with separate 2020/2019 bands (full raw SHA retained in context artifact). Abbott `0001104659-15-052323`, `a15-15970_1ex99d1.htm`, table 11, has the caption Second Quarter Ended June 30, 2015 and 2014, explicit `2Q15`/`2Q14` two-column bands, then a separate percent-change column (full SHA retained in artifact). These show parser geometry limits; they do not establish source currency, fiscal calendar or extraction precision.

General correction: `quarter_columns` binds split month/day and year cells through explicit enclosing quarter-duration spans and excludes YTD spans. Explicit caption quarter/year pairs may bind matching abbreviated year tokens. Contradictory dates, overlapping date bands, unsupported rowspan, absent/crossing duration spans and percent columns remain excluded. Value assembly permits split dollar/parenthesis cells but requires exactly one digit-bearing cell, rejecting concatenation of distinct amounts. Currency rules remain unchanged: inspected originals with bare `$` still produce NULL values. Fiscal-calendar identity rules remain unchanged: missing historical source declaration stays missing. No later profile/calendar or inferred currency is introduced. Bounded partial receipts now reverify SEC manifest/output and catalog identities before admission; drift rejects the receipt without publishing a stage.

Original-shaped fixtures cover AAR split quarter/YTD geometry and Abbott quarter abbreviations/percent exclusion, with explicitly synthetic USD positive controls separated from source-shaped bare-dollar negatives; wrong comparative year/quarter, misaligned duration spans, overlapping/conflicting dates, multiple numeric fragments and rowspan exclusions are retained. Focused command: `atx-db/.venv/Scripts/python.exe -m pytest atx-db/tests/test_alpha_panel_ea_actuals.py atx-db/tests/test_alpha_panel_ea_actuals_pipeline.py atx-db/tests/test_alpha_panel_events_sources_budget.py -q -n 0`, with PYTHONPATH pointing to src: 30 tests PASS, no warnings. Owned-path `git diff --check` passes; unrelated foreign whitespace is not changed. This is engineering evidence only; no real materialization rerun after code changes. Earlier 100-unit run key refers to pre-round-2 code and cannot certify this revision. Independent review is required before another materialization experiment. Gates remain 0/5 PASS; manual precision remains 0/200.

### Pre-round-2 bounded measurement details

Controller granted one exclusive cache-only slot with unchanged 0.6 GiB native cap. No HTTP, full parse, feature build, return calculation or stage publication occurred. The approved parse100 and immediate zero-new-unit resume both exited 0; no live child remains and the slot was released.

Commands from `C:/atx/atx-db`, with `PYTHONPATH=C:/atx/atx-db/src` and `OPENBLAS_NUM_THREADS=1`, used `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 60 --quiet --receipt <receipt> --stdout <stdout> --stderr <stderr> -- .venv/Scripts/python.exe -m atx_db.alpha_panel.ea_actuals --phase parse --limit-new-documents <limit>`. Unique receipt/stdout/stderr stems under `logs/` are `t9-r1-parse100-20261003` (limit 100) and `t9-r1-resume100-20261003` (limit 0).

First pass: 100 new complete source units, 21.1 seconds, native peak 318,849,024 bytes (0.297 GiB), cap_hit false. Resume: 100 cache hits, 0 new units, 13.1 seconds, peak 299,978,752 bytes (0.2794 GiB), cap_hit false. Both return `partial_cache_only`, `published:false`, identical run key `6d995da4afd51d81da1a3d1b1bfda72684a28c0c85bf6d22edc73284bbbedb81`, SEC manifest SHA `5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693`, catalog SHA `a76d19d7458481fcd67de4f7397617234f34b98843b2449c291628564690bf64`. The bounded cache identity verifies source/code/input identity; this establishes reuse and measured bounded-prefix memory, not full-corpus or feature memory bounds. Partial return does not perform the final publication-time producer recheck, because it publishes nothing.

The 100 document units cover 100 accessions and only CIKs 1750, 1800, 2034, selected by existing issuer/acceptance/accession order. They are a cache prefix, not the frozen 200-release manual denominator or a representative release-recall sample. All 100 return `no_qualified_quarter_table`, NULL financial values, and `fiscal_quarter_or_calendar_unqualified`. Independent source period qualification succeeds on 76; 13 lack an explicit period and 11 have ambiguous periods. Rejection entries include mixed-duration columns and missing/ambiguous currencies, alongside many layout/non-GAAP/quarter-duration exclusions. Counts of table rejections are multiple entries per document and cannot be interpreted as document-level cause attribution. Exact original/table-context investigation is pending a short guarded source read; no source gap or correct-extraction precision can be inferred from this prefix.

M2 now has actual 100-unit resource/reuse evidence, but full-corpus resident collections and feature memory remain unresolved. Final Task2 universe remains stale, Task5 fundamentals_v11 is absent, gates remain 0/5 PASS, frozen manual precision remains 0/200, and historical all-attempt accounting/network allocation uncertainties remain binding. Existing calendar outputs remain immutable.

### Fix round 3/5 — duration ancestry and percentage intervals

Read the full independent `review-T9-prebuild-r2-findings.md`; round 2 was not approved. Both Important findings are addressed in the smallest shared header-admission helper. `quarter_columns` now retains YTD and percentage/change/growth/decline header intervals alongside quarter durations. Crossing or overlapping quarter/YTD spans reject the grid; date bands must not cross a quarter parent boundary or intersect a YTD/percentage interval. The same admission check applies to full dates, split year dates and caption-bound abbreviated quarter tokens, including tokens underneath multilevel percent headers. Adjacent disjoint quarter/YTD spans remain supported. Parser semantic version advances to `ex99-qualified-quarter-v4`; earlier actual cache evidence does not certify v4.

Regression fixtures reproduce the exact reviewer 900-million overlapping-duration case and 2.2-million percentage-as-actual case, then assert NULL financial values. Additional negatives cover conflicting ancestry above a nonintersecting child date, same-cell percentage/change/growth labels, multilevel percent spans with full dates and quarter abbreviations. A disjoint percentage span positive control retains separate legitimate current/comparative monetary bands. Existing source-shaped AAR/Abbott positive controls and source-shaped bare-dollar negatives remain passing. No currency/fiscal-identity/duration requirements were weakened and the frozen 200 sample was not changed.

Validation from C:/atx with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1: `atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_ea_actuals.py atx-db/tests/test_alpha_panel_ea_actuals_pipeline.py atx-db/tests/test_alpha_panel_events_sources_budget.py -q --tb=short` — **36 passed in 12.65 seconds**, no warnings. Owned-path whitespace check passes. Only owned parser/tests/report files are changed.

Operational state: no data slot requested or used, no real source/product reads or scans, no HTTP/acquisition, no full parse/features/returns, no stage or existing-calendar publication. This is engineering fix round **3 of the 5-round cap**; independent review is required before operational action. Actual gates remain **0/5 PASS**, frozen manual precision **0/200**; full resource bounds, source-qualified EPS bridge, final producer integrity/rdq refresh and historical source-allocation uncertainty remain unclosed.
