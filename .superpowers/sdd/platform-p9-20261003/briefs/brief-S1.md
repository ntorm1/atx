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

## Lane S1: marginal verb speed and the Release IC exe

**Pool / branch:** 18, `feat/p9-s1-20261003`. **Effort:** M. **Serves:** infrastructure (wave wall time).
**Read:** composition review §1 "Marginal verb", §2 C++-internal duplicates, §4 "Marginal rows for book members", §5,
CM-1, CM-6; DS review §3 "Cache-key completeness" and "Debug vs Release", §7 item 1; v8y loop §4 design notes;
`strategy_marginal_ic.cpp`, `marginal_rank_ic.{hpp,cpp}`, `orthogonalize.cpp`, `strategy_ic_signal_cache.cpp:41-151`,
`strategy_ic_result_cache.cpp:29-48, 114-124`.
**Deliver:** `--candidates ID,...` (residualise only the listed ids; pairwise rho only for pairs with a listed id);
rows compacted to the role's decision members; a pair cache keyed by (role SHA, payload_a, payload_b, min_names,
method version), reusable across waves on a role; no re-hash of payloads the u pass verified (accept verified
digests); theme-regressor cap 10 -> 33 (`marginal_rank_ic.hpp:45`); DetPool date bands with lane-owned writes; timers
(hash / kernel / pairwise) in the output; the engine `research_return_guard` reused instead of `:359-407`; the copied
helpers `:52-92` replaced by the shared ones; the member-row bias fix (composite excluding the member) only behind
`--exclude-self`; a token for build type, NDEBUG, CRT flavour and xsimd version in `vm_identity` / `ic_identity`
(`ic_identity` from `ic_screen.cpp`'s TU); the Release equity preset target for `atx-equity-strategy-ic`
(`CMakePresets.json`). Gtests: `MarginalIc.CandidatesSubsetEqualsFullRows`, `.CompactedRowsEqual`,
`.PairCacheHitEqualsCompute`, `.ThirtyThreeThemes`, `.BandsByteIdenticalAt1And4`, `IcIdentity.BuildTokenSeparatesCaches`.
**Files in scope:** plan §2.2 row S1. **Forbidden:** `research_cycle.py` (add `--candidates` to `MARGINAL_SPEC_FLAGS`
is a one-line cross-lane edit you list; E2 owns the file in wave 2).
**Root verifies:** builds Debug and Release `atx-equity-strategy-ic` and `atx-impl-strategy-ic-tests`; the alpha
oracle and conformance suites under Release (NDEBUG compiles out `ATX_ASSERT`); flag-absent marginal rows
byte-identical; Release u / w byte-identical to Debug on X-5; marginal wall per pass logged (target <= 30 s).
**Out of scope:** VM kernels (S2), the gate (B1).

