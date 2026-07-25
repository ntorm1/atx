# Backtest Framework Waves C/D/E — SDD Progress

Controller: Claude (session b8ae4870). Repo root C:\atx, branch main, in place.
Base commit at Wave C start: 587ee97 (Wave B closed).

Plans:
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-c.md
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-d.md
  docs/superpowers/plans/2026-07-24-atx-vol-backtest-framework-wave-e.md
Each carries a "Controller decisions (pre-dispatch)" block that overrides its body.
Design spec + errata: docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md
Wave B ledger (committed): .superpowers/sdd/backtest-wave-b/progress.md

User decisions for this stretch:
  - Wave C = SPINE-ONLY. T7 (mag7 RunArchive write) DROPPED — its reader is Python,
    which is out of scope, so the write would be dual-write (forbidden by design §2
    and by the sprint's hard-cutover decision).
  - Sprint close = per-wave gate + ONE fresh whole-sprint review over A-E at the end.
  - ALL Python work is dropped. No pytest task in any wave.

## Planning outcomes (3 parallel Opus planners, read-only)

WAVE C — 7 tasks after the T7 drop. L11's headline measured FALSE: stage-by-stage
  across the 5 named drivers, only stages 5+6+7 (timed engine call -> tearsheet fold
  -> stats capture) are common to all five, and 4 of those 6 nodes are ALREADY
  library calls. Args/split/join/fmt_num 2/5; RunConfig overlay 2/5; clock source
  0/5; strategy construct 0/5; output shape 0/5 (four distinct shapes);
  EngineRunStats 1/5. So Wave C extracts ONE function (RunOutcome + two run_timed
  overloads), not a BacktestJob. Three design-§4.3 errors found and recorded as
  errata in the design spec (no third "tradeable manual evaluator" engine slot; the
  "six drivers" sixth is Wave B's thin surface-backtest command; only the RETURN
  type was right). The synthetic-corpus helper trio L11 folds into the spine appears
  in 28 files — real duplication, wrong home, separate hygiene sweep.

WAVE D — 7 tasks. StepObserver signature settled:
    struct StepEvent { std::size_t step_index; const SnapshotRef &ref;
                       const MarketSnapshot &snapshot; const IStrategy &strategy; };
    using StepObserver = std::function<Status(const StepEvent &)>;
  Fired after both on_step sites (backtest.cpp:1862, :1999) and BEFORE
  validate_strategy_transition — the shadow loop read strategy state with nothing in
  between, so that is the only definitionally-equivalent point. All four members are
  read in-tree (Wave B had to delete four dead fields; not repeating that).
  Blast radius verified empirically: 46 files include backtest.hpp; zero positional
  aggregate-init sites, zero sizeof/offsetof asserts, nothing hashes RunConfig — so a
  defaulted tail field is source-compatible, but T1 still mandates a FULL build.
  Anti-vacuity: cold-route rows=0 cannot falsify, so the proof rides on
  --execution configured (rows > 0), with a RED probe on the comparator and a
  Vacuity Ledger entry per task.

WAVE E — 9 tasks. KEPT P5, P2, P3, narrowed P1. DROPPED P4/P6/P7 with evidence
  (P4: parallelism already exists, n_threads=0=auto, and a range batch would hold
  ~6.9k OpraPanels resident. P6: surface_fingerprint 15611810793130839 is a byte
  golden in the committed trade_schedule.tsv, so the swap is a re-baseline not a perf
  pass. P7: precondition provably false — corpus built from all_symbols over the same
  universe file, so an archive cannot exceed the universe).
  P5 found LARGER than documented: the divergence replay loop is all 135 sessions,
  not ~60, so the double-deserialize is 2.2x the review's assumption.
  P3 found UNDERSTATED: the 9-field vector is ~6 allocs/row, not 1, plus L3's
  throwaway 696 MB re-serialization.
  Biggest correctness risk: P2 narrows three whole-panel fail-closed gates to the
  ~102 consumed leg keys, and its outputs (reconciliation, contract_marks) were the
  only artifacts with NO golden — Task 1 pins them first and BLOCKS P2.

## Task ledger
(dispatch begins below)
