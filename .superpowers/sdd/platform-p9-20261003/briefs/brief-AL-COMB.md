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

## Lane AL-COMB: theme-cov-hedge-v1 and mom-volman-v1 (blind registrations)

**Pool / branch:** 19, `feat/p9-alcomb-20261003`. **Effort:** M. **Serves:** Sharpe.
**Read:** lit §1.1 [21], §1.6 [16]-[20], §6 rows 1 and 7, §7 (DMRS caveat, restatement risk), §8 Q5; plan DEC-15,
DEC-16; D1's rule table and K-P9-6; `strategy_ic_theme_tsmom.{hpp,cpp}` (schedule mechanism); `review-x5-theme-erc.md`.
**Deliver:** (1) `theme-cov-hedge-v1`: kernel `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp` (per theme:
beta_perp of each name on its own theme sleeve return over the trailing 252 decisions ending d-2, orthogonalised to the
score; h by trailing regression re-estimated every 21 decisions; w = rank(score) - h . rank(beta_perp)), estimated
causally at apply time; runner rule `atx-impl/src/strategy_ic_theme_hedge.{hpp,cpp}` as one row in D1's table.
(2) `mom-volman-v1`: the price_momentum sleeve mass scaled by sigma_target / sigma_hat (126 sessions), sigma_target the
running mean of the sleeve's own estimates (the real-time form of [17]), as a schedule rule beside theme-tsmom.
(3) A zero-trial diagnostic spec for lit §8 Q5 (the book's variance share on risk-store style columns that carry no
theme) for root to run before P9-H is registered. (4) Templates `scripts/specs/p9/templates/{theme-cov-hedge,
mom-volman}.json` with `rules:` blocks; rule registration files with K-P9-11 keys, the constants and why each is fixed.
Gtests: `CharCovHedge.ClosedForm2Names`, `.FutureReturnDoesNotChangePast`, `ThemeHedgeRule.FlagAbsentIdentity`,
`MomVolman.StepVarianceClosedForm`.
**Blind:** no constant chosen after any read; one variant each; no grid. **Root verifies:** ic-tests; X-5 w byte-identical
with both rules absent.
**Out of scope:** running anything; info-clock (AL-CLOCK).

