# Task 6 offline checkpoint (continuation below supersedes initial kernel checkpoint)

Historical initial status: NEEDS_CONTEXT / implementation incomplete at 5da989cb.
Agent model: inherited controller model; GPT-6.1-Sol was requested, runtime identifier is not independently exposed.
No real-data reads, scans, builds, evaluations or acquisitions performed.
No production factor/risk stage is claimed. Parent must not accept this checkpoint
as satisfying full Task6 implementation or fixture results as real gates.

Built: new factors.py/risk_inputs.py; bounded offline formation/regression kernels;
exact explicit release manifest/output SHA checks; streaming prepared-input CLI;
immutable generation/publish-last JSONL; full-window benchmark gate function;
documentation and intentionally non-registerable stubs. No data landed.

Test command from C:/atx/atx-db:
`$env:PYTHONPATH='C:/atx/atx-db/src'; $env:OPENBLAS_NUM_THREADS='1';
.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_alpha_panel_factors.py
tests/test_alpha_panel_risk_inputs.py` => 15 passed, exit 0.

| Criterion | Status | Measured | Command |
|---|---|---|---|
| Offline boundary/PIT/missingness/SHA/regression checks | PASS | 15 tests | pytest above |
| Mkt .99/SMB .90/UMD .90/HML .85/RMW .75/CMA .75 | FAIL unmeasured | 0 real months evaluated; 120 required | Real command deferred under code-only phase |
| q IA/ROE .75 | FAIL unmeasured | 0 real months evaluated; 120 required | deferred |
| Exposure coverage .99 on universe | FAIL unmeasured | 0 real cells; full denominators pending | deferred |

Incomplete engineering/source gaps: native lake source adapters, independently
verified original share economic basis (CIK77159 discrepancy), historical universe
eligibility, correct fixed formation carry, q EG model and independently reviewed
q definitions, complete style construction, FF49/liquidity/borrow source joins,
production Parquet, publisher locking/resumable bounded units, production CLI
gate measurement/manifest validation. Prepared input certification assertions do
not establish any underlying source truth. No acceptance denominator is shortened.

For a tiny guarded schema-only read once producer slot is free: obtain Parquet
schema names (no value reads) for final prices_history files, universe output,
compustat_std_v11 funda/fundq, ratios_v11 fund_state, market/liquidity and borrow
proxy/classification; exact final manifest paths/hashes from controller. Need
source-authoritative histories/units and dates, not numerical-fit inference.

Self-review: no upstream/shared/foreign path edited. Snapshot verification is
streaming, but immutable inputs/exclusive producer slot remain necessary. Existing
modules are unchanged. Complete source definitions must be frozen before any real
return comparison; current CONFIG is an initial offline proposal and is not yet
approved as the full literature formula freeze.

## Continued implementation checkpoint

Status: DONE_WITH_CONCERNS for code/offline implementation checkpoint, pending independent
review. This is not Task6 dataset acceptance and does not authorize a real build.
The earlier kernel-only checkpoint was not accepted; the native machinery below
supersedes its missing-adapter/publication statements.

### Implemented

- Explicit native Parquet adapters for prices_history/current prices, dated universe,
  final compustat_std_v11 fundq/funda and ratios_v11 state; liquidity, borrow, FF49,
  calendar, RF and benchmarks. Each requires exact manifest/output SHA/size identities;
  all final v11 bindings are explicit, old stage defaults rejected.
- Daily/monthly own MktRF, FF5 2x3, monthly UMD/STREV/LTREV formation carry,
  independent q 2x3x3 and q5 expected-growth WLS forecast machinery, original BAB
  lookback/shrinkage/rank weights. Source absence yields nulls and explicit failures,
  never current-membership backfill or a reduced comparison window.
- EG fits stream one monthly <=20,000-issuer sample at a time over 120 months,
  require 30 fits, 1/99 predictor winsorization, ME weights, complete independent
  predictor and realized-outcome availability clocks at the historical fit cutoff,
  and four-calendar-month outcome seasoning. Future outcomes cannot enter a fit.
- Per-line 252-session CAPM/FF5 exposures, idiosyncratic risk, FF49, all eight
  styles, ADV/spread and explicit public borrow pack. Post2022 regressions and
  return-derived styles remain missing under the controller-confirmed GC2 reading;
  current nonreturn primitives can publish. Full 2020-2025 denominators remain required.
- Bounded Parquet units, file-backed DuckDB 350MB/one thread, 1024-row Arrow batches,
  deterministic source/config/code generations, output-SHA verified unit resume,
  exclusive publisher lock and publish-last single-generation pointer. No existing
  producer or shared registry was modified. CLI requires the memory-guard environment.
