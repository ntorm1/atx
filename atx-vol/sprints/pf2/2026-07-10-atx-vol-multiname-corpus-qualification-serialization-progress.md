# Multi-Name Corpus Qualification and Serialization Progress

**Plan:** `2026-07-10-atx-vol-multiname-corpus-qualification-serialization-sprint.md`

**Branch:** `feat/atx-vol-corpus-qualification`

**Worktree:** `C:/atx-wt/atx-vol-corpus-qualification`

**Base:** `c56867e` (`chore(atx-vol): land in-flight fit/bit-identity fixes and reorganize sprint docs`)

**State:** active

## Parallel-work boundary

The American pricing/portfolio performance sprint owns these files or surfaces
until its branch is merged:

- root `CMakeLists.txt`;
- `atx-vol/CMakeLists.txt`;
- `atx-vol/tests/CMakeLists.txt`;
- `atx-vol/bench/**` and its baseline/comparison tooling;
- `atx-vol/include/atx/vol/counters.hpp`;
- `atx-vol/src/american.cpp`;
- `atx-vol/src/correction.cpp`;
- `atx-vol/src/portfolio_pricer.cpp`; and
- archive cache payload/framing added by performance P4, if any.

Do not edit those paths before the post-performance rebase. New production logic
must use already-built source files or header-only pure types; tests land in
existing registered test translation units. Archive/benchmark tasks remain
deferred rather than creating competing infrastructure.

## Task ledger

| Task | State | Evidence / note |
|---|---|---|
| P0-1 fresh worktree | complete | branch/worktree above, based before performance sprint |
| P0-2 post-performance API inventory | deferred | requires completed performance merge |
| P0-3 canonical benchmark fixture | deferred-conflict | `atx-vol/bench/**` owned by performance sprint |
| P0-4 before benchmark JSON | deferred-conflict | baseline harness owned by performance sprint |
| P1-1 quality metric/admission types | complete | pure metrics/rules/decision API; absent metrics use `std::optional` |
| P1-2 selected-family OOS scorer | complete | one-family wrapper reuses selector split/filter; direct/pinned OOS contracts green |
| P1-3 deterministic admission reasons | complete | stable primary priority plus complete failure mask; predicate tests green |
| P1-4 corpus FitContext/fallback provenance | complete | per-board context wired; final decision/fallback/kind consistency reported |
| P1-5 quality report round trip | complete | versioned TSV, exact evidence counts, aggregate verification, atomic file I/O |
| P2-1 OPRA instrument-id provenance | complete | additive pull schema; strict/legacy loader; aligned IDs + dictionary + ambiguity gates |
| P2-2 per-cell market inputs | complete | canonical sorted table, no-lookahead validation, explicit fallback/quarantine/error |
| P2-3 dividend/context wiring | complete | cash divs, escrowed-forward treatment, FitContext, tags, and corpus bridge |
| P2-4 per-expiry rate fit/query/archive | complete-no-framing-change | term rates reach selector/fit/parity/live/archive queries; existing slice `df` persists them |
| P2-5 source/config fingerprint | complete | semantic path-free source, market-input, automatic run-input, and policy fingerprints |
| P3-1 streaming qualified corpus | complete | shared fit core, ordered `CorpusBuildSession`, source-failure variant, peak-live counter |
| P3-2 crash-safe/resumable commit | complete | atomic date checkpoints reuse only exact input/policy matches and CRC-valid mapped archives; stale/partial state fails loudly |
| P3-3 synthetic multi-profile corpus | complete | 1 index + 12 names x 3 dates; all five dispositions and below-survivor date asserted |
| P3-4 cached-real breadth corpus | complete | all 14 cached fixtures produced a qualified scoreboard; no network path |
| P3-5 property/fuzz corpus | complete-fast/long-ready | 250 generated boards green across all profiles; identical 10k harness is opt-in via `ATX_VOL_LONG_CORPUS=1` |
| P3-6 existing dispersion e2e smoke | complete | admitted 39-cell archives feed ATM-straddle strategy; drop/unpriced/closure and determinism gates asserted |
| P4 archive performance | deferred-conflict | wait for completed performance archive and benchmark shape |
| P5 committed performance gate | deferred-conflict | wait for completed performance baseline tooling |

