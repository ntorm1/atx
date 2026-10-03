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

## Lane D1: composition rule table, ordered stages, one theme table

**Pool / branch:** 16, `feat/p9-d1-20261003`. **Effort:** M. **Serves:** Sharpe (new rules become cheap), infrastructure.
**Read:** composition review (all; CM-4, §3 touch points, table row 8), main review F-8, F-13, the DS review §3
"Memory", `strategy_ic_admission.cpp:413-840` and `:233-287`, `strategy_ic_runner.cpp:282-289`,
`strategy_ic_composition.{hpp,cpp}`, `strategy_ic_detail.hpp:146-176`, `fit_composition_weights.py:247-267, 2337-2473,
280-291`.
**Deliver:** `strategy_ic_rules.{hpp,cpp}`: a `CompositionRule` table `{id, block_key, parse, verify, apply_stage,
recipe_text, working_bytes}` replacing the 4-row `theme_standardise` table (`admission.cpp:500-526`) and key-presence
dispatch (`:814-835`); `IcComposition` takes an ordered stage list (replacing the 20-parameter `score_role` list);
`atx-equity-strategy-ic --list-rules --json` (K-P9-6); the theme list read from
`atx-impl/strategies/alphas/registry.json` and passed to the exe (replaces `strategy_ic_theme_resid.hpp:24-29` and the
theme column of `strategy_two_speed.hpp:27-32`; the fitter's theme tuple reads the registry); a Python rule plugin list
replacing the fitter's if / elif (dispatch only, no arithmetic change); one centred-tied-rank helper (3 C++ copies, CM
§2); the memory admission prints `required_bytes` from `--plan-only` even when over the cap (`:283-285`) and documents
the worst-candidate + theme-plane peak. Gtests: `CompositionRules.TableCoversEveryRule`,
`.RecipeTextPinned` (byte-equal to today's recipe text per rule), `.IncompatiblePairRefused`, `.ThemeTableFromRegistry`,
`IcAdmission.PlanPrintsRequiredBytesOverCap`; pytest for the plugin list and the registry theme read.
**Files in scope:** plan §2.2 row D1. **Forbidden:** `strategy_marginal_ic.*` (S1), the NAV files (C1).
**Root verifies:** builds `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`; X-5's fit, u and w byte-identical
(recipe text pinned); the list-rules JSON pinned; pytest `atx-impl/tools`.
**Out of scope:** any new rule (AL-COMB), fit verbs (D2), weights v2 (D3).

