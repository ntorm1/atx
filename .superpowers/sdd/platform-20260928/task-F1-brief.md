# Task F1: fixes from the R1 adversarial review (I-1, M-5, M-6, M-7 + the m findings in those files)

**Pool:** C:/atx-wt/pool-7. `git status` clean, then `git checkout -B feat/platform-v7-f1-r1fixes-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build, never run binaries or real data, never spawn subagents, never
touch C:/atx, never read validation/VAL/2023/2024/2025 files. Read .agents/cpp/agent.md first. Post-implementation gtests /
pytests. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-F1-report.md (<= 30 lines) in the
pool-2 sprint dir; reply < 10 lines. Do NOT touch strategy_live.{cpp,hpp} or tools/equity_strategy_targets.cpp (lane W4
owns them; M-1..M-3 go there) and keep edits to strategy_nav_v7.cpp confined to the capacity pass (lane W1 registers a
new rule in that file; a rebase conflict there is root's problem only if you edit outside the capacity code).

**Read first:** task-R1-review.md in full (rows I-1, M-5, M-6, M-7 and every m row whose file you touch), the L4 report
and brief for intent, literature-v7.md S3 R3.3 (bias harness) and S4 R4.2 (capacity by replay).

**Fixes**
1. I-1 (strategy_nav_v7.cpp:38,281-287,513): under `--capacity-curve`, price each capacity book's v6 cost c_i with that
   book's scaled S2 law (NAV x m), so c_i/c_bar, band, theta_t and targets are the NAV-m book's; or refuse the combination
   with a clear message if the pass structure cannot support it. Add a gtest: a v6 capacity pass at m = 4 differs from the
   base-scale pricing, and m = 1 stays bit-identical to the main pass.
2. M-5 (strategy_risk_model.cpp:781-797,829-834): the bias harness must not silently drop factors without a forecast
   (< 63 obs) from x'Fx nor uncovered names from realised returns. Either exclude the whole observation and count it, or
   carry a structural forecast; write counts (`dropped_factor_exposures`, `uncovered_name_returns`) into bias_summary.json
   and refuse when the dropped share exceeds a declared threshold (5%). Test with a planted short-history factor.
3. M-6 (prepare_research_fields.py:494,2436,2580): `--reuse` must key on the builder code identity too (code_sha256 of the
   field's producing functions or of the module, plus spec text), not on a hand-bumped FORMULA_REVISION alone; the manifest
   must record the reused payload's original code sha and never claim it under a new code sha. Test: change the module
   text -> no reuse.
4. M-7 (mega_report/pitch.py:173-196): the cache-scan fallback filters entries by the library's per-candidate DSL sha
   (recompute from the library JSON) and reports ambiguity otherwise. Test with two v2 entries for one id.
5. The m rows in task-R1-review.md that fall in these four files: fix or state why not, one line each in the report.

**Root acceptance:** gtests + pytests green; the v6.1 cell's v5 outputs and S1/S2/S3 byte-identical (identity on real
data); risk verb re-run on lo1 prints the new counts; `--reuse` still reuses 40 from v6b when the code is unchanged.
