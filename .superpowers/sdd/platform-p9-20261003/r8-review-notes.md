# P9 RESUME 8: per-lane traces for reviewers (PM, 2026-10-06)

Reviewers read `r7-review-dispatch.md` (the contract), `resume8-addendum.md` (REV-BASE, the state), then this file's
section for their lane, then the lane's section of `r7-dispatch.md` and `r8-dispatch.md`. BASE per REV-BASE. The
dispatch prompt names POOL, HEAD and the review file name. Every review: spec verdict item by item, quality verdict,
"What root must check at merge".

## B2 (re-review 2; pool-14)

- Spec: `r7-dispatch.md` "B2" items 1-6 and `r8-dispatch.md` "B2"; re-review 1's R1 / R2 (B2-11) closed?; D2R-2 (b);
  PRE-15 survives; the G-P5 deletion holds on the merged tree; `exes.eval` named and pinned per v2 cell (B2-10 (c)).
- B2-14: every conflict region of the merges `c37ec671`, `b1df1829`, `c3201a60` (the report's tables): check each
  resolution against both parents (`git show <merge>^1:<path>`, `^2`), that no region was resolved by a wholesale
  side, that `git diff <merge>^1 <merge>` holds only the lane's hunks plus the other side's.
- The merge-level fix in E2's fake world (`scripts/tests/wave_fixture.py:363-367`, `:401-415`; one call line in
  `test_wave_kinds.py:171`, D3's file): fixture only, no rule weakened; v1 worlds unchanged (the 33-line v1 plan
  byte-equal).
- Teeth: B2's mutation Edit was refused by the permission system (ledger RESUME 8). Try ONE mutation of the code under
  test yourself (e.g. the `judge_verb` pin check); if refused, do not retry, report it; the tests' passing controls on
  the pinned verb are then the evidence.
- Trace: the judge never publishes statistics from another verb (`wave_stage_record.py` `judge_verb`, `bundle_once`,
  `scored_verdict`); the verb-on-disk check happens before the summ runs; `nav_summ.py` `allow_abbrev=False`.

## D3 (first review; pool-20)

- Spec: `r7-dispatch.md` "D3" items 1-8 under D3-2 (scope = the kind files only) and `r8-dispatch.md` "D3" (C3-13).
- C3-13: a kind `rule` wave naming a NAV row whose `x-flag` is `--rule <id>` sets `nav.rule`; exactly one `--rule` on
  the derived argv for (a) the parent's own aim rule with other params and (b) another aim rule; `--rate` /
  `--target-rule` / `--cost-law` rows keep the append route (fixtures unchanged); E2's raw-rule-flag guard accepts.
- The C3-3 alias guard (every route refused: raw alias in constants / change flags; abbreviation; inherited alias
  passes; template removing it refused unless `rules` names the row; named row + alias refused; malformed alias
  refused) and the E2-17 `inputs.*` guard (every route; role wave `change.inputs.role` allowed, other keys refused):
  teeth for each (mutate the code, never the test).
- W3-8a: `SCHEMA_ONLY_FIXTURES` empty ("10 runnable, 0 schema only" or what is true); the walk-forward kind derives
  exactly `--walk-forward expanding --walk-forward-lag 2 --walk-forward-step 21 [--walk-forward-min-decisions 21]`
  (D3-1 / D3-2) and renames outputs; the live-table test is skipped without `ATX_EQUITY_BIN`.
- C3b's edits in the same files are NOT in this tree (root resolves): do not count their absence as a finding.

## D3W (first review; pool-23)

- Spec: `r7-dispatch.md` "D3W" (1)-(6) and `r8-dispatch.md` "D3W"; D3-1 constants; D3W-EVAL; ABS-1 ("For ABS" names
  the extension point); "For PRE" P9-W block present.
- Flag absent = every byte unchanged: the fitter's `producer_fingerprint` equal before / after (no edit inside `fit` /
  `fit_prior` or their closure; `atx-engine/tools/code_fingerprint.py`); `WeightsV3.SingleRowEqualsV1Bytes`; the
  mom-volman / theme-tsmom / theme-cov-hedge outputs byte-identical (ALCOMB-4 / -5).
- Look-ahead: `WalkForward.NoRowUsesFutureDecisions` (a row at j reads only TRAIN decisions d with d + lag <= j; lag
  2); the first row's window holds min_decisions; the book is flat before the first row; from_session strictly
  increasing; the runner uses the row in force per date (no row from the future).
- Refusals with teeth: v1 / v2 refuse the block; v3 requires it; tsmom / resid / era refused; > 4096 rows; marginal
  keeps refusing v3.
- D2-9 / D2R-3 (S1, S2): strict int / float in `parent_check`; the S-2 refusal in `check_factor_series`; the S-1 test.
- Run the gtests yourself if the lane left a built tree (EXE-CUR vs HEAD first), else "from the lane's logs only".

## C3b (re-review; pool-15)

- Spec: `r7-dispatch.md` "C3b" items 1-9 and `r8-dispatch.md` "C3b"; item 6 pending by rule iff B2 not in root;
  item 7 dropped by C3-11 (`sign_pm7_35` kept, its G-P5 row kept); PRE-40 (the two rule files NOT edited).
- Retirement of the Python `pm6-6` route: refused at preflight / run / match; ledgered v1 manifests still validate;
  the ledgered files of design note (a) never edited; `gross_match` default removed (C2-9); the digest re-check is the
  first act of `wave_result.build` (C2-10 / C3-12, exit 3, nothing written); `read_grid` duplicate key (exit 1, before
  any output); S-12 console line.
