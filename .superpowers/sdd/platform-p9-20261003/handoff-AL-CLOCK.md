# Handoff AL-CLOCK (lane AL-CLOCK, pool-8, branch `feat/p9-alclock-20261004`)

Stopped by the OWNER STOP relayed by the PM (R6-3), before any code was written. Contract: `resume4-dispatch.md`,
`resume6-addendum.md`, `r6-wave3-common.md`, `briefs/brief-AL-CLOCK.md`, `wave3-carry.md` (AL-CLOCK, Q9 / W3-9,
Q16 / W3-16), plan section 2.4 :601-605, the dispatch prompt (items 1-5, "For PRE").

## State

- Lease base `0dbfa459` (C3 part A APPROVE head). Root head `5c57c1ef` merged: merge commit `ccc96e89` (clean, no
  conflict). Root has since moved to `438f0481`: NOT merged (merge it first on resume).
- Head = `ccc96e89` + this handoff commit. Tree clean. No code change, no build, no test, no build tree (nothing to
  delete). No permission refusal.

## Required items

| item | status |
|---|---|
| 1 rule row `info-clock-rate-v1` + one parser entry (append-only, W3-9) | not started (design below) |
| 2 flag-absent identity | not started |
| 3 tests (kernel at the 21 boundary, missing field, cadence != 1, `incompatible[]` refusals, 1-vs-4 `--book-workers`, E2 `rule_flags` preflight on fakes) | not started |
| 4 summary keys (fast-rate share of name-days, turnover share, gross) | not started (design below) |
| 5 registration + template | not started; `source_sample_end` found (below) |

## What was established (read-only; use it)

