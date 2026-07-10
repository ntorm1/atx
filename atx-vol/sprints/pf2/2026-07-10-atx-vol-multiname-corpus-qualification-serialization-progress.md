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
| P2-1 OPRA instrument-id provenance | pending | atx-core pull/opra loader; no known performance overlap |
| P2-2 per-cell market inputs | pending | opra/corpus files only |
| P2-3 dividend/context wiring | pending | skip any American-kernel changes |
| P2-4 per-expiry rate fit/query/archive | partially deferred | fit/query can proceed; archive framing waits for rebase |
| P2-5 source/config fingerprint | pending | depends on quality report schema |
| P3 streaming qualified corpus | pending | corpus files; archive call seam may need post-rebase adaptation |
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

## Commits

None yet.