- Benchmark CLI with explicit source columns/units/hashes and exactly 120 required
  months 2013-2022, strict pre2023 filtering before own aggregation/statistics,
  source/generation/code lineage, and separate validation evidence.
- Independent cap/membership audit source contract requires reviewed manifest status,
  exact audited share/universe/history producer hashes, primary evidence hashes and
  per-row source/evidence clocks. Boolean caller assertions do not certify inputs.
  CIK77159 remains excluded unless that independent audit explicitly resolves it.

Historical 92fd8f8e frozen artifact: `atx-db/docs/datasets/factors.config.json`; canonical sorted-JSON semantic SHA256
`7b948fa14ee2684915fe20c6fa6379123d13da320cc78471b9f7680683095578`.
Exact primary literature references and formula/source deviations are recorded in
the artifact and factors.md. No actual return comparison preceded the freeze.

### Authorized metadata inspection

The controller granted exactly one schema-only read under a 0.3GiB guard. Command:
`.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py
--job-gb 0.3 --wait-minutes 30 --receipt ../.superpowers/sdd/tier1-v4/task6-schema-guard.json
-- .venv/Scripts/python.exe ../.superpowers/sdd/tier1-v4/task6-schema-inspect.py`.
Exit0, peak0.0402GiB, 1.0s guarded child execution; slot released immediately.
Evidence: task6-native-schemas.json and task6-schema-guard.json. Footer schemas only;
no rows, values, return computations or source-acceptance measurements read.
Inspected six prices_history files, nine price/universe/liquidity/borrow files,
one existing funda/fundq/ratios-state/industry file each. These existing producer
schemas are provisional for final v11 interfaces, not approved producer snapshots.

### Offline tests

26 owned offline tests passed cumulatively: 15 initial boundary/PIT/missingness/
snapshot/regression tests plus 11 native tests. The native file exercised visible
annual and quarterly vintage selection, independent cap qualification, holdout
boundaries, immutable resume/locks, original BAB minimum history, EG future-label
rejection, all120 missing-history monthly outputs, latest nonreturn risk publication,
exact benchmark mapping/missing-month rejection and actual final-release adapter setup.

Commands from C:/atx/atx-db with PYTHONPATH=C:/atx/atx-db/src and
OPENBLAS_NUM_THREADS=1:

- `pytest -n 0 -q tests/test_alpha_panel_factors.py tests/test_alpha_panel_risk_inputs.py`
  =>15 passed at initial checkpoint.
- `pytest -n 0 -q tests/test_alpha_panel_factor_native.py` =>10 passed before adding
  the final source-interface fixture.
- `pytest -n 0 -q tests/test_alpha_panel_factor_native.py -k 'native_setup or native_risk
  or eg_training or quarterly or annual'` =>5 passed on current adapters; the last
  source-interface fixture made the cumulative native total11. Relevant earlier
  generation/benchmark focused subset =>6 passed. No repository-wide suite run.

### Acceptance gates (not claimed from fixtures)

| Criterion | Status | Measured | Command/evidence |
|---|---|---|---|
| Offline engineering/PIT/publication/interface behavior | PASS | 26 cumulative tests | owned pytest commands above |
| Schema inspection <=0.3GiB, no data values | PASS | 0.0402GiB peak, exit0 | exact guarded metadata command above |
| French Mkt .99 / SMB .90 / UMD .90 / HML .85 / RMW .75 / CMA .75 | FAIL unmeasured | 0 real comparison months;120 required | future guarded factors --phase validate; no authorization yet |
| q IA/ROE .75 | FAIL unmeasured | 0 real comparison months;120 required | same deferred validator |
| Exposures >=.99 of full universe/year cells | FAIL unmeasured | 0 real measured cells; all six years required | future guarded risk_inputs native build |

Engineering evidence:2 PASS; real acceptance:3 grouped FAIL/unmeasured. No correlation,
coverage or formula-equivalence gate is waived or weakened.

### Remaining real source/measurement dependencies

Final Task2 universe and final Task5 v11/std/ratio manifests/output hashes do not yet
exist. Existing universe sources are stale and cannot be accepted. Historical price
schemas lack CIK, dated membership, certified shares and availability fields; dated
universe currently begins2018. No latest-membership splice is permitted for2013-2017.
Inspected funda has SALE but no REVT: original EG cash-flow predictors remain missing
without independently documented equivalent source mapping. Preferred-stock redemption/
liquidation and quarterly deferred-tax primitives are absent; book-equity fallbacks
are documented and remain source-equivalence limitations. Original announcement-clock
ROE is conservatively delayed to actual available statement clocks until announcement
actual values are a certified source. Price/delisting/vendor-vintage quality, genuine
cap-source basis, and source/benchmark unit mappings need independent certification.
Neither CIK77159 share series is presumed correct; audit ownership stays with controller.

