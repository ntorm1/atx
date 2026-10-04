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

## Lane T1: tiny-world canary, CTest registration, statistics tie fixture, guard tests

**Pool / branch:** 19, `feat/p9-t1-20261003`. **Effort:** M. **Serves:** significance, infrastructure.
**Read:** main review F-1, F-14, F-10, P9-R10; fields review §1 (fields tests not in CTest, `atx-engine/tests/
CMakeLists.txt:364-367`); DS review §6; audit §4 (class-C list and deletion rule); `scripts/tests/fixtures/
tiny_world.py`, `scripts/tests/test_cycle_e2e.py`, `scripts/research-build.ps1`, `atx-impl/tools/backtest_integrity.py:
160-486`, `nav_summ.py:420-463`, `dsr_total.py`, engine `eval/{deflated_sharpe,min_trl,pbo,trial_clusters}.hpp`.
**Deliver:** (1) the canary: `test_cycle_e2e.py` runs u / fit / w / NAV on the real exes when `ATX_EQUITY_BIN` is set,
pins SHA-256 goldens (root records them), and checks the planted members' mean IC and HAC t within one SE of the planted
value; `research-build.ps1 -Canary` runs it after a build. (2) CTest registration with labels `atx_research` /
`atx_equity_strategy` for `atx-engine-research-fields-tests` and every strategy / research test exe. (3) The tie
fixture `atx-engine/tests/fixtures/eval_tie/`: committed daily series (synthetic), a generator that runs today's Python
(PSR, DSR house definition, MinTRL, PBO, ONC, paired CBB dSR with block 21 / seed 20260929 / 4,999, Memmel SE) and
stores its values, and a gtest that reproduces each from the engine headers (tolerance 0 where reduction order allows,
else 1e-12, the reason stated per value); a value the engine cannot reproduce is reported, not loosened. (4) Guards:
`test_no_versioned_scripts.py` (allowlist frozen at base), `test_no_python_mirror.py` (allowlist rows with the lane that
retires each). (5) The class-C deletion as one separate commit (audit §4 list and their tests), merged only after root's
`generate_from_spec.py --spec specs/library-v71.json --check` exits 0.
**Files in scope:** plan §2.2 row T1. **Forbidden:** the Python statistics themselves (B2 deletes them in wave 2).
**Root verifies:** canary goldens on Debug, then Release; `ctest -N -L atx_research` count in the log; the tie gtest.
**Out of scope:** the eval verb (B2), the CMake split of `atx-impl-core` (C3).

---

