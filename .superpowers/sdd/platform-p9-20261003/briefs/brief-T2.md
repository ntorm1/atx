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

---

## Lane T2: re-date the legacy `atx-engine/tools` test modules, retire the conftest window bind (PM-added, ruling A1-C)

**Pool / branch:** assigned at wave-2 dispatch, `feat/p9-t2-<date>`. **Effort:** L. **Serves:** seal discipline (G-P9),
infrastructure. Not in the plan's lane catalogue: the PM added it (ledger ruling A1-C) when A1 found that removing the
`atx-engine/tools` conftest window bind breaks 115 tests in 22 modules whose synthetic fixtures carry 2024 dates.
**Read:** the merged A1 report section "Legacy modules for T2" (table of the 22 modules and why each breaks);
`atx-engine/tools/conftest.py` (the bind); the seal code in `atx-engine/tools/prepare_research_fields.py` and
`field_registry.py`; ledger rulings A1-C, P13, T2-GOLD.
**Deliver:** (1) Re-date the synthetic fixtures of the 14 modules that fail on the seal or on window-dated counts
(`test_prepare_research_fields{,_reuse,_sec,_sv,_sic,_module_reuse}`, `test_research_fields_{holdings,connected,
mgr13f,gold,v9_nt,xdata}`, `test_build_fundamental_events`, `test_prepare_identity_bridge`) so every fixture date is
strictly before 2024-01-01, keeping each date's calendar role: weekday vs weekend, session vs holiday, month/quarter end,
13F filing quarter and deadline, fiscal quarter. A whole-year shift is valid only where those roles survive it; check
each. Every assertion keeps its meaning (a count that tested "one sealed, one kept" still tests that). (2) Ruling
T2-GOLD: `test_lo1_delisting`, `test_linked_operating_v3`, `test_linked_operating_v2` keep their expected hashes
untouched (rule 4) and get an explicit module-scoped fixture binding today's 2025 window, with a comment naming T2-GOLD.
(3) The 5 knock-on modules (`deals`, `divevent`, `ivshape`, `v8`, `v8_quarters`): fix the isolation leak (shim fields
left bound in the builder by an errored module) so each passes in any order and in one process with the rest.
(4) Remove the global window bind from `atx-engine/tools/conftest.py`; the suite runs under the repository window.
**Identity:** test files and fixtures only; no production module, no output byte, no expected hash moves.
**Tests:** `atx-engine/tools` pytest 0 failed in one process and under `PYTHONHASHSEED` 0 and 1, plus each touched
module alone; paste counts. A grep over the touched fixtures for `2024-` / `2025-` dates, with every remaining hit
explained (T2-GOLD modules only).
**Files in scope:** `atx-engine/tools/conftest.py`, the 22 modules above and their fixture files. **Forbidden:**
production modules under `atx-engine/tools` (other lanes own them), any expected hash, any real data.
**Root verifies:** the tools suite 0 failed after merge; `git diff --stat` touches only test files and fixtures.
**Out of scope:** the C++ seal consumer (S2, C3); refusing an absent seal (P13, after root's manifest check).
