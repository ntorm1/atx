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
- R45 E-25 finished 9cc0d3cb (check 08c9ac7b, text 8bb2ee3d, test 44451659; pool 11). Never compiled. After merge:
  integrator wires FIX-AB's B-3 refusal into `check_label_manifests(role, label)` on `role` only
  (strategy_target_replay.cpp). Open: label role `score_member_counts` unchecked against payloads (NAV never reads it).
- Review part 2, area P done (review-w1-P.md; 28 rows): I 0, M 3, m 8. P-1 history-read ledger line (label ERA, no
  era_of) counted as a TRAIN cell by dsr_variance / cells / ledger_net_series; P-2 pitch config and scorecard omit
  registered criteria (R-6 E-14 and tripwire and E-31; R-5 S3 not lower; R-7 turnover not higher); P-3 ladder_checks
  never compares recorded verdict with the computed rule nor parent = last accepted. All three go to fix lane 2.
- Review part 2, area T done (review-w1-T.md; 34 rows): I 0, M 4, m 9. T-1 ew-theme-std-v1 C++ fixture cannot
  fail a wrong rule 1 (degenerate ranks, signs, equal weights); T-2 gscore7_lowbm tests cannot tell which session's
  me_company is read (uniform mutation; a t+1 look-ahead passes); T-3 spo-v2 pin still placeholder 0 (test skips;
  root captures the pin at integration 5); T-4 no independent reference for the spo-v3 solver (multi-name optimum,
  external_gap, gamma annualisation unpinned). T-1, T-2, T-4 go to fix lane 2 (tests strengthened, code untouched).
- Review part 2, area N done (review-w1-N.md; 18 rows): I 0, M 3, m 5. N-1 holdings fields (ftd_shares_ratio21,
  regsho_threshold_days63, sv_offexchange_share126) depend on the seal but the reuse inputs never bind it; N-2 spo-v3
  refuses --capacity-curve (E-29 needs the 4x report on every R cell) and theta has no effect on spo-v3 (R-9
  undefined there); N-3 plan R-7 acceptance gates on K6 marginal IC while prereg rule 8 says marginal IC gates
  nothing; the card loads K6 unbound to role / window / library with an optional pin.
- Ruling E-36 (N-3, declared before any read): pre-registration rule 8 governs -- marginal IC gates nothing and
  selects nothing at cell acceptance. The plan's R-7 marginal-IC text is read as the entry screen (which candidates
  enter the cell, as for R-2), not as the acceptance rule. R-7's acceptance is rule 5: paired S2 net dSR > 0 against
  its parent, mechanics, and its mechanical criterion "turnover not higher" (the scorecard criterion of P-2). The
  card binds K6 to role, window, pool and library and its pin becomes mandatory (fix lane 2) -- the prereg file is
  the registration of record and the more specific text -- cost if wrong: R-7 may accept a member a K6 gate would
  have refused; one cell, disclosed on the card.
- Ruling E-37 (N-2, declared before any read): spo-v3 accepts --capacity-curve as a report-only pass (E-29; the
  primary series unchanged); R-9 (theta) is defined only on a parent whose rule reads theta; if the accepted parent
  at R-9 time is spo-v3, R-9 is skipped as undefined (it is optional in the registration) and the ledger says so
  -- theta is a v7 rule parameter with no meaning in the tracker -- cost if wrong: R-9 unrun; it is optional.
- Ruling PM3-4 (fix lane 2 and the era lane): the part-2 I/M findings are fixed by two lanes branched from root
  after integration 5 part A (FIX-C, FIX-AB, R45, A2 merged): lane ERA = E-35 + P-1 (pool 3); lane FIX-2 = P-2,
  P-3, T-1, T-2, T-4, N-1, N-2, N-3 card binding (pool 7); they run beside the H3 integration and the identities
  and merge before Wave 0 -- branching from the merged head avoids conflicts with the in-flight fix lanes --
  cost if wrong: Wave 0 starts one lane-turn later.
- FIX-C done d0d081f2 (pool 3; C-1 337ed421, C-2 9ddffedd, C-9/10 6aa72350, C-3 396e3e94, C-4 c0e90ab7, C-5
  c211624f, C-6 0aa411a9, C-7 1e82e212, C-13 af9f4d76, C-11 91c5dc5b, C-12 51aa3202, E-33 448bdc85). scripts/tests
  130 passed 4 skipped; atx-impl/tools 463 passed 2 skipped. Synthetic output moves: C-1 DSR .42 -> .91 (SR0 1.10 ->
  .33); C-7 admission 0 -> 1; C-9 zero cells now false. Cross-lane for integration 5: A2 specs must set summ.origin,
  keep --origin out of extra, give verdict specs summ.ledger; H3 mining verb must write `campaign_line`.
  holdout_gate now requires --ledger. Minors C-14..C-22 untouched.
- FIX-AB done dd677d3b (pool 7; B-2 c7c16599, B-3 dd9cbcf2, B-4 931c655a, A-2 f1a928f9, A-3 8e5ea802, A-4 98b0a0e8,
  A-1 f425e4ec, B-1 b900b387; history rewritten locally, unpushed). Verdicts: B-4 CONFIRMED (73-row manifest
  605,601 B of 1 MiB; widest 927,292 B; bound now 16 MiB); B-2 CONFIRMED partly (no theme re-rank in the marginal
  verb; the grouping mismatch does not occur for generated libraries); A-2 part 1 CONFIRMED (gamma at first warm-up
  decision), part 2 NOT A DEFECT on real risk stores, fixed anyway (warm-up moves as aim-partial-v5, reads no risk
  row). Pins: dsl_vm_sources_sha256 re-pinned ad6c4ca7 (B-3 refusal, no semantics bump). pytest 36 passed.
- Ruling PM3-5 (B-1 and the reuse identity): B-1 pins the NYSE rule calendar digest into the price fields' reuse
  inputs, so the first `--reuse` recomputes ret_overnight, ret_intraday, ceq_iss_5y, coskew_60m once. Identity 6
  (field reuse step 2) therefore expects 45 reused and 18 recomputed with those four payloads byte-identical to
  their previous payloads; the integrator records both counts -- the fingerprint widened, the values did not --
  cost if wrong: one identity re-run.
- Integration 5 part A dispatched: merge FIX-C d0d081f2, FIX-AB dd677d3b, R45 9cc0d3cb, A2 79440cfa; build v8-6;
  suites; wire B-3 into check_label_manifests(role); A2 spec keys per FIX-C.

## 2026-09-30 integration 5 part A closed (log section "integration 5 part A"; head b44774d6)
- Merged FIX-C d0d081f2 (d3b9513d), FIX-AB dd677d3b (d3e6d855), R45 9cc0d3cb (4b57a6af), A2 79440cfa (67c5aff0).
  Builds v8-6, v8-6a clean. C++: data 285/14 skipped, combine 220, factory 377, book 147, ic 105, strategy 46,
  target 233/1 skipped, impl 957 passed 7 skipped 1 failed (known ConfigJsonNotInDiscoverDigest). Python: strategies
  163, engine tools 241, impl tools 465/2 skipped, scripts 163/3 skipped. tiny_world 5/5, no golden moved.
- Integration fixes: d833b25f (B-3 refusal on --role only in check_label_manifests, test), 7c77fac2 (base-lo1/lo3
  summ.origin "prior"), 229f8e78 (SameRoleIsIdentity fixture row 140, test slip). dsl_vm_sources f24cfbbe ->
  ad6c4ca7 (FIX-AB re-pin, tripwire passes). Open: spo-v2 pin needs a pre-R6 build (part C); B0c's 60-session
  warm start must build a book or A-3 refuses it (expected: it builds one).
- Dispatched from b44774d6: integrator part B (H3 339c07b1, root), lane ERA (pool 3, E-35 + P-1), lane FIX-2
  (pool 7, P-2, P-3, T-1, T-2, T-4, N-1, N-2, N-3), scoped re-review of the fix lanes plus first read of the R45
  and A2 follow-up code (reader on pool 10 detached at b44774d6; output review-w1-fixes.md).

## 2026-09-30 owner directive: more parallel lanes on alpha generation (atx-engine + atx-impl)
- Ruling E-38 (new cells and the N budget, declared before any read): the registration holds N <= 51 with R-8 at 48
  and R-9's three theta cells at 49-51. E-37 makes R-9 undefined on an spo-v3 parent. Therefore: if R-6 is accepted,
  slots 49-51 run, in order, R-10 (`ic-shrink-v1`), R-11 (`theme-resid-v1`), R-12 (one add-alpha cell on the
  library v8.2 candidates that pass the screen; the three remaining admission trials; if none passes, slot 51 is
  unused); if R-6 is rejected, R-9 runs as registered and R-10..R-12 are not run in v8 (registered for v9). Each
  rule's constants are fixed blind by its lane and written in its report before any cell read (E-30 precedent)
  -- the hard budget is the owner's; the slots exist only where R-9 is undefined -- cost if wrong: three cells of
  combination work deferred to v9.
- Ruling E-39 (lo1 delisting returns; runbook R15 open question): linked-operating-v1 gains `--delisting-returns`
  (returns corrected as v3 does, membership unchanged) so B0c's label role exists whichever of lo1 / lo3 wins B0b;
  lane DLRET builds it blind -- B0c is the baseline and must not depend on which role wins -- cost if wrong: one
  lane-day; if lo3 wins the path is unused in v8.
