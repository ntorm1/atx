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

## Lane E1: wave driver hardening (task 0 = P0-FIX, the Phase 0 blockers)

**Pool / branch:** 17, `feat/p9-e1-20261003`. **Effort:** M (task 0: S, deliver first). **Serves:** infrastructure.
**Read:** `docs/plans/2026-10-02-p9-code-review-orchestration.md` (all), `docs/plans/2026-10-02-v8y-research-loop.md`,
main review F-5, F-7, F-9, NV-4 (nav review §4 "Completeness hole"), plan §1.2.
**Task 0 (P0-FIX; commit it alone, first, and report its SHA at once: root merges exactly that commit before Y-S):**
(a) `RUNNER_MAX_SECONDS = 600` in `scripts/research_tree.py`; `run_bounded_research.py:92-94` uses it; manifest load
(`wave_manifest.py:225-229`), spec load (`research_cycle.py:369-376`) and `wave_steps.py:181-183` refuse a phase cap
above it with a message naming the key; `test_wave_speed.py:80-86` stops asserting 720 against fakes. (b) budget
`admission_cycle_prefixes` (a list; the old string key stays valid) in `wave_manifest.py` and
`wave_stage_preflight.py:72`. (c) capacity completeness: a NAV phase whose argv has `--capacity-curve` is done only when
`summary.json`, `capacity_curve.csv` and `v7_extras.json` exist (`research_cycle.py:1176`); `wave_readers.py:79-81`
refuses None when the curve is expected. (d) `executable_sha256` kept in phase rows (`wave_stage_util.py:58-68`) and
`wave-result.json`; verify records parent vs cell NAV exe SHA and forces the ref pass when they differ
(`research_cycle.py:1015-1019`). (e) `O_EXCL` lock around verify-and-append (`backtest_integrity.py:1094-1118`; that
function only). (f) `scripts/tests/test_research_spec.py:60-91`: expected null pins derived from each spec's kind
(template / child / add-alpha copy / gm), not a file list. (g) a documented two-seed suite command. Root edits
`y-s.json` itself (DEC-1, DEC-2); you do not touch any registration file.
**Then (wave 1 proper):** K-P9-10 (`argv_sha256`, `attempt`, `executable_sha256`, `build_type` in every bounded
`receipt.json`; resume refuses a mismatch: OR-3); attempt sub-dirs `<output>/attempt-k/` with auto-advance when the
runner refused and wrote nothing (OR-4); bounded waiting launch admission (free >= declared peak + floor; no `cl.exe`,
`clang-cl`, `ninja`, `lld-link` process) (F-5 (a)); a host memory semaphore over declared caps so card || marginal,
ref || u and summ || bundle || reader run in parallel (OR §5); `lock` writes `exes_sha256`, verify compares parent and
cell (OR-2); receipt chain digests over content keys only (no `started_utc` / seconds; OR §3); queue history dated from
the manifest, not today; reader reuse keyed on the reader code SHA; `cycle_verdict.json` written per run, never
overwritten; complete timings (screen u / fit / card / marginal, register, readers, bundle, git) in `wave-result.json`
and `scoreboard --timings`; K-P9-11 keys (`source_sample_end`, `predicted_mechanism`, `data_class`) in `wave_queue.py`.
**Files in scope:** `scripts/run_bounded_research.py`, `scripts/research_tree.py`, `scripts/wave_*.py`,
`scripts/research_wave.py`, `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`, `atx-engine/tools/stage_chain.py`,
their tests; task 0 only: `research_cycle.py` (the lines named), `backtest_integrity.py` (the lock),
`scripts/tests/test_research_spec.py`. **Forbidden:** any `scripts/specs/**` file, any C++.
**Tests (yours):** `test_runner_max_refuses_720` (manifest, spec, wave step), `test_budget_prefix_list_counts_v8ys`,
`test_capacity_missing_is_not_done`, `test_exe_sha_in_phase_rows`, `test_ledger_append_lock_excl`,
`test_spec_kind_null_pins`, `test_resume_refuses_argv_mismatch`, `test_attempt_subdir_after_floor_kill` (a planted
floor kill on fakes resumes to completion), `test_launch_waits_for_free_memory`, `test_receipt_digest_time_free`,
`test_timings_complete`. All of `scripts/tests` under `PYTHONHASHSEED=0` and `=1`.
**Root verifies:** `wave plan y-s.json` after the amendment; a tiny-world wave end to end; the next real wave's receipts.
**Out of scope:** splitting `research_cycle.py` (E2), any rule or NAV logic, the gm rule (C2).

