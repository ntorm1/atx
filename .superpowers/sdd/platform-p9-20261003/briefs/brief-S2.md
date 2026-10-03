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

## Lane S2: VM kernels in parallel, one `OpSig` table, IC label and coverage options

**Pool / branch:** 18, `feat/p9-s2-20261003`. **Effort:** L. **Serves:** infrastructure, significance.
**Read:** `docs/plans/2026-10-02-p9-code-review-dsl-ic.md` (all; DS-2..DS-5, §3 HAC and labels, §6); `vm.hpp:720-730,
1348-1372, 1399-1443, 1450-1495, 1498-1535`; `registry.hpp:74-359`; `typecheck.hpp:88-157`; `streaming_engine.hpp:
202-231`; `oracle.{hpp,cpp}`; `ic_screen.cpp:63, 203, 242-260, 283-358`; `ic_screen_config.hpp`.
**Deliver:** band-split of the serial kernels by the declared `chunk_axis` with per-worker scratch (as `ts_col_thr_`);
columns extracted once per instruction in `eval_lit_ts`; one constexpr `OpSig` table with family, axis, needs_group,
stateful (static_assert size == last opcode + 1), every predicate derived from it, `lookahead_safe` derived;
`IcScreenConfig::min_coverage` (default .8) keyed in the IC-result scope; `--label-terminal imputed-v1` (label-only
price column with imputed terminal returns; report the missing-exit share and IC under both rules); `--eval-mode
audit-exact` with the cache under its own identity token; the IC runner's HAC rule named and selectable beside
`eval/hac.hpp`'s (default unchanged). Gtests: worker parity 1 / 4 / 16 for every banded kernel, a per-opcode
truncation-invariance and slot-reuse sweep over all opcodes including `formulaic_ops()`, `OpSig.PredicatesAgree`
(streaming and VM), `IcScreen.MinCoverageConfig`, `IcLabels.TerminalImputedClosedForm`.
**Files in scope:** plan §2.2 wave-2 row S2. **Forbidden:** the marginal verb (S1's), composition (D lanes).
**Root verifies:** alpha / factory groups and the IC exe build; golden `0x889874a3b9b29c55` at 1 and 4 workers;
`AlphaVmSlotReuse.*`; X-5 u / w byte-identical with every flag absent; cold u wall before / after.
**Out of scope:** adopting the label or AuditExact default (P9-L cell and a P9-B0 ruling).

