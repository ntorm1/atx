# SDD ledger -- plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md

Recovery map: newest entries at the bottom of this head block ("Execution log"). Older planning entries follow below it.
Root: C:/atx-wt/pool-2, branch feat/platform-v8-20260929, base main 7fbfc379. PY="C:/Program Files/Python312/python.exe".

## Owner directive for execution (2026-09-29, /goal)
"implement sprint plan using sub agent driven dev using multiple parallel opus 5.5 sub agents. Speed up development by
not using test driven dev and not deploying review agents after each task. Only use for larger complex implementations
and logical breaking points. You are approved for all recommendations. Focus on rapid development, high quality /
modular / reusable code, and overall building out the atx-engine to support future work and the atx-impl to prove it works."

## Rulings made before dispatch
- Ruling E-1: owner decisions OD-2, OD-4, OD-5, OD-8 take the plan defaults -- owner approved all recommendations --
  cost if wrong: one re-run of the affected cells. OD-2: IC-pass cap 2,560 MiB and 300 s. OD-8: children are Opus 5.5,
  trailer `Co-Authored-By: Claude Opus 5.5`.
- Ruling E-2: OD-3 (history read 2013-2019) and OD-7 (mining campaign) keep the plan default "tooling only, no read, no
  campaign" -- the plan's recommendation is to build the tools and leave the read to an explicit owner ruling; a read
  cannot be undone -- cost if wrong: the owner grants it later and one session is lost.
