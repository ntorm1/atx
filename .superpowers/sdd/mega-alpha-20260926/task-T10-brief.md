# Task T10 — NAV replay financing model `swap-fin-v1` (PB portfolio-swap financing + tiered borrow)

Owner: the T2/T4 implementer (pool-5), continuing on its branch **after T4 is integrated** (same files;
root gives base). Files: `atx-impl/src/strategy_nav_replay.hpp/.cpp`, `atx-impl/tools/equity_strategy_targets.cpp`
(nav CLI), `atx-impl/tests/strategy_nav_replay_test.cpp`. Uses (does not modify) the engine tier model
`atx-engine/include/atx/engine/cost/borrow_tiers.hpp` (`estimate_borrow_tier`, `BorrowTierRecipe`) and the
T6 role-fields format (`atx.research-role-fields/v1`, `<name>.f64` date-major role dates x ids; read
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T6-report.md`).

## Why (owner ruling 2026-09-27; handoff 2 §2b has sources)
The replay charges a flat 300 bps/yr on short $ and nothing on longs. On a PB portfolio swap the fund
pays benchmark + long spread on long notional and receives benchmark - short spread - borrow fee on short
notional; with NAV collateral earning ~benchmark and a dollar-neutral book, the benchmark cancels and the
excess-return cost is `s_L x long$ + sum_i (s_S + fee_i) x short$_i`. ~83% of US short interest is GC
(~30 bps) while hard-to-borrow names (~10% of names) cost several % to tens of % and are where anomaly
short legs earn. Flat 300 is too harsh on GC and too lenient on specials.

## Requirements
1. Financing spec per scenario (replace the single `annual_borrow_bps`):
   `FinancingRule { FlatShortV0, TieredSwapV1 }`, `long_spread_bps`, `short_spread_bps`, tier rates
   (GC/warm/special bps), `day_count` (360|365), `block_special_shorts` (bool).
   - `flat-300-v0` = FlatShortV0, 300 bps on all short $, long 0, D365, no block — must reproduce
     today's S1/S2/S3 numbers **bit-identically** (legacy fixtures unchanged).
   - `swap-fin-v1` (PRIMARY) = TieredSwapV1, long 40, short spread 20, tiers GC 30 / warm 100 /
     special 500 bps, D360, block on.
   - `engine-tiers-v1` (stress) = as swap-fin-v1 but tier rates = engine `BorrowTierRecipe` defaults
     (27.5 / 300 / 2750 bps).
2. Scenario matrix every invocation when `--fields` is given: S1/S2/S3 costs x `swap-fin-v1`, plus
   S2 x `flat-300-v0` and S2 x `engine-tiers-v1`. `primary_scenario` = S2 x swap-fin-v1. Without
   `--fields`: legacy S1/S2/S3 x flat-300-v0 only, summary `primary_financing_available: false`.
3. CLI: `nav --fields DIR --fields-sha256 SHA` (the fields `manifest.json` sha). Pin: manifest role pins
   must equal the loaded role (manifest/sessions/ids/member sha), else refuse before output. Load only
   `mktcap_lagged`, `shares_out`, `si_shares`. Record fields sha + spec in recipe.json.
4. Tier per (decision d, name i), computed once per decision, as-of d, via `estimate_borrow_tier` with
   the recipe rates above. Predictors:
   - market cap = `mktcap_lagged[d,i]` if finite, else `shares_out[d,i] * raw_close[d,i]`;
   - raw price = `raw_close[d,i]`;
   - SI ratio = `si_shares[d,i] / shares_out[d,i]` (NaN if either NaN or shares_out <= 0; declared:
     shares_out >= float understates the ratio);
   - IPO age = calendar days since the name's first `present` session in the role; names present at
     role session 0 are seasoned (+inf);
   - any missing predictor or an engine `Unavailable` result -> **warm**, counted.
   Read the engine contract for `available_at_ns` / decision time and satisfy it with the field clocks
   (fields are strict as-of the session).
5. Accrual at MARK t over (t-1, t]: `calendar_days / day_count` x [long$ x s_L + sum_{short i}
   short$_i x (s_S + fee_i)], fee from the tier of the latest decision <= t-1, on pre-mark $ (same
   basis as today). Update the identity to `r = gross - trade_cost - financing` (keep
   `borrow_return` as the short-leg total for compatibility; add `long_financing_return`).
6. Locate block (when on): at DECIDE, after neutralize/band, for special-tier names
   `next_i = max(next_i, min(cur_i, 0))` — cannot open or increase a short; may reduce/close it.
   Count blocked short $ per day. A short that migrates into special is charged special until exit.
7. Reporting per scenario (summary + daily CSV): financing $ by leg and tier; short-$ share by tier
   (mean, p95); blocked short $; missing-predictor short name-days; net exposure mean/max (block can
   un-neutralize).

## Constraints
- House style `.agents/cpp/agent.md`. Do NOT compile or run; root builds. Not TDD: implement, then
  fixtures: (a) flat-300-v0 legacy outputs bit-identical to existing fixtures; (b) two-name hand-computed
  NAV with long spread + GC short across a weekend under ACT/360; (c) tier mapping for 0/1/2+ flags,
  mktcap fallback, missing predictor -> warm; (d) locate block: special name cannot open/increase a
  short, can reduce, blocked $ counted; (e) as-of: rewriting field values after d leaves rows <= d
  bit-identical; (f) fields manifest role-pin mismatch refused before any output.
- No subagents. Commit (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
`C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T10-report.md`. Return only: status, commit
SHAs, one-line summary, concerns.
