# Backtest Framework Wave B (listed_dispersion_pipeline) — SDD Progress

Controller: Claude (session b8ae4870)
Repo root: C:\atx. Branch: main, in place (user-authorized).
Plan: docs/superpowers/plans/2026-07-23-atx-vol-backtest-framework-wave-b.md
  (incl. Controller decisions section — created_ts_ns from identity hash,
  M1 trim at assemble_reconciliation_snapshots, no schema bump, T7 also
  mirrors 86f2210 fsync-before-rename in write_run_archive_file)
Design spec: docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md
Sprint review: docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md
Wave A ledger (committed): .superpowers/sdd/backtest-wave-a/progress.md
Base commit at Wave B start: 6e3af60

Implementers/reviewers: Opus 4.8 subagents.
Commit trailer: Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>

Batching (ONE C++ build slot, build-rel Release):
  B1 = T1 (pipeline module foundation, C++ slot) ∥ T8 (python hardening)
  B2 = T2 (M1 fix)      B3 = T3 (schedule build extraction)
  B4 = T4 (projection + I1 parity)   B5 = T5 (dispersion_book_var)
  B6 = T6 (column single-source) → T7 (created_ts_ns + fsync + count-gate)
  B7 = T9 (thin-CLI cutover)   T10 = controller gate
  Reviews: fresh Opus reviewer per batch, raw diff packages via
  `rtk proxy git diff BASE..HEAD > .superpowers/sdd/review-BASE..HEAD.diff`.

Constraints: explicit-path commits ONLY (never -A/-u/.); ONE build at a time;
never touch C:\atx-data (controller only); golden fixtures untouchable
(SP\paired, SP\t2-check, SP\t6-post, SP\t7-check, wave_a_fixture.atxrun,
sha256 71ea9632…29f7424); schema-format frozen (golden 0xdcce47781ac8390d,
registry change = new golden + kRaMinor bump — NOT this wave); parquet.dll
needs build-rel\bin on PATH; full gtest MUST run from C:/atx/build-rel CWD
(stale repo-root artifact-cache). Known pre-existing red (do NOT chase):
BoundaryHoist.PriceBitIdenticalToPrechange, SurfaceV2Qualification
Latency/Balanced (t10-failure-triage.md).

Gate targets (T10): 3-session fixture final_nav=-456.5769067 (dates=3
rolls=1); parity-full dates=135 rolls=7, listed NAV 125026.0592,
projected-cold 123243.1172, corr 0.9972, mark_divergence rows=0; dump
golden hashes a05470c7… / cbabca44… / b640b3ab… / d6793d46….

## Task ledger
B1 kickoff: T1 implementer (pipeline module foundation, owns C++ build slot)
  ∥ T8 implementer (python reader hardening, pytest test_runarchive.py only).

T8 (python hardening): implementer DONE (commit 12a6e4c; test_runarchive.py
  25→33 passed; fixture sha256 unchanged). RED-first only for #12 close()
  _fh leak + #13 non-utf8 → ValueError; #10/#11 forge tests are regression
  locks (reader already raised clean ValueError — probed). close() now
  releases _fh in finally, retains _mm for live views, idempotent. +2 extra
  tests (minor-too-new gate, close-retry). Review in flight
  (package review-wb-t8-12a6e4c.diff).

T1 (pipeline foundation): implementer DONE (commit 4d12d96; 3/3 new
  ListedDispersionPipeline tests + 32/32 regression). RED via missing-header
  compile error. Deviations: (1) seams take MarketSnapshot (load-constructible,
  needs ts_ns()) not design-spec SurfaceSet — code reality supersedes §4.4;
  (2) fingerprint test also pins 51/60/3/40 defaults + occ_ess_authority
  sensitivity; unknown-uid fail-closed asserted. Review in flight
  (package review-wb-t1-4d12d96.diff).
