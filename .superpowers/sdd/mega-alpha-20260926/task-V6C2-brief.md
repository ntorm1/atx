# Task V6-C2: industry neutralisation `price-risk-ind-v1` / `-v2`, exposure plumbing, RAM reserve at actual geometry

**Pool:** C:/atx-wt/pool-11. First: `git status` clean, then `git checkout -B feat/mega-alpha-v6-c2-20260927 e0dfb8c7`.
Work ONLY in pool-11. Never build, never run real data, never spawn subagents, never touch C:/atx or other pools.
Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
**Model:** Opus 5.5. **Report:** `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V6C2-report.md`.
Reply < 15 lines: status, commits, test summary, concerns.

**Read first:** `.superpowers/sdd/mega-alpha-20260926/v6-code-review-exec.md` sections 2 (F6, F7, F9), 4 (lever 6-8) and
5 (C3, C4); `.agents/cpp/agent.md`; the "## v6 revision" section of `.superpowers/sdd/mega-alpha-20260926/v4-prereg.md`
(item C5 is this task). Files: `atx-impl/src/strategy_price_exposures.cpp` (+ hpp), the neutralisation call sites in
`atx-impl/src/strategy_nav_replay.cpp` (field loading ~66 and ~1494-1534, workspace reserve ~1413-1421) and
`atx-impl/src/strategy_target_replay.cpp` (`form_desired`, `neutralize_target`). Fields `grp_ff12`, `grp_ff49`, `mkt_ret`
exist in the TRAIN fields-v6 manifest (see `build-equity/` role/fields manifests referenced by `studies/v51_train.sh`).

**Deliverables (new neutralisation ids; `price-risk-v1` bytes unchanged):**
1. `price-risk-ind-v1`: the price-risk-v1 regressors PLUS within-FF12 demeaning of the target (Frisch-Waugh: demean target
   and regressors within group, then the existing OLS; document the exact order). Groups with < 5 members fall back to the
   universe mean. Names with missing group id are treated as one residual group. Deterministic; same amplification guard.
2. `price-risk-ind-v2`: ind-v1 with slower exposure windows (vol 126, ladv 252; beta window unchanged unless the code
   makes 252 trivial). Expose the windows via the neutralisation id only (no new CLI surface beyond `--neutralize`).
3. Optional (only if cheap): `price-risk-ind-v1-mkt` variant with beta against the `mkt_ret` field instead of the EW
   all-instrument mean (F6). Skip if it needs new field plumbing beyond what 1 already adds.
4. Field plumbing: load `grp_ff12` (and `mkt_ret` if 3) through the existing `load_fields` path; charge the workspace
   reserve at ACTUAL geometry (names x dates x books actually used) instead of the fixed 20,000 x 4,096 maximum (C4), with
   the same refusal semantics. Report the expected RSS delta for +1 integer field on the TRAIN panel (5,627 instruments x
   ~753 dates).
5. F9 (perf, only if it stays bit-identical and < 150 LOC): ring buffer of per-session logs/returns in
   strategy_price_exposures so each decision does not re-read 253 sessions x all instruments. Otherwise write the design
   in the report and skip.

**Tests:** postimplementation GoogleTest fixtures: (a) `price-risk-v1` byte-identical to today on the existing synthetic
fixture, (b) ind-v1 on a fixture with two groups leaves within-group means of the neutralised target at 0 (1e-12) and
book gross rescaled to 1, (c) small-group fallback, (d) reserve-at-geometry accepts a panel that the fixed reserve refused
and refuses one above budget. List the exact test targets and filter for the root.

**Constraints:** do not edit order/fill/exit logic in strategy_nav_replay.cpp (lane V6-C1 owns `order-basis`, `exit-rate`,
locate-in-aim); confine nav_replay edits to field loading, the reserve, and the neutralisation dispatch. Do not edit
v5_train.sh / v51_train.sh / nav_summ.py. RAM/time must stay within 180 s / 1536 MiB for TRAIN; estimate the added log()
cost. Report: changes (file:line), byte-identity argument, RSS/time estimates, root commands (`--neutralize price-risk-ind-v1`).
