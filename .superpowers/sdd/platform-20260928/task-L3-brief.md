# Task L3: daily decide path (holdings emit, `decide` verb, deploy manifest) -- TRAIN-only, no seal change

**Pool:** C:/atx-wt/pool-4. `git status` must be clean, then `git checkout -B feat/platform-v7-l3-decide-20260928 <BASE>`
where BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Work only in pool-4. Never build C++ (root builds), never run the
pipeline or any real data, never spawn subagents, never touch C:/atx. You may read C:/atx-wt/pool-2/build-equity/** by
absolute path (TRAIN outputs only; never anything named validation/VAL/2023/2024/2025). Commit trailer:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report: C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/
task-L3-report.md (<= 40 lines). Reply in chat < 15 lines. Read .agents/cpp/agent.md before touching C++. No TDD ceremony:
implement, then post-implementation gtests.

**Read first:** code-review-v7.md findings B1, B2, B3, B6, B9 and S4 Lane 3; plan-v7.md S2 (you own strategy_nav_replay.cpp;
lane L4 will add one construction-rule dispatch hook and rebase on you, so keep the rule dispatch site small and stable);
literature-v7.md S7 (what production monitors: implementation shortfall, transfer coefficient R2.5). Code:
atx-impl/src/strategy_nav_replay.cpp (decision loop ~896-960, last decision end-3 at ~908, emit at ~845-866 and ~1436-1511,
no_short mask ~937-942), strategy_target_replay.cpp:222-282 (aim-partial path dependence via current[i]), 366-372 (mask),
strategy_data.cpp:21 (kSeal -- DO NOT CHANGE), atx-impl/tools/equity_strategy_targets.cpp (verbs), the NAV gtests.

**Deliverables**
1. `--emit-holdings <dir>`: per decision date, one row per name with target weight, planned trade, filled trade, capped/
   unfilled/blocked flags, end-of-day holding (weight and dollars), side, borrow tier; plus a per-date summary. Flag off
   -> every existing output byte-identical (prove in a gtest with a synthetic replay: run twice, compare bytes).
2. `atx-equity-strategy-targets decide --deploy <manifest.json> --asof <session> --positions <csv> [--locates <csv>]
   --output <dir>`: computes today's target weights and the order deltas from the ACTUAL positions given, using exactly the
   same construction code path as the replay (call the same detail:: functions; do not fork the logic). Inputs are a
   role/fields/combined set already built for the as-of session (TRAIN snapshots for now). Output: targets CSV, orders CSV
   (weight delta and dollars; shares rounding is out of scope), a decision summary JSON with pins and the transfer
   coefficient corr(alpha_i/sigma_i^2, w_i) as a diagnostic.
3. `atx.book-deploy/v1` manifest schema (JSON): library, recipe, orientations, composition weights, fields list/recipe,
   universe rule, NAV rule + flags, L, exe SHAs, source SHA, seal policy. `decide` refuses a manifest with a missing or
   mismatched pin (tests for each refusal). A session past the seal is refused unless the manifest carries an
   `owner_gate` record -- and even then this lane does NOT change kSeal; add the refusal test only.
4. Health checks in the decision summary (B9): gross/net leverage vs band, turnover vs band, neutralisation-skip flag
   surfaced as an ERROR (today it silently skips the rebalance), count of names without locate.
5. Tests (gtest, synthetic): identity with flag off; decide at three dates of a synthetic replay reproduces the replay's
   planned weights bit-for-bit when fed the replay's emitted holdings of the prior date; manifest refusals; health flags.

**Root acceptance:** flag off -> v6.1 NAV cell outputs byte-identical; at 3 pinned TRAIN decision dates, decide from the
emitted holdings equals the replay's planned weights bit-for-bit; refusal tests pass; no seal change.

Report: changes with file:line, the holdings/orders/manifest schemas, the exact root command lines, and risks.