B2 kickoff: T2 M1-fix implementer (owns C++ build slot; trim at
  assemble_reconciliation_snapshots per controller decision #2) ∥ T1 reviewer
  (read-only, no builds).

T8 review VERDICT: Spec ✅ / Quality Approved (task-8-review.md). 0 Critical,
  0 Important. Empirically verified: 33 passed, fixture sha256 exact, commit
  touches only 2 intended files, close() Windows-safety proven by live-view
  test (mmap dups handle). T8 CLOSED.
  Minor roll-up: (1) #10/#11 tests locks-not-RED (disclosed, branch-precise
  regexes); (2) #13 wrap scoped to _string_table — section/column-name
  decodes (:401,:475) still raise raw UnicodeDecodeError; (3) redundant
  _recompute_header_crc in version tests (version gate precedes CRC gate).

T1 review VERDICT: Spec ✅ / Quality Approved (task-1-review.md). 0 Critical,
  0 Important. Lift fidelity char-level verified (constant, quotes fn, both
  closures byte-identical; capture narrowing safe; methodology literals
  confirmed at example :317/:439/:532; fingerprint covers 11 admission + 6
  methodology fields). MarketSnapshot supersession authorized + required.
  T1 CLOSED.
  Minor roll-up: (1) core_min_names_per_roll{40} has NO example literal —
  inert in T1; T3 MUST NOT silently activate a new <40 gate (CARRIED into
  T3 brief); (2) fingerprint sensitivity test omits admission sub-fields +
  query_route; (3) "order-independent identity" comment imprecise (fixed-
  order key; property is padding independence); (4) fixed-name test temp dir
  not parallel-safe (codebase pattern).

T2 (M1 fix): implementer DONE (commit bb0e744; 37/37 regression, 2 new
  tests; low-level ListedDispersionReconciliation* 4/4 untouched). Trim at
  assemble_reconciliation_snapshots per decision #2;
  reconcile_listed_dispersion UNCHANGED (defensive invariant). RED = link
  error on declared-undefined seams; RED anchor
  ReconcileClockCoupling_AbortsOnWarmupLeadIn pins defect; equality test vs
  manually-trimmed reconcile. No material deviations. Review in flight
  (package review-wb-t2-bb0e744.diff).
B3 kickoff: T3 schedule-extraction implementer (owns C++ build slot;
  BINDING caution: core_min_names_per_roll{40} inert — must NOT silently
  activate new <40 gate in build path) ∥ T2 reviewer (read-only).

T2 review VERDICT: Spec ✅ / Quality Approved (task-2-review.md). 0 Critical,
  0 Important. Trim boundary exact (subspan(start), first roll kept, pinned
  by test); only-warm-up → clean Err(InvalidArgument), no empty subspan;
  deep-equality preservation proof; reconciliation .cpp untouched (last
  5d104df); T9 drop-in confirmed (example :642-643). T2 CLOSED.
  Minor roll-up: (1) RED anchor matches ErrorCode only (3 sources) — assert
  message string to nail M1; (2) start=0 wrapper path + ≥2-session warm-up
  proven transitively, not asserted; (3) snapshots vector elements borrow
  caller quote storage via span — borrow undocumented in header (public-seam
  footgun); (4) nit: linear first-roll scan fine at ~135 sessions.

T3 (schedule extraction M7): implementer DONE (commit f9fb4e2; 39/39, 2 new
  tests). <40 gate kept INERT per binding caution — accept_listed_schedule
  gate = example's `rolls.empty() || (core_mode && rolls.size() < 3)` only;
  2-names-per-roll ACCEPTED test locks inertness. Disclosed: (2) internal
  hash_archive_file duplicates example hash_file byte-for-byte (T9 removes);
  (3) build-side clock/first-roll coupling enforced but not unit-tested
  (Clock has no public ctor) — deferred to T10. Review in flight
  (package review-wb-t3-f9fb4e2.diff).
B4 kickoff: T4 projection+I1-parity implementer (owns C++ build slot;
  shared ProjectionConfig constant; bit-exact leg-mark two-route test) ∥
  T3 reviewer (read-only).

