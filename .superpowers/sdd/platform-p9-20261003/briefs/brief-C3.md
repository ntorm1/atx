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

## Lane C3: typed NavSpec, cost-law and trade-rate registries, the NAV file split

**Pool / branch:** 15, `feat/p9-c3-20261003`. **Effort:** L. **Serves:** infrastructure, gross return.
**Read:** nav review §1 (layout), §2 (67 flags in two parsers; compiled constants), §3 (cost of a rule), NV-6; main review
F-8, F-10, F-11; C1 / C2 as merged.
**Deliver:** task 1, move-only: split `strategy_nav_replay.cpp` at the reviewed seams (validation, cost / liquidity,
MARK, EXECUTE, DECIDE, writers, loaders, summarize, publish, CLI) into `atx-impl/src/book/*.cpp`, byte-identical
outputs; commit alone. Task 2: NavSpec JSON (`nav --spec S.json`), the flag parser kept as a thin adapter producing the
same NavSpec; cost-law registry (FlatBps, SqrtImpact, the v2 laws) and trade-rate registry (aim-partial-v5,
per-name-v1, two-speed) completing K-P9-7; compiled constants (initial NAV, liquidity window, min pairs, capacity
multiples) become NavSpec fields with today's defaults; the leverage range a NavSpec field (default [1, 2]; wider only
by OD-P9-3). Task 3: split `atx-impl-core` into `atx-impl-strategy` and `atx-impl-pipeline` (F-10). Gtests:
`NavSpec.FlagsAndSpecSameConfig`, `CostLawRegistry.*`, `TradeRateRegistry.*`, the existing NAV suite unchanged.
**Files in scope:** the NAV files, new `atx-impl/src/book/**`, `atx-impl/CMakeLists.txt`, tests.
**Root verifies:** X-5 and Y-F0 NAV via `--spec` byte-identical to the argv form; full target-tests suite; build time of
the research exes before / after the core split.
**Out of scope:** new rules (AL-CLOCK adds the first).

