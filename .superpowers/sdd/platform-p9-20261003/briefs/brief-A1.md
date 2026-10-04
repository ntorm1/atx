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

## Lane A1: field registry and the freeze on Python builders

**Pool / branch:** 12, `feat/p9-a1-20261003`. **Effort:** M. **Serves:** infrastructure, significance (seal).
**Read:** `docs/plans/2026-10-02-p9-code-review-fields.md` (all), `docs/plans/2026-10-02-platform-core-migration.md`
§3-§4, audit §2 field rows, main review F-6, F-12.
**Deliver:** (1) K-P9-1 `atx-engine/tools/field_registry.json` + loader `field_registry.py`, generated once from today's
four registration mechanisms (inline dicts prep:194-278 / 411-440 / 480-504; `FIELD_MODULES` bind; the holdings wrap
prep:3186-3187; the engine shim engine.py:146-160), in today's manifest order, with declared `dtype`; a test that the
generated registry reproduces today's v15 field list and order on fixtures. (2) One entry `prepare_research_fields.py
--registry <json> --fields <list|all>` replacing the shim one-liners (ohlc:11-12); the four byte-identical shim
`register()` / `main()` bodies become thin deprecated wrappers over it; holdings onto `FIELD_MODULES` (reuse interface
holdings:1216-1245). (3) Freeze: `test_no_new_python_builder.py` fails on a `research_fields_*.py` /
`prepare_research_fields_*.py` outside a committed allowlist. (4) Reuse key and manifest record numpy, pyarrow,
duckdb and Python versions (FD-2); a test that every `PRODUCERS` closure's cross-module names are in `IMPORTS`. (5) Seal
(FD-5): remove the 2025 bind (`atx-engine/tools/conftest.py:15`) and regenerate the affected fixtures under the
repository window; `load_prior` (prep:2916-2931) and the field readers refuse `seal.exclusive_end` != the research
seal; rename `rows_available_on_or_after_2025_dropped` to `rows_sealed_dropped` (prep:948, 1689, 1754; the C++ key
`research_fields_cli.cpp:116` is A2's: declare the rename in the report). (6) Fix `engine.py:13`'s dead test reference.
**Files in scope:** `atx-engine/tools/prepare_research_fields*.py`, the registration blocks of `research_fields_*.py`
(no builder arithmetic), `code_fingerprint.py`, `atx-engine/tools/conftest.py`, new `field_registry.{py,json}`,
`atx-engine/tools/test_*` for these, `atx-engine/tests/fixtures/research_fields/` Python generator only.
**Forbidden:** any C++, `scripts/**`, builder arithmetic.
**Root verifies:** pytest `atx-engine/tools`; fields v15 rebuilt through the entry with `--reuse` into a new dir: 84
payload SHA-256 equal to v15's; manifest bytes differ only in the documented keys (one re-pin ruling).
**Out of scope:** porting any builder to C++ (A2, A3, A4); new fields.