## Verification log

- 2026-07-10: `git worktree add` created the isolated branch at `c56867e`.
- 2026-07-10: current performance ownership was audited from the primary
  worktree; no primary-worktree file was modified by this branch setup.
- 2026-07-10: `CorpusAdmission.*:CorpusQualityReport.*:QualifiedCorpus.*`
  passed 8/8 after qualified archive exclusion and report file integration.
- 2026-07-10: P1 report schema now records fit/OOS numerators, denominators,
  weighted sums, final-family consistency, profile routing, and fallback state.
- 2026-07-10: final P1 subset passed 10/10, including direct one-family OOS,
  pinned OOS=`NA`, sparse-profile admission, and successful one-sided quarantine.
- 2026-07-10: legacy `Corpus.*:PricerFitterPolicy.*` passed 9/9 correctness
  tests; the opt-in throughput timing test skipped as designed.
- 2026-07-10: combined OPRA/P1 contract matrix passed 38/38, including strict
  point-in-time identity, per-cell inputs, no-lookahead, and fingerprint mutation.
- 2026-07-10: term-rate fit/live/archive gate passed; `SurfaceArchive.*` plus
  legacy `Corpus.*` passed 23/23 correctness tests (one opt-in timing skip).
- 2026-07-10: final qualification/streaming matrix passed 15/15; source
  failures, date ordering, canonical duplicate rejection, interrupted
  publication, final sidecars, and the peak-live bound are asserted.
- 2026-07-10: synthetic 13-symbol x 3-date qualified scoreboard passed with 39
  planned cells and exact admitted/quarantined/source/fit/empty partitions.
- 2026-07-10: all 14 cached-real breadth fixtures produced the qualified
  scoreboard (including VXX as its own diagnostic profile); no skip was needed.
- 2026-07-10: fixed-seed 250-case admission/report property artifact round trip
  passed, including absent and non-finite measured evidence.
- 2026-07-10: strict OPRA plus per-cell market-input matrix passed 30/30,
  including the 14-fixture cached-real qualified scoreboard and term-rate
  fit/live/archive parity.
- 2026-07-10: `SurfaceArchive.*:Corpus.*:PricerFitterPolicy.*` passed 25/25
  correctness tests; the one opt-in throughput timing test skipped as designed.
- 2026-07-10: the existing `MultinamePipeline.*` backtest regression suite
  passed 17/17 before the qualified-corpus-specific P3-6 gate was added.
- 2026-07-10: P3-6 now runs the admitted 39-cell archives directly through the
  existing dispersion strategy; explicit drop reasons, unpriced lot/Greek
  counts, attribution closure, worker determinism, and promised analytic/FD
  identities passed.
- 2026-07-10: P3-2 interruption/resume gate passed: exact date checkpoints
  avoid refitting, completed indexes reuse on equality, and input or policy
  fingerprint changes fail loudly rather than accepting an existing archive.
- 2026-07-10: fixed-seed 250-board generated-corpus gate passed twice (final
  formatted run 204.3 s) across all seven profiles, with economic,
  microstructure, and corruption variation; finite/calendar-clean admitted
  surfaces; archive query round trips; and byte-identical 1/4-worker outputs.
  The identical 10,000-board overnight gate is implemented behind
  `ATX_VOL_LONG_CORPUS=1` but was not run this session.
- 2026-07-10: the synthetic breadth fixture now proves a real C8-to-eSSVI
  fallback on an underidentified event board. Direct C8 fitting rejects fewer
  than eight observations instead of silently labeling a zero-bump seed C8;
  the healthy event-C8 regression remains green.
- 2026-07-10: final formatted qualification/fallback matrix passed 18/18.
  `SurfaceArchive.*`, legacy `Corpus.*`, term-rate archive parity, and the
  cached-real scoreboard passed 24/24 correctness tests; the opt-in timing test
  and opt-in 10,000-board gate skipped as designed.

## Commits

- `8119e5e feat(atx-vol): qualify corpus fits with evidence`
- `8bfcb3c feat(atx-vol): stream point-in-time qualified corpora`
- `c9e2110 feat(atx-vol): resume qualified corpus dates safely`