- Ruling E-3: OD-6 data asks cannot be served by this sprint (atx-db is another session's tree). Members that need absent
  data are withdrawn before any read -- cost if wrong: R-7 is smaller.
- Ruling E-4: no TDD and no per-task review (owner directive). Tests named in the plan are still written, after the code.
  Review lanes run at wave ends and on B-3 (move-only split), R-6 (optimiser) and H-3 (mining verb) -- cost if wrong:
  defects are found at integration instead of at the task.
- Ruling E-5: child pools 3, 4, 7, 8, 9, 10, 11 are used by branch switch on their existing dead leases, as in v7
  (handoff-1 section 4: "all v7 lanes ran on existing dead leases; that was fine"). No `git worktree add` -- cost if
  wrong: a foreign session leases a pool in use; all pools were clean at start and `-Status` is checked at each wave.
- Ruling E-6: children never build (host: 15.7 GB RAM, 3.5 GB free, atx-db session active). Root serialises builds and
  real-data runs in pool-2. Root may use one integrator agent at a time in pool-2 for merge, build, test, compile fixes --
  keeps the PM context for coordination -- cost if wrong: integration defects fixed without a second reader; the wave
  review covers them.
- Ruling E-7: child reports are written and committed in the child's own worktree (not in pool-2), so pool-2 stays clean
  between runs.
- Ruling E-8: all lanes branch from one base and W0-1 lands by merge. W0-1 edits are one-line constant replacements;
  textual merges with C, F and V lanes are expected to be clean or trivial.

## Pre-flight scan (shared files and interfaces)
| tasks | shared file or interface | finding | ruling |
|---|---|---|---|
| B-1, F-2 | `atx-impl/tools/equity_strategy_ic.cpp` | both add CLI surface | F-2 adds one dispatch line for verb `marginal`; logic in `strategy_marginal_ic.cpp`. Merge B before F |
| B-3, B-1, B-2 | `strategy_ic_runner.cpp` | sequence inside one lane | one lane, order B-3, B-1, B-2 |
| A-3, A-2, H-1 | `scripts/research_cycle.py` | sequence | one lane A, then H-1 after A merges |
| W0-1, C-1, C-3, F-1 | constant sites in C and F files | one-line edits | Ruling E-8 |
| A-1, B-3 | K1 plan rows | A-1 codes against the contract before B-3 merges | contract text in section 6.3 is binding; A-1 test uses a recorded plan JSON fixture |
| E-3, A-3 | K3 `--no-git` | E-3 consumes before A-3 lands | E-3 test skips when the flag is absent; root records goldens after A-3 merges |
| V-1, H-1 | `atx-impl/tools/nav_summ.py` | V-1 moves the file, H-1 adds pooling | H-1 after V merges |
| D-0, D-1, R-4, R-5 | `strategy_nav_replay.cpp`, `strategy_target_replay.cpp` | sequence | D first; R-4 then R-5 in one lane |
| R-1, B | `strategy_ic_composition.cpp` | B-1 skips composition, R-1 edits it | R-1 starts after B merges |
| C-2, F-2, G | K6 `marginal_ic.json` | reader before writer | contract binding; readers tolerate absence |
| W0-4 step 2, D | warm start D-0 is specified in the W0-4 block, owned by lane D | plan text split across two tasks | D brief carries the W0-4 step 2 text |
| each task | tests vs code inside the task | names and flags agree within each block | none |

## Lanes (wave 1 dispatch)
| lane | pool | branch | tasks in order |
|---|---|---|---|
| W0E | 3 | feat/platform-v8-w0e-20260929 | W0-1, E-1, E-4 |
| EV | 8 | feat/platform-v8-ev-20260929 | E-3, V-1, V-2 |
| B | 10 | feat/platform-v8-b-20260929 | B-3, B-1, B-2 |
| C | 9 | feat/platform-v8-c-20260929 | C-1, C-2, C-3 |
| A | 11 | feat/platform-v8-a-20260929 | A-3, A-1, A-2 |
| D | 4 | feat/platform-v8-d-20260929 | D-0, D-1, D-2 |
| F | 7 | feat/platform-v8-f-20260929 | F-2, F-1 |

## Execution log
- 2026-09-29: root branch created from main 7fbfc379. Plan, inputs, briefs, lane rules committed. Lanes dispatched.

---

# platform-v8-20260929 -- ledger (newest first; `git add -f` this dir)

Goal (owner, 2026-09-29): build v8 of atx-impl. Number one goal: a mega alpha implementation of a US equity long/short
strategy that is high Sharpe, high capacity, low turnover. (1) Raise Sharpe: more orthogonal alphas, better existing
alphas, better combination and trading. (2) Keep building the atx-engine platform so research and backtesting iterate faster.

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md

Inputs written this session (read-only work; nothing built, nothing run on book data):
- code-review-v8-platform.md (P-1..P-15), code-review-v8-signal.md (S-1..S-13, C-1..C-9), code-review-v8-engine.md (O1..O10)
- reports/Equity long short alpha v8.md (literature report) and research_notes/Equity long short alpha v8/ (5 note files)

Standing rules: as platform-20260928, with the owner change below.

## 2026-09-29 sprint planned; owner ruling on the window
- OWNER RULING (2026-09-29, during planning): "expand the TRAIN window to include 2023 to give us more sample size, keep
  2024+ hidden and out of sample." TRAIN becomes [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later.
- Disclosure recorded with the ruling: 2023-2024 was read twice at book level as validation in earlier sprints, so 2023
  enters TRAIN partly selected and 2024 is hidden but not pristine. 2025 and later has never been read.
- Nothing has been read on the new window. No code changed. No trial run. Appendix A unchanged from platform-20260928:
  TRAIN construction cells 37; validation trials 2 spent; 2025+ reserved.
- Open owner decisions: OD-2 (IC-pass memory cap for the 4-year role), OD-3 (history 2013-2019), OD-4 (DSR variance
  convention), OD-5 (report seal regex), OD-6 (atx-db data asks), OD-7 (mining), OD-8 (implementer model). See plan section 2.
- Caveat from the research lanes: all five literature lanes ran out of web-search budget. Items listed as gaps in the
  notes are unresearched, not rejected. Effect sizes in the plan are estimates.
- Next: owner reviews the plan and rules on OD-2. Then Wave 0 (W0-1 window constants, W0-2 role and fields, W0-3
  pre-registration, W0-4 baseline cells) beside Wave 1 platform lanes.

## 2026-09-29 W0-2 runbook received (w0-2-runbook.md, w0-2-runbook-open-questions.md)

Rulings on the runbook's open questions. All declared before any build or read on the 4-year window.

- Ruling W0-d (Q1, regsho stage republished): pin the live regsho_threshold manifest; the ".02 coverage" acceptance is
  waived for `regsho_threshold_days63` and its coverage difference is reported -- no v7.1 candidate reads the field and
  atx-db is another session's tree -- cost if wrong: one field rebuilt later.
- Ruling W0-e (Q5, Q6): rebuild the r4 identity bridge with seal 2024-01-01 and fundamental events v3 on the sealed CIK
  scope, for both roles -- the old scope used link evidence dated 2024, which is now hidden -- cost if wrong: lo3
  fundamental coverage shifts slightly; the overlap report shows it.
- Ruling W0-f (Q8): the pre-registration quotes max compiled slots 8 (it reproduces the recorded admission bytes).
  Estimated IC-pass admission on the 4-year role: 1,952 MiB at about 6,100 instruments; bound 2,540 MiB at 8,000.
- Ruling W0-a, extended (Q9, Q10): the overlap is reported per field and per candidate. A difference whose cause is
  identified and documented (instrument-union dependence of `me_company` and `sv_ratio126`; summation order over a
  wider row) is disclosed with its size and does not stop the re-base -- every v8 paired comparison is between two
  cells on the same 4-year role, so old 3-year values are not a comparator -- cost if wrong: continuity with the v7
  ledger is weaker than stated; the scorecard says so. A difference with no identified cause still stops the re-base.
- Ruling W0-g (Q11): the factor-break repair rule v1 is applied mechanically to the longer window. New flagged sessions
  or changed repairs are reported in the overlap report -- the rule predates the window and is not tuned -- cost if
  wrong: one role rebuild.
- Ruling W0-h (Q13, Q15): until A-3 lands, specs carry integer `dsr_n` (38, 39, 40). PBO, effective N and cross-trial
  variance over cells of unequal windows are the "legacy" figures of OD-4 and gate nothing.
- Ruling W0-i (Q16): role, projection and fields builds on the 4-year window run under 600 s and 2,560 MiB -- same
  reasoning as OD-2; these are data preparation, not trials -- cost if wrong: none for inference.
- Ruling W0-j (Q18): if B0a (lo1) wins, B0c is built on lo1 with delisting returns. The role builder gets the delisting
  option for linked-operating-v1 (lane F) -- terminal returns are a property of the return series, not of the universe
  rule -- cost if wrong: if the option cannot be added cleanly, B0c runs on lo3 and the ledger says B0c changed two
  things at once.
- Ruling W0-k (Q24, Q25): counters of refused sealed rows in manifests are metadata and are accepted. Whole-file SHA
  pinning of a multi-year stage hashes bytes without parsing them and is compliant.
- Ruling W0-l (Q27): the optional bridge `--check` against the atx-db warehouse is skipped -- root never touches atx-db.
- Ruling W0-m (Q29, Q30): B0 cells run the monitor without holdings and bias inputs; lo3 fields with no 3-year
  reference are reported, not gated.
- Q32 confirmed: `HOLD_BEGIN_NS` drives screen v3-admit-v1 only; untouched.
- Q33 is an owner decision: validation-window artifacts that hold 2024 data stay on disk untouched. Nothing is deleted.
- Deferred minor (Q26): `prepare_identity_bridge.py` DEFAULT_ROLES names a validation role; root always passes `--role`.

Disclosure (Q34): the runbook agent's first manifest filter printed per-year row coverage counts for 2009-2026 from the
atx-db earnings_calendar and sec_filings manifests, including 2024-2026. These are data coverage counts, not returns,
IC or any statistic of a signal. Nothing uses them. No other hidden-window content was opened.

Routed to lanes: second seal constant and sealed-partition guards (W0E); `backtest_integrity.py` TRAIN end and protocol
ledger lines (EV); protocol lines in `research_cycle.py`, per-phase caps, `cache gc` (A); delisting option on lo1 (F).

Disk: 69.9 GiB free; W0-2 plus B0a, B0b needs about 16 GiB, B0c about 7 GiB. No deletion needed now. Superseded
candidate caches up to v6.1 hold about 25.6 GiB; deletion is left to the owner.

## 2026-09-29 lane D ruling (D-0)
- Lane D: at `score_begin` each book is resized to initial NAV and every reported quantity restarts there; row
  `score_begin` is the base row and return rows start at `score_begin + 1`. K = 0 is byte-identical. Commit 168278f2.

## 2026-09-29 integration 1 (B-3)
- Merged f5f8754e, 0e95a072, bdca4c3d (lane B, B-3). Build v8-1b clean. atx-impl-strategy-ic-tests 75/75.
- Move check: every function body verbatim. dsl_vm_sources pin 29 -> 30 entries (adds lit_ops.hpp); neither pin is part of a cache key.
- K1 changes `--plan-only` JSON: `candidates` is now an array, the count moved to `candidate_count`.
- Identity run v8-b3-id-u-run1: time-limit at 300 s on a cold cache under host memory pressure, 39 of 48 done. The 39 signal
  payloads are byte-identical to the v7.1 cache; train_daily_ic.csv is a byte prefix of the accepted file. Identity is
  not closed; the re-run on the populated cache closes it (integration 2).
- Lane commits waiting: EV d6083b33 425d16db; D 168278f2; B be51d529; F c560b8c8; C cfa18014; A fa61c67f ea7cba01.

## 2026-09-29 ledger fold-in (integration 3: ledger-pending.md and ledger-pending-2.md)

### From ledger-pending.md: Ledger entries pending fold-in to progress.md (integrator 2 holds the tree)

### 2026-09-29 wave 1 lanes finished (code complete, C++ not yet compiled unless merged by an integrator)
| lane | pool | commits | state |
|---|---|---|---|
| B (B-3, B-1, B-2) | 10 | f5f8754e 0e95a072 bdca4c3d be51d529 6c7cb27e | B-3, B-1 merged (integration 1, 2); B-2 waits |
| A (A-3, A-1, A-2, cache gc) | 11 | fa61c67f ea7cba01 97f5ca02 59b89d07 | A-3, A-1 merged (integration 2); A-2, gc wait |
| EV (E-3, V-1, V-2) | 8 | d6083b33 425d16db 3abf78fd a27e69fe | E-3 and the tool move merged; V-1, V-2 wait (need W0-1 first) |
| F (F-2, F-0, F-1) | 7 | c560b8c8 872b7125 1e44f90a | F-2 merged; F-0, F-1 wait |
| D (D-0, D-1, D-2) | 4 | 168278f2 d1b63cff 83375f5b | D-0 merged; D-1, D-2 wait |
| W0E (W0-1, E-1, E-4) | 3 | 880faac7 59001f4a 40163643 | W0-1 merge ordered as integration 2 part 5; E-1, E-4 wait |
| C (C-1, C-2, C-3) | 9 | cfa18014 4e953ff7 07be7eff | C-1 merged; C-2, C-3 wait |

Second wave of lanes (dispatched as pools freed): R1 (pool 10: overlap tool 39bacec7, then R-1 composition v8),
R45 (pool 11: R-4 hysteresis, R-5 ADV cap), G (pool 8: single trial count, diagnostics), R6 (pool 7: target-tracking
optimiser, solver in atx-engine), H3 (pool 4: mining verb glue), draft lane (read-only: library-v8-draft.md).

### Rulings
- Ruling E-9 (trial count): the N that gates is the validation kit's defect-rule count (`backtest_integrity.trial_counts`):
  invalid cells, window re-runs and protocol lines add 0. `research_cycle.py` calls the same function (lane G) --
  two tools printing two values of N would make every DSR figure disputable -- cost if wrong: one number restated.
- Ruling E-10 (B0c scope): B0c differs from the winner of B0a / B0b only in the return series (terminal returns on) and
  the warm start (60 sessions). Signals, fields, admission and weights are those of the winning cell; the role with
  delisting returns is used for labels of the NAV replay only -- the imputed terminal return must not feed a signal
  dated the same session, and the correction must be isolated to be read -- cost if wrong: the baseline mixes two changes.
- Ruling E-11 (report seal check): the seal check covers data paths under the research output root. Document sources
  (sprint folders under `.superpowers/sdd/`, `docs/plans/`) are exempt from the year rule by root class; the named
  pattern (validation, holdout, VAL) still applies to them -- a folder's run date is not a data date -- cost if wrong:
  a quote from a validation-era document renders; the config lists every quote source. Fix in the Wave 6 report lane.
- Ruling E-12 (plan defect): the plan's build preset name `equity` does not exist; presets are `equity-dev` and `equity-rel`.
- Ruling E-13 (OD-2 trigger): the raised IC caps apply when the role has more than 1,200 dates (not scored sessions);
  the 4-year role has about 1,405 dates -- as coded by lane A.
- Lane D ruling recorded: timers are written only with `--stage-timers` because `summary.json` is pinned in the ledger.
- Lane F note: `ret_overnight` and `ret_intraday` need an `open` column in the role's source file; unconfirmed. If absent
  the build stops with `FieldNeedsOpen` and the candidate `night_day` is withdrawn before any read.

### Disclosures (hidden-data rule)
- Lane C, while counting fields, globbed `build-equity/*fields*/manifest.json` and so opened the six manifests of
  `recent-fast-validation-2023-2024-v1-fields-v1..v6`. Printed: field count, whether `sv_ratio126` is listed,
  `role.last_session`, whether a reuse block exists. No coverage figure, return, IC or other statistic was printed or
  used. Recorded here; nothing depends on it.

### Open items for integration 3
- Merge order: W0-1 (if not merged in part 5), then V-1 / V-2, B-2, C-2, C-3, A-2 + cache gc, F-0, F-1, D-1, D-2, E-1, E-4,
  overlap tool. After the merge: remove lane C's `window_id()` fallback and lane D's copy of `research_window.hpp` (take
  W0-1's), recompute the two source pins, check that the IC executable's `--help` names `--no-composition` and
  `marginal` (lane A detects both from the help text), check K6 file layout against the card reader, run
  `test_plan_rows_equal_static_validation` against a real plan JSON (`ATX_V71_PLAN_JSON`).
- Spec wiring: `fit.work_dir` and card `--work-dir` in the v8 specs.
- Synthetic fixtures dated 2024 in the SEC and holdings tests: lane W0E bound the superseded window for those tests in
  `atx-engine/tools/conftest.py`; redating is a deferred minor.

### From ledger-pending-2.md: Ledger entries pending fold-in to progress.md (integrator 3 holds the tree)

### 2026-09-29 library v8 draft received (library-v8-draft.md, written blind; no data, IC or return read)
READY 7 (ear_mom_12m, earn_surprise_comp, op_rd, dtc_slow, pct_accruals, fip_id, si_low_io). NEEDS-FIELD 6 (the FF49
block as 8 `_f49` re-screens, comp_eq_iss_5y, coskew_60m, gscore_lowbm, nonreliance_402, earn_consistency). WITHDRAWN 3
at 0 trials: tax_book (current tax not exported, OD-6), night_day (no agreed sign; open unconfirmed), nt_late (no
producer reads NT forms, OD-6). Admission trials 9 to 12 of 15.

Rulings on the draft's section 9, declared before any read:
- R2-a: earn_surprise_comp inner terms X_m are the members' single-decay `x` -- a double decay adds staleness -- cost if
  wrong: the composite is one decay faster than intended.
- R2-b: budget exception granted for earn_surprise_comp (8 extra fields, about +131 MiB) -- inside the OD-2 cap -- cost
  if wrong: none for inference.
- R2-c: op_rd uses the cbop zero-fill of xrd_ttm on existing fields -- same registered semantics, no wait for fields v10
  -- cost if wrong: rows with NaN sale_ttm get R&D 0 instead of NaN.
- R2-d: dtc_slow keeps raw volume -- one variant per hypothesis -- cost if wrong: split names carry an inflated ratio
  for up to 126 sessions.
- R2-e: the FF49 regrouping enters as new `_f49` ids. The 8 re-screens count 0 admission trials -- the same hypothesis in
  another peer group, declared before any read -- cost if wrong: the admission count is understated by 8; every
  Appendix A block states "plus 8 re-screens" so the reader can count them either way.
- R2-f: the static checker of record is K1 (`--plan-only` through `generate_library`). The legacy checker
  check_fund_ic_v6.py is not run on v8 libraries; no respelling of ts_mean -- respelling changes bytes for no gain --
  cost if wrong: a refused string at add-alpha time, caught before any read.
- R2-g: pct_accruals carries the house zero-denominator guard.
- R2-h: op_rd tier A- (the brief is the registration).
- R7-a: the roster cap is 64 for v8.1 -- the cap is mechanical, not statistical -- cost if wrong: none for inference.
- R7-b: filing_events may be a one-member theme; the member cap 1/(2T) of ew-theme-std-v1 bounds its weight -- cost if
  wrong: concentration on a sparse short flag, bounded by the cap.
- R7-c: fields v11 carries F-B, F-C, F-D when built; anything absent at the freeze is withdrawn at 0 trials.
- Fields lane F3 (pool 9) builds grp_ff12f49, k8_item402_63, gscore7_lowbm, eps_consist_4y from the draft's section 8.

## 2026-09-29 integration 3 (W0-1 and nine lane commits; stopped before the identities)
- Merged W0E 880faac7 (W0-1), 40163643 (E-1, E-4); EV a27e69fe; B 6c7cb27e; C 07be7eff; A 59b89d07; F 1e44f90a;
  D 83375f5b; R1 39bacec7; G ec206a59. Head after the goldens: 5cc9eb0d. Details: integration-log.md "integration 3".
- Literal seal fallbacks removed (fitter window_id, overlap tool, exposures refusal); lane D's research_window.hpp dropped
  for W0-1's. dsl_vm_sources re-pinned to 31 paths afbae65d... (research_window.hpp listed); ic_result_sources unchanged.
- Builds v8-3 (mega-build) and v8-3a failed on the JSON include path of the engine data tests; v8-3b, v8-3c clean with
  scripts/research-build.ps1 (receipts carry Tag, Script, BuildDir, Executables).
- Tests green except pre-sprint FactoryOos x2 and ConfigJsonNotInDiscoverDigest. tiny_world admission golden moved by
  W0-1's train_window_ns only; re-recorded. ParallelLockstepGrid not built (parallel group not configured).
- Findings: real v7.1 plan differs from the recipe's static figures on 5 of 154 values (lane A); marginal step lacks
  --role and a themes file (lanes A, F).
- Not done (owner stop): Part 4 identities a-g (W0-1 NAV, C-1 context ruling, C-3 reuse, B-2 workers, D-1 timers,
  F-0 lo1 role, E-1 last build).

## 2026-09-30 STOP at owner instruction; handoff 1 written
- Owner (2026-09-29 late): "stop at the next logical break point for all agents and write a detailed handoff file for the
  next parent agent + a goal prompt". All 8 running agents stopped at a commit boundary; every lane tree clean; every
  lane report committed in its worktree with a STOPPED HERE section.
- Root HEAD ca0e59d8 (integration 3 parts 1-3 done; identities a-g NOT run). Wave 1 merged. Unmerged lane heads:
  R1 ec2dfd16 (pool 10 old branch), R45 638957ab (11), G fea9b6e9 (8), R6 52501f31 (7), H3 763b0759 (4),
  H1 a30b6038 (3, design only), F3 79e5e006 (9), REPORT 01158204 (10).
- Handoff: docs/plans/2026-09-30-platform-v8-handoff-1.md. Goal prompt: docs/plans/2026-09-30-platform-v8-next-goal-prompt.md.
- Open rulings for the next PM (handoff section 5): spo-v3 S_prior (lane finding: 1.0 loses the stock-level bets on
  synthetic data; 20 tracks at .96); ADV cap NAV at 4x; hold-band decide parity; F-1 reuse interface defect; H-1 blockers;
  two FactoryOos pin test failures of unknown cause; C-1 context_sha256; plan-row static figures differ on 5 values;
  marginal step lacks --role.
- Trial accounting unchanged: TRAIN construction cells 37; no read on the 2020-2023 window; validation reads 2 (2023-2024,
  before v8); 2025+ never read. Two hidden-data disclosures this sprint (manifest metadata only), recorded above.

## 2026-09-30 PM session 2 (goal: finish v8 from handoff 1). Rulings before dispatch; no read yet on 2020-2023
State at start: root bef1c906 clean; all lane trees clean; no `ledger-pending*.md` left (folded in integration 3).
Order of work: integration 3 Part 4 identities (a-g), integration 4, Wave 1 review, Wave 0, cells.

- Ruling E-14 (R-6 S_prior, pre-read amendment): the spo-v3 registration is amended to S_prior = 20, and the cell gains
  the mechanical criterion "mean correlation of the traded book with the aim over scored decisions >= .9" -- 1.0 was a
  guess, not literature; on the lane's synthetic prototype 1.0 follows factor loadings only (correlation .35, gross
  .37 L), which is another book, not a cheaper way to hold the accepted one; no TRAIN statistic informed the change --
  cost if wrong: the tracker sits so close to the aim that the cell measures only a small cost saving; one trial.
- Ruling E-15 (R-5 NAV in the ADV cap): accepted as coded (R5-b): the cap uses the run's initial NAV for every capacity
  book -- the rule is a $1bn rule and the 4x figure is a stress of that book; a per-multiple cap needs a per-book
  desired target -- cost if wrong: the 4x acceptance measures a cap set for $1bn (holdings up to 4 x Q of ADV at 4x);
  disclosed on the cell.
- Ruling E-16 (R-4 decide parity): the holdings export gains `rank_set` / `desired_prev` (lane R45); it does not block
  the R-4 cell -- v8 deploys nothing -- cost if wrong: `decide --check-replay` fails on a hold-band book until it lands.
- Ruling E-17 (H-1 finding 1): "frozen book" for a history read means frozen rules with pooled re-admission (Python
  tooling as designed); no runner change in v8; era window rule in `backtest_integrity` and the pooled fitter mask are
  approved cross-lane edits -- rebinding E3 weights to another role would defeat the role binding -- cost if wrong:
  the owner wants frozen weights and a small runner change follows. No era is read in v8 (Ruling E-2 stands).
- Ruling E-18 (G runtime): diagnostics run in `--only` splits under the preparation caps 600 s / 2,560 MiB -- they are
  not trials and gate nothing -- cost if wrong: none for inference.
- Ruling E-19 (plan rows): K1 (`--plan-only`) is the checker of record (R2-f). The A-1 test compares DSL SHA, lookback
  and extra fields per candidate and the maxima, and requires `validate_plan` to accept; the recipe's static node and
  slot figures stay as committed (the v7.1 recipe must regenerate byte for byte) -- cost if wrong: a one-node
  difference between the Python mirror and the exe goes unflagged; K1 still enforces the budget.
- Ruling E-20 (F-C, F-D open questions, declared before any build): F-C quarterly sales growth is
  `sale_ttm(P) / sale_ttm(P - 1q) - 1`; both variances need 12 of 16 quarters; bm uses `me_company` at t-1; peers are
  member names; ratios are scaled by `at` as the house fields do (Mohanram scales by beginning assets and uses the
  lowest BM quintile: disclosed deviations; the tercile is registered). F-D: denominator
  `mean(abs(EPS_q-4), abs(EPS_q-8))`, NaN when 0 (the CZ code as recalled, not re-read: disclosed); shares on the
  as-reported basis, as CZ's raw EPS (a split inside the span distorts g for up to 8 quarters: disclosed); sign +1 --
  one variant per hypothesis, no data read -- cost if wrong: a C+ / B- member is mis-specified and its prior is
  weaker than stated.
- Ruling E-21 (F-1 reuse interface defect): lane F3 fixes it as its first commit (Python, testable in the lane);
  integration 4 merges it.
- Ruling E-22 (FactoryOos x2): triaged read-only against main before Wave 0; if pre-sprint, recorded as an owner item
  and not fixed here (factory goldens are outside the v8 plan).
- C-1 `context_sha256`: ruled after identity b states what it hashes.

## 2026-09-30 integration 3 Part 4 closed (identities a-f; log section "integration 3 Part 4", commit c5492e4a)
- a W0-1 NAV identity PASS (12 files byte-identical). d B-2 workers 4 vs 12 PASS (daily IC, planned targets, combined
  byte-identical to each other and to mega-v71-train-u-1; cold 119 s / 1,212 MiB and 143 s / 1,216 MiB, both inside
  300 s). e D-1 timers PASS (summary.json gains only `stage_seconds`).
- c C-3 FINDING: recorded argv refused (regsho stage republished, as W0-d); with the live pin 62 of 63 payloads equal
  fields-v9, regsho_threshold_days63 differs by the new stage. Reuse copied 0 of 63: W0-1 changed every builder
  group's code fingerprint (one-time). The copy path itself is still untested: step 2 (`--reuse` from the new dir,
  expect 63 reused) runs in integration 4.
