# Backtest Framework Wave A (RunArchive) — SDD Progress

Controller: Claude (session b8ae4870)
Repo root: C:\atx. Branch: main, in place (user-authorized).
Plan: docs/superpowers/plans/2026-07-21-atx-vol-backtest-framework-wave-a-runarchive.md
Spec: docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md
Review: docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md
Base commit at start: 66200ca

Implementer model: Fable 5 (user-directed). Parallel batches only where file sets
disjoint; ONE C++ build at a time (build-rel, Release).

Batching:
  B1 = T1+T2 (schema registry + ABI structs, one agent)
  B2 = T3+T4 (writer + reader, one agent)
  B3 = T5 (encoders + commit python fixture .atxrun)
  B4 = T6 (diagnostics, C++) PARALLEL T8 (python reader, no C++ build)
  B5 = T7 (RunDir)
  B6 = T9 (hard cutover) ; T10 = controller gate

Commit trailer (Fable implementers): Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Constraints: explicit-path commits ONLY; never touch C:\atx-data; never modify golden
fixtures; parquet.dll needs build-rel\bin on PATH to run exes.

## Task ledger
(none complete yet)

## Minor findings roll-up (for final review triage)
(none yet)

B1 (T1+T2): complete (commits 152df29..14adf68, review clean — Spec ✅, Approved)
  run_archive_schema.hpp (registry, 10 sections, backtest 27-col nav@16 verified vs
  tearsheet.cpp; projected_* alias kBacktestCols by pointer) + run_archive.hpp ABI
  (256B header, 4 structs sizeof+offsetof pinned). 7/7 gtest.
  Minor roll-up: (1) golden schema-hash pin missing — CARRIED into B3/T5 acceptance;
  (2) backtest middle columns not individually name-asserted (closed by golden pin);
  (3) RaColumnDescriptor tail not strictly descending-alignment (cosmetic, pinned);
  (4) RaSectionHeader::flags@40 + RunArchiveHeader::reserved_u16@90 lack offset pins.
  CHECKPOINT: unit strings + meta={key,value} become format-frozen at first durable
  archive write (B3 commits the python fixture) — confirm before B3 commit.

MODEL SWITCH (user-directed, post-B3): implementers/reviewers now Opus 4.8 subagents.
Opus commit trailer: Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>

