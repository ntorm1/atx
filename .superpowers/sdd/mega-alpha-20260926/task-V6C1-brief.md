# Task V6-C1: turnover-waste fixes in the NAV replay (delta orders, nonmember exit rate, locate-in-aim, post-ramp L, fixed-rate liquidity cache)

**Pool:** C:/atx-wt/pool-10. First: `git status` clean, then `git checkout -B feat/mega-alpha-v6-c1-20260927 e0dfb8c7`
(e0dfb8c7 = pool-2 HEAD). Work ONLY in pool-10. Never build, never run real data, never spawn subagents, never touch
C:/atx or other pools. Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
**Model:** Opus 5.5. **Report:** `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-V6C1-report.md` (write it
from pool-10 into that pool-2 path; it is a report file, not code). Reply < 15 lines: status, commits, test summary, concerns.

**Read first:** `.superpowers/sdd/mega-alpha-20260926/v6-code-review-exec.md` sections 2 (F1, F2, F4, F5, F8), 5 (C1-C5)
and 6 -- it cites exact file:line in `atx-impl/src/strategy_nav_replay.cpp` (NAV) and `atx-impl/src/strategy_target_replay.cpp`
(TR). Also `.agents/cpp/agent.md` (house style) and the v6 pre-registration section "## v6 revision" at the end of
`.superpowers/sdd/mega-alpha-20260926/v4-prereg.md` (items C1-C4 there are this task).

**Deliverables (all default-off; v5 output bytes must be unchanged when no new flag is passed):**
1. `--order-basis {target,delta}` (default `target`). `delta`: at DECIDE store `delta_i = (planned_i - current_i) * nav_post`
   in decision-NAV dollars; at EXECUTE the requested fill is `delta_i - filled_so_far_i` so one-day price drift rides and the
   next DECIDE corrects it at theta. Recipe key + summary JSON key recording the basis. Decide how partially filled / capped
   residuals carry over under `delta` and document it.
2. `--exit-rate r` (default 1.0 = current behaviour) in the target replay: for a non-live (nonmember) name
   `next = current * (1 - r)`, snapped to 0 when `|next| <= dust/N_d` (same dust band as live names). Names with no price /
   stale write-off keep the existing exit path. Recipe/summary key.
3. Locate-in-aim: borrow tiers are classified before the desired target is formed (NAV ~868 vs ~872 per the review); pass
   the special-tier set into `form_desired` and zero special-tier NEGATIVE aims BEFORE `neutralize_price_risk` so the
   regression re-balances net and beta. Flag `--locate-in-aim` (default off). Keep the existing post-neutralisation block as
   a safety net.
4. nav_summ.py (`.superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py`): add `mean_gross_leverage_post_ramp` and
   `mean_net_leverage_post_ramp` (rows after the first 63 CSV rows) in text and `--json`; every existing key stays
   byte-identical for the same inputs. Extend `studies/test_nav_summ.py`.
5. F8: enable `LiquidityCache` on the fixed-rate path too (its contract says bit-identical). If you cannot prove
   bit-identity by reading the code, make it opt-in via `--liquidity-cache` and say so.

**Tests:** postimplementation GoogleTest fixtures in the existing NAV/target test targets: (a) `order-basis target` byte-
identical to the pre-change path on the existing synthetic fixture, (b) `delta` with theta = 1 reproduces `target`,
(c) `exit-rate 1` identical to today, `exit-rate .05` geometric decay with snap, (d) locate-in-aim leaves |net| smaller than
the post-block path on a fixture with one special-tier short. You cannot build: write the tests, desk-check them, and list
in the report exactly which test targets the root must run (`atx-equity-strategy-targets`,
`atx-impl-strategy-target-tests`, filter `TargetReplayV5.*:NavV5*:*BitIdentical*:*V6*`).

**Constraints:** do not touch `strategy_price_exposures.cpp` or the neutralisation math (another lane owns them; you may
add the special-tier zeroing in the caller only). Do not edit `v5_train.sh`/`v51_train.sh`; instead write a NEW
`studies/v6_train.sh` derived from `v51_train.sh` that adds env knobs `ORDER_BASIS`, `EXIT_RATE`, `LOCATE_AIM`, `LCACHE`
and dir names `build-equity/mega-nav-v6-<combined>-t<theta>-d<dust>-<rate>-ob<basis>-x<exit>[-loc][-L<lev>]`, and that
exits non-zero when a nav run fails (T34c m3). Keep RAM/time behaviour equal or better (180 s / 1536 MiB bound).
Report: what changed (file:line), how bytes are proven unchanged, open risks, and the exact root command lines.