- f F-0 FINDING, accepted: all 7 role payloads byte-identical; manifest differs in 19 paths, all explained (two code
  identities, W0-1's seal text and 12 seal-drop counters).
- No disclosure. No statistic read on 2020-2023.
- Ruling E-23 (C-1 `context_sha256`): accepted as coded. It hashes data (five context arrays, role SHA, window, two
  constants); C-1 moved the fitter's code SHA out of it into the code fingerprint, which is why admission.json differed
  in two provenance hashes and admission.csv in none -- code identity and data identity are separate keys by design
  -- cost if wrong: none for inference.
- Ruling E-24 (workers): IC passes stay at `--workers 4` on this host (12 workers: ic 18.3 -> 8.9 s but vm and
  composition slower, net +23 s) -- measured -- cost if wrong: seconds.

## 2026-09-30 FactoryOos triage (read-only lane; nothing built)
- Verdict: PRE-SPRINT, confidence about 85%. Pins `factory_oos_test.cpp:893` (4049056013) and `:1589` (703512706)
  were last set in 3a1197b5 (2026-09-24). a187e2fe (2026-09-26, on main before base 7fbfc379) added
  `&& !bind_recipe_identity_` to the V1 short-circuit in `store.hpp` `record_crc`, so each alpha's segment_crc is now
  crc32(integrity_crc, recipe_crc) on a fresh directory (rule ExistingOrSignedV2). `version_id` moves; digest and
  admitted count do not. No sprint file feeds `compute_version_id` (`manifest.hpp:123-159`).
- Ruling E-22a (amends E-22): integration 4 runs the one-build confirmation (pass `CorrIndexRule::LegacyBandsV1` to
  `Library::open` in both tests; the old pins must come back), then reverts that edit and re-pins the two
  `kPinnedVersionId` values to 916304603 and 3123399341 with a comment citing a187e2fe -- a standing red test hides
  new failures, and the move is the intended V2 design (`library.hpp:580-582`) -- cost if wrong: a factory golden is
  re-pinned over a real regression; the confirmation build bounds that. If the old pins do not come back, no re-pin:
  owner item.

## 2026-09-30 lanes dispatched (PM session 2; all merged root 41ac94fd first)
| lane | pool | branch | work |
|---|---|---|---|
| R6 | 7 | feat/platform-v8-r6-20260929 | spo-v3 rule part 2 (S_prior 20, E-14) |
| R45 | 11 | feat/platform-v8-r45-20260929 | R-5 missing tests; E-16 hold-band state in the holdings export |
| F3 | 9 | feat/platform-v8-f3-20260929 | F-1 reuse fix (E-21), F-B, F-C, F-D (E-20) |
| REPORT | 10 | feat/platform-v8-report-20260929 | tasks 2-4 |
| H3 | 4 | feat/platform-v8-h3-20260929 | mining verb parts 2-3 (fixture only) |
| H1 | 3 | feat/platform-v8-h1-20260929 | era shards tooling (E-17) |
| A2 | 8 | feat/platform-v8-a2-20260930 | marginal step argv; E-19 test; v8 spec drafts |

## 2026-09-30 rulings after lanes A2, R45, R6, REPORT reported (declared before any read on 2020-2023)
- Ruling E-25 (B0c label role): the NAV verb gains `--label-role MANIFEST --label-role-sha256 SHA`: signals, fields and
  decisions stay bound to the winner's role; prices and returns that mark the book come from the label role (the same
  base, same dates, instruments and membership, built with delisting returns; anything else refused). Spec input
  `inputs.label_role`. Flag off is byte-identical -- E-10 isolates the return correction from the signals, and the
  NAV verb binds one role today -- cost if wrong: B0c mixes two changes or slips one lane-day.
