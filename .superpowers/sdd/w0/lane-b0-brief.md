# Lane W0-B0: Replay correctness

Host tag: **RAM:light** · Batch: **W0b** · Pool: **`C:\atx-wt\pool-9`** ·
Branch: **`feat/w0-b0`** (run id `aes-w0-b0`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-B0 · Replay correctness
- **Closes:** B-02 (engine), B-03, B-04, B-05.
- **Owns:** `book/replay.{hpp,cpp}`, `book/borrow_schedule.hpp`, `book/report.hpp`, `atx-impl/src/stage_report.cpp`.
- **Build:**
  - Reject `execution_delay=0` unless `allow_same_close` is set.
  - New delisting policy `TerminalReturn` as the default. It uses a supplied terminal-return table, falling back to Shumway −30% (NYSE/AMEX) or −55% (Nasdaq), flagged. `Abort` remains available as an option.
  - A locate breach clips and reports; it no longer aborts.
  - Borrow fee and rebate are counted once.
  - Legacy report: delisted return applied, borrow charged per period length, horizon-correct annualization.
- **Suites:** `BookReplayDelay_*`, `BookReplayDelist_*`, `BookBorrowSingleCount_*`, `BookLegacyReport_*`.
- **Accept:**
  - The PCS 2013-05-01 fixture runs to the end.
  - A missing close without evidence yields −30%/−55% (flagged), never 0 and never an abort.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| B-02 | H | `book/replay.cpp:184`; `stage_equity_mine.cpp:1179,1244-1247`; `config.cpp:455-464` | Execution delay 0 is accepted everywhere (same-close fills). | W0-B0 + W0-I0b |
| B-03 | M | `book/report.hpp:172-178`; `stage_report.cpp:441-446,443-446,593-598` | The legacy report gives a delisted held name 0 return, charges borrow once per period regardless of length, and mis-annualizes weekly books. | W0-B0 |
| B-04 | H | `book/replay.hpp:72`; `replay.cpp:228-236,312-318` | A delisting (missing close) or a locate breach **aborts** the run, which pushes users toward a survivor-filtered universe. | W0-B0 |
| B-05 | L | `borrow_schedule.hpp:15-17` vs `replay.cpp:443-444` | The borrow fee is charged and the rebate credited, which double-counts. | W0-B0 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-engine/include/atx/engine/book/replay.hpp`, `atx-engine/src/book/replay.cpp`
  - `atx-engine/include/atx/engine/book/borrow_schedule.hpp`
  - `atx-engine/include/atx/engine/book/report.hpp`
  - `atx-impl/src/stage_report.cpp` — EXCEPT the diagonal-risk call site near line 499 (I-04), which I0a owns in W0
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `book` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-book-tests`, `atx-impl-tests (+ atx-shm-worker)`.
- Suites: `BookReplayDelay_*`, `BookReplayDelist_*`, `BookBorrowSingleCount_*`, `BookLegacyReport_*`; must stay green: whole book target + atx-impl-tests.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-b0-report.md`.

## Lane notes (orchestrator)

- B-02 is split: engine rejection of `execution_delay=0` unless `allow_same_close` is yours; the CLI flag `--allow-same-close` and equity-mine sites are I0b's. Name the engine field `allow_same_close` so I0b can wire it.
- 'PCS 2013-05-01 fixture': grep the tests for an existing PCS/MetroPCS fixture; if none exists, build a synthetic fixture reproducing that event (name leaves the panel mid-run without delisting evidence) — no real-data reads.
- Pool-9 is configured for book;risk;combine — reconfiguring is optional.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
