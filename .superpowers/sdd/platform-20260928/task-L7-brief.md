# Task L7: library v7.0 (v6.1 + wave 1), fields v8, research-cycle spec (no runs)

**Pool:** C:/atx-wt/pool-10. `git status` clean, then `git checkout -B feat/platform-v7-l7-libv70-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build C++, never run IC/NAV/fit or any returns-conditioned
statistic (the builder on synthetic data is fine; `research_cycle.py plan` dry-run is fine), never spawn subagents, never
touch C:/atx (read-only for docs), never read validation/VAL/2023/2024/2025 statistics. Python "C:/Program Files/
Python312/python.exe". Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-L7-report.md (<= 30
lines) in the pool-2 sprint dir; reply < 10 lines.

**Binding spec:** v7-prereg.md section "Library v7.0 = library v6.1 + wave 1" and library-v7-draft.md §0-§1 at f43e54d9
(the five candidates, their DSL, prior signs, themes, tiers, citations). Implement literally; where the draft's DSL uses a
W2 op, check the exact spelling/signature in atx-engine/src/alpha/registry.cpp and task-W2-report.md and fix the DSL to
match (report each change). Verify the q5_eg slopes against Hou-Mo-Xue-Zhang, Review of Finance 2021 (web allowed);
if they differ from the draft, use the RoF values and list both in the report.

**Deliverables**
1. `atx-impl/strategies/generate_fund_ic_v70.py` -> `fund_industry_ic_v70.json` + `.recipe.json`: library v6.1 entries
   byte-identical and in the same order (test), the five wave-1 members appended with their theme/tier/prior/citation,
   roster cap 56, deterministic canonical JSON, sha256s recorded. Extend `check_fund_ic_v6.py` (or its v7 sibling) so it
   validates v7.0 against a fields manifest containing the v8 fields and the W2 ops (vec_sum via POLICY_OPS); add
   `ownership_flow` to the fitter's theme list (fit_composition_weights.py) with a test that an empty theme is harmless
   and that v6.1 weights are byte-identical.
2. Fields v8: register the six q5_eg fields (as defined in the draft; PIT rules as the existing fundamentals fields, same
   lag convention) in prepare_research_fields.py or a small module `research_fields_q5.py` hooked below the W5 hooks (do
   not edit the W5 modules); manifest coverage recording; byte-identity of the other fields when not requested (test).
3. `scripts/specs/v70.json` for research_cycle.py: fields v8 = `--reuse-fields` from lo1-fields-v7 (only the six new
   computed), u pass with library v7.0 on a new cache `mega-candidate-cache-v70` seeded by `--cache-legacy-fields` from
   lo1-fields-v7 so the 39 v6.1 candidates hit, fit ew-theme-v1 / v4-prior-v1, gate (print the five wave-1 admission
   rows; stop before w if none admitted), w, nav with the v6.1 final construction and L 1.247 fixed (output
   `mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`), summ paired vs the v6.1 cell with --dsr-n 34, card and
   monitor phases as in v61-ops.json. `research_cycle.py plan specs/v70.json` must print every line with pins resolved.
4. Tests: generator determinism + v6.1 identity; checker accepts v7.0 and refuses an unknown op; q5 field formulas on a
   synthetic role; spec parse + plan dry-run; fitter theme-list identity.

**Root acceptance:** `plan` prints; fields v8 built with 41 reused + 6 computed; u pass hits 39 + evaluates 5; the rest
per the pre-registration. Report: DSL as implemented (all five), slope values with sources, files, test counts, the exact
root command lines, expected u-pass memory (the six new fields add resident bytes).
