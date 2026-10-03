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

## Lane B2: one engine `eval` verb and the ledger library

**Pool / branch:** 14, `feat/p9-b2-20261003`. **Effort:** L. **Serves:** significance.
**Read:** main review F-1; orchestration review §3 "Ledger", "DSR/PBO duplicated", OR-5, OR-6; audit §3 rows 4-5;
migration §4 slice 8; T1's tie fixture; lit §5.1-§5.2 ([70], [73], [74]); `nav_summ.py`, `backtest_integrity.py`,
`dsr_total.py`, `scripts/research_ledger.py`, `atx-impl/src/trial_ledger.{hpp,cpp}`, engine `eval/*.hpp` named above.
**Deliver:** `atx-research-eval` (K-P9-5) over the engine headers plus the paired CBB dSR and Memmel SE ported from
`nav_summ.py`; DSR `house_v1` (N = construction count, V = window cell variance; `backtest_integrity.py:1254-1277`)
beside the engine's named methods; the winner's-curse-adjusted cumulative gain (Andrews-Kitagawa-McCloskey [73]),
printed only; `atx-engine-research-ledger` (trial_counts, N_tot, era pooling; `trial_ledger.cpp` moved down, atx-impl
keeps a thin include); the record stage writes the ledger head into the sprint dir (cross-lane edit in
`wave_stage_record.py`, listed). `nav_summ.py --engine-stats` calls the verb; the Python statistics stay until root's
identity, then a separate prepared commit deletes them. Gtests: T1's tie fixture through the verb, `EvalVerb.JsonSchema`,
`ResearchLedger.TrialCountsEqualPython`, `Conditional.ClosedFormNormalCase`.
**Files in scope:** plan §2.2 wave-2 row B2. **Forbidden:** acceptance rules (`wave_rules.py`).
**Root verifies:** builds the eval and ledger targets; `nav_summ --protocol v8` on X-5 and on Y-F0 prints identical values
with and without `--engine-stats`; the ledger chain verifies.
**Out of scope:** any change to which statistic decides (PM7-34 stays).