Latest regression completeness will FAIL under strictGC2; missing own factor history
cannot be forward filled or replaced with benchmark returns. Actual native memory,
build rows/bytes, historical coverage, correlations and yearly completeness must be
measured under a newly authorized exclusive guarded slot after producer completion.
Task17 must resolve manifest-selected generation files when folding registration stubs.

### Files and self-review

Owned source: factors.py/risk_inputs.py plus narrowly named _factor_native.py,
_factor_sources.py and _factor_io.py. Owned tests: three test_alpha_panel_factor/risk
files. Docs: factors.md/risk_inputs.md, both registry stubs and frozen config.
Report/metadata/scratch artifacts remain under tier1-v4. No raw data landed, no upstream,
foreign, shared registry, controller ledger, branch/worktree or unowned process changed.
Self-review corrected the initial permissive prepared-publication API by removing it,
added exact audit-source evidence binding, preserved fixed formation labels, avoided
daily long-short compounding, and included delayed-component clocks in derived states.
Independent review should assess scientific/source-equivalence limitations before real
comparisons; fixture correctness is not gold certification.


### Fix round 1/5 — review-T6-prebuild-findings.md (supersedes prior code assessment)

Status: DONE_WITH_CONCERNS, offline code checkpoint ready for independent review;
no real source reads, return jobs, fit/evaluation, HTTP acquisition or acceptance.
All six Important findings addressed in owned Task6 sources:

- I1: complete eligible line/session grids precede compounding; absent bars remain
  NULL; duplicate/overlapping selected prices reject; monthly signals require daily
  support; historical BAB market retains missing eligible lines and monthly BAB holds
  formed members. Delisting/source uncertainty is not converted to liquidation returns.
- I2: audited previous-session ME and matching qualification feed issuer-consolidated
  size/value; universe observation available_at also strictly precedes cutoff;
  SI/ratio/FTD/threshold strict individual clocks; return history ends at
  prior session and own factors require strict availability before its cutoff.
- I3: generation identity now binds complete selected file lists/parts, RF adapters/
  mappings, all release and audit/share/membership bindings, config and code.
- I4: own factors require complete matching producer release, selected-source, config
  and current code lineage before use and again before dependent publication.
- I5: shared latest-visible audit selection precedes status checks in formation,
  returns, EG fiscal/training ME, BAB and risk; revoked/failed replaces old pass;
  equal-clock ambiguity rejects. Missing issuer lines do not get silently omitted.
- I6: independent expected all_common/r3000p/member_equity grids left-join output;
  absent output cells remain denominator; each universe/year2020-2025 tested; missing
  calendar-day universe or explicitly unavailable member_equity yields FAIL.

Minor M1 corrected: hashes are semantic versus physical. The old92fd8f8e semantic
hash was7b948fa14ee2684915fe20c6fa6379123d13da320cc78471b9f7680683095578;
review reported oldLF-normalized artifact bytes0d559fbe513571ddb8f61325b50a7c137f5a96b43188e82a8c2b1edeac64bb4a.
New `task6-frozen-v2-review1` artifact committed before any real return comparison:
canonical sorted-JSON semantic SHA256
`b5b512372b318be2a23e51413f41a0d21c63795af11d48d4ccb49100d25f7f58`;
physical LF artifact byte SHA256
`668de8d4770daf08e47e9e22f09abe99c56414ee591f78130194330865fc7a6b`.
CRLF checkout bytes may differ; semantic and physical LF hashes are distinct contracts.

Focused verification from C:/atx, PYTHONPATH=C:/atx/atx-db/src and
OPENBLAS_NUM_THREADS=1; all fixture connections128MB/one thread/UTC:
`atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts= -p no:cacheprovider -q --tb=short atx-db/tests/test_alpha_panel_factor_native.py -k 'review or native_risk or native_setup or annual or quarterly'`
14 PASS,7 deselected,9.68s. Native risk test invokes actual argument-parsing CLI with
tiny synthetic registered relations and fixture guard marker, not production sources.
During fixture development a clock-boundary check exposed fixture DuckDB local timezone; fixture now
explicitUTC like production common.connect; corrected test and focused set passed.
No unchanged broad suite rerun and no live child jobs remain.

Real gates remain3 grouped FAIL/unmeasured,120 benchmark months fixed, coverage years
and eligible denominators intact. Final qualified release (controller's future explicit
versioned universe binding/R15), audited economicbasis/delisting/history, genuine
member_equity source, EG primitive/training gaps and production-memory measurements
remain independent dependencies. This change does not accept unsafe Task2 checkpoints
or certify synthetic audit flags/source membership as external evidence.