- Ruling E-40 (R-8 built): the optional R-8 (ex-ante risk target, plan rule verbatim: sigma_star 5.0%, b 1.15,
  clip [.8, 1.25] L, 21-session cadence, book_variance on the gross-1 book) is built as an atx-engine scaler plus a
  flag, so it can run at N 48 after R-7 -- it is registered; the plan's D15 is the only variance-reduction lever in
  the registration -- cost if wrong: one lane-day.
- Dispatched (briefs in task-ALPHA-briefs.md): RISK (pool 9, from 28051c4c), DLRET (pool 4, from fd2ff7a8),
  LIB2 (pool 8), COMB2 (pool 11), ORTH (pool 10). All Opus 5.5. Merge after FIX-2 and ERA (integration 6).
- Scoped re-review of the fix lanes done (review-w1-fixes.md, at b44774d6): part 1 21 ADDRESSED, 0 NOT ADDRESSED
  (all I/M of W1 A/B/C plus E-33; NOT A DEFECT verdicts upheld; integration fixes agreed). Residuals F-1 (M: rerun_of
  checked only for existence/kind/window; unbounded blind re-runs each add 0), F-2..F-7 (m). Part 2 (R45 session 3,
  A2 follow-up): F-8 (M: add-alpha child of a labelled parent runs `ref` without --label-role; R-2 / R-7 would
  hard-stop at ref-s2-daily), F-9 (M: template cells hash only the template; parent chain unpinned; matching spec
  digest skips the argv check), F-10 (M: ew-theme-aim-v1 normalises gains across themes, no 1/(2T) cap), F-11..F-14
  (m; F-14: the r6 template removes --capacity-curve against E-37).
- Ruling E-27a (F-10): E-27 applies whichever parent R-3 runs on: `ew-theme-aim-v1` renormalises the gains inside
  each theme (theme share 1 / T kept) and applies the member cap 1 / (2T) after, exactly as `ew-theme-std-aim-v1`
  -- "on top of the parent's weights" cannot move weight across themes -- cost if wrong: R-3 on a rejected R-1 is
  not separable; it binds only if R-1 is rejected.
- Ruling PM3-6: lane FIX-3 (pool 10, from fd2ff7a8) fixes F-1, F-8, F-9, F-10 (E-27a), F-14 and the cheap minors
  F-2..F-7, F-11..F-13; it is on the critical path (R-2, R-7, N integrity), so it takes pool 10 ahead of lane ORTH,
  which starts when ERA or FIX-2 frees a pool -- cost if wrong: ORTH starts one lane-turn later.

## 2026-09-30 integration 5 part B closed (log "integration 5 part B"; head d22b735a)
- H3 339c07b1 merged e2bb716b, no conflicts, 0 build fixes (v8-7, v8-7a). One integration fix ccb66a87: the mine
  verb's ledger_line now equals `campaign_line` (E-33); new verb `research_cycle.py ledger-campaign` appends it
  chained. mine-tests 18/18; factory 387; ic 105 (pins hold); strategy 46; target 233/1 skipped; impl 965/7/1
  known; scripts 164/3 skipped; impl tools 465/2. Golden 0x889874a3b9b29c55 holds at 1 and 4 workers. Fixture
  acceptance (StrategyMineCampaign x3) passes. Open: H3 memory estimate untested on real fields; IC runner does not
  use ResearchRole yet; nothing appends a campaign line automatically (OD-7 only).
- Ruling E-33a (campaign registry count; binds only under OD-7): a campaign line's `registry.count` is the number
  of records this campaign added (new_records); the cumulative registry size is carried as `registry.total` --
  shared registries must not count earlier campaigns twice -- cost if wrong: one field rename before OD-7.
- Ruling PM3-7 (spo-v2 pin, T-3): the pin is captured from the current build after identity 7 (spo-v2 side files
  byte-identical to the pinned v7 side files) passes; identity 7 is the proof that the current spo-v2 equals the
  pre-R6 code, so no pre-R6 build is needed -- cost if wrong: none while identity 7 holds; if it fails the pin
  is not captured and the mismatch is the finding.
- Dispatched: integrator part C (identities 1-8, PM3-5 expectation 45 / 18, PM3-7), reader MINE (mining verb),
  reader SPO (optimiser: solve_tracking, spo-v3, E-14, E-26, E-31, FIX-AB A-2 / A-4), both read-only at d22b735a.
- DLRET done f51c5fd8 (pool 4; tests 5db4ce53). Finding: the brief premise was stale -- linked-operating-v1 has taken
  --delisting / --delisting-returns since F-0 (872b7125, merged 33742f7a), same function as v3, manifest records
  returns_applied and the events digest; runbook R15's "no lo1 rule" is wrong (corrected by this line). Added:
  Python test of an lo1 decision / label pair under the E-25 payload rule with a committed fixture
  (atx-impl/tests/fixtures/lo1_label_role_pair), three NavLabelRoleLo1 gtests. 28 pytest pass. Not compiled.
  B0c registration must name the delisting stage ($DL, as lo3) and build the label role from the same pins and
  tool commit as train-2020-2023-lo1. E-39 stands as the ruling that B0c's label role exists for either winner.
- ORTH dispatched on pool 4 (branch feat/platform-v8-orth-20260930 from fd2ff7a8).
- ERA done cddb5c8e (pool 3; E-35 80112395, P-1 dc84f27f). Pooled fit implements ew-theme-v1, v6, ew-theme-std-v1,
  ew-theme-std-aim-v1; any other id refused by name; fit_prior no longer falls back silently. History lines carry
  era_of; dsr_variance / cells / ledger_net_series skip every history line. 666 passed 6 skipped. Scratch identity
  against b44774d6: single-window fitter byte-identical on all 8 compositions; H-1 pooled v1 / v6 fits identical.
  Cross-lane: backtest_integrity.py (FIX-C contracts kept), research_roles.py. Aim-record cache fingerprint changed
  (one recompute, no output change).
- Ruling E-41 (history-read accounting; ERA concern 1, declared before any read): an OD-3 history read, one era or
  pooled, adds 0 to the construction N (prereg rule 2: re-runs of ledgered cells add 0) and 1 to a separate
  "history reads" count printed in the Appendix A block beside the validation reads; the task-H brief's "costs 1
  trial" is read as this count -- the cell-count DSR is a construction-trial statistic; a history read selects
  nothing -- cost if wrong: the DSR N is one lower than a stricter reading; disclosed in the block.
- Ruling E-35a (ERA concern 2): the pooled fit also implements `ew-theme-aim-v1` under E-27a (ew-theme-v1 base
  weights, gains inside each theme, cap after) through the same path as the std-aim variant; ERA adds it now --
  an OD-3 read of a V8-F on a rejected R-1 must not block -- cost if wrong: one test.
- Review of the optimiser done (review-spo.md at d22b735a): I 0, M 1, m 5. SPO-1 (M): E-31 is not self-enforced --
  a primary-book run with limits_unmet > 0 on scored decisions exits 0, writes NAV and returns, prints S2 net
  Sharpe, tripwire "clear". Objective is the registered one; no worker-count dependence (book-workers > 1 refused);
  warm-up never touches the dual; no look-ahead; gamma and S_prior units consistent (annual Sharpe 20). Minors
  SPO-2 (E-14 correlation lags one decision), SPO-3 (NaN iterate reads converged), SPO-4 (capacity-pass traps for
  N-2: rows enter the tripwire; capacity books planned with the base-NAV trade limit), SPO-5 (--spo-tol /
  --spo-iters loosen convergence from argv), SPO-6 (risk model version unchecked).
