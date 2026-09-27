# Mega-alpha v5 — interim parent handoff (2026-09-27, paused mid-P1/P2 at owner request)

Author: parent 5 (Claude Fable 5.1, SDD controller). Integration checkout `C:/atx-wt/pool-2`, branch
`feat/aes-codex-integration-20260925`, HEAD `e620d3c1` (50 commits ahead of `d63a7058`; plus this file's commit).
Spec: `docs/plans/2026-09-27-mega-alpha-v5-dag-plan.md`. Ledger (newest first): `.superpowers/sdd/mega-alpha-20260926/progress.md`.
Trust the ledger and `git log` over memory.

## 0. TL;DR

- P0 done: T28 audit (frozen v4.1 ran at mean gross 0.256 TRAIN / 0.235 VAL — validation trial #2 was a 24%-gross book, ruling R-1),
  T29 pre-registration `## v5 revision` R1'-R7' committed at `56e5b148` before any v5 TRAIN read; v5.1 family pre-registered at `c8192c46`.
- P1: **T30 (aim-partial-v5 rule) complete and landed in pool-2**, compiled (tag v5-0) with 10/10 v5 fixtures and 49/49 suite green;
  D2 byte-stability of the frozen v4.1 cell holds on real data with the T30 binary. **T31 (ew-theme-aim-v1 fitter, v5_train.sh,
  nav_summ netting/ΔSR/DSR) complete and landed** (81 pytest at root). **T34b (library v5.1 = v4 + opex_at proxy) complete and
  landed**; its `--plan-only` is admitted (1,449,071,914 B, 38 candidates). T33a delisting feasibility = **NO-GO** (59.9% classifiable
  < 80%): lane parked, T33b/T32 not built, U3 raised.
- P2 partially done ahead of T37 (ruled safe: no C++ dependence): **aim fit W_aim `54f823c1`** (1 pass, g_k ∈ [0.358, 0.985],
  EW-REFIT of ew-theme-v1 IDENTICAL modulo SHA fields) and **weighted IC pass C_aim `00439b98`**.
- **Two child lanes were live at the pause** (see §2). No root build or real-data process is running.
- No validation run. No 2023+ data read (T28's re-read of two existing VAL NAV CSVs is a disclosed book-level diagnostic, not trial #3).

## 1. Where things stand (task by task)

| Task | State | Evidence |
|---|---|---|
| T28 audit | complete | `construction-audit-v4.md`, commit `44e6c28a` |
| T29 prereg + rulings | complete | `v4-prereg.md` `## v5 revision`, commit `56e5b148`; ledger `d4ec515d` |
| T30 aim-partial-v5 (C++) | complete, review clean (0C/0I/6m), cherry-picked `8ef8529c`/`25f41c2c` | `task-T30-report.md`, `task-T30-review.md`; build `mega-v5-0-receipt.json`; D2 run `mega-nav-v5-baseline-check-run` |
| T36 per-name rate (C++) | **IN PROGRESS** in pool-3 (6 uncommitted files at pause) | agent `t36-rate`; report expected at `pool-3/.superpowers/sdd/mega-alpha-20260926/task-T36-report.md` |
| T31 aim fitter + pipeline + nav_summ | complete after fix round 1 (DSR producer), cherry-picked `81e9977d`/`22c30c38`/`c8358931` | `task-T31-report.md`, `-review.md`, `-rereview-1.md` |
| T33a delisting feasibility | complete, NO-GO | `delisting-feasibility.md`, commit `2c9f7627` |
| T33b / T32 | **not built** (parked by R-6) | ledger ruling |
| T34a breadth check | complete, GO with proxy | `breadth-check-v5.md`, commit `c8192c46` |
| T34b library v5.1 | complete, review clean (0C/0I/4m), cherry-picked `353348ee`/`33c43832`; `--plan-only` admitted | `task-T34b-report.md`, `-review.md`; `mega-v51-plan-run` |
| T34c v51_train.sh (added task) | **IN PROGRESS** in pool-8 (just dispatched) | agent `t34c-v51-script`; report expected `pool-8/.../task-T34c-report.md` |
| T35 | not needed (plan admitted) | — |
| T37 build v5-1 | **not started** (waits on T36) | — |
| T38 | steps 1-2 done (fit, w pass); step 3 grid **not started** | ledger sections "v5 TRAIN read #1" and "T38 step 2" |
| T39 | step 1 (delisting) does not run; step 2a plan-only done; 2b (u/fit/w/NAV for v5.1) waits on T34c + T37 | — |
| T40-T42 | not started | — |

## 2. Live children at the pause (check before doing anything)

1. `t36-rate` — pool-3 `feat/mega-alpha-v5-construction-20260927` on `98277c45`. Dispatch: per-name rate `rate per-name-v1`
   (rulings R-a..R-e in the ledger's T36 dispatch; rate_stats field names; span validation; liquidity rows once per decision).
   If `task-T36-report.md` exists and commits are on the branch: package `review-T36.diff` (base `98277c45`) and review. If the
   tree is dirty and no report: re-dispatch a fresh Opus 5.5 implementer with the same brief (`task-T36-brief.md`) and the dispatch
   text from the ledger; tell it to inspect `git status`/`git diff` first and continue from what is there.
2. `t34c-v51-script` — pool-8 `feat/mega-alpha-v51-pipeline-20260927` on `e620d3c1`. Dispatch: `studies/v51_train.sh` (u/fit/w/nav
   for library v5.1; `DRY=1` mode; no Python changes). Same recovery rule.

## 3. Exact next steps (in order)

1. Resolve §2. Review T36 (Opus 5.5 reviewer, `reviewer-contract.md`, named risks: ADV dollars vs shares, rate span size validation,
   Fixed-rate bytes == T30 bytes, no NaN). ≤ 5 fix rounds. Cherry-pick into pool-2.
2. **T37**: `powershell -File build-equity/mega-build.ps1 -Tag v5-1 -Targets "atx-equity-strategy-targets,atx-impl-strategy-target-tests,atx-impl-strategy-ic-tests"`
   (v5-0 is used; v5-1 is free). Run `atx-impl-strategy-target-tests.exe` (filter `TargetReplayV5.*:NavV5*` then full; 49 + T36's new
   tests expected) and `atx-impl-strategy-ic-tests.exe` (67). Re-run the D2 check with the v5-1 binary (command in ledger section
   "D2 byte-stability"): SHAs must equal `af058239` (recipe) and `3f846525` (S2 CSV). Ledger + commit.
3. **T38 step 3** (10 bounded NAV runs, reference first), from `C:/atx-wt/pool-2` with a clean tree:
   ```bash
   S=.superpowers/sdd/mega-alpha-20260926/studies
   for C in ew aim; do for cell in "t.05 d.1 fixed" "t.03 d.1 fixed" "t.08 d.1 fixed" "t.05 d0 fixed" "t.05 d.1 per-name"; do
     set -- $cell; THETA=${1#t} DUST=${2#d} RATE=$3 LEV=1 COMBINED=$C bash $S/v5_train.sh nav; done; done
   ```
   Step 4 only if the reference cell (`mega-nav-v5-ew-t.05-d.1-fixed`) mean `gross_leverage` (daily CSV mean over all sessions — ruled
   definition) < 0.90: `LEV=round(1/mean_gross,3)` re-run for both C (+2 disclosed trials). Step 5: the `nav_summ.py --weights … --reference …`
   line in `task-T31-report.md` §"Root command lines" (pass all 10 cells so DSR uses V[SR_n] across cells, N = 10).
   Root checks from the T31 review ⚠️ list: `--cadence 1` present (it is), per-name flags accepted by the binary, `mean_gross_leverage`
   CSV vs summary once.
4. **T39 step 2b** (v5.1, after T34c lands and is reviewed): `bash $S/v51_train.sh u` → `COMP=ew-theme-v1 … fit`, `COMP=ew-theme-aim-v1 … fit`
   → `w` for both → NAV reference cell only (θ .05, dust .1, fixed) for both. Trials: admission 38, composition +2, construction +1
   (or +2 if both C are run — declare which before running; the prereg says "reference cell only").
5. **T40** gate per R6' (mechanics gross ∈ [0.90, 1.05], |net| ≤ .02, τ mean ≤ .20 / p95 ≤ .30; paired ΔSR vs reference with Memmel SE
   + block bootstrap; DSR N = 10 (12/13 if extra cells)). Freeze proposal ONLY if a cell's TRAIN net ≥ 1.0. Write `task-T40-report.md`
   with the Appendix A block. **No validation run** (needs U1).
6. **T41** whole-branch Opus 5.5 review (`git diff d63a7058..HEAD -- atx-impl atx-engine .superpowers/sdd/mega-alpha-20260926/studies`,
   plus the deferred-minor list in the ledger). **T42** handoff 4 + owner packet U1-U5.

## 4. Numbers so far (TRAIN 2020-2022 only)

- Aim gains (θ .05): value .960-.978, profitability .907-.985, investment .936/.976, low_risk .854-.975, momentum .890-.926,
  short_interest .972/.935/.449 (si_change), earnings_momentum .753-.847, iv_rv_spread .565, reversal .487/.358. Half-sample max
  |Δg| .05. Aim theme weights: investment .131, value .132, profitability .131, low_risk .127, momentum .125, earnings .111,
  short_interest .108, options .077, reversal .058. weighted_standalone_turnover .0425 (ew-theme-v1 .0519).
- Pins: library `daa9663e`; role v2 `210fff96`; fields-v6 `32565c32`; admission `b41ba653` (31, same set as v4.1 `880a0a6a`); W_ew
  `9a9c949a` (unchanged); W_aim `54f823c1`; C_ew `24a6cc76`; C_aim `00439b98`; v5.1 library `9e5ea08c`; IC exe `647c71a7`
  (unchanged); NAV exe from v5-0 `b6b21d88` (T30 only; T37 replaces it).
- Trial accounting (TRAIN): admission 37 (+38 v5.1 pending read); composition +1 (ew-theme-aim-v1) so far; construction v5 grid 0/10
  run; validation none new (#1 v3, #2 v4.1 24%-gross; #3 needs U1).

## 5. Rulings made this session (all in the ledger; each with cost-if-wrong)

R-1..R-7 (plan §9, verbatim); this ledger = SDD ledger; T28 does not commit; preflight: T32 branches only after T30+T36 land;
R-a amended (no θ==1 special case — uniform formula is baseline's exact IEEE sequence); dust-entry fixture tolerance
[0.9θG, θG]; rate_stats field names; T28 VAL re-read = disclosure re-read; banded-share denominator = members; v5.1 = opex_at
proxy `sale_ttm - oi_ttm` (D&A included, pre-registered); T34b relaxes the cross-section-rank assertion for profitability_quality;
delisting lane parked (R-6), U3 raised; "mean gross" for D1/R5'/R6' = daily exposure gross_leverage mean over all sessions;
"37 rows byte-identical" excludes family-description metadata; v5-0 pre-build of T30; D3 byte-stability = identical modulo the
three SCRIPT_SHA-derived fields (literal SHA equality impossible for any edited fitter); AR(1) fixture exact on full grid;
< 50 rankable names → floor gain; DSR producer lives in nav_summ.py multi-dir mode; T38 steps 1-2 may precede T37;
v5.1 gets its own `v51_train.sh` (T34c). Deferred minors (T30 ×6, T31 ×8+2, T34b ×4) are listed under each review section for T41.

## 6. Owner decisions still open (do not block T36-T41)

U1 holdout policy (any 2023-2024 read = trial #3; or release 2025+); U2 pre-2020 history; U3 delisting data — now concrete: bridge CIK
coverage for 203 no-CIK terminated lines (ETFs/ETNs/SPACs/ADRs/preferreds/warrants) or a sealed submissions export + ticker-continuity
input; U4 cost target; U5 merge into main (main has diverged).

## 7. Goal prompt for the next parent agent

```
/goal Resume the mega-alpha v5 "deploy the book" DAG sprint. Read, in order: docs/plans/2026-09-27-mega-alpha-v5-dag-plan.md
(the spec), docs/plans/2026-09-27-mega-alpha-v5-interim-handoff.md (this pause), and the top sections of
.superpowers/sdd/mega-alpha-20260926/progress.md (newest first). Trust the ledger and git log over memory.

State at pause (pool-2 HEAD e620d3c1 + handoff commit): P0 done (prereg 56e5b148; v5.1 family c8192c46). Landed in pool-2:
T30 aim-partial-v5 (built as tag v5-0, 10/10 + 49/49 green, D2 byte-stability holds on real data), T31 ew-theme-aim-v1 +
v5_train.sh + nav_summ (81 pytest), T34b library v5.1 (--plan-only admitted). T33a NO-GO: delisting lane parked (T33b/T32 not
built). Already run (ruled safe before T37): aim fit W_aim 54f823c1 (g_k in [.358,.985], EW-REFIT identical) and weighted IC
pass C_aim 00439b98. Two children were live: t36-rate in C:/atx-wt/pool-3 (branch feat/mega-alpha-v5-construction-20260927
on 98277c45, per-name rate T36, tree was dirty) and t34c-v51-script in C:/atx-wt/pool-8 (feat/mega-alpha-v51-pipeline-20260927
on e620d3c1, studies/v51_train.sh). First: inspect both pools (git status/log, task-T36-report.md / task-T34c-report.md); if a
report and commits exist, package review-TN.diff and review; else re-dispatch a fresh Opus 5.5 implementer with the same brief
and the dispatch rulings recorded in the ledger, telling it to continue from the dirty tree.

Then: T36 review -> cherry-pick -> T37 build tag v5-1 (targets atx-equity-strategy-targets, atx-impl-strategy-target-tests,
atx-impl-strategy-ic-tests; re-run the D2 check with the v5-1 binary: recipe af058239, S2 CSV 3f846525) -> T38 step 3 the
10-cell NAV grid exactly as R5' (reference = ew, theta .05, dust .1, fixed; run it first), step 4 L re-run only if the
reference mean daily gross_leverage < 0.90, step 5 nav_summ.py with all 10 cells (--weights both W, --reference, DSR N=10)
-> T39 step 2b v5.1 via v51_train.sh (u, both fits, w, reference NAV cell; disclosed trials admission 38 / composition +2 /
construction +1) -> T40 gate per R6' (mechanics + paired dSR Memmel SE + block bootstrap + DSR); freeze proposal ONLY if a
TRAIN cell's net >= 1.0; otherwise report -> T41 whole-branch Opus review (deferred minors listed in the ledger) -> T42 handoff
4 with Appendix A trial accounting and the owner packet U1-U5. Stop after T42. Do not run validation.

Method: subagent-driven development (superpowers:subagent-driven-development). Opus 5.5 for EVERY implementer, explorer,
researcher and reviewer; preserve your own context: artifacts as files, children get the brief path
(.superpowers/sdd/mega-alpha-20260926/task-TN-brief.md), plan-s3-constraints.md, their Interfaces block and lane-contract.md;
reviewers get review-TN.diff + reviewer-contract.md (re-reviews: re-review-contract.md); <= 5 fix rounds; controller never
implements (small script tasks go to a child in a pool). Pools: 2 = root; 3 (T36), 8 (T34c) live; 4, 5, 7, 9 reusable via
git checkout -B <branch> <pool-2 HEAD>; 10, 11 free; never 1 or 6.

Rules (verbatim, binding): work only in C:/atx-wt/pool-2 (feat/aes-codex-integration-20260925); never mutate/build/switch/commit
in C:/atx (read-only ok; the tier1-v2 session owns C:/atx and atx-db: no locks, never kill its processes). Root alone builds
via powershell -File build-equity/mega-build.ps1 -Tag <new> -Targets "<t1,t2>" (tags v5-0 and v5-1... check
build-equity/mega-<tag>-receipt.json does not exist). Root alone runs real data under
"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512
--output build-equity/<new-dir> --bind <manifests> -- <cmd>; tree must be clean (untracked included): commit docs, briefs,
ledger and scripts (git add -f for the gitignored sprint dir) before every run. Children: own pool worktree, never build,
never run real data, never spawn subagents, Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com> trailer, task-TN-report.md,
reply < 15 lines. TRAIN 2020-2022 only; 2023-2024 used twice (any new read = disclosed validation trial #3, needs owner gate
U1); 2025+ reserved; never read a per-candidate VAL statistic. No pushes, warehouse writes or broker actions. No TDD;
postimplementation fixtures; tasks accepted on root real-data measurements. RAM: limits stay 180 s / 1536 MiB, efficiency
fixes not longer caps. Every ruling: "Ruling: <decision> -- <why> -- cost if wrong: ...", declared before any measurement it
could bias; report every TRAIN result with the Appendix A trial-accounting block. U1-U5 do not block T36-T41.
```