- Cross-lane hunks in D3's files are the smallest possible and listed by file and hunk.
- Canary: C3-14 (a wall-time miss with matching digests, re-run once; two misses = timing finding, not a red).
- Teeth: Python P1-P6 and the C++ `read_grid` mutation; re-do one Python mutation yourself.

## AL-CLOCK (first review; pool-8)

- Spec: `r7-dispatch.md` "AL-CLOCK" items 1-7, `r8-dispatch.md`; ALCLOCK-1 (print-only opt-in `--info-clock-print`:
  flag absent = bytes unchanged; on a non-rule book only the `info_clock` block is added to `summary.json`; rule on
  without the flag = no block; `--spec` equals argv), ALCLOCK-2 (`source_sample_end` 2019, flagged; the
  `ea_days_since` mapping stated), ALCLOCK-3 (the loosened `--capacity-curve` refusal accepts only this rate: a test).
- W3-9: the registry row and parser entry are append-only (tail of the table / parser); W3-16
  `two_speed_fast_theta(cadence)`; the NAV argv token `--rate info-clock-rate-v1` and how a kind `rule` wave naming it
  resolves (root's C3-5 probe at merge); 1-vs-4 `--book-workers` byte test with the rule on; the E2 `rule_flags`
  preflight on fakes.
- Quality: look-ahead in the rate's state (information-release dates read at d-1 or earlier only; `ea_days_since` not
  peeking); determinism (thread-order writes under 4 workers); `/W4 /WX`; 100 columns.
- Registration / template: K-P9 keys; the registration promises only figures the exe prints.

## COV-MV (first review; pool-24)

- Spec: `r7-dispatch.md` "COV-MV" items 1-7, `r8-dispatch.md`; COVMV-1 (exactly the two out-of-list edits: the new
  impl file + CMake tail; one term in `nav_workspace_reserve_bytes` with a flag-absent test); COV-11 (a = 2 control;
  per-row group check in COV-MV); COV-13 (c) (never F^-1); COV-14 (the P9-MV summary prints the eigen 00-02 share).
- Kernel: `mv_aim` never inverts F; rank-deficient F gives the dense solve's target (finite); demeaned alpha over
  active; NaN = no opinion; gross-normalised; dollar-neutral on the priced set; scalar washes out; working-bytes bound;
  refusals with teeth.
- Rule: hook before `record_construction` so the calibrate replay sees the post-rule target; incompatible set
  (aim-partial-v6, spo-*, two-speed, adv-hold, hold-band, construction-grid); `risk_model` / `risk_model_sha256`
  required (PRE-9); store open only with the rule; the stats count scored decisions once (no double count on the
  calibrate pass 2); flag absent = bytes unchanged incl. help / header text changes being help-only.
- COV-4 trace (the reviewer traces it): the covariance read at decision d is store row d (no row after d); the
  specific vol used is the store's D at d.
- Registration: `source_sample_end` verified from a primary source or "unverified"; the manifest pin equals
  `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3`; the lane never opened the pinned store.
- AL-CLOCK's keep-both row (W3-9): present if AL-CLOCK is in root at the lane's last merge, else noted for root.

## SQL3 (first review; pool-13)

- Spec: `r7-dispatch.md` "SQL3" items 1-6; item 5 pending by rule (B2); SQL3-1 (`atx.run-store/v1` listed, not in
  `classes.json`; the classes guard red names only it); G-P10 (a)-(e) each landed / partial / P10.
- The hook never changes an exit code or a JSON byte; catalog off = no subprocess, bytes identical (the tiny-world
  digests); `store` block last in `start.json` / `receipt.json`; the seal walk (W3-4): outermost `.git` ancestor,
  fs error = whole path sealed (F-2); `book_monitor.py` refuses an indexed `--fit-work` dir, fail closed, fitter
  untouched (W3-5).
- Run the store suite and `test_research_store_hook.py` yourself (no exe needed for the fake-store cases).

## OD3P (first review; pool-21)

- Spec: `r7-dispatch.md` "OD3P" items 1-7; item 6 (T2) pending by rule; OD3P-1 (the carry tool zeroes nothing; the
  carried weights are TRAIN's bytes; `fit.carry.drop` -> `--absent-members <file> --absent-members-sha256 <hex>` on
  history roles' u and w passes only; refuse an IC exe without `absent-members-v1`, a drop with no history role, a
  base applying a listed rule, pending rules, no `--era`); ALCLOCK-2 (`info-clock-rate-v1` in `rules`; `mv-aim-v1`
  alone pending); the runbook's required lines (four history lines; `holdout_gate.py` never; the identity-bridge
  flag on every print; "not read: no producer" fallback; values unknown marked so).
- `od3p-absent-contract.md`: complete and consistent with OD3P-1 (schema, keys, flag spelling, capability, the
  defining equivalence, where the cycle puts the flags); it is lane ABS's input.
- Blind: the runbook and report carry no 2020-2023 statistic (the two R-2 figures of the v8 protocol are disclosed
  as not used).
- Teeth: re-do two of the 22 mutations yourself.

## A4, ABS, SIGN, D3W-PB (first reviews; when they hand back)

Spec = `briefs/brief-<LANE>.md` + the lane's `r7-dispatch.md` / `r8-dispatch.md` section; identity vs the Python
builders (A4), the defining equivalence (ABS), row-for-row identity vs the Python predicate (SIGN), flag-absent bytes
(all). Stretch lanes merge only if APPROVE at the gate: a Required finding that the lane cannot close in one round
means P10 for that lane, not a weakened verdict.