- Ruling E-14a (SPO-2, declared before any read): the E-14 criterion compares the traded book after decision d's
  trades with the aim at d (both known at d's close; no look-ahead); the planned-weights value stays beside it --
  a one-decision lag would measure aim drift, not tracking -- cost if wrong: none to the book; the criterion only.
- Ruling E-31a (SPO-1): a run whose primary book has limits_unmet > 0 on any scored decision voids itself before
  any return file is written or printed (exit non-zero, `voided: limits_unmet` in the manifest), independent of
  --specific-ceiling-void; the r6 template's pre-return check names it; `--spo-tol` and `--spo-iters` are refused
  under spo-v3 (the registered tolerances are constants) -- E-31 is a mechanics rule and must bind the run, not
  the reader -- cost if wrong: an R-6 rerun after a blind fix, no new trial. Fix: FIX-2 round 1 (owns the spo-v3
  files), with SPO-4's two traps on the capacity pass.
- Review of the mining verb done (review-mine.md at d22b735a): I 0, M 10, m 8. MINE-1 ledger-campaign refuses every
  real campaign (16-hex head vs 64-hex required; tests work around it); MINE-2 confirm t has no date floor; MINE-3
  confirm window in no identity (re-run makes a second confirm read, line skipped as present); MINE-4 rule 10 has
  no budget input (hurdle from realised n_raw; fresh registry resets it); MINE-5 E-33a not implemented; MINE-6
  Bonferroni on a Bartlett lag-21 t on overlapping h21 labels is about 1.2x anti-conservative [unverified]; MINE-7
  without --pool the "marginal" t is the raw t and rho has no members; MINE-8 ResearchRole::load never calls the
  B-3 refusal; MINE-9 fixture tests cannot fail a wrong promotion rule; MINE-10 memory estimate guessed. Clean:
  hook off is identical at 1 and 4 workers; windows cannot overlap or pass the seal. No campaign runs in v8 (OD-7).
- Ruling E-32a (MINE-4, MINE-6, MINE-7; binds only under OD-7): a campaign declares `--budget N` in advance and the
  Bonferroni hurdle is computed from the budget, never from the realised count; `--pool` is mandatory for mined-v1
  (no pool, no campaign); the confirm t's hurdle is corrected for label overlap by a factor the MINE-FIX lane
  derives by simulation under the null on overlapping h21 labels and pins as a constant (written in its report)
  -- rule 10 says the budget is fixed in advance; E-32 says the confirm is the marginal statistic -- cost if wrong:
  a stricter campaign; nothing in v8.
- Ruling PM3-8: lane MINE-FIX fixes MINE-1..MINE-10 (E-32a, E-33a) on the next free pool; it is not on the v8
  critical path (no campaign runs in v8) and merges in integration 6 or later -- cost if wrong: none to v8 cells.

## 2026-09-30 integration 5 part C closed (log "integration 5 part C: identities"; head 64a23b34)
- Identities 1-8 all PASS on v8-7a exes through the bounded runner: NAV flags absent 12/12 files and holdings 4/4
  identical to v7-w4-holdings (holdings.f64 ce5523c6); hold-band 0 identical; adv-hold-q 1e9 identical (0 clipped);
  composition v8 re-rank/cap off identical (combined f64 1cf245b1); H3 warm u pass 48/48 hits, 0 VM runs; field
  reuse step 2: 49 reused / 14 recomputed, 63/63 payloads identical; spo-v2 side files 9/9 identical; --label-role
  equal to --role 10/10 identical. spo-v2 pin captured 3bfd293e (weights 0xb039820b40d5cf24, replay
  0xd24b61721a7c698c); target-tests 234/234 on v8-8.
- Ruling PM3-5a: PM3-5 withdrawn -- the four calendar-pinned price fields are not in the 63-field argv of the
  3-year role, so identity 6's expectation was and is 49 / 14 (F-3's number). On the 4-year window fields v9 is
  built fresh with the pin in the fingerprint, so v10 from v9 reuses 49 / computes 21 and v11 from v10 reuses 70 /
  computes 3 (F-3) -- cost if wrong: one reuse count re-read.
- Ruling PM3-9 (Wave 0 split): Wave 0 R1-R9 (projection, base role, repair, bridge, fundamental events, roles lo1
  and lo3, admission probe) runs now on the current head; R10-R14 (fields v9, plan-only, cold u pass, overlap
  reports, pins) run after integration 6 merges FIX-2 (N-1 seal pin in the holdings fields), FIX-3 and ERA, so
  fields v9 is built once on final code -- root would otherwise idle for an hour -- cost if wrong: R1-R9 are
  rebuilt (about 10 minutes) if integration 6 changes a role builder, which no pending lane touches.
- LIB2 done 5d64644e (pool 8; field exch_up_365d b63edb31, holdings kind xsw). Registration (task-LIB2-report.md,
  frozen before any read): iv_vol_of_vol (options_implied, B-, +1, Baltussen et al. 2018), day_rev_freq
  (reversal_seasonality, B-, +1, Akbas et al. 2022; needs ret_overnight / ret_intraday of fields v10), exch_switch
  (C+, +1, Dharan and Ikenberry 1995; new field exch_up_365d, fields v12). Static budget fits (40/5/1, 40/4/2,
  0/3/1). Expected v12 --reuse from v11: 73 reused, 1 computed. 251 engine-tool tests pass.
- Ruling E-42 (LIB2-a..e): exch_switch joins the existing `filing_events` theme (its text widened to "filing and
  listing events"); no new theme is opened (a one-member theme would give a sparse short-only flag a full 1 / T
  share). Roster cap 64 (R7-a) holds (worst case 60). day_rev_freq is withdrawn at 0 trials if the vendor open is
  absent at the v10 build. The three candidates are R-12's screen set (E-38); three admission trials -- cost if
  wrong: one C+ member sits in a theme whose text had to be widened.
- Disclosure (LIB2): the lane read ALPHA_PANEL_METRICS.md sections 1 and 1b, whose coverage table carries
  availability shares for 2024, 2025 and 2026 (data presence counts, no return or signal statistic); nothing was
  used from them. Recorded under the hidden-data rule; no selection effect.
- MINE-FIX dispatched on pool 8 (branch feat/platform-v8-minefix-20260930 from 864b7836).
- RISK done 35bcda95 (pool 9; scaler 9d607b0e, NAV wiring f5a8eefe, template r8.json 855b1e9b, report 2cb3cf00).
  Registration as coded: L_t = clip(S / (b sigma_hat), .8 L, 1.25 L), S .05, b 1.15, cadence 21, 252 periods;
  sigma_hat = sqrt(252 x book_variance) of the current gross-1 book after fills; first estimate at the first decision
  with a forecast, non-flat book, positive variance; L_t held between estimates; aim-partial-v5 moves toward
  L_t x desired; spo-v3 tracks L_t x desired with gamma on L x desired and gross bound 2L; ADV cap and desired
  target read L. Refused: aim-partial-v6, spo-v1/v2. Cross-lane: strategy_spo.hpp/.cpp (value-preserving unset),
  test_research_spec.py (union with r10 / r11). Not compiled.