T3 review VERDICT: Spec ✅ / Quality Approved (task-3-review.md). 0 Critical,
  0 Important. Statement-by-statement match vs example :446-535; <40
  inertness genuinely locked; hash duplication success-path byte-identical;
  M1-check defer-to-T10 correct (false branch structurally unreachable via
  builder, listed_dispersion_schedule.cpp:283). T3 CLOSED.
  Minor roll-up: (1) PhaseTimer not threaded into builder — T9 build_schedule
  diagnostics loses per-phase granularity (FLAG FOR T9 brief); (2)
  hash_archive_file dup (disclosed, T9 removes); (3) ListedScheduleSpec
  field-defaults mirror RunSpec on all 8 fields — inert, harmless.

T4 (projection M6 + I1 parity): implementer DONE (commit 17b1477; 41/41,
  2 new tests). Shared ProjectionConfig{analytic, ColdReference} constant;
  TwoRouteColdParity_LegMarksEqual bit-exact EXPECT_EQ on raw doubles vs
  independent cold recompute; ProjectionConfigColdIsCanonical pins constant.
  Disclosed: per-roll printf stdout diag kept in library fn (T3 precedent);
  defensive null-guard after archive lookup (NotFound semantics preserved).
  Review in flight (package review-wb-t4-17b1477.diff; reviewer told to be
  skeptical the parity test exercises the REAL projected route, not the same
  fn twice).
B5 kickoff: T5 dispersion_book_var implementer (owns C++ build slot; ×100
  through kVegaVolPointToUnitVol) ∥ T4 reviewer (read-only).

