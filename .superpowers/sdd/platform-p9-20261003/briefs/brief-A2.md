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

## Lane A2: C++ field registry, builder kinds and the fields exe

**Pool / branch:** 13, `feat/p9-a2-20261003`. **Effort:** L. **Serves:** infrastructure.
**Read:** fields review §5 and §7 (FD-1), migration §1, §3, §4 slice 3, the headers under
`atx-engine/include/atx/engine/research/fields/` and `atx-engine/src/research/fields/` (all), A1's K-P9-1 (plan §2.3;
code against the schema, A1 merges first).
**Deliver:** `research/fields/registry.{hpp,cpp}`: `BuilderKind {id, parse, build}` (K-P9-2); `vol_126`, `si_shares`,
`si_dtc` registered as kinds (replace the name dispatch `research_fields_cli.cpp:28, 124-133`); `atx-research-fields
build --registry R --spec S --receipt OUT` reading K-P9-1; publish-last manifest and the reuse decision in the exe
(`manifest.{hpp,cpp}`, `reuse.{hpp,cpp}`; mig slice 3, not yet implemented); producer identity (K-P9-3) in every engine
entry, reuse of an engine entry keyed on it, identity claim on payload + coverage only (FD-1); the sealed-rows stats key
renamed `rows_sealed_dropped` (with A1); seal refusal on a prior manifest whose seal differs. Gtests:
`ResearchFieldsRegistry.*` (kind lookup, unknown kind refused, parse round-trip), `ResearchFieldsManifest.*`
(publish-last, producer block, reuse hit / miss on exe identity), the existing fixture identity unchanged.
**Files in scope:** `atx-engine/include/atx/engine/research/fields/**`, `atx-engine/src/research/fields/**`, the fields
block of `atx-engine/CMakeLists.txt` (:266-282), `atx-engine/tests/research_fields/**`,
`atx-engine/tools/prepare_research_fields_engine.py` (becomes a registry `kind: engine` caller).
**Forbidden:** other `atx-engine/tools/*.py` (A1), `atx-engine/tests/CMakeLists.txt` CTest lines (T1).
**Root verifies:** builds `atx-engine-research-fields`, `atx-engine-research-fields-tests`, `atx-research-fields`;
`--gtest_filter=ResearchFields*`; fixture identity (8 x 300); the three engine fields on the TRAIN role equal v15's
payload SHA-256.
**Out of scope:** parquet sources (A3); new builders.

