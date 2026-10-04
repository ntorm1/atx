# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---


---

## Lane C1: per-book NAV config, capacity in the main lockstep, Release-safe cost, complete receipts

**Pool / branch:** 15, `feat/p9-c1-20261003`. **Effort:** L. **Serves:** gross return, capacity, infrastructure.
**Read:** `docs/plans/2026-10-02-p9-code-review-nav.md` (all; NV-1..NV-4, §4, §5), `strategy_nav_replay.cpp` (layout
in NV §1), `strategy_nav_v7.{hpp,cpp}`, `strategy_vol_target.*`, `strategy_risk_target.*`,
`atx-engine/src/book/replay_cost.cpp`, `strategy_cost_v2.{hpp,cpp}`, the YCOMB review's two composition bugs
(`review-ycomb.md:9-10`).
**Deliver:** (1) the leverage rule (fixed L, `vol-target-v1`, `risk-target-v1`) as a per-book member of
`NavReplayConfig` with per-book state, replacing the thread-local hook (v7:103), so `--book-workers` and the
`--aim-leverage` grid key work under the scalers (today refused, nav:2299-2304, 3220-3221); the leverage part of
K-P9-7 (`nav --list-rules --json`). (2) Capacity books in the main lockstep (book cap 8 -> 16; identified by
`capacity_multiple(id)`, v7:503, 535) instead of the argv re-dispatch (v7:1121-1133). (3) `sqrt(x)` where the impact
exponent is .5 (`replay_cost.cpp:92`); `pow` stays for other exponents (`strategy_cost_v2.cpp:45,138,196,231`); a probe
test of `cost_fraction` bits on fixed inputs. (4) `summary.json` written last, binding `v7_extras.json` and the
capacity summary (v7:349-430, 1107, 1134); exe identity (git SHA, build type) and the argv SHA-256 in the recipe.
(5) adv-hold capacity reads each multiple's NAV, not the initial NAV (nav:800; update `strategy_live_test.cpp:2234`'s pin
only with the reason). (6) A truncation / look-ahead test on the scaler path (NV §4 GAP). Gtests: `NavBookRule.*`
(grid at L {1.0, 1.5, 2.0} in one pass == three single runs, byte for byte; vol-target under `--book-workers 4` ==
serial), `NavCapacityLockstep.*` (curve == the separate-dispatch curve), `ReplayCostSqrt.*`, `NavSummaryBinding.*`,
`VolTarget.TruncationInvariant`, `AdvHoldCapacityPerMultiple`.
**Files in scope:** `atx-impl/src/strategy_nav_replay.{cpp,hpp}`, `strategy_nav_v7.{cpp,hpp}`,
`strategy_vol_target.*`, `strategy_risk_target.*`, `strategy_cost_v2.{cpp,hpp}`, `atx-engine/src/book/replay_cost.cpp`,
their tests in `atx-impl/tests/` and the engine book group. **Forbidden:** `strategy_target_replay.cpp` beyond the
call sites the rule move needs (list them), `scripts/**`.
**Root verifies:** builds `atx-equity-strategy-targets`, `atx-impl-strategy-target-tests`, the engine book group;
X-5's NAV re-run under the new Debug build: every file byte-identical except the list you state before the run
(summary keys added; cost columns only if `sqrt` moves a bit); then a Release NAV build compared to Debug bit for bit.
NV-3 and DS-1 disagree on the 1-ULP cause; your probe test is the evidence; state in the report what root should expect.
**Out of scope:** `--calibrate-gross` (C2), NavSpec / registries / file split (C3), L > 2.