B2 (T3+T4): implementer DONE (commits b4099ba, a7ee490; 14/14 gtest) — review in flight.
  Notes: writer enforces registry dtype/kind agreement, unknown dynamic cols unit="";
  section() eagerly validates dict/enum code ranges (hold the view, don't re-call);
  identity() returns ArchiveContentIdentity via fwd-decl (callers include surface_archive.hpp);
  open_mapped real (tsdb::Mapping via open_borrowed), no fallback needed.

B2 review verdict: Spec ✅ / Quality ❌ — ONE Critical. C1 CLOSED by 5b50e92
  (Opus fixer): n_rows > (1<<48) cap in open_impl directory loop + division-form
  size check in section() + RejectsForgedRowCountOverflow regression test
  (forged 2^62 n_rows, CRCs recomputed, ParseError). 22/22 gtest. B2 APPROVED
  per reviewer's standing "no re-review needed with C1 fixed".
  C1 (original finding): u64 wraparound in RunArchive::section() —
  `cd.data_size != sh.n_rows * ra_dtype_size(cd.dtype)` wraps for forged huge
  n_rows (2^62 DictStr / 2^61 F64) with data_size=0; passes framing, then eager
  dict/enum code-range scan reads OOB before ParseError. Fix: reject
  `de.n_rows > (1ull<<48)` in open_impl directory loop (mirror writer cap) +
  forged huge-n_rows reader test. Reviewer: "With C1 fixed I would approve
  without re-review of anything else."
  Minor roll-up: (1) plain-U32 dtype never round-tripped in any test;
  (2) open_borrowed ≥8B alignment precondition unchecked/undocumented;
  (3) flush not fsync-equivalent + remove→rename window (matches mandated
  pattern; MSVC fs::rename may not need remove-first); (4) inert oddities —
  duplicate/unsorted directory tolerated, sh.data_offset unused.

B3 (T5): implementer DONE (commit accfcd7; 21/21 gtest) — review in flight
  (package .superpowers/sdd/review-a7ee490..accfcd7.diff, raw via rtk proxy).
  Golden schema hash pinned 0xdcce47781ac8390d (closes B1 minor #1). Python
  fixture committed: python/tests/data/runarchive/wave_a_fixture.atxrun,
  sha256 71ea96322ba31378e32f75818486830605223cd9b6e6e4850283040ed29f7424.
  FORMAT-FREEZE checkpoint: units + meta {key,value} frozen as of accfcd7.
  Brief-vs-registry conflict: brief said fingerprints dict-str; registry pins
  I64 — implementer followed registry (authoritative). Changing later = new
  golden + kRaMinor bump.

B4 kickoff: C1 fixer (owns C++ build slot) ∥ B3 reviewer (read-only) ∥ T8
  python implementer (pytest only, no C++ build). T6 queued behind fixer.

B3 review verdict: Spec ✅ / Quality Approved (no Critical/Important; full
  review at backtest-wave-a/task-5-review.md). Encoders verified byte-for-byte
  vs writers+registry; backtest dbl_cols char-identical to tearsheet.cpp:190-216;
  EncoderArena/shared_ptr storage ownership sound; no new wraparound. B3 CLOSED.
  Minor roll-up: (1) WritesPythonFixture regenerates/overwrites committed
  fixture instead of asserting byte-identity — determinism regression would
  silently rewrite tracked file, not fail red; (2) recon/schedule/marks tests
  assert one value per dtype-class — same-dtype value-source swap on unasserted
  column uncaught; (3) 25-double column list duplicated across 4 sites
  (encoder/test/tearsheet/registry), archive↔TSV equivalence hand-synced;
  (4) informational: signal name colliding with registry column fails write
  (AlreadyExists) — stricter than TSV, acceptable.

T6 (run_diagnostics): implementer DONE (commit 5764604; 24/24 gtest, 2 new
  RunDiagnostics tests; RED verified via link error). PhaseTimer lifted
  verbatim; diagnostics SubTable in kDiagnosticsCols order; storage arena
  convention. Disclosed design note: total row wall_ms = phase-sum (no
  independent command total in signature) — documented header/.cpp/report.
  Review VERDICT: Spec ✅ / Quality Approved (task-6-review.md). PhaseTimer
  char-for-char verbatim confirmed; dict framing byte-identical to T5; arena
  lifetime correct; encoder pure (never calls now()). T6 CLOSED.
  Minor roll-up: (1) doc comment above class reworded vs original (correct
  call, outside class body); (2) TDD RED was link-error not assertion-level;
  (3) subcommand dict hand-built vs DictBuilder — byte-identical, stylistic.
B5 kickoff: T7 RunDir implementer (owns C++ build slot) ∥ T6 reviewer
  (read-only). T8 python still in flight.

T8 (python reader): implementer DONE (commit 4bf7992; 21/21 new tests, full
  python suite 70 passed, no C++ builds). runarchive.py (mmap + framing/CRC/
  schema-hash validation, zero-copy numpy views, dict/u8enum decode, lazy
  payload CRC, read_backtest_section shim) + generated _schema.py + generator
  tools/gen_runarchive_schema.py (parses run_archive_schema.hpp → folds to
  golden 0xdcce47781ac8390d). CRC-32C + FNV fold verified bit-exact vs fixture
  BEFORE writing reader. Standalone import proven via subprocess test.
  Disclosed deviations: standalone generator (not CMake codegen, build-slot
  avoidance); BacktestResult-like plain object (not binding type); KeyError
  misses / ValueError corruption.
  Review VERDICT: Spec ✅ / Quality REQUEST CHANGES (task-8-review.md).
  Framing parity complete incl. n_rows>2^48 cap; CRC bit-identical; zero-copy
  views read-only; ABI offsets verified; fixture untouched.
  Important-1: from_bytes path broken — _string_table .decode on memoryview
  slice → AttributeError on every dict/enum column (only mmap path works,
  from_bytes untested). Important-2: schema drift not caught — golden pinned
  from generated _schema.py, nothing re-parses .hpp in CI/pytest.
  FIXER dispatched (python-only, parallel with T7 C++; disjoint files).
  Minor roll-up: (1) no negative section()-framing tests; (2) version-mismatch
  path untested; (3) close() leaks _fh on BufferError until GC; (4) forged
  non-utf8 string table raises UnicodeDecodeError not documented ValueError;
  (5) pure-python per-byte CRC slow on large sections (lazy by design).
  FIX ROUND 1 DONE (commit 4d31ad2): I1 bytes(...) materialization before
  decode (zero-copy numeric views locked by assertion test); I2
  test_schema_py_not_stale_vs_cpp_header runs generator --check via subprocess
  (drift-detection proven by injection). 25/25 runarchive tests, python suite
  74 passed. T8 CLOSED (fixes match reviewer's prescribed direction exactly;
  no re-review).

T7 (RunDir): implementer agent DIED after committing d4f1290 (no report file).
  Controller verified: commit complete (RunDir spec/clock/schedule/
  write_run_archive/archive/verify; run_identity_hash = wyhash fold of
  run_spec bytes + universe-schedule fingerprint, forced nonzero; verify gates
  envelope/existence/inputs/count/core-mode >=60 dates >=3 rolls >=40 names),
  build clean, 29/29 on RunArchive*:RunDiagnostics*:RunDir*. Review in flight
  (package review-3295eb9..d4f1290.diff; reviewer told no report exists).
  Review VERDICT: Spec ✅ / Quality Approved (task-7-review.md). Core-mode
  thresholds match example verbatim; count gate lifts cardinality invariant;
  atomic write delegates to write_run_archive_file; all 4 TSV parsers reused.
  T7 CLOSED. Minor roll-up: (1) run_archive.hpp now hard-includes backtest/
  dispersion_workflow/listed_dispersion_schedule headers (coupling; optional
  run_dir.hpp split); (2) verify count gate cardinality-only (documented);
  (3) no negative test for count gate mismatch path; (4) write_run_archive
  hardcodes created_ts_ns=0 → writer stamps system clock, bytes vary
  run-to-run (identity hash is the stable pin, matches brief); (5) NotFound
  if run_spec.tsv absent — write-ordering dependency by design.
  NOTE: interleaved commits 3295eb9/385c7c7 (surface-db docs) on main from
  parallel session — not part of this sprint.

B6 kickoff: T9 hard-cutover implementer (owns C++ build slot; fixture recipe
  from dispersion-parity/task-9-report.md; SP\paired pristine; known-good
  final_nav=-456.5769067 dates=3 rolls=1) ∥ T7 reviewer (read-only).

T9 (hard cutover): implementer DONE (commit 717e08d; task-9-report.md).
  run-backtest/build-schedule/project-schedule/run-projected-backtest publish
  run.atxrun via RunDir::write_run_archive; loose result TSVs stop (backtest/
  reconciliation/contract_marks/mark_divergence/projected_backtest/diagnostics_*);
  retained text inputs unchanged; verify → RunDir::verify(); `runarchive dump
  <run_dir> <section> [--tsv]` escape hatch (backtest byte-identical to golden);
  python io.read_backtest_archive + parity build_parity_report_from_archive
  (binding-free); phantom step_pnl_total deleted from _SERIES. Fixture economics
  exact (final_nav=-456.5769067); 29/29 gtest filter; python 78 passed.
  Review VERDICT: Spec ✅ / Quality Approved (task-9-review.md). T9 CLOSED.
  Minor roll-up: (1) parity archive route labels projected_nodiv track same as
  cold; (2) dump prints nan (not NA) for non-backtest sections' NaN doubles;
  (3) verify dropped reference_reconciliation.tsv existence check (documented);
  (4) meta section duplicated run_spec keys vs dedicated columns (stylistic);
  (5) collect_mark_divergence_replay arena reserve heuristic; (6) e2e pytest
  copies fixture occ_ess paths byte-level (fragile if fixture layout changes).

T10 GATE (controller) — Step 1: build current at 717e08d (ninja no-op).
  Full gtest from build-rel CWD: 3 failures — BoundaryHoist.
  PriceBitIdenticalToPrechange (last-ulp golden, american_test.cpp:2468) +
  AllQualityModes/SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversion
  Budgets/{Latency,Balanced}. NOTE: running full suite from repo root adds 11
  false MultinamePipeline failures — stale repo-root artifact-cache/ (cached
  corpus is CWD-relative); run from build-rel. Triage subagent dispatched
  (pre-existing vs Wave A). Python suite rerun in flight.
  Step 2: parity-full 135-session rerun launched (state/log in session
  scratchpad t10-parity/, prior sprint markers untouched). Economics check =
  pre-T9 hashes (dispersion-parity/parity_full_hashes_preT9.txt): dump backtest
  --tsv vs a05470c7…, projected_cold vs cbabca44…, trade_schedule b640b3ab…,
  projected_schedule d6793d46….
  Step 3 proof: stale loose result TSV mtimes snapshotted pre-run
  (t10-parity/pre_run_stale_tsv_mtimes.txt) — must be unchanged post-run.
  FINAL whole-branch review dispatched (Opus): package
  review-66200ca..717e08d.diff + Minor roll-ups triage → final-review.md.

T10 GATE RESULTS — ALL GREEN:
  Step 1: python suite 78 passed. gtest triage (t10-failure-triage.md): all 3
  residual failures PRE-EXISTING — BoundaryHoist = 1-ULP SSE2 golden-pin drift
  (file last touched 55cd3ca, pre-base); SurfaceV2Qualification Latency/
  Balanced = max_borrow_pairs cap lowered by on-main perf commit e7d5ebb, test
  re-pin unmerged on parallel feat/pipeline-m; zero file-level AND zero
  include-closure intersection with Wave A diff. NOT blocking.
  Step 2: parity-full 135-session rerun (steps 1-4, ~5.5 min total; surfaces
  pre-cached). Economics EXACT: listed final_nav=125026.0592, projected-cold
  final_nav=123243.1172, dates=135 rolls=7, corr=0.99718, mark_divergence
  section present rows=0. dump backtest --tsv sha256 == pre-T9 backtest.tsv
  (a05470c7…) BYTE-IDENTICAL on 135 sessions; dump projected_cold --tsv ==
  cbabca44…; trade_schedule.tsv == b640b3ab…; projected_schedule.tsv ==
  d6793d46… (4/4 golden hashes). Both archive snapshots (scratchpad
  t10-parity/run_listed.atxrun + run_projected.atxrun) open in python reader,
  validate_all OK; build_parity_report_from_archive renders 154KB report.
  Step 3: post-run loose result TSVs = same 8 stale files, mtimes byte-equal
  to pre-run snapshot (all 2026-07-21) — nothing rewrote them; only run.atxrun
  new. Cutover proven on production run dir.
  FINDINGS (non-blocking, → Wave B):
  (F1) SINGLE-ARCHIVE CLOBBER: all subcommands write <run_dir>/run.atxrun —
  sequential parity workflow (build-schedule→run-backtest→project-schedule→
  run-projected-backtest) in one dir leaves only the LAST subcommand's archive;
  listed archive recaptured by re-running run-backtest + snapshot copy.
  Wave B listed_dispersion_pipeline must give each track its own archive
  (per-track run dirs or multi-archive naming).
  (F2) verify on parity-full dir → NotFound: read_quality_report_file —
  quality.tsv absent from that dir since prior sprint (environmental; same
  check existed pre-cutover; prior runner never invoked verify; 3-session
  fixture verify passes).
  Step 4: plan checkboxes all checked (49); ledger committed with final
  review verdict below.

FINAL WHOLE-BRANCH REVIEW (Opus, final-review.md): Spec ✅ PASS (caveat: "one
  container holds every result section" only realized per-route — see I1).
  Quality: REQUEST CHANGES — 0 Critical, 1 NEW Important.
  I1 = T10 finding F1 formalized: RunDir::write_run_archive rebuilds
  run.atxrun from only its own sections (no merge) → run-projected-backtest
  in the listed dir silently destroys backtest/reconciliation/contract_marks/
  meta. Canonical listed pipeline (one route per dir) unaffected.
  Cross-cutting PASSED: format-freeze integrity, reader hostile-archive
  hardening (no wraparound siblings, C++ AND python), from_bytes fix, C++↔py
  schema parity, error taxonomy, dead code, CMake registration.
  Minor triage: 4 FIX-NOW / ~18 DEFER-TO-WAVE-B or DROP (table in report).
  FIX ROUND (parallel Opus fixers, disjoint files):
  - C++ fixer: I1 → MERGE-WRITE with identity-hash staleness guard (carry
    forward non-replaced sections when existing archive's run_identity_hash
    matches; new-wins on collision; hash mismatch/corrupt → fresh container)
    + FIX-NOW 1 (WritesPythonFixture asserts byte-identity vs committed
    golden, no overwrite) + FIX-NOW 2 (dump help wording: byte-identical only
    for backtest schema, nan-not-NA elsewhere) + FIX-NOW 4 (--out help: meta
    provenance only, no file written).
  - Python fixer: FIX-NOW 3 (parity projected_label derived from resolved
    section; explicit override wins) + test.
  Merge-write is one of reviewer's prescribed remedies → close without
  re-review per standing SDD precedent (C1/T8), gates rerun by controller.

FIX ROUND RESULTS — WAVE A CLOSED:
  Python fixer commit b7fe5a1 (parity label from resolved section; 5 new
  tests, 2 RED-first; test_parity.py 20 passed).
  C++ fixer commit 191e409 (merge-write w/ identity-hash guard + 3 RunDir
  tests; MatchesCommittedPythonFixture byte-identity vs golden; dump + --out
  help wording). 32/32 gtest filter; committed fixture sha256 unchanged
  71ea9632…29f7424. Fixer disclosure: only the union test is a true RED
  anchor (stale-drop/corrupt-fresh pass trivially vs old always-fresh writer).
  Controller gates post-fix: full python suite 83 passed. I1 fix proven
  END-TO-END on production parity-full dir: restored listed archive snapshot,
  ran run-projected-backtest same dir → run.atxrun = UNION of 8 sections
  (backtest, contract_marks, diagnostics, mark_divergence, meta,
  projected_cold, reconciliation, trade_schedule); carried listed backtest
  dump STILL byte-identical to pre-T9 golden (a05470c7…); projected dump
  cbabca44…; validate_all OK; NAVs exact (125026.0592 / 123243.1172).
  Reviewer's I1 blocker resolved via prescribed remedy → branch verdict:
  Spec ✅ / Quality APPROVED.
  Final commit chain (base 66200ca): 152df29, 14adf68 (B1) → b4099ba,
  a7ee490 (B2) → accfcd7 (T5) → 5b50e92 (C1 fix) → 5764604 (T6) → 4bf7992
  (T8) → d4f1290 (T7) → 4d31ad2 (T8 fix) → 717e08d (T9) → b7fe5a1 +
  191e409 (final-review fixes). [3295eb9/385c7c7 = foreign surface-db docs.]
  DEFERRED to Wave B: ~18 triaged minors (final-review.md table) + per-track
  archive naming in listed_dispersion_pipeline (I1 merge-write covers the
  shared-dir workflow; route-scoped meta/diagnostics names need schema bump).
