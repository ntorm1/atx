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

## Lane B1: `factors` verb and `research/admission` in C++

**Pool / branch:** 14, `feat/p9-b1-20261003`. **Effort:** L. **Serves:** significance, infrastructure.
**Read:** composition review (all; CM-3, table rows 6-7, §4 "Sign rule"), main review F-3, audit:127-129,
`atx-impl/tools/fit_composition_weights.py:1130-1170` (factor record), `:1572-1583` (NW t), `:1613-1671` (screen_v4),
`:1641-1670` (greedy), `atx-impl/src/strategy_exposures_verb.{hpp,cpp}`, `atx-engine/include/atx/engine/eval/hac.hpp`.
**Deliver:** (1) `atx-equity-strategy-targets factors` (K-P9-4) in new `strategy_factors_verb.{hpp,cpp}`: the
neutralised gross-1 rank book q per candidate on the exposures basis, f = q . r(d+2), tau; one dispatch line in
`equity_strategy_targets.cpp`. (2) `atx-engine-research-admission`: `screen_v4` (no_prior; < 250 live days; tau > .70;
NW HAC t < -2.0, Bartlett lag 5, divisor n, no small-sample correction; first failure wins) on
`eval::hac::mean_inference` with a method value that reproduces the fitter's arithmetic; greedy redundancy (|rho| > .90
over >= 250 common days; (tier, roster index) order; strict >); the PM7-35 sign rule as one predicate parameterised by
the PM's ruling (the gate requires runner sign = prior, `wave_rules.py:50-51` keeps sign 0: write both, the PM picks
one before merge); the traded-horizon columns (IC and HAC t at 21-session overlap) as report-only output (F-3).
(3) A comparator pytest that imports the fitter's `screen_v4` / `factor_record` as library functions on synthetic data
and checks the C++ outputs' committed fixture bytes (no fitter edit). Gtests: `ResearchAdmission.ScreenV4ClosedForm`,
`.FirstFailureWins`, `.GreedyOrderTierThenRoster`, `.SignPredicate*`, `FactorsVerb.EqualsFixture`,
`FactorsVerb.FutureReturnDoesNotChangePast`.
**Files in scope:** new `atx-engine/{include/atx/engine,src}/research/admission/**` and its CMake block and tests, new
`atx-impl/src/strategy_factors_verb.{cpp,hpp}`, the dispatch line in `atx-impl/tools/equity_strategy_targets.cpp`, the
`atx-impl` CMake source line, new tests and fixtures. **Forbidden:** `fit_composition_weights.py` (D1 owns it in wave 1;
D2 wires it), `strategy_ic_admission.cpp` (D1).
**Root verifies:** builds `atx-equity-strategy-targets`, the admission library and tests; factor series of X-5's library
equal the fitter's records (bytes, else 1e-12 with the reason); X-5's and the Y-S screen's `admission.csv` reproduced
byte for byte by the C++ screen.
**Out of scope:** fit verbs (D2), changing the gate's horizon (a ruling).