T4 review VERDICT: Spec ✅ / Quality Approved (task-4-review.md). 0 Critical,
  1 Important — NOT a T4 defect, a RELAYED T9 OBLIGATION (approval contingent
  on T9 honoring it, gated by T10). T4 CLOSED.
  == BINDING T9 OBLIGATIONS (carry into T9 brief) ==
  (O1) I1 closure: thread shared ProjectionConfig{} (BOTH .execution AND
    .analytic) into the projected-backtest replay RunConfig at example
    :923-983 — replay still hardcodes ColdReference at :952 + engine-default
    analytic; two configs exist until T9. T10 gates via projected_schedule
    hash d6793d46… + mark_divergence rows=0.
  (O2) ListedArchiveLookup: missing roll date → Ok(nullptr) or the example's
    exact NotFound message ("no qualified archive for roll date").
  (O3) Lifetime: ListedArchiveLookup returns borrowed MarketSnapshot* —
    T9 closure must OWN snapshots across the whole project_listed_schedule
    call (UAF otherwise).
  (O4) Thread PhaseTimer into builder diagnostics (T3 minor #1) + remove
    hash_file duplication (T3 minor #2).
  Minor roll-up: (1) T4 report/header language overstates ("impossible by
  construction" — actually value-pin + kernel proxy until T9); (2) library
  fn unconditional stdout diag (impure, harmless); (3) see O2; (4) see O3;
  (5) static_assert canonical pin would beat runtime EXPECT (cheap T9 add).

T5 (dispersion_book_var M8): implementer DONE (commit c2e5463; 42/42, 1 new
  test). Disclosed: (1) added ProjectedMaturitySpec& maturity param — genuine
  input read at :1124, outside :1119-1194 lift range; MUST stay relative
  days(N) template (per-scenario aging); (2) vega ×100 M9 site (:1110) is
  book-building UPSTREAM of lift — stays in CLI until T9.
  (O5) added to T9 obligations: swap example :1110 `* 100.0` →
  kVegaVolPointToUnitVol per T9 checklist (T5 reviewer told to verify T9
  plan owns it).
  Review in flight (package review-wb-t5-c2e5463.diff).
B6 kickoff: T6 column-single-source implementer (owns C++ build slot; frozen
  kBacktestCols + golden hash 0xdcce47781ac8390d untouchable; TSV + archive
  bytes must be identical pre/post) ∥ T5 reviewer (read-only).

T5 review VERDICT: Spec ✅ / Quality Approved (task-5-review.md). 0 Critical,
  0 Important. Fidelity verified statement-level; maturity param necessary
  (DispersionBook has no maturity field — plan signature couldn't compile);
  ×100 ownership verified BOTH ways (:1110 upstream of lift; T9 plan L227/L55
  owns swap). T9 cutover constraints noted: relative days(N) maturity,
  cfg.n_threads=fit_workers, :1110 swap. T5 CLOSED.
  CONTROLLER NOTE → T10: run-projected-var numeric economics unpinned
  Wave-B-wide (parity.py/e2e/goldens all skip projected_var) — capture a
  projected-var golden during T10 gate.
  Minor roll-up: (1) test pins structure only — add free EXPECT_GE(VaR99,
  VaR95)/GE(ES,VaR)/EQ(reference, frames.back()) invariants; (2) test's
  kVegaVolPointToUnitVol exercise symbolic (equals default; doesn't test M9
  swap).

T6 (column single-source): implementer DONE (commit b1cfd16; 44/44 + TearSheet
  6/6). constexpr {name, member-ptr} table backtest_series_columns() consumed
  by tearsheet writer + archive encoder; static_assert pins vs frozen
  kBacktestCols[2..26]; byte-identity proven (MatchesCommittedPythonFixture +
  new TSV⟷archive⟷member triangulation test); golden hash + fixture sha256
  unchanged; registry/_schema.py empty diff. Disclosed: tests in
  run_archive_test.cpp; test-only lists left as independent witnesses
  (anti-circularity); static_assert added atop runtime test. Review in flight
  (package review-wb-t6-b1cfd16.diff).
B6 phase 2: T7 implementer (owns C++ build slot; deterministic created_ts_ns
  from run_identity_hash per decision #1; fsync-before-rename mirroring
  86f2210 per decision #6; negative count-gate test) ∥ T6 reviewer
  (read-only).

T6 review VERDICT: Spec ✅ / Quality Approved (task-6-review.md). 0 Critical,
  0 Important. Diff==commit verified; 25 pairs identical both consumers;
  static_assert no off-by-one; witnesses independent; goldens untouched.
  T6 CLOSED.
  Minor roll-up: (1) run_report.cpp:78 write_backtest_series_csv = 5th
  hand-kept copy of the 25 names (different order, CSV, unguarded) — header's
  "ONLY place" comment overstated; candidate cheap fix in T9 or defer;
  (2) parity test's 3 legs share the member pointer — member-vertex not
  independent witness (real independence via backtest_dbl_cols + fixture
  test); (3) nit: fixture zero-valued columns evade value-swap detection
  (name-order pinned; closed by verbatim-copy match).

T7 (determinism+fsync+count-gate): implementer DONE (commit 9a24e78; 46/46;
  fixture sha256 + golden hash unchanged). RED anchor WriteIsByteDeterministic
  (pre-change wall-clock ns stamp → bytes differ); created_ts_ns now derived
  from run_identity_hash; fsync-before-rename inlined mirroring 86f2210
  (shared primitive lives on unmerged feat/pipeline-c — disclosed);
  VerifyRejectsCountGateMismatch = green lock (gate pre-existed);
  _wfopen→_wfopen_s (/WX). Review in flight (package
  review-wb-t7-9a24e78.diff).
B7 kickoff: T9 thin-CLI-cutover implementer (owns C++ build slot; BINDING
  obligations O1-O5 in brief — I1 replay ProjectionConfig threading is the
  critical one; economics gate final_nav=-456.5769067 + dump byte-identity
  vs t7-check golden + e2e pytest) ∥ T7 reviewer (read-only).

T9 (thin-CLI cutover): implementer DONE (commit 382fee2; 5 files, net -48;
  46/46 filter; full suite 1967/43skip/3 known reds; python 92 incl. new e2e;
  3-session economics EXACT + dump byte-identical to t7-check golden).
  O1-O5 all DONE (I1 closed: one ProjectionConfig authority; grep-verified
  no ColdReference/analytic literals in projection/replay). Disclosed
  library-touching deviations (O4-authorized PhaseTimer* param + additive
  prepared_fingerprint; project-schedule archive_load folded into
  cold_solve).
  Review VERDICT: Spec ✅ / Quality Approved (task-9-review.md). 0 Critical,
  0 Important. All obligations independently verified MET (O1 grep, O2
  byte-identical message, O3 map node-stability, O4 statement-for-statement
  timer parity, O5 both swaps + verbatim error strings); additive-only
  library edits confirmed (no ABI/layout/test break). T9 CLOSED.
  Minor roll-up: (1) project-schedule archive_load phase reads 0/0 (folded,
  non-golden); (2) run-projected-var error path no longer writes partial
  frame/leg TSVs pre-error (healthy path byte-faithful, route unpinned);
  (3) run-projected-var elapsed_seconds spans prepare+evaluate (telemetry);
  (4) run_report.cpp:78 5th column/×100 copy untouched (carried T6 minor).

T10 GATE (controller) — in progress:
  Full gtest (independent rerun, build-rel CWD): 1967 passed / 43 skipped /
  3 failed = exactly the documented pre-existing reds. Python full suite:
  87 passed + 5 e2e ERRORS under concurrent load (exe 0xC0000409 fail-fast,
  empty output, while full gtest + 135-session parity ran simultaneously —
  exe healthy standalone). RESOLVED: isolated e2e rerun = 5 PASSED. The
  errors were a controller scheduling mistake (three heavy suites at once),
  not a code defect. Python gate therefore GREEN: 87 + 5 = 92.
  Parity-full 135-session rerun (fresh t10b state): economics EXACT
  (listed 125026.0592, projected-cold 123243.1172, dates=135 rolls=7,
  corr=0.99718, mark_divergence rows=0); 4/4 golden dump/schedule hashes
  (a05470c7/cbabca44/b640b3ab/d6793d46) — NOTE union archive served the
  listed backtest dump AFTER steps 3-4 (merge-write proven in production
  sequence, no snapshot dance); UNION = 9 sections (+projected_schedule vs
  Wave A's 8 — this run included step 3 under merge-write); validate_all OK;
  parity report renders (153986 B).
  Determinism note: whole-file run.atxrun bytes differ across same-input
  rerun BECAUSE diagnostics section carries wall-clock wall_ms — by design;
  economics sections byte-stable (dump hashes exact across rerun); T7
  guarantee (identical payloads → identical bytes) unaffected.

T7 review VERDICT: Spec ✅ / Quality Approved (task-7-review.md). 0 Critical,
  0 Important. Determinism verified both branches (identity nonzero →
  wall-clock fallback dead; int64 round-trip bit-exact; merge recomputes);
  fsync gate `wrote&&synced&&closed` before rename, temp preserved on
  rename-fail (matches 86f2210); count-gate test trips step 4 specifically;
  ABI intact (offsetof 16); fixture immune (direct writer, pinned ts).
  T7 CLOSED.
  Minor roll-up: (1) determinism test fresh-writes only — merge-path
  untested (sound by construction); (2) inline fsync mechanism differs from
  86f2210 (live-fd _commit vs reopen+FlushFileBuffers) — benign, dedup if
  feat/pipeline-c merges; (3) parent-dir entry not fsync'd post-rename (same
  scope as 86f2210).

## Minor findings roll-up (for final review triage)

Triaged by a fresh Opus reviewer against HEAD (382fee2); full table in
`.superpowers/sdd/backtest-wave-b/minors-triage.md`.

**31 rows -> 7 STALE, 7 FIX-NOW, 16 DEFER** (T9-4 is T6-1 carried, not a
separate finding). The STALE ones were all closed by later Wave B commits:
T1-1/T3-2 by f9fb4e2 + 382fee2, T3-1/T4-3/T4-4 by the T9 obligations O2/O3/O4,
T4-1 by O1 plus the T10 gate.

FIX-NOW, in application order — 3 files, ~6 comment lines + ~15 test lines,
nothing touching goldens, fixtures, schema or economics:

1. T6-1 `backtest_series_columns.hpp:38-39` — narrow the false "ONLY place"
   claim; name `run_report.cpp:78` as a separately-ordered CSV contract.
2. T2-3 `listed_dispersion_pipeline.hpp:109` — document that the returned
   snapshots' `quotes`/`surfaces` stay borrowed from `full_timeline`.
3. T1-3 `listed_dispersion_pipeline.hpp:67` — "order-independent" is wrong;
   the property is layout-independence (padding-free) at fixed field order.
4. T4-5 `listed_dispersion_pipeline_test.cpp:490` — `static_assert` the I1
   parity constant rather than pinning it only at runtime.
5. T2-1 `…test.cpp:334` — assert the M1 message string, not just the
   `ErrorCode` (three call sites share that code).
6. T1-2 `…test.cpp:244` — extend fingerprint sensitivity to an `admission`
   sub-field and `query_route`.
7. T5-1 `…test.cpp:655` — add the free VaR invariants (99% >= 95%,
   ES >= VaR, `reference_value == frames.back().value`).

**`run_report.cpp:78` (the carried T6 minor) — verdict: DEFER the code change,
fix the comment.** The CSV *can* be driven off `backtest_series_columns()`
byte-identically, but only through an explicit permutation: both lists hold the
same 25 names in the same relative order with `nav` displaced from canonical
index 14 to CSV index 1, i.e. `csv == [pnl_total, nav] ++ (canonical \ {pnl_total,
nav})`. Naive direct iteration moves `nav` from CSV field 3 to field 16 and
rewrites the header and every data row. Formatting is not the obstacle (both
writers use `%.17g` / `%lld`; only the separator differs). Deferred because
`run_report.cpp` is untouched by Wave B (not among the 14 diff files), proving
byte-identity needs a build plus the mag7 C++/python suites, and — decisively —
deduping the `.cpp` would still leave three more copies of the CSV-ordered list
(`run_report.hpp:46-55`, `run_report_test.cpp:129-134` `kPinnedHeader`,
`mag7_dispersion_report_test.py:52-57` `SERIES_HEADER`), so the "ONLY place"
claim would remain false regardless. Code dedup is a schema decision for a later
wave.

## FINAL WAVE B REVIEW (fresh Opus, 6e3af60..382fee2, read-only)

VERDICT: Spec compliance ✅ / Code quality **REQUEST CHANGES**.
0 Critical, 3 Important, 7 Minor. Full report: `final-review.md`.
**WAVE B IS NOT CLOSED.**

I1-FINAL (Important #1) — M1 warm-up-lead-in fix does NOT work end-to-end.
  CONTROLLER-VERIFIED against the code before accepting:
  - `spy_dispersion_backtest.cpp:555-559` builds reconciliation_snapshots over
    the FULL clock (clock.size() entries).
  - `:568` reconcile_listed_schedule -> assemble_reconciliation_snapshots
    trims to the first roll date (`listed_dispersion_pipeline.cpp:184`
    `full_timeline.subspan(start)`) -> rows = clock.size() - lead_in.
  - `:569` validate_listed_reconciliation_backtest hard-requires
    `reconciliation.rows.size() != backtest.size()` -> Err
    (`listed_dispersion_reconciliation.cpp:344-348`), and `backtest` still
    spans every clock date (run_backtest emits one row per clock step).
  => lead_in > 0 still aborts, one line downstream, with a WORSE message
     ("invalid tolerance or row count"). `RunDir::verify` repeats the gate at
     `run_archive.cpp:1630`.
  Why T2 stayed green: the test drives assemble_reconciliation_snapshots in
  isolation and never reaches the validator.
  Why T10 stayed green: parity-full has date_lo == first roll, so lead_in == 0
  and the trim path never executes. The controller's Step-3 claim that "M1 is
  exercised in production" was an OVERSTATEMENT — corrected in the plan doc.
  FIX: make both gates date-aligned — require the reconciliation rows to be a
  contiguous suffix of the backtest dates and compare pairwise from that
  offset. Behaviour-identical when lead_in == 0, so no golden moves.

I2-FINAL (Important #2) — ListedArchiveLookup borrow contract changes the
  memory profile: the pre-lift loop freed each MarketSnapshot per roll; the
  borrowed-pointer seam makes the caller retain every roll-date board (full
  heap deserialize, not mmap) for the whole call. Harmless at 7 rolls;
  ~120 boards resident on a multi-year corpus.

I3-FINAL (Important #3) — ListedDispersionMethodology is a THIRD copy of the
  thresholds, not the single authority its header claims: `verify` still uses
  RunVerifyOptions' independent 60/3/40 (`run_archive.hpp:566-568`) and the
  cold route reads ProjectionConfig. 4 of 7 fields dead; worst is
  `query_route` (a ColdReference nothing reads, beside the real authority).

Minor (7): T6 static_assert cannot pin member bindings + the only independent
  oracle uses {0.0,0.0} for 23 of 25 fixture columns; BuildScheduleSymbolIsDeclared
  cannot fail; TwoRouteColdParity_LegMarksEqual is f(x)==f(x) (real I1 gate is
  the python e2e); permanently-zero archive_load diagnostics row; projected-VaR
  failure gate moved ahead of the diagnostic TSV writes (stale artifacts, lost
  evidence); undocumented borrow in assemble_reconciliation_snapshots' return;
  merge-write identity hash covers only run_spec.tsv + universe_schedule.tsv
  (pre-existing at 191e409, outside the diff).

CLEARED by the reviewer (do not re-check): hash_archive_file byte-identical
  (read_text already binary-mode); build-schedule loop + cold projection
  line-for-line verbatim; every literal->policy substitution value-identical;
  `analytic_greeks = true` matches RunConfig's existing default (no economics
  change); rename unreachable unless write+sync+close all succeeded;
  created_ts_ns bits round-trip while ArchiveContentIdentity still
  discriminates content.

## Remaining to close Wave B
1. Fix I1-FINAL (date-aligned suffix in validate_listed_reconciliation_backtest
   + RunDir::verify) behind a RED test that drives the PRODUCTION path with a
   nonzero lead-in — the gap that let this ship.
2. Decide I2-FINAL (borrow contract / memory profile) and I3-FINAL (collapse
   the threshold copies or drop the dead fields and stop claiming authority).
3. Apply the 7 FIX-NOW minors from `minors-triage.md` (~21 lines, 3 files).
4. Capture the `run-projected-var` golden (still the one unpinned route).
5. Re-run the gate, then commit ledger + plan.

## POST-REVIEW FIX ROUND (controller, 2026-07-24)

User directed: fix I1 + the FIX-NOW minors in one build, and fix I2 and I3 now.

**I1-FINAL — FIXED.** `validate_listed_reconciliation_backtest`
(`listed_dispersion_reconciliation.cpp:341`) no longer requires equal row counts.
The reconciliation must now be a contiguous SUFFIX of the backtest matched BY
DATE: locate its first date in `backtest.date`, require it to run to the last
backtest row, then compare pairwise from that offset. A first date absent from
the backtest, or a reconciliation that stops early, is an explicit error. Empty
reconciliation is accepted only against an empty backtest. With a zero lead-in
the offset is zero and the arithmetic is bit-identical to the historical
row-for-row comparison — so no golden can move.
`RunDir::verify` (`run_archive.cpp:1630`) got the SAME semantics, and was
upgraded from a row-count comparison to a real date comparison over the two
sections' `dict_col("date")` (non-empty, no longer than the backtest, tail
matches row-for-row).

  NEW TEST `ListedDispersionPipeline.ValidateReconciliationAcceptsWarmupLeadIn`
  drives the PRODUCTION pair — reconcile_listed_schedule over a full clock
  timeline, then the validator against a full-clock backtest — with a nonzero
  lead-in. That is precisely the gap that let the defect ship: the two existing
  M1 tests stop at the seam and never call the validator. It also pins that
  suffix-ness is still enforced (a backtest extending past the reconciliation is
  rejected; a disjoint date range is rejected) and that the zero-lead-in path is
  unchanged.

**I2-FINAL — FIXED.** Verified in the code that `project_listed_schedule` binds
the borrowed `MarketSnapshot *` to a per-iteration local and never retains it
across rolls (the rolls it emits are plain data: strikes, sizes, greeks). So the
whole-call lifetime the header demanded was never necessary. The header contract
is now "valid until the NEXT lookup call" with the reason stated, and the CLI's
cumulative `std::map` snapshot cache became a SINGLE SLOT that releases the
previous board on each new roll date (`spy_dispersion_backtest.cpp:621`). Peak
resident boards: O(n_rolls) -> O(1).

**I3-FINAL — FIXED by removal.** All four dead fields deleted from
`ListedDispersionMethodology`: `admission`, `core_min_names_per_roll`,
`query_route`, `occ_ess_authority`. None was read by any consumer — they were
folded into `policy_fingerprint()` and nowhere else — and the `admission`
default did NOT match the production `CorpusAdmissionRule` the CLI builds inline
at `spy_dispersion_backtest.cpp:340-353`, so it was an active trap. The struct is
now exactly the three floors a consumer reads (`min_names_entry` 51,
`core_min_dates` 60, `core_min_rolls` 3), and the header states its scope
explicitly plus the two places that deliberately keep their own copies:
`RunVerifyOptions` (60/3/40, because the result store must not depend on the
listed route — they must be changed together) and `ProjectionConfig` (the cold
route's asserted parity constant). Fingerprint key bumped v1 -> v2. SAFE: the
methodology fingerprint is persisted NOWHERE — the corpus `policy_fingerprint`
the CLI stamps is an unrelated `hash_text` literal (`:358`) — so no stored bytes
move.

**FIX-NOW minors applied:** #1 backtest_series_columns.hpp "ONLY place" claim
narrowed, naming run_report.cpp:78's separately-ordered CSV contract and the
exact permutation; #2 assemble_reconciliation_snapshots borrow documented;
#3 "order-independent" -> layout-independent at fixed field order;
#4 `static_assert` on the I1 parity constant (compile-time, not just runtime);
#5 M1 abort test now asserts the message string, not just the shared ErrorCode;
#7 VaR invariants (ES >= VaR, VaR/ES monotone in confidence, reference ==
frames.back().value) — each verified against `historical_projection.cpp:119-149`
before being asserted, not assumed. #6 (fingerprint sensitivity for admission
sub-fields + query_route) is OBSOLETED by the I3 removal.

Build: clean (exit 0), atx-vol-tests + atxvol_spy_dispersion_backtest.
Targeted gtest `ListedDispersion*:RunDir.*:RunArchive*:Tearsheet*`: 74/74 PASSED.

RED PROVEN for the I1 fix (the discipline whose absence let the defect ship):
  the pre-fix equal-row-count gate was temporarily restored, rebuilt, and
  `ValidateReconciliationAcceptsWarmupLeadIn` FAILED against it with
  "InvalidArgument: TEMP RED PROBE: row count" — so the new test is a real gate,
  not a tautology. Probe removed and rebuilt clean afterwards.

POST-FIX-ROUND GATE (controller):
  Full gtest from C:/atx/build-rel CWD: 3 failed = EXACTLY the documented
  pre-existing reds (BoundaryHoist.PriceBitIdenticalToPrechange, the two
  SurfaceV2Qualification budget tests). No new failure from I1/I2/I3.
  Targeted `ListedDispersion*:RunDir.*:RunArchive*:Tearsheet*`: 74/74 PASSED.
  Parity re-gate on parity-full (steps 2-4; step 1 skipped — nothing in the fix
  round touches build_listed_dispersion_schedule and its trade_schedule golden
  already stood): economics EXACT — `backtest complete: dates=135 rolls=7
  final_nav=125026.0592`, `projected backtest complete [cold]: dates=135 rolls=7
  final_nav=123243.1172`.
  GOLDEN HASHES 4/4 EXACT after the fix round:
    dump backtest --tsv        a05470c7 (expect a05470c7)
    dump projected_cold --tsv  cbabca44 (expect cbabca44)
    trade_schedule.tsv         b640b3ab (expect b640b3ab)
    projected_schedule.tsv     d6793d46 (expect d6793d46)
  => I1's date-aligned gate, I2's single-slot snapshot cache and I3's field
     removal are value-preserving, as designed.

## Wave B status after the fix round
All 3 Important findings CLOSED. Remaining, carried out of the wave:
  - `run-projected-var` economics still unpinned (no golden ever captured).
  - The 16 DEFER minors in minors-triage.md, incl. the run_report.cpp:78 CSV
    dedup (a schema decision, not a cleanup).
  - The Python test-suite restructure is UNVERIFIED and deliberately
    UNCOMMITTED (see the plan doc's "Carried out of Wave B").