- Ruling E-26 (spo-v3 under an accepted R-4 / R-5): `w_aim = L x desired` of the accepted rule includes the hold band
  and the ADV cap when those cells were accepted; spo-v3 therefore accepts `--hold-band` and `--adv-hold-q` for the
  desired target (the tracker's own limits unchanged) -- the registration says "the accepted rule" -- cost if wrong:
  R-6 tracks an aim without the accepted shaping and the cell confounds two changes.
- Ruling E-27 (R-3 on R-1 weights): the persistence gains g_k multiply the member weights of the parent's composition
  (tier weights inside each theme under ew-theme-std-v1), renormalised inside the theme so each theme keeps 1 / T; the
  member cap 1 / (2T) is applied after; id `ew-theme-std-aim-v1` when the parent is R-1, `ew-theme-aim-v1` otherwise
  -- "applied on top of the R-1 weights" (plan R-3) -- cost if wrong: gains shift weight across themes and R-3 is not
  separable from R-1.
- Ruling E-28 (w-pass memory with the theme block): phases whose composition is ew-theme-std-v1 (or its aim variant)
  run under 3,072 MiB (runner and `--max-memory-mib`), 300 s -- admission about 2,606 MiB at n 6,100; mechanical, not
  statistical; measured RSS is about 82% of admission -- cost if wrong: the runner refuses on the free-memory floor;
  then workers 1 or a quiet host.
- Ruling E-29 (capacity curve on B0c): confirmed, report only; the primary series and scored files are unchanged --
  every R cell reports 4x against its parent (plan section 9) -- cost if wrong: seconds.
- A2 concerns accepted as work items: add-alpha with a v8 parent spec; `cache gc` must keep a derived child of a
  named store base; `_f49` ids join the R-2 gate when the fields dir with `grp_ff12f49` exists.

## 2026-09-30 lane results (session 2)
- R45 done 8fa1005f (R-5 tests, E-16 state in the holdings export, grid fix dca2165a). Then re-tasked: E-25 label role.
- R6 part 2 done 23d663b4 (spo-v3, S_prior 20). v2 digest pin is a placeholder (capture needs a build). Re-tasked: E-26.
- REPORT tasks 2-4 done 72ac06d4; follow-up (legacy blocks, header, final check) running.
- A2 done 115b8877 (marginal argv; E-19 test passes on the real plan; v8 specs and templates). Re-tasked: add-alpha
  with a v8 parent, cache gc, R-3 gains (E-27), caps (E-28).
- H1 done cf757c97 (era_pool, pooled fitter / nav_summ / ledger, roles loop, era_data_audit; 83 new tests; no era
  read). Open: the pooled fit refuses ew-theme-aim-v1 (and knows nothing of ew-theme-std-v1): an OD-3 read of a V8-F
  that carries a v8 composition needs a follow-up after A2's R-3 commit. TickerHistory3 coverage of mid-2012
  unverified. Whether B0c's era run is ledgered is a PM decision at OD-3 time.
- F3 done fc8ff96c (E-21 fix eca04c18; F-B 0687e82f; F-C 1444310f; F-D 0f32d581; 147 field tests pass). Expected
  `--reuse`: fields v10 from the rebuilt v9 reuses 49 and computes 21; v11 from v10 reuses 70, computes 3. v10 has 70
  manifest rows, v11 73 (B-2's cap 1,024 is merged). F-D is defined from about mid-2019 (events start 2014).
- Ruling E-30 (F-C / F-D lane choices, accepted as the registration before any build): fiscal quarters matched within
  +-20 days, ties to the later quarter; peer medians per measure; tercile boundary inclusive of ties; "sign differs"
  read strictly (a zero is not a differing sign) -- chosen blind by the lane, one variant per hypothesis -- cost if
  wrong: a B- / C+ member is slightly mis-specified against its paper.
- REPORT follow-up done bf3de0dc / a9a244f6 (legacy book blocks as section 5, header, `final` check; 384 tool tests).
- R6 E-26 done 3bb1dea4 / f60a524e: spo-v3 already took the shaped desired target through the shared `form_desired`;
  the rule id records the shaping; spo-v1 / v2 now refuse both flags (they accepted them silently). Flags absent: no
  spo-v3 byte changes.
- Ruling E-31 (R-6 unconverged solves, declared before the cell): no early exit is added in v8 (any exit changes the
  book and the carried dual of every later decision). On the R-6 run the convergence counts (`unconverged`,
  `limits_unmet`, mean iterations) and the tripwire are read before any return. `limits_unmet > 0` on scored
  decisions of the primary book is a mechanics defect: the run is invalid, the fix is decided without seeing returns
  and the rerun replaces it with no new trial (pre-registration rule 7) -- a book that misses its net or beta limit is
  not the registered problem -- cost if wrong: one lane-day for the infeasibility handling.

## 2026-09-30 integration 4 part A closed (log section "integration 4 part A"; head 7af37e9d)
- Merged R1 ec2dfd16, R45 8fa1005f, G fea9b6e9, R6 23d663b4 (parts 1 and 2), F3 0687e82f (F-A, E-21 fix, F-B),
  REPORT ec47b8ce, H3 95859cc9 (part 1), H1 1e66a7e1 (report), A2 d55ad8e1. Last good build v8-4e (11 targets).
- Pins: `dsl_vm_sources` 33 paths, digest f24cfbbe...; `ic_result_sources` unchanged; no semantics bump (verbatim move).
- E-22a: with LegacyBandsV1 the old FactoryOos pins came back; re-pinned (c41d401e); factory 377/377.
- C++: data 284 passed 14 skipped; combine 220; book 147; ic 101; strategy 45; target 219 passed 1 skipped; impl 940
  passed, 6 skipped, 1 failed (known ConfigJsonNotInDiscoverDigest). Python: strategies 163, engine tools 214, impl
  tools 314 (2 skipped), scripts 97 (3 skipped). tiny_world passes, no golden moved. DSR N test passes (handoff 5.8).
- Open: spo-v2 digest pin needs a pre-R6 build (test skips); part B identities; ParallelLockstepGrid group not
  configured; build provenance records 5c6efcd4 for v8-4e.
- Next: integration 4 part B (merge F3 fc8ff96c, H1 cf757c97, REPORT a9a244f6, R6 f60a524e; build v8-5; identities)
  beside the Wave 1 adversarial review at the fixed commit 7af37e9d (three read-only reviewers by area, one review).

## 2026-09-30 lane H3 done (339c07b1; engine part 2 2beb83c2, atx-impl part 3 12fc27a9; not compiled, fixture not run)
- Built: pluggable signal-fitness hook in the search driver (default off, golden digests pinned at 1 and 4 workers),
  eligibility mask, catalogue options, research-IC fitness; shared research-role loader; `atx-equity-strategy-mine`
  (two stages, V3 trial registry, rule mined-v1), target `atx-impl-strategy-mine-tests`.
- Ruling E-32 (mined-v1 confirm statistic): the confirm read's "HAC t 2.0" is the marginal IC HAC t on the confirm
  window with the sign frozen from discover -- the same statistic as the promotion hurdle; a raw-IC confirm would pass
  a spanned signal -- cost if wrong: the confirm is stricter than the plan meant; it binds only under OD-7.
- Ruling E-33 (ledger kind): `mining-campaign` joins `LEDGER_KINDS`; a campaign line adds 0 to the construction N and
  carries its own registry count (pre-registration rule 10: mined campaigns have their own budget) -- cost if wrong:
  N understated by the promoted members' cell, which is still counted when a mined member enters a construction cell.
  Work item for the fix lane. No campaign runs in v8 (OD-7 tooling only).
- Open for a real campaign (OD-7): memory admission of the mine verb is an estimate (8 VM slots per cell).

## 2026-09-30 STOP at owner instruction (PM session 2); status 2 written
- Owner: "stop here and write a detailed status markdown file, then determine if there is enough progress to justify
  a v8 pitch with improved metrics". All six running agents stopped at a clean boundary; every tree clean.
- Root head 237486fe. Integration 4 part B stopped after its merges, build and tests: F3 fc8ff96c, H1 cf757c97,
  REPORT a9a244f6, R6 f60a524e merged; build v8-5 clean (ic 4b4ffb7b..., targets 0d0a6921..., risk f45e8870...);
  target-tests 222 passed 1 skipped; Python strategies 163, engine tools 240, impl tools 452 (2 skipped), scripts 121
  (3 skipped); tiny_world passes, no golden moved. Identities i1-i7 NOT started; i8 (E-1 receipt) passes.
- Unmerged lane heads: R45 126a5f5f (E-25 label role; wip ec7356a4, payload check of cleared members and its test
  remain), A2 79440cfa (add-alpha on a v8 parent, cache gc, ew-theme-std-aim-v1, templates), H3 339c07b1 (mining verb
  parts 2-3; never compiled).
- Wave 1 adversarial review (three read-only readers on the most capable model, fixed commit 7af37e9d) stopped
  early: PARTIAL coverage, no test file read. Findings: I 1, M 20, m 23, in review-w1-A.md, review-w1-B.md,
  review-w1-C.md (this directory). Not fixed. The I finding (C-1): the cycle's verdict and headline DSR do not use
  the pre-registered cross-trial variance (`--dsr-ledger` never passed). Must be fixed before any cell verdict.
  C-9 / C-10 (overlap tool can report `bit_identical: true` on zero cells; NaN-vs-value ignored) must be fixed
  before the W0-a overlap reports.
- Ruling E-34 (review C-8, declared before any read): the freeze gate's "bootstrap p < .10" is one-sided, as coded
  in nav_summ since the v7 bundles -- the registered hypothesis dSR > 0 is directional -- cost if wrong: the gate is
  twice as loose as a two-sided reading; the scorecard prints both p values.
- Trial accounting unchanged: TRAIN construction cells 37; admission trials this sprint 0; no role, field, IC pass
  or cell on 2020-2023; validation reads 2 (before v8); 2025+ never read. No disclosure this session.
- Pitch determination (status 2, section 9): a v8 pitch with improved metrics is NOT justified: no v8 metric exists.
- Status: docs/plans/2026-09-30-platform-v8-status-2.md.

## 2026-09-30 PM session 3 (root head 81abfa12; code 237486fe; build v8-5)
- Goal prompt: docs/plans/2026-09-30-platform-v8-next-goal-prompt.md (8 steps). Owner approved every recommendation
  in the plan, handoff 1 and status 2. Model: Opus 5.5 for every lane and reader; PM coordinates only.
- Ruling PM3-1 (fix-lane split): the Wave 1 fix work runs as two lanes with disjoint file ownership -- FIX-C
  (pool 3, branch feat/platform-v8-fixc-20260930 from 81abfa12: C-1, C-2, C-9, C-10, C-3..C-7, C-13, C-11, C-12,
  E-33) and FIX-AB (pool 7, feat/platform-v8-fixab-20260930: B-2, B-3, B-4, A-2, A-3, A-4, A-1, B-1) -- the C
  findings are Python research tooling, the A/B findings C++ and field builders; parallel halves the wall clock --
  cost if wrong: one textual conflict for the integrator.
- Ruling PM3-2 (pools): lease-worktree shows every pool leased by dead heartbeat owners of 2026-09-25..27 sessions;
  sessions 1-2 used pools 3,4,7,8,9,10,11 directly and every tree is clean at its lane head; this session does the
  same -- no live owner exists -- cost if wrong: none observed; a real owner would show a live keeper.
- Ruling PM3-3 (review part 2): the cut-short Wave 1 review completes as three read-only readers on root at
  81abfa12: T (every test file), N (files never read), P (everything merged after 7af37e9d); briefs in
  review-w1-part2-brief.md; outputs review-w1-T.md, review-w1-N.md, review-w1-P.md -- same method as part 1 --
  cost if wrong: duplicated findings, deduplicated at the fix lane.
- Ruling E-35 (era fit and v8 compositions; open decision of status 2): the pooled (era) fit must support
  `ew-theme-std-v1` and `ew-theme-std-aim-v1` before any OD-3 history read; today it refuses the aim variant and
  ignores the std variant, and a V8-F that descends from an accepted R-1 would carry one of them. Implemented in a
  lane after A2 merges (integration 5), tested on synthetic data, no era read -- B0c's lineage will carry a v8
  composition whenever R-1 is accepted, and the OD-3 read is on the critical path at the freeze gate -- cost if
  wrong: one lane-day unused if R-1 is rejected.
- Dispatched: FIX-C (pool 3), FIX-AB (pool 7), R45 finish E-25 (pool 11), readers T, N, P (root, read-only).