- Ruling E-43 (R-8 acceptance; RISK concern 1, declared before any read): pre-registration rule 5 governs -- R-8
  is accepted on paired S2 net dSR > 0 against its parent AND mechanics AND realised volatility inside
  [.8, 1.2] x sigma_star in each TRAIN year; the plan's "dSR not lower by more than one SE" is the plan's
  expectation, not the rule (E-36 precedent) -- cost if wrong: R-8 rejected where the plan would accept; the net
  return gain is still reported. Noted: the rule scales the aim, so realised vol is expected near S / 1.247 under
  aim-partial-v5 (lane's first-principles estimate); if the band fails for that reason the registered rule failed
  and is not re-parameterised (rule 5: no retry).
- ERA round 1 done ebc254f0 (E-41 0b855971: history reads add 0, Appendix A prints `history reads K`; E-35a
  51d59f48: pooled ew-theme-aim-v1 under E-27a through its own branch; 670 passed 7 skipped). Integration 6 items:
  after FIX-3 merges drop `pooled_aim_weights` and its elif so both paths run FIX-3's E-27a code and un-skip the
  one-era equality test; research_cycle plan-time `summ.dsr_n "ledger+1"` must add 0 for a history-read cell
  (integrator fix). Noted: the 1/(2T) cap refuses a fit where every theme has one member (std-aim does the same).

## 2026-09-30 Wave 0 part 1 closed (log "Wave 0 part 1 (R1-R9)"; head 351e33d9)
- R1-R9 all pass on v8-7a exes through the bounded runner: projection 1,405 sessions to 2023-12-29; base role
  n 5,922 (membership overlap with the old role True); one mass session 2021-01-04 repaired; bridge sealed
  2024-01-01 (max_end 2023-12-31); fundamental events through 2023q4 (6 runs, 203 s, 657 MiB); lo1 and lo3 both
  `1155 1405 True`; the three live stage manifest hashes equal the runbook pins at R8; R9 admission probe
  required_bytes 1,989,405,564 (1,897 MiB) for both roles, slots 8, inside 2,560 MiB. Disk after 46 GB free.
  Nothing dated 2024+ opened. Blockers 2, 3, 4, 6 verified fixed at head; R4 `--check` skipped (opens the live
  warehouse, Q27).
- Ruling W0-n (runbook Q1, regsho_threshold republished): fields v9 on the 4-year roles pins the live stage
  manifest re-hashed immediately before R10 / R11; the R13 field overlap report under W0-a is the check that
  the republished stage left the common 2020-2022 cells unchanged -- no reuse from the old fields dir is possible
  anyway -- cost if wrong: the overlap report stops the re-base and names the cell.
- Ruling PM3-10 (integration 6 split): part A merges the finished lanes DLRET f51c5fd8, ERA ebc254f0, RISK
  35bcda95, LIB2 5d64644e and builds now (root is free); part B merges FIX-2, FIX-3, COMB2, ORTH, MINE-FIX when
  they report, applies ERA's post-FIX-3 items, builds and runs every suite; Wave 0 part 2 (R10-R14) follows --
  cost if wrong: one extra build tag.
- COMB2 done 692b57d8 (pool 11; kernel 9dbabf54, C++ rule b8a86b1b, fitter d0813fc5, r10.json 42614cc2). Registered
  blind (task-R-10-report.md): IC = each member's admission-row train_mean (the parent fitter's window; no new read);
  shrunk_k = .5 theme-mean + .5 ic_k; floor 0 (a theme with no positive value falls back to 1 / n); theme share
  1 / T; cap 1 / (2T); runner re-applies the rule to the recorded ICs (1e-12). Acceptance: dSR > 0, mechanics,
  planned turnover per unit gross not higher than the parent (R-1's criterion). Python 137 passed; not compiled.
  Cross-lane: fit_composition_weights.py (4 hunks; FIX-3 edits it too), test_research_spec.py; a C++ rule table
  created in strategy_ic_admission.cpp (ORTH adds its row).
- Ruling E-44 (R-10 / R-11 on an aim parent; COMB2 concern 1, declared before any read): the slot-49..51 rules
  run on the last accepted parent whatever its composition. If that parent carries R-3's gains, ic-shrink-v1
  multiplies its shrunk weights by the gains inside each theme, renormalised, cap after (E-27a mechanism), id
  `ic-shrink-aim-v1`; theme-resid-v1 acts on composites and is unchanged by gains (it must accept an aim parent
  and record the parent's rule). The mechanical criterion for both is R-1's (planned turnover per unit gross
  not higher than the parent) -- the registration says "parent = last accepted" -- cost if wrong: one variant
  unused.
- FIX-2 done d43949b7 (pool 7; N-2 28051c4c, P-2 6471c38f, P-3 6a90caae, N-3 4e834a9c + c314a12b, N-1 6ed5fef8,
  T-1 fc83c456, T-2 6006fe9d, T-4 edcb597b). No defect found under T-2 (code reads t-1; both look-ahead patches
  fail the strengthened tests) or T-4 (by reading). Python 204 + 55 + 91 pass; C++ not compiled. One-time cost:
  fields dirs built before 6ed5fef8 recompute their 9 holdings fields once (manifests gain seal_date). The card now
  needs --marginal-ic-pool-sha256 with --marginal-ic. Untouched: T-10 (R-1 planned turnover), prepare_research_fields
  rule text. Round 1 dispatched: SPO-1 / SPO-5 (E-31a), SPO-4, SPO-2 (E-14a).
- ORTH done c1cc57ce (pool 4; kernel 19cc08ef, C++ rule 75534a36, fitter + r11.json 121bfb15). Registered blind
  (task-R-11-report.md): theme order = fit_composition_weights.PRIOR_THEMES (registry order), themes with a weighted
  member only; per session each theme's standardised composite regressed by least squares on an intercept and the
  earlier composites (not residuals), names where the theme is present, absent earlier theme = 0; tolerance 1e-10
  relative, dependent regressor dropped; residual re-ranked (centred tied rank); first theme unchanged bit for bit;
  blend with the parent's shares, weights, signs, cap. Acceptance per E-44 (rule 5 plus R-1's turnover criterion;
  the lane wrote rule 5 only -- E-44 governs). Python 475 + 165 pass; C++ not compiled; test vectors verified by a
  loop port. Cross-lane: fit_composition_weights.py (+4), test_research_spec.py, strategy_ic_runner_test.cpp, CMake.
- Ruling E-45 (ORTH concern 1, declared before any read): R-10 and R-11 are defined only on a parent whose weights
  file carries a rerank-true theme_standardise (R-1 accepted); if R-1 is rejected they are skipped as undefined
  (ledger says so) and slots 49-51 hold only R-12 (E-38) -- a rejected rule is never retried in another guise --
  cost if wrong: two cells unused.
- FIX-3 done b6899ed7 (pool 10; F-8 df42ed69, F-1 fe5f5396, F-9 124d3d11, F-10 c9a154c2, F-14 9c53fe63; minors
  F-2 ca1db951, F-3 ac59103f, F-5 baa31ce5, F-6 f78c9a7a). 166 + 248 tests pass. Untouched minors F-4, F-7, F-11,
  F-12, F-13 (other owners / need rulings). Procedure rule from concern 3: a re-run cell is scored alone (a
  multi-dir summ with --rerun-of refuses the prior cells). A cell ledgered invalid through nav_summ needs a ruled
  defect line (ledger-defect --ruling --date) before its re-run.
- Ruling E-27b (FIX-3 concern 1): the E-27a rule on an ew-theme-v1 parent takes the new id `ew-theme-aim-v2`;
  `ew-theme-aim-v1` keeps its v5 semantics (reproducible from code) and is refused for v8 cells; the pooled fit
  (ERA, E-35a) implements v2, not v1 -- a rule id must name one rule for ever -- cost if wrong: an id rename.
  FIX-3 round 1 does the rename; integration 6 part B aligns ERA's list.

## 2026-09-30 integration 6 part A closed (log "integration 6 part A"; head 48c625fc)
- Merged DLRET f51c5fd8 (2ddcece9), ERA ebc254f0 (1f2c98ab), RISK 35bcda95 (d1c86389; FIX-2's N-2 came along),
  LIB2 5d64644e (31c69d9d); no conflicts. Edits e41d71b1 (E-41 plan-time N), 7ca53c67 (E-42 theme move in the
  LIB2 registration). Builds v8-9, v8-9a clean; RISK compiled first time. C++: target 253, book 154, strategy 46,
  ic 105, mine 18, factory 387, impl 979/5/1 known. Python: strategies 163 + 9, engine tools 252, impl tools 482/2
  skipped, scripts 168/3 skipped. tiny_world unmoved. DLRET tool diff empty; LIB2 holdings fingerprints unchanged.
- Next: part B after FIX-2 r1, FIX-3 r1, COMB2 r1 (merge FIX-2, FIX-3, COMB2, ORTH; drop ERA's pooled aim elif;
  E-27b id alignment; un-skip the equality test; build; suites); then Wave 0 part 2 (R10-R14). MINE-FIX merges in
  integration 7 (off the v8 critical path).
- COMB2 round 1 done 1e8af5b8 (C++ variant 4ca3ab4c, fitter / r10.json 2cf5ca89): ic-shrink-aim-v1 = share x gain
  renormalised inside each theme (1 / T), engine cap 1 / (2T); runner re-applies with recorded gains (1e-12);
  r10.json maps the parent's composition to the variant. 145 + 165 + 33 tests pass. Part B edits: delete the
  ew-theme-v1 and ew-theme-aim-v1 entries of the r10 parent map (E-45: R-10 only on a standardised parent) and
  rename any ew-theme-aim-v1 reference to v2 (E-27b).
- FIX-3 round 1 done 75acb091 (00e710ec: ew-theme-aim-v1 back to v5 bytes; ew-theme-aim-v2 = E-27a via the shared
  theme_gain_weights; v8 specs refuse aim-v1 naming v2; r3 template maps an ew-theme-v1 parent to v2). 130 + 31 +
  186 tests pass. No C++ carries the id. Part B: ERA's pooled list takes aim-v2 (drops the v1 elif), pooled path
  refuses aim-v1 for v8.
- FIX-2 round 1 complete de32b9ad (E-31a / SPO-1 / SPO-5 ad50e574, SPO-4 dd1b923e, E-14a 8ca8326e, fixture
  f1cd0e43). E-15 stands as recorded (ADV cap at the run's initial NAV at every multiple); the round-1 message's
  "own NAV" wording applied to the trade limit and impact only, which the lane already had. Not compiled.
- MINE-FIX stopped d7f98f1b at the owner stop: MINE-1 73a0da26, MINE-5 7ea78ade, MINE-4 5858228f, MINE-3 45a0dc9c,
  MINE-2 631a81a0 done; MINE-7, 8, 6, 9, 10 remain (report says so). Off the v8 critical path.

## 2026-09-30 STOP at owner instruction (PM session 3); status 3 written
- Owner: "stop here and write a status file and goal prompt for the next parent agent". Every agent stopped at a
  clean boundary; every tree clean. Root head after this commit: see git log (code head 48c625fc, build v8-9a).
- Unmerged finished lanes: FIX-2 de32b9ad (pool 7), FIX-3 75acb091 (pool 10), COMB2 1e8af5b8 (pool 11), ORTH
  c1cc57ce (pool 4); MINE-FIX d7f98f1b (pool 8) half done. Merged this session: FIX-C, FIX-AB, R45, A2, H3, DLRET,
  ERA (with round 1), RISK, LIB2.
- Wave 0: R1-R9 built; R10-R14 pending (after integration 6 part B). No statistic from 2020-2023 read. N 37;
  admission trials 0; history reads 0; validation reads 2 (before v8); 2024+ never opened (LIB2 disclosure above).
- Pitch determination unchanged: no v8 metric exists.
- Status: docs/plans/2026-09-30-platform-v8-status-3.md. Goal prompt: docs/plans/2026-09-30-platform-v8-next-goal-prompt-3.md.

## 2026-10-01 PM session 4 (root head 272cc897; code 48c625fc; build v8-9a). Rulings before dispatch; no read yet on 2020-2023
- Goal prompt 3 (7 steps). Model: Opus 5.5 for every lane, integrator and reader; PM coordinates and rules only.
  All pools clean at their lane heads (3 ebc254f0, 4 c1cc57ce, 7 de32b9ad, 8 d7f98f1b, 9 35bcda95, 10 75acb091,
  11 1e8af5b8); root clean; host 6.2 GB free memory, 59 GB disk.
- Ruling PM4-1 (MINE-FIX resumed): the lane resumes now on pool 8 for MINE-7, 8, 6, 9, 10 (its report's list) --
  the goal is to finish v8 and a half-fixed verb with an unwired hurdle factor is worse than either end state; the
  lane is isolated (no build, no data) and costs root nothing -- cost if wrong: one lane's tokens.
- Ruling PM4-2 (integration 7 timing): MINE-FIX merges (integration 7) only after the V8-F freeze gate, never
  between Wave 0 part 2 and the last cell -- it adds a member to an engine header (ResearchIcRead.ic_dates) and
  would move executable and tool-commit pins under locked specs mid-research; nothing in v8 runs a campaign --
  cost if wrong: the mining verb lands a few hours later.
- Ruling PM4-3 (identities after part B): after the part-B build the integrator re-runs identities 1, 4, 7, 8
  (argv of "integration 5 part C") because FIX-2, COMB2 and ORTH touch strategy_nav_v7, strategy_spo and the IC
  runner, and fields v9 and every cell are built on these executables -- byte identity with the flags absent is
  the lane rule and is cheapest to check before the 4-year build -- cost if wrong: about 10 minutes of root time.
- Ruling PM4-4 (R-11 criterion text): r11.json's recorded acceptance carries R-1's turnover criterion beside
  rule 5 (E-44 governs; ORTH wrote rule 5 only); the integrator aligns the template text and test in part B --
  cost if wrong: none; E-44 already binds the verdict.
- Ruling PM4-5 (E-15 wording, FIX-2 round-1 concern): E-15 stands as recorded (the aim's ADV cap reads the run's
  initial NAV at every capacity multiple); no per-multiple desired target is built in v8 -- the capacity curve is
  report-only (E-29, E-37) and the primary series cannot move -- cost if wrong: the 2x / 4x capacity rows are
  slightly optimistic on the aim cap; disclosed beside the curve.
- Dispatched: integrator 6B (root; brief task-INT6B-brief.md; tag prefix v8-10); MINE-FIX resume (pool 8).

## 2026-10-01 integration 6 part B closed (log "integration 6 part B"; head d4dffc6d, code 9c5cfa0c)
- Merged FIX-2 de32b9ad (78571ef2), FIX-3 75acb091 (b3360dcf), COMB2 1e8af5b8 (de2f9636), ORTH c1cc57ce (7e5ff769);
  conflicts resolved as unions; N-2 a no-op. Part-B edits dbfc09bc, 8ea2e7bf, 393910ed, f6288685, 486aa4f3 (ERA
  pooled aim elif dropped, aim-v2 in the pooled list, one-era equality test un-skipped and passing for std-v1,
  std-aim-v1, aim-v2; r10 parent map per E-45; r11 text per PM4-4; 393910ed: the theme-resid fitter accepts
  ic-shrink parents -- confirmed by the PM: E-44 / E-45 define R-11 on any rerank-true parent, finding R6B-O-1).
  Builds v8-10 (twelve targets), v8-10a; 0 compile fixes: about 6,000 lines of lane C++ compiled first time under
  /W4 /WX. 3 test fixes (wrong premises; rules still asserted exactly). C++: target 256, book 155, strategy 46,
  ic 139, mine 18, factory 387, combine 233, impl 1003 / 5 skipped / 1 known. Python: strategies 163 + 9, engine
  tools 253, impl tools 529 / 1 skipped, scripts 178 / 3 skipped. tiny_world unmoved; spo-v2 pin holds.
  Identities 1, 4, 7, 8 PASS (PM4-3; identity 4 weights file differs only in module_sha256, FIX-3 changed
  composition_rules.py; data files identical). Not verified: the E-31a void exit path end to end (R6B-S-1).

## 2026-10-01 re-review at integration 6 part B and fix round FIX-4 (entries parked while root was held)
- Ruling PM4-6 (scoped re-review runs beside integration 6 part B, on the lane heads): three read-only readers,
  Opus 5.5, start now on the fixed lane SHAs -- SPO (pool 7, d43949b7..de32b9ad: E-31a, E-14a, SPO-4, SPO-5),
  COMB (pool 10 b6899ed7..75acb091: E-27b; pool 11 whole lane to 1e8af5b8: ic-shrink-v1 / ic-shrink-aim-v1, r10,
  E-44, E-45), ORTH (pool 4 whole lane to c1cc57ce: theme-resid-v1, r11, E-44, E-45). Outputs review-6b-spo.md,
  review-6b-comb.md, review-6b-orth.md (written outside the repository, folded in when root is free). The part-B
  edits and the integrator's compile fixes get a short diff read after part B closes -- the lane code is frozen
  and is the bulk of the diff; a finding found now is fixed before fields v9 is built on it, and root idles
  less -- cost if wrong: a defect that exists only in integration code is seen one step later (still before
  Wave 0 part 2).
- Re-review SPO done (review-6b-spo.md, pool 7 d43949b7..de32b9ad): SPO-1, SPO-2, SPO-4, SPO-5 all ADDRESSED;
  I 0, M 0, m 2; likely compile failures 0. No path found by which a spo-v3 run with an unmet primary-book limit
  writes or prints a return or Sharpe; warm-up decisions record no rows; spo-v1 / v2 unchanged. Minors: R6B-S-1
  (the tiered primary-label line strategy_nav_v7.cpp:510 and the CLI void path, exit 3 and the `voided` keys,
  are untested; correct by reading), R6B-S-2 (E-14a back-fill tested on one book only). Notes: E-14a's "known at
  d's close" is literally d+1's close (fills), as the lane recorded; a B0c warm-up book whose beta starts outside
  the limit could void R-6 on its first scored decision (E-31a then applies: void, blind fix, rerun, no new
  trial); the r6 template `--capacity-curve` line is FIX-3 F-14's (merge conflict expected, in the 6B brief).
- Re-review COMB done (review-6b-comb.md; pool 10 round 1, pool 11 whole lane, plus the integration tree):
  ew-theme-aim-v2, ic-shrink-v1, ic-shrink-aim-v1 all MATCH their registrations (hand examples, fixtures check);
  I 0, M 3, m 5; likely compile failures 0. R6B-C-1 (M): POOLED_COMPOSITIONS leaves out both ic-shrink ids, so an
  OD-3 history read of a V8-F that carries an accepted R-10 would refuse (E-35 forbids that). R6B-C-2 (M): the
  E-44 turnover criterion of R-10 (and R-11) exists only as prose; the v8 ladder of the pitch config stops at
  R-7, nothing computes or checks R-8, R-10, R-11, R-12 criteria. R6B-C-3 (M, already fixed by the integrator at
  393910ed): R-11 on an accepted R-10 parent was refused (resid_block required the ew-theme-std-v1 id). Minors:
  C-4 the E-27b guard is lexical (`--composition=ew-theme-aim-v1`, an abbreviation or `--protocol=v8` passes);
  C-5 the runner does not check the file's recorded rule against its block's rule; C-6 no ruling on an
  infeasible-cap fit; C-7 r10 forces the w cap to 3072 (can only lower an inherited cap); C-8 Python and C++
  sum in different member orders (measure-zero disagreement, runner refuses loudly).
- Ruling PM4-7 (R6B-C-1; E-35 extended, declared before any read): the pooled (era) fit must implement every
  composition a V8-F can carry: ic-shrink-v1, ic-shrink-aim-v1 and theme-resid-v1 join it, through the same
  shared functions as the single-window fit, with the one-era equality test extended to them -- the OD-3 read is
  on the freeze-gate path and must not block on a rule accepted in slots 49-51 -- cost if wrong: one fix round
  unused if R-6 is rejected.
- Ruling PM4-8 (R6B-C-2; criteria of the optional cells are machine-checked, declared before any read): the v8
  ladder (pitch config, scorecard template, mega_report/v8.py ladder_checks) gains R-8 (rule 5 plus realised
  volatility inside [.8, 1.2] x 5% in each TRAIN year, E-43), R-9 (three report-only frontier cells, no
  acceptance), R-10 and R-11 (rule 5 plus planned turnover per unit gross not higher than the parent, E-44),
  R-12 (below) -- P-2's rule: a registered criterion that nothing computes is a criterion the reader applies by
  eye -- cost if wrong: none.
- Ruling PM4-9 (R-12 acceptance, declared before any read): R-12 is a library wave like R-2 and R-7: accepted
  whole on rule 5 (paired S2 net dSR > 0 against its parent, mechanics) plus "book turnover not higher than the
  parent" (E-36 precedent; marginal IC is the entry screen and gates nothing at acceptance) -- E-38 named the
  cell but not its mechanical criterion; the library waves' criterion is the only registered one for an
  add-alpha cell -- cost if wrong: R-12 judged on a criterion slightly different from what the owner intended;
  one cell, disclosed.
- Ruling PM4-10 (R6B-C-6, infeasible cap): a fit refused because fewer than 2T members are admitted (cap 1/(2T)
  infeasible) is a cell that cannot be formed: it is ledgered as undefined (kind note, adds 0 to N), not as a
  rejected trial, and the next cell keeps the same parent -- no return was read and no choice was made --
  cost if wrong: one slot unused.
- Re-review ORTH done (review-6b-orth.md; pool 4 whole lane): theme-resid-v1 code computes the registered steps
  (23 fixture values checked by hand); I 0, M 3, m 4; likely compile failures 0. R6B-O-1 (M): the fitter's
  resid_block refuses an ic-shrink parent (E-44 / E-45 define R-11 on it; partly fixed by the integrator at
  393910ed); apply must run after COMB2's attach. R6B-O-2 (M): `filing_events` (theme added at v8.1; exch_switch
  joins it, E-42) is missing from the frozen theme order, so R-11 refuses once such a member is weighted.
  R6B-O-3 (M, the rule not the code): re-ranking the residual spreads every tie block of a theme's composite in
  the order of the fitted earlier themes with the sign of a tiny beta; a sparse flag theme becomes a full-width
  bet against earlier themes and can flip between sessions. Minors: O-4 runner accepts any theme order; O-5
  nothing checks R-11's weights equal the parent's; O-6 aim parent untested; O-7 the fixture cannot detect
  regressing on residuals or dropping the intercept.
- Ruling PM4-11 (R6B-O-2, declared before any read): the registered order of theme-resid-v1 is the registry
  order of PRIOR_THEMES with any later-registered theme appended in registration order; `filing_events` is last,
  after `ownership_flow`; derived from PRIOR_THEMES with a frozen-prefix check -- the registration says "registry
  order" and a later theme is later in the registry; the newest sparse theme then contributes only what the
  older themes do not -- cost if wrong: the order of one theme in one optional cell.
- Ruling PM4-12 (R6B-O-3, declared before any read; the registered text is silent on ties): names tied in theme
  t's own standardised composite stay tied after residualisation: the residual is replaced by its mean inside
  each exact tie block of z_t and that block mean is re-ranked. A composite without ties gives the registered
  result bit for bit. The id stays `theme-resid-v1` (nothing was run or written under it; E-30 precedent:
  constants and silent points are fixed blind before the cell) -- a theme cannot order names it does not
  distinguish; ordering them by a statistically meaningless beta adds turnover and a bet the hypothesis
  excludes -- cost if wrong: R-11 differs from the lane's first text on tied names only; one optional cell.
- MINE-FIX done b772ef61 (pool 8; MINE-7 8051803e, MINE-8 318e5a2c, MINE-6 6b0aeb2f, MINE-9 12f3dfd8, MINE-10
  5cddc990; minors MINE-11 791b18e0, MINE-12 62f1b818, MINE-17 d81303ab, MINE-18 81be48a2; MINE-13 covered).
  16 pytest pass; nothing compiled; golden 0x889874a3b9b29c55 unverified until integration 7 (search_driver.hpp
  changed: wide rebuild). Not done: MINE-14, MINE-16 (not cheap), MINE-15 (needs a ruling). Departures: MINE-10
  slot bound refuses at evaluation time and has no node bound; MINE-9 rule checks on a separate stage-1-only
  campaign; MINE-6 written as t / F >= z. Concerns: the 1.55 factor is derived to the 99.8% tail; at the
  504-row floor the ratio keeps rising (1.46 at 99.95%, 1.51 at 99.99%), nominal per-trial rate reached near
  N = 1,000; a default campaign on the 4-year role is estimated at 10.85 GiB.
- Ruling PM4-13 (mined-v1 budget ceiling; binds only under OD-7): a campaign's `--budget` above 1,000 is refused
  (`kMinedMaxBudget = 1000`) until the overlap factor is re-derived at the campaign's own Bonferroni level (v9)
  -- E-32a says the hurdle must be valid at the declared budget and 1.55 is checked only to about N = 1,000 --
  cost if wrong: large campaigns wait for v9; nothing in v8. The 10.85 GiB estimate is recorded for OD-7 (a real
  campaign needs an owner-approved memory cap, E-6); no code change. MINE-15 deferred to v9 unruled.
- MINE-FIX round 1 done 20e7bd19 (PM4-13 code and tests 28a928a1: kMinedMaxBudget 1000 in the rule header,
  recipe and campaign.json; verb, campaign_line and ledger-campaign refuse above it). 16 pytest pass; not
  compiled. Cross-lane: FIX-C's E-33 test budget 2048 -> 1000. Lane head for integration 7: 20e7bd19.
- Fix round FIX-4 (brief task-FIX-4-brief.md), two lanes from the part-B head: FIX-4a (pool 4): O-1..O-7, C-1
  (PM4-7), C-5; FIX-4b (pool 10): C-2 (PM4-8, PM4-9), C-4, S-1, S-2. C-7 and C-8 stand as recorded. Integration
  6 part C merges both, builds, re-runs the touched suites and identity 4; Wave 0 part 2 follows.
- FIX-4a dispatched on pool 4 (feat/platform-v8-fix4a-20261001), FIX-4b on pool 10 (feat/platform-v8-fix4b-20261001),
  both from root 43a0447d.
- Ruling PM4-14 (Wave 0 part 2 split, declared before any read): dispatch 2a (R10, R11: fields v9 on lo1 and lo3,
  and the field overlap report, R13 report 1, under W0-a) runs now on build v8-10 while the FIX-4 lanes work;
  dispatch 2b (R12 plan-only, the cold u pass, the signal and daily IC overlap reports, R14 pins, protocol line,
  lock --write) runs after integration 6 part C, because FIX-4a changes IC runner sources and the locks pin the
  executables -- FIX-4 owns no field builder or field module (brief file lists), so fields v9 is built on final
  field code and root does not idle for a lane-turn -- cost if wrong: fields v9 is rebuilt once if part C
  changes a field module fingerprint (the v10 reuse count 49 / 21 would show it).

## 2026-10-01 Wave 0 part 2a closed (log "Wave 0 part 2a (R10, R11, field overlap)"; head b01d8696)
- R10 fields v9 lo1: 63 / 63 computed fresh, 166.8 s, 960 MiB, manifest 888e6616e441e863...447b8695. R11 fields v9
  lo3: 63 / 63, 154.6 s, 1,014 MiB, manifest 9f1563638b5e4f7e...bd9021ef. Sealed 2024-01-01; no source named 2024+.
  Live stage hashes (W0-n) re-hashed before R10 and R11: all 11 equal the runbook pins and the R8 values
  (regsho_threshold fb073c62). Disk 58 -> 50 GB. No fix. Disclosure: the integrator read 20 R10 progress lines
  carrying counts of rows past the seal (no value, no statistic).
- Field overlap lo1 vs the v7.1 fields: 409,448,655 cells (1,155 sessions x 5,627 instruments), 62 / 63 fields
  bit-identical, none in the below-1e-9 class. STOP class on one field: `regsho_threshold_days63`, 3,006,158 of
  6,499,185 cells differ (3,006,153 a value against an old NaN; 5 value changes, max_abs_diff 5.0), first cell
  2018-06-04 instrument 2234. No v7.1 candidate reads the field.
- Ruling PM4-15 (W0-a on regsho_threshold_days63; declared before the signal overlap and before any statistic):
  W0-a stops the re-base "until the cause is found". The cause test is the decomposition overlap of fields v9
  lo1 against `build-equity/v8-i3p4-c-fields2` (built after the stage was republished) on this field: (a) if
  bit-identical there, the whole difference is the republished stage (runbook blocker 1, W0-n), the cause is
  found, the 4-year values are the reference, and the re-base continues -- no v7.1 candidate reads the field,
  so no ledgered signal, IC or paired comparison can move (the signal overlap report, R13 report 2, must then
  be bit-identical and is the proof); (b) if not identical there, the re-base stays stopped and the first
  differing cell is traced to code, role or stage before anything else runs -- W0-a exists to keep old values
  from changing silently; a field no ledgered cell reads cannot change one -- cost if wrong: a v8.0 / v8.1
  candidate that reads the field is screened on values the old stage did not have; that is the declared
  republish, disclosed here.
- PM4-15 outcome (a) (log "Wave 0 part 2a: regsho decomposition"; head 154affe6): fields v9 lo1 against
  v8-i3p4-c-fields2 (stage pin fb073c62, the live republished stage; v7.1 was pinned to the old 68f431f0):
  `regsho_threshold_days63` bit-identical on 6,499,185 cells; the other 62 fields bit-identical; 409,448,655
  cells in all. Cause found: the republished stage. The longer role, the seal move (N-1 pin) and the holdings
  code changes moved no common cell. No registered or drafted v8 candidate reads the field (registry 48 alphas,
  43 fields; v8 draft never names it). The re-base continues; the 4-year values are the reference; the signal
  overlap (R13 report 2) must be bit-identical.

## 2026-10-01 STOP at owner instruction (PM session 4); status 4 written
- Owner: "at the next logical breakpoint stop; if enough information exists to build the v8 report, build it,
  otherwise write a status file", then "stop here". No v8 cell has run, so no v8 metric exists and no report
  was built. Both FIX-4 agents were stopped at once (not at a boundary of their choosing); neither wrote a report.
- FIX-4a (pool 4) at the stop: R6B-O-1 test d9726cfa, O-2 e8997d64 (PM4-11), O-3 72696785 (PM4-12), O-4 263077f8
  committed; the unfinished O-5 saved as WIP commit b8b68f4e (7 files, incomplete, untested; committed by the PM
  so the tree is clean and nothing is lost); O-6, O-7, C-1, C-5 and the report not started.
- FIX-4b (pool 10) at the stop: C-2 f408a6f1, C-4 cf15052d committed, tree clean; S-1, S-2 and the report not
  started. The lanes' last pytest results did not reach the PM: unverified until the lanes resume.
- MINE-FIX finished 20e7bd19 (pool 8), unmerged by Ruling PM4-2.
- Wave 0: R1-R11 built (fields v9 on lo1 and lo3), field overlap explained (PM4-15); R12-R14 pending after
  integration 6 part C. No statistic from 2020-2023 read. N 37; admission trials 0; history reads 0; validation
  reads 2 (before v8); 2024+ never opened (one disclosure: progress-line row counts past the seal, no value).
- Root code head 9c5cfa0c (build v8-10); every tree clean.
- Status: docs/plans/2026-10-01-platform-v8-status-4.md. Goal prompt: docs/plans/2026-10-01-platform-v8-next-goal-prompt-4.md.

## 2026-10-01 PM session 5 (root head 61fe8317; code 9c5cfa0c; build v8-10). FIX-4 lanes finished; rulings before integration 6 part C; no read yet on 2020-2023
- Goal prompt 4 (6 steps). Model: Opus 5.5 for every lane, integrator and reader; PM coordinates and rules only.
  Host 4.2 GB free memory, 47 GB disk. Root clean; pools 4, 8, 10 clean.
- FIX-4a done 98ef8d89 (pool 4): O-1 d9726cfa, O-2 e8997d64 (PM4-11), O-3 72696785 (PM4-12), O-4 263077f8, O-5
  b8b68f4e (WIP, reviewed hunk by hunk by the resumed lane) + aeda5bd7 (a key present in one file only, even as
  null, is a difference; `--theme-resid-parent` refused by name with `--era`), O-6 d928c299, O-7 c3110301, C-1
  233accd9 (PM4-7), C-5 2a47da99; report task-FIX-4a-report.md. Pytest 361 passed / 4 skipped (192 impl tools,
  169 scripts). C++ uncompiled (O-3, O-4, O-7, C-5). Root builds `atx-impl-strategy-ic-tests`,
  `atx-equity-strategy-ic`; filter `ThemeResid.*:ThemeResidRunner.*:CompositionV8.*:StrategyIcRunner.*`.
- FIX-4b done 0b093a4c (pool 10): C-2 f408a6f1 (PM4-8, PM4-9), C-4 cf15052d, S-1 0089b5e8 (tests only: the tiered
  primary-label test and the CLI void test, exit 3 and the `voided` keys), S-2 b69317f8; report
  task-FIX-4b-report.md. Pytest 168 passed (lane suites), 196 passed / 4 skipped (downstream). C++ tests
  uncompiled. Root builds `atx-impl-strategy-target-tests` (and `atx-impl-tests`); filter
  `SpoV3.*:SpoTripwire.*:SpoPin.*`. The CLI void test rests on an unrun premise (zero volume from session 30
  makes the primary book's limit unmet); verified at integration 6 part C on the built executable.
- Ruling PM5-1 (O-5, module SHAs compared strictly): stands as the lane coded it; R-11's re-fit must equal the
  parent's weights file including the recorded module SHAs -- the parent cell and R-11 are fitted on the same
  locked tool commit (PM4-2 keeps MINE-FIX out until after the freeze gate), so a differing module SHA can only
  mean an unplanned tool change between the two cells, which is exactly the lineage drift the finding names --
  cost if wrong: after an unplanned tool fix R-11's fit refuses loudly and the parent's weights need a blind
  re-fit first (no return read, no trial).
- Ruling PM5-2 (O-5, parent flags optional at the fitter): accepted -- the r11 template always passes them, the
  lock pins the template, `--era` fits refuse them, and no cell is ever fitted by a bare command (cells brief) --
  cost if wrong: a hand-run fit without the flags skips the check; the scoped review at part C confirms the
  template path cannot drop them.
- Ruling PM5-3 (PM4-10 refusal text): `composition_rules.py` is not reworded in v8 -- naming the ruling in the
  message changes a hashed module for no behaviour and would move `module_sha256` under every weights file; the
  integrator's log records "undefined (PM4-10)" for such a cell -- cost if wrong: none.
- Ruling PM5-4 (FIX-4b cross-lane edit): the nav CLI input helpers moved unchanged from `strategy_spo_test.cpp`
  into the shared `strategy_spo_cli_fixture.hpp` are accepted (tests only; the pinned `strategy_spo_fixture.hpp`
  untouched; `SpoTripwire.*` re-checks the old user) -- cost if wrong: none, it is a test-file move.
- Dispatched: integrator 6C (root; brief task-INT6C-brief.md; tag prefix v8-11): merge 98ef8d89 then 0b093a4c.

## 2026-10-01 integration 6 part C closed (log "integration 6 part C"; head 3298ea8a, code 41ef00fb, build v8-11)
- Merged FIX-4a 98ef8d89 (c17fb449), FIX-4b 0b093a4c (41ef00fb); one conflict (import block of
  test_research_spec.py, union). 0 compile fixes, 0 test fixes: all lane C++ compiled first time under /W4 /WX.
  C++: ic 145, target 259, impl 1012 / 5 skipped / 1 known; lane filters FIX-4a 79 / 79, FIX-4b 22 / 22; spo
  pins hold. Not rebuilt, not re-run: book, strategy, mine, factory, combine (targets, risk and mine executables
  keep their v8-10 bytes). Python: strategies 163 + 9, engine tools 253, impl tools 567 / 1 skipped, scripts 182
  / 3 skipped; tiny_world unmoved. The CLI void test (exit 3, status void, voided limits_unmet, no NAV or returns
  file) ran and passed unedited: the open item "E-31a void exit path untested end to end" is closed. Identity 4
  PASS (step 1 weights byte-identical to part B; step 2 differs in timing fields only). Identity 1 not run: no
  NAV source changed (FIX-4a IC runner sources only; FIX-4b tests and Python only). Review range
  9c5cfa0c..41ef00fb. Open: executables are on two build tags (v8-10, v8-11); integration 7 rebuilds all on one.

## 2026-10-01 owner directive and re-design (entries parked while root was held; all before any read)
- Ruling PM5-5 (scoped FIX-4 review runs beside integration 6 part C, on the frozen lane heads; parked while
  root was held): one read-only reader, Opus 5.5, on pool 4 `43a0447d..98ef8d89` and pool 10
  `43a0447d..0b093a4c`: the tie rule PM4-12 in the Python fitter and the C++ kernel (same expression, same order,
  no-tie bit identity), the theme order PM4-11, the pooled fit PM4-7, the ladder criteria PM4-8 / PM4-9 against
  E-43, E-44, E-45, and the O-5 template path (PM5-2). Output review-6c-fix4.md, written outside the repository
  and folded in when root is free. The integrator's compile fixes get a short diff read by the PM after part C
  closes -- PM4-6 precedent: the lane code is frozen and is the whole diff; a finding found now is fixed before
  Wave 0 part 2b pins the executables -- cost if wrong: a defect that exists only in integration code is seen one
  step later (still before the locks).
- Owner directive (2026-10-01, during integration 6 part C): spawn more subagents; the sprint / goal design may change
  to prioritise real progress in the core alpha-generation features of atx-engine and atx-impl.
- Ruling PM5-6 (integration 7 moves before the locks; supersedes the timing of PM4-2): MINE-FIX 20e7bd19 merges
  right after integration 6 part C and before Wave 0 part 2b writes any lock -- PM4-2's reason was pins moving
  under locked specs; no lock exists yet, MINE-FIX changes strategy_research_role and an engine header, and this
  is the last point before the freeze gate where the executables can still change; the mining verb is then
  compiled and its golden checked in v8, and the AG lanes build on a verified base; identities 1, 4, 7, 8 re-run
  on the new executables -- cost if wrong: one wide rebuild (about 40 minutes of root) before the cells; if the
  golden fails and the fix is not a slip, the merge is reverted and PM4-2's timing returns.
- Ruling PM5-7 (wave AG, alpha generation, beside the cells): four lanes, registered for v9, merged after the
  freeze gate (integration 8): MINE-MEM (pool 8), MINE-STAT (pool 9), MINE-RUN (pool 3), LIB3 (pool 11); brief
  task-AG-brief.md. The v8 trial program is untouched (N <= 51; prereg rule 10 stands; no mined campaign in v8
  without an owner ruling on OD-7 and a memory cap, E-6) -- root is serial by memory and by the parent chain, so
  more agents cannot shorten the cells; they can make the miner runnable and the next library wave ready --
  cost if wrong: four lanes' tokens.
- Ruling PM5-8 (MINE-15, declared before any campaign): an undefined rho pair fails the rho rule; id mined-v1
  unchanged (never run) -- the rule says 'to every member' -- cost if wrong: a thin-coverage candidate is not
  promoted.
- Ruling PM5-9 (MINE-14): the greedy rho step runs over the whole above-hurdle list, then the cap -- the cap
  otherwise fills with variants of one field -- cost if wrong: none (only promotions were lost).
- Ruling PM5-10 (mining memory target): the default 4-year campaign must fit 2,560 MiB or the lane states the
  smallest bit-identical footprint; no owner memory ruling is assumed -- 10.85 GiB cannot run on this host --
  cost if wrong: the campaign needs an owner cap above OD-2's.
- Dispatched: MINE-MEM, MINE-STAT, MINE-RUN (from 20e7bd19), LIB3 (from 67f04389).
- Dispatched: integrator 7 (root; brief task-INT7-brief.md; tag prefix v8-12): merge 20e7bd19, one tag for every
  research executable, golden at 1 and 4 workers, every suite, identities 1, 4, 7, 8.

## 2026-10-01 integration 7 closed (log "integration 7"; head 8f5e35e2, code 807af678, build v8-12); review 6C; wave AG results (entries parked while root was held)
- Integration 7 (PM5-6): MINE-FIX 20e7bd19 merged as 807af678; 2 Python conflicts (backtest_integrity.py,
  test_trial_ledger_rules.py), both unions; 0 compile fixes, 0 test fixes. Build v8-12, twelve targets, one tag,
  246 s. Executables (all v8-12): IC ab7e2cbd...1615452d; NAV / targets 5497c89d...35b9bca6; risk
  8967952c...a7eed258; mine cd661fe9...8588c91d. Golden 0x889874a3b9b29c55 holds at 1 and 4 workers. C++: mine
  31, factory 390, target 259, book 155, strategy 46, ic 145, combine 233, impl 1,022 / 5 skipped / 1 known.
  Python: strategies 163 + 9, engine tools 253, impl tools 571 / 1 skipped, scripts 183 / 3 skipped; tiny_world
  unmoved; spo pins hold. Identities 1, 4, 7, 8 PASS on v8-12 (identity 4: timing fields only): the
  strategy_research_role and engine header changes moved no byte of an IC or NAV output with the flags absent.
  No revert. Nothing dated 2024+ opened; no cell, no Wave 0 step, no campaign on real data.
- Scoped review of FIX-4 done (review-6c-fix4.md; pool 4 43a0447d..98ef8d89, pool 10 43a0447d..0b093a4c): I 0,
  M 2, m 5; likely compile failures 0 (confirmed by build v8-11). Tie rule PM4-12: MATCH in both languages (same
  key, exact ==, sum from the first name ascending, divided by block size, same centred rank); no-tie bit
  identity holds; first theme untouched. Theme order PM4-11: MATCH. Pooled fit PM4-7: MATCH (a history read can
  still produce theme-resid-v1; the cycle never passes the parent flags to a pooled fit). Parent check: MATCH
  (unchecked paths are those accepted by PM5-2). C-5, C-4: MATCH. Ladder: MISMATCH on two points: R6C-1 (M) a
  cell recorded accepted whose rule is n/a (missing year_table, TRAIN year or nav_summ row) passes
  ladder_checks; R6C-2 (M) the R-1 / R-10 / R-11 texts say 'planned turnover' over a check that reads executed
  turnover (same root as T-10, never ruled). Minors: R6C-3 tie fixture does not pin the mean's divisor, no C++
  summation-order case; R6C-4 R-11's order reads the PRIOR_THEMES constant while admission reads registry.json;
  R6C-5 'N after' static after an undefined cell; R6C-6 an own-verdict 'undefined' is unchecked; R6C-7 a file
  without provenance.rule skips the C-5 check.
- Ruling PM5-11 (R6C-2 and T-10; the statistic of R-1's criterion, declared before any read): 'planned turnover
  per unit gross' in R-1, R-10 and R-11 is nav_summ's tau_gmv_mean / mean_gross_leverage_all_rows on S2
  (executed one-way GMV turnover per unit of gross), cell against parent, 'not higher' meaning <=. The three
  texts drop the word 'planned' and name the statistic -- plan 12.1 registers 'turnover per unit gross';
  nav_summ carries no planned-turnover statistic; every other turnover criterion (R-2, R-4, R-7, R-12) reads
  executed turnover; the cell and its parent trade through the same execution, and cost is paid on executed
  trades -- cost if wrong: where caps or blocked orders make planned and executed turnover disagree in sign
  against the parent, the three composition cells are judged on the executed number; disclosed in the scorecard.
- Ruling PM5-12 (fix round FIX-5, Python only, before the locks): R6C-1 (an accepted non-baseline, non-report
  row whose rule is n/a is refused, naming the missing part), R6C-2 texts per PM5-11, R6C-3 Python fixture (a
  date with unequal blocks beside singletons that fails under a block sum and under mean-of-ranks), R6C-4
  (under --theme-resid the registry's theme tuple must equal PRIOR_THEMES before anything is computed), R6C-5
  (N after computed from the cell states), R6C-6 (an own-verdict undefined cell whose paired file exists is
  refused). Deferred to integration 8 with wave AG: the C++ halves of R6C-3 and R6C-7 (reachable only through a
  hand-edited weights file; the lock pins the fitter's output) -- no executable changes after integration 7, so
  Wave 0 part 2b pins final binaries; the report tools are fixed before any scorecard is read -- cost if wrong:
  the C++ tie kernel keeps one unpinned property (divisor, summation order) that the reviewer verified by
  reading and by hand examples.
- Dispatched: FIX-5 (pool 10, feat/platform-v8-fix5-20261001, from 41ef00fb).
- MINE-RUN done 6ea76460 (pool 3): prereg draft docs/plans/2026-10-01-v9-mine-campaign-prereg.md (15 owner
  decisions with recommended values), spec template scripts/specs/v9/mine-c1.json, runbook, scripts/research_mine.py
  (research_cycle.py mine lock|pool|probe|plan|run|wave), 32 tests; pytest 169 passed / 3 skipped. Blocks on a
  real campaign: memory 7.3-10.1 GiB at the C1 shape against the runner's 8,192 MiB; about 1,050 s at 1 worker
  against 600 s; PM5-9's rho step must be streamed (relayed to MINE-STAT and MINE-MEM: at most shortlist cap + 1
  panels held; the memory model carries that term).
- LIB3 done e9e1ee20 (pool 11): docs/plans/2026-10-01-v9-library-draft.md, 10 prior-class candidates: READY
  stmom, ind_leadlag; NEEDS-FIELD nt_late (sec_filings events carry NT 10-K / NT 10-Q with an acceptance clock),
  earn_season; NEEDS-DATA lazy_prices, tnic_mom (text landing at 7%), conn_rev, fund_fit (N-PORT not built),
  tax_book (fundamentals_notes not built), iv_skew (no put-wing IV source). No data read. Disclosures: the lane
  saw CZ statistics for 2005-2024 printed in the v8 literature note and filing-metadata counts for 2024-2026 in
  two atx-db docs (no return or signal figure); none used. Open points LIB3-a..h (theme texts, the v7 cost
  exclusion against stmom and conn_rev, C+ candidates without a post-2004 test, roster 70 above the cap of 64,
  ind_leadlag at the 7-slot limit) are v9 registration matters: recorded, not ruled in v8; nothing in v8 reads
  the draft.
- Ruling PM5-13 (lane FIELDS-V9, pool 11): the two NEEDS-FIELD candidates get their field builders now, as new
  module files with synthetic tests, off by default and in no v8 field list, merged at integration 8 -- a draft
  that waits on a field is not progress; new files leave every existing module fingerprint and the v10 / v11 /
  v12 reuse counts unchanged -- cost if wrong: one lane's tokens. The four data asks (filing text, N-PORT,
  fundamentals notes, put-wing implied volatility) go to the owner in handoff 2 (OD-6 form).
- MINE-MEM done 57bb5abb (pool 8): memory 449a78a8, MINE-16 86be4ab1 (rung failures get their own status).
  Default 4-year campaign: 8,421 MiB (1 worker) / 11,111 MiB (4 workers) -> 5,319 MiB at both; the first
  campaign's shape (mine-c1) 9,164-9,426 MiB -> 6,034-6,296 MiB at 4 workers, inside the runner's 8,192 MiB.
  PM5-10's 2,560 MiB is NOT met: the peak is the promotion phase (strategy_mine_promote.cpp, MINE-STAT's file).
  Remaining levers named by the lane: streaming the members by date in rho_check (3,226 MiB at 1 worker, 4,572
  at 4; mine-c1 3,978 MiB); mapped extra fields (needs an owner ruling on whether mapped pages count against
  the cap); a shared VM slot budget. Uncompiled; pinned byte counts and rung_failed == 0 on the fixture come
  from reading (STOP case at integration 8 if rung_failed != 0). Performance note: on the racing path engines
  and rung panels are rebuilt each generation (mine-c1 has racing off). Term to reconcile at the MINE-STAT
  merge: shortlist (K + 1) panels, rho rows M + K.
- MINE-STAT done c4bd8d09 (pool 9): overlap factor tables 781b6bea; MINE-14 / MINE-15 f6e0b985 (+ c4bd8d09).
  F by budget band on the 504-row floor, importance-sampled tails, ratio + 2 SE rounded up: 1-100 1.47;
  101-1,000 1.54; 1,001-10,000 1.63. kMinedMaxBudget and MINED_MAX_BUDGET 10,000 (lifts PM4-13). The confirm
  read gets its own factor Fc on the 200-row floor, keyed on m (reads reaching the confirm): m <= 16 1.77,
  m <= 64 1.96, m <= 256 2.15. PM5-9 streamed: at most max_promotions panels held; no new memory term. Pytest 20
  passed. C++ uncompiled. Golden untouched (atx-engine not edited); the impl mine fixture's registry heads move
  (recipe gained keys; no test pins them). Cross-lane edits in MINE-MEM's files (strategy_mine.cpp,
  strategy_mine_detail.hpp, strategy_mine_test.cpp).
- Ruling PM5-14 (mined-v1 silent points, declared before any campaign; id unchanged, never run): (a) a
  candidate whose rho against an earlier KEPT candidate is undefined also fails (the lane's stricter reading
  of PM5-8); (b) the confirm read uses Fc by m as derived, the discover hurdle uses F by budget band; (c) the
  recipe carries both tables and campaign.json records the factors used -- (a) decorrelation that cannot be
  computed is not shown; (b) the 200-row confirm tail ratio is higher than the discover one, so one factor
  would make BY's p too small; (c) a recipe keyed on the budget's F would let a second budget buy a second
  confirm read on the same expressions (MINE-3) -- cost if wrong: fewer promotions; the tables are conservative
  (30 names, floor lengths).
- Ruling PM5-15 (lane MINE-JOIN, pool 8): one lane joins MINE-MEM 57bb5abb, MINE-STAT c4bd8d09 and MINE-RUN
  6ea76460 into one branch (lane-local merge, conflicts resolved by reading both reports), then streams the
  members by date in rho_check (MINE-MEM's named lever: default 3,226 MiB at 1 worker, mine-c1 3,978 MiB),
  reconciles the memory model with the streamed rho step (K panels, not K + 1), and aligns research_mine.py
  and the prereg draft with the new ceiling, F / Fc tables and footprint -- integration 8 then merges one SHA
  whose three parts were reconciled by a lane that read all three, not by the integrator at build time --
  cost if wrong: one lane's tokens.
- Dispatched: MINE-JOIN (pool 8, feat/platform-v8-minejoin-20261001, from 57bb5abb); FIELDS-V9 (pool 11).
- Dispatched: integrator W0-2b (root; task-W0-run-brief.md, dispatch 2b, on build v8-12). FIX-5 (Python only)
  merges inside this dispatch before R14 and before any lock: its SHA is sent to the integrator when the lane
  reports; without it the integrator stops after the overlap reports.