**source_sample_end = 2019.** Bowles, Reed, Ringgenberg, Thornock, "Anomaly Time", JF 79(5) 2024 (lit [29]), the
"Journal of Finance forthcoming" text dated 2023-11-06 (`https://www.boonebowles.com/assets/papers/anomaly-time.pdf`,
read 2026-10-04), p. 14: "Our sample period begins January 1990 and goes through 2019." (also p. 48 Table I "covers
1990 through 2019", p. 78). The 2019 working paper (Wharton Jacobs Levy copy) used 1997-2017: the published sample is
the one to register. So the rule is FLAGGED under PRE-19 / PRE-31 (end >= 2015): it is excluded from the clean-source
book. [29] measures from the accounting "information release" date (Compustat Snapshot); the house form maps that to
the latest primary earnings announcement's reaction session (`ea_days_since`): state it as a fixed choice.

**The field.** `ea_days_since` (`atx-engine/tools/research_fields_sec.py:160`, `:898`): sessions since the latest
visible primary EA's reaction session, >= 0, NaN when no fresh announcement. It is in the P9 parents' fields manifest
(`scripts/specs/v8/base-lo3.json:116`, fields v9; Y-F0 `lib-v8ysb-gm.json` NAV argv is aim-partial-v5, cadence 1,
`--trade-fraction .05`, delta, exit .05, locate, liquidity-cache, warm start 60, `--capacity-curve` = v7 seam). The
NAV exe loads fields only through `book/nav_load.cpp` `load_fields` (`:120-161`, `load_field` `:105-115`).

**Where the rate enters.** `detail::update_weights(..., per_name_rate)` -> `aim_partial_weights`
(`strategy_target_replay.cpp:345-399`) already takes one rate per name (`check_rates` `:403-413`: every entry in
[0, 1], so a nonmember needs a valid number, e.g. theta_s). Rates reach it from `plan_decision`
(`book/nav_decide.cpp:241-242`) through `plan_weights` -> `v7::plan` / `v7::plan_book` -> `plan_rule`
(`strategy_nav_v7.cpp:641`, `:685-688`, `:741-742`); aim-partial-v6 / spo refuse a non-empty span at run time
(`:664-667`); the v7 parse refuses a non-fixed `--rate` with `--two-speed` and spo (`:1218-1221`, `:1292`).

**Design (planned, not coded).**
- `NavRateRule::InfoClockV1 = 2` (`strategy_nav_replay.hpp:120`); `NavReplayInput` gains a trailing
  `std::span<const f64> info_clock{}` (date-major `ea_days_since`; aggregate inits stay valid, cf. `nav_deploy.cpp:38`).
- Rate per member at decision d: `0 <= v <= 21` -> `eb::two_speed_fast_theta(cadence)` (W3-16), NaN -> slow, else
  (negative / inf) refused as outside the field's domain; slow = `eb::two_speed_slow_theta` and validate_nav_config
  refuses `trade_fraction != .05` (as two-speed, `strategy_target_replay.cpp:175-178`), non-default `rate_*`, and
  any rule but aim-partial-v5. The vector is book-independent: form it ONCE per decision on the calling thread in
  `run_books` (`book/nav_session.cpp:338-359`), so `--book-workers > 1` stays legal (adjust
  `nav_lockstep.cpp:202-206`, which refuses only PerNameV1); a grid must keep the base cadence (extend
  `nav_lockstep.cpp:193-198`).
- Stats per book (book-owned, in `plan_decision` after `plan_weights`, scored rebalance decisions, members only):
  name_days, fast_name_days, missing_name_days, planned turnover sum |planned - current| and its part on fast
  name-days. Publish beside `rate_stats` in `nav_publish.cpp:85-91` as `construction.v5.info_clock {rule, cadence,
  theta_fast, theta_slow, window_sessions 21, name_days, fast_name_days, missing_name_days, fast_share,
  turnover_share}`; recipe `rate` + rule keys in `nav_write.cpp:218-221`; `ea_days_since` into the recipe's
  `fields_used` (rule input). Gross: C3-9's `ann_mean_gross` (`--ann-mean-gross`, both sides alike).
- "Turnover share up" (plan 5.3 :946, prereg B.4 / F) compares P9-F0h with Y-F0, so the reference needs the same
  print: a print-only opt-in (e.g. `--info-clock-print`, NavSpec key) that loads the field and prints the block at
  unchanged rates, recipe unchanged (field SHA in the block, not in `fields_used`), as PRE-7a did. Decide on resume.
- Row (append to `nav_rules.cpp:57-71`, size-deduced table): `{"info-clock-rate-v1", TradeRate, "--rate
  info-clock-rate-v1", no_params, none, {per-name-v1, two-speed-v1, aim-partial-v6, spo-v1, spo-v2, spo-v3}, ...}`.
  Parser: `nav_cli.cpp:191-194` (`--rate` value), help `:59-60`; NavSpec key `rate` is free text
  (`nav_spec.cpp:77`). `--rate` requires `--fields` (load in `admit_and_load`, `nav_load.cpp:214-219`). The decide
  path already refuses any non-fixed rate (`nav_deploy.cpp:87`).
- C3-4: the rule changes only the per-name rate, not L; realised gross stays bounded by L (state it in the
  registration). DEC-16 argument: Y-5 buckets THEMES (half-life table) into two sleeves and nets them; this rule has
  one aim and buckets NAME-DAYS by an event clock (per-name rate vector), no sleeve, no netting; R-3 is a per-signal
  aim gain.

## Next step for a fresh agent

1. `git merge feat/platform-p9-20261003` (root `438f0481` or later) in pool-8.
2. Implement the design above (C++ in the files named; one row + one parser entry append-only), then the tests of
   item 3 in `atx-impl/tests/strategy_nav_spec_test.cpp` / `strategy_nav_replay_test.cpp`, the E2 preflight pytest on
   fakes, then registration `scripts/specs/p9/rules/info-clock-rate-v1.json` and template
   `scripts/specs/p9/templates/info-clock.json` in AL-COMB's shape (`mom-volman-v1.json`, `mom-volman.json`).
3. Builds: `equity-dev` / `equity-rel`, tags `p9-alclock-a`, `-b`, ... (none used yet); canary goldens both presets.

## Notes

- Anchoring disclosure: while locating Y-F0's spec, two public ledger lines with Y-5 / X-10 verdict numbers
  (`progress.md` :147, :156) were shown by a grep; no constant or prediction is taken from them.
- PDFs fetched by WebFetch sit in the session's tool-results dir (not the lane's files).
