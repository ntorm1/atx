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

## Lane A3: one shared vendor panel in C++, price and ohlc builder kinds

**Pool / branch:** 12, `feat/p9-a3-20261003`. **Effort:** L. **Serves:** infrastructure (fields build time).
**Read:** fields review §3 (look-ahead rules per source, seal readers that decode sealed rows), §4 (5 TickerHistory3
readers), §6 (7 SHA passes, ~10 scans), FD-3, FD-5; migration §4 slice 4; A2's registry and K-P9-2;
`research_fields_price.py`, `research_fields_ohlc.py`, `prepare_research_fields.py:1019-1135` (th group,
factor_breaks), `repair_role_factor_breaks.py`.
**Deliver:** `research/fields/sources/vendor_panel.{hpp,cpp}` (vcpkg arrow / parquet): hash the file once, one scan over
the union of requested columns, the `tradingDate < seal` filter pushed down (sealed rows never decoded), the
observation contract of the price module applied once; factor-break-v1 in C++ once per run (one implementation for the
two Python copies, audit §3 row 2); builder kinds for the price-module and ohlc fields on it; registry rows `kind:
engine` for each moved field (added to A1's JSON). Gtests: `VendorPanel.HashOnce`, `.SealPushDown` (a planted 2024 row
is never decoded), `FactorBreak.ClosedForm`, one planted-leak probe with teeth per moved builder, fixture byte identity
vs the Python builders.
**Files in scope:** `atx-engine/{include/atx/engine,src}/research/fields/sources/**`, new builder files under
`research/fields/`, their tests and fixtures, rows in `field_registry.json`. **Forbidden:** the Python builders (they
are deleted one slice after root's identity).
**Root verifies:** fields targets build; per moved field the TRAIN payload SHA-256 equals v15's; fields build wall
before / after.
**Out of scope:** SEC / holdings (A4).

