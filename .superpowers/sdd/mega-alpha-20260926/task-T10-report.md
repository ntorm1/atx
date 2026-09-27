# Task T10 report: NAV replay financing model `swap-fin-v1`

- **Lane:** pool-5, branch `feat/mega-alpha-nav-t4-20260927`, built on T4 `6a38fe39`.
- **Commit:** `9c279411` ("feat(strategy): swap-fin-v1 financing, borrow tiers, locate rule in NAV replay [mega T10]").
- **Build status:** not compiled or run in the lane (lane contract). The root builds it.
- **Data:** no real data was read or computed.

## Files

| File | Change |
|---|---|
| `atx-impl/src/strategy_nav_replay.hpp` | Adds `NavFinancingRule`, `NavFinancing`, `nav_financing_scenarios()`, `nav_scenario_matrix(bool)`, `NavFinancingFields` (on `NavReplayInput`), `NavBorrowTiers` and `nav_borrow_tiers()`, `NavFieldsPin`, and a 4-argument `run_nav_replay(cfg, limits, fields, progress)`. `NavScenario::annual_borrow_bps` is replaced by `NavScenario::financing`. It also adds day, summary and financing fields. |
| `atx-impl/src/strategy_nav_replay.cpp` | Accrual, tiers, locate rule, fields loader and pinning, matrix, reporting and CLI. |
| `atx-impl/tests/strategy_nav_replay_test.cpp` | Legacy helper `flat()` now sets `financing.flat_short_bps` (fixture expectations unchanged). Fixture 9 checks `financing` instead of `annual_borrow_bps`. Adds 7 T10 tests and their helpers. |
| `atx-impl/tools/equity_strategy_targets.cpp` | Comment only. The `nav` flags live in `dispatch_nav_replay`. |

## Root rulings applied (they override the brief where the two differ)

- **Market cap** is `shares_out × raw_close` **only**. `mktcap_lagged` is not loaded and not used; the brief's `mktcap_lagged`-first fallback is dropped. Only `shares_out` and `si_shares` are loaded.
- **`shares_out` domain:** a value outside `[1e5, 5e10]` (or NaN) counts as missing, giving warm, counted. The same rule applies to the SI ratio `si_shares / shares_out`. `si_shares` that is non-finite or negative is also missing.
- **IPO age** is calendar days since the name's first present role session (role row 0, not the score window start). A name present at row 0 is seasoned; it gets `DBL_MAX` days, which is finite because the engine rejects non-finite ages.
- **`--fields` takes the manifest.json path**, not a directory, plus `--fields-sha256`. The schema is `atx.research-role-fields/v1` as the fixed producer (`c099cade`) writes it.
  - The manifest's `role.manifest_sha256` must equal `--role-sha256`.
  - `role.sessions_sha256`, `role.ids_sha256` and `role.member_sha256` must equal the pinned role manifest's own `files[...].sha256` receipts, as the brief also requires.
  - Every used field must have `point_in_time: true`. A field set to `false`, or missing the key, is refused. An unused non-PIT entry such as `mktcap_lagged` is tolerated.
  - All of this is refused before the blend load and before any output.
- **Tiers** come from engine `estimate_borrow_tier`.
  - A missing predictor maps to warm (counted) before the engine is called. An engine `Unavailable` also maps to warm and missing.
  - An engine `InvalidArgument` on predictors that passed the domain checks is returned as an error, never turned silently into a tier.
- **Engine clocks:**
  - `available_at_ns = session + 22h`: the fields' visibility mark, which is also the role close mark.
  - `decision_time_ns = session + 23h`: the role's `clock_recipe modeled-session+22h-mark+23h-decision-v1`.
- **Accrual:** ACT/360 on calendar days between sessions for tiered books.
- **Overhead:** tiers are computed once per decision (every decision session, whether or not it is a rebalance) and shared by every lockstep book. Tiers depend only on the rate-independent flag count; each book maps a tier to its own fee.

## Key logic (line numbers at 9c279411, `strategy_nav_replay.cpp`)

- **`valid_financing` (:185).** Each rule admits only its own parameters.
  - FlatShortV0: `flat_short_bps` in [0, 1e5]; spreads, tiers and block must be zero or false; `day_count` 365.
  - TieredSwapV1: `flat_short_bps` 0; spreads in [0, 1e4]; `gc <= warm <= special <= 1e5`; `day_count` 360 or 365.
- **`accrue_financing` (:407).** Charges pre-mark dollars.
  - FlatShortV0 keeps the legacy expression exactly: `factor = borrow_rate * calendar_days / 365.0`, `short += -h * factor` summed in name order.
  - TieredSwapV1: `long += h * (long_rate * days / basis)` and `short += -h * (short_rate[tier] * days / basis)`, where `short_rate[tier] = (short_spread + tier fee) * 1e-4`.
  - When tiers exist, the short leg is split by tier (GC/warm/special) for any rule, and missing-predictor shorts are counted.
- **`mark_session` (:441).**
  - Returns: `nav_pre = nav_post + pnl + writeoff - short - long`, and `r = gross - trade_cost - borrow - long_financing`, checked to 1e-9.
  - `borrow_*` remains the whole short leg; `long_financing_*` is new.
  - Cash is debited for both legs.
  - A flat book's long leg is exactly +0.0, so `x - 0.0` leaves every legacy value bit-identical.
- **`borrow_predictors` (:523) and `classify_borrow` (:546).**
  - Presence rows up to d are folded monotonically into first-present rows, starting from role row 0; each row is scanned once per replay.
  - Only row d's fields and prices are read.
  - Output: per-name tier and missing flag, plus a member census.
  - The shared state is `BorrowTiers` (:145). It is inactive (empty) without fields.
- **Locate rule.**
  - `block_special_plan` (:576), applied after `update_weights` (rule, band, partial move, budget): for special names, `planned = max(planned, min(current, 0))`. Blocked dollars are `(floor - planned) * nav_post`, and names are counted.
  - `clamp_kept_order` (:589): on a non-rebalance day, a special name's kept working order is clamped to `min(held, 0)` and deactivated when nothing is left to trade.
  - `plan_decision` (:603) wires both in.
  - Planned turnover, gross/net and the v2 budget stay the rule's plan before the block; blocked dollars are reported separately.
- **`run_books` (:678).**
  - MARK at t uses the shared tiers of decision t−1, or of the latest earlier decision on the final sessions.
  - At each decision (`t + 2 < end`), tiers are classified once before the shared construction. The census is copied to every book's day.
- **Loader.**
  - `pinned_document` (:1243) reads the whole file under a size cap, checks it against the SHA pin, then parses it.
  - `read_field` (:1259) checks the exact byte extent and the SHA-256 of the payload.
  - `used_field` (:1279) requires exactly one entry, PIT true, `file == <name>.f64` (never a path), `<f8`, date-major, shape `[dates, instruments]` of the pinned role, and a files receipt equal to the entry SHA with the right byte count.
  - `load_fields` (:1309) charges `2 × 8 B/cell` against the budget left after the NAV reserve, before the blend loader is charged.
- **Scenario sets.**
  - `nav_financing_scenarios` (:1352) returns swap-fin-v1, flat-300-v0 and engine-tiers-v1. engine-tiers-v1 uses literal 27.5/300/2750 bps; a test asserts these equal `BorrowTierRecipe{}` × 1e4 exactly (`0.00275*1e4 == 27.5` etc.).
  - `nav_scenario_matrix` (:1381) returns `[S1, S2, S3] × swap-fin-v1`, then `S2 × flat-300-v0` and `S2 × engine-tiers-v1`. The primary index stays 1 (S2 × swap-fin-v1). Without fields it returns `fixed_nav_scenarios()`, which is S1/S2/S3 × flat-300-v0.
- **Diagnostics API.** `nav_borrow_tiers(in, d)` (:1449) returns exactly the tiers the replay forms at d.
- **Publication and CLI.**
  - `publish_scenario` (:1594), `publish_nav` (:1625), `run_nav_replay` 4-argument (:1657).
  - `dispatch_nav_replay` adds `--fields` / `--fields-sha256`; giving only one of them is refused with exit code 2.

## Outputs

**Books and labels.**
- Without `--fields`, the labels are the trading ids, so the file names are the legacy ones (`daily_<trading>.csv`).
- With `--fields`, the label is `<trading>+<financing>`, for example `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv`.
- Summary and recipe `primary_scenario` hold that label.

**recipe.json.** Every run adds:
- per scenario: `trading_scenario`, `financing_scenario` and a `financing` spec object, which replaces `annual_borrow_bps` / `borrow_day_count`;
- top level: `financing` (declaration), `primary_financing_available`, and `financing_fields` (null without fields).

With fields it also adds:
- `financing_fields`: schema, `manifest_sha256`, `role_manifest_sha256`, and `fields_used` (per field: sha256, `point_in_time`, and the producer's `clock` and `staleness` strings);
- `fields_not_used`: mktcap_lagged;
- `borrow_tiers`: a declaration.

`cost_input_status` is `declared-unfitted-scenario-no-locate` without fields (unchanged) and `declared-unfitted-scenario-modeled-locate-rule` with them. The recipe hash therefore changes once for legacy runs.

**summary.json.**
- Top level: `primary_financing_available` (false without fields, as the brief requires) and `financing_fields`.
- Per scenario: `trading_scenario`, `financing_scenario`, and a `financing` block containing:
  - the spec and `tiers_available`;
  - long, short and total financing dollars, and short financing by tier;
  - summed long and short financing returns;
  - `short_dollar_share_by_tier`: mean and p95 of each tier's share of pre-mark short dollars over return rows that have shorts; null without tiers;
  - `missing_predictor_short_name_days` and `missing_predictor_member_decisions`;
  - `member_decisions_by_tier`;
  - `blocked_short_dollars` and `blocked_short_name_decisions`;
  - `net_exposure`: mean net, mean |net| and max |net| leverage over the previous session's closing book.

**daily CSV.**
- Column order: T2 columns, then the GMV columns, then construction columns (only when active), then **financing columns only with fields**: `long_financing_dollars`, `long_financing_return`, `short_{gc,warm,special}_dollars`, `short_financing_{gc,warm,special}_dollars`, `missing_predictor_shorts`, `missing_predictor_short_dollars`, `blocked_short_names`, `blocked_short_dollars`, `member_{gc,warm,special}`, `member_missing_predictors`.
- Legacy-mode CSVs are byte-for-byte the T4 layout.

**Progress line.** It now also prints total financing $, short financing $ and blocked short $. No test reads it.

## Regression: flat-300-v0 is bit-identical

- **Accrual:** the per-name expression and summation order are unchanged, and the zero long leg subtracts exactly.
- **Identity check:** the identity only gains an exact `- 0.0` term.
- **Admission:** `nav_reserve_bytes(…, books = 3, tiers = false)` equals the old 3-scenario value.
- **Existing fixtures:** all T2 and T4 fixtures are unchanged except for two spelling-only edits:
  - the `flat()` helper now writes `financing.flat_short_bps` (plus an id);
  - fixture 9 now asserts `financing.{id, rule, flat_short_bps, day_count}` in place of `annual_borrow_bps == 300`.

## Fixtures (`StrategyNavReplay.*`, 7 new; 20 total in the file)

1. **`FinancingScenariosAndMatrixCarryTheDeclaredNumbers`:**
   - every swap-fin-v1, flat-300-v0 and engine-tiers-v1 number, with engine-tiers-v1 equal to `BorrowTierRecipe{}` × 1e4;
   - the legacy matrix and the 5-book matrix order and ids;
   - primary = S2 × swap-fin-v1.
2. **(a) `FlatLegacyFinancingIsBitIdenticalInsideTheMatrix`.** Random panel and random fields (mixed tiers), under the default rule and a cadence-1 rule.
   - S2 × flat-300-v0 inside the 5-book matrix is bit-identical (days, construction, events, participation) to the legacy S2 replay without fields.
   - Every legacy book with fields is bit-identical to the same book without fields, including `max_return_identity_error`.
   - Flat books show a zero long leg, no blocks, and by-tier short financing that sums to `borrow_dollars` (1e-9 relative).
   - S2 × swap-fin-v1 ends at a different NAV.
3. **(b) `SwapFinancingTwoNameWeekendHandComputedAct360`.** Fixture 2's Thu/Fri/Mon/Tue panel with GC fields and 6 bps fills.
   - Monday charges 3/360 on the pre-mark long 500 and short 500; Tuesday charges 1/360 on 550 and 550.
   - Checked: NAV path, returns, cash ratio, identity, by-tier short $ and financing.
   - Weekend = 3 × the one-day rate per dollar.
   - Summary totals, tier shares and summed long-financing return.
   - Contrast: flat-300-v0 pays `500 × 300e-4 × 3/365` with no long leg.
4. **(c) `BorrowTierMappingFlagsAndMissingPredictorIsWarm`.** 14 names at d = 5 via `nav_borrow_tiers`.
   - GC: zero flags; seasoned despite 7 days of history; cap exactly 1e9 (strict `<`); `shares_out` exactly 5e10.
   - Warm, one flag each: small cap from `shares_out × raw` (5e6 × 100); low price 4.99; SI exactly 0.10 (`>=`); young (first present at row 1 → 6 days); `shares_out` exactly 1e5 (valid, small).
   - Special: two flags; four flags.
   - Missing → warm: `si_shares` NaN at d only; `shares_out` 9.9e4; `shares_out` 6e10; absent at d.
   - Fields at d±1 are NaN for a GC name, proving only row d is read.
   - Row 0 tiers: the young name is missing there because it is absent.
   - Replay census at d is `{3, 8, 2}` with 3 missing members.
   - Refusals: a tiered scenario without fields (InvalidArgument), `nav_borrow_tiers` without fields, a mixed spec (`flat_short_bps` on a tiered rule), and warm > special.
5. **(d) `LocateBlockRefusesSpecialShortGrowthAllowsReduction`.** Cadence 1, fraction 1, zero trading cost, constant prices.
   - Special name 0 is never shorted; 375000 is blocked at d0.
   - The book is net +0.375: the block un-neutralizes.
   - GC name 1 turns special at decision 4. Its growth to −0.375 is blocked (expected dollars hand-computed from `current`). Its short stays bit-identical at sessions 5 and 6. It is charged special from MARK 5 at `(20 + 500) bps × 1/360`, and still charged special at MARK 7, the session it exits (reduction to 0 is allowed at decision 6).
   - Blocked names and dollars reconcile to the summary.
   - The same book without the block shorts both names: day 1 short is 500000 and day 5 is more than 3×.
6. **(e) `BorrowTiersAreAsOfFieldRewritesAfterDoNotChangePast`.** From row m, every field value is rewritten so every present name is special.
   - Rows before m are bit-identical, including all financing and tier fields and events.
   - Row m's MARK (pre-trade NAV, both legs, short dollars by tier) is bit-identical.
   - `nav_borrow_tiers` at d < m is equal; at m it differs and every present name is special.
   - The census at m is all special.
   - Pre-trade NAV at m+1 differs.
7. **(f) `FieldsPinnedToRolePublishFinancingMatrixElseRefused`.** Pinned end-to-end run, with the short name special.
   - Checked: the five labels; the primary; `primary_financing_available` true vs false in the legacy run; the fields binding in recipe and summary; the recipe SHA; CSV and events SHAs; headers with financing columns; blocked dollars and long financing in the primary.
   - **S2 × flat-300-v0 equals the legacy run's S2:** identical events SHA; every daily line equals the legacy line plus `,<financing columns>`; equal net_sharpe, final_nav, total_net_return, observations and borrow dollars.
   - The CLI `dispatch` with `--fields` / `--fields-sha256` succeeds; `--fields` without the SHA exits 2 and writes nothing.
   - **Refusals, each with no `out/` directory:** role manifest, sessions, ids or member SHA mismatch; `point_in_time` false; `point_in_time` key missing; wrong schema; wrong external SHA pin; a tampered payload byte.
   - A control with no edit runs.

## Decisions to confirm

1. **Planned turnover and the v2 budget are recorded before the locate block.** `planned_*` and `month_planned` show the rule's plan; blocked dollars are reported separately. The primary uses baseline at cadence 1, so this matters only for v2.
2. **Blocked dollars have two bases.** Blocks on the plan are in decision-NAV dollars. Clamps on kept working orders (non-rebalance days only) are in order dollars.
3. **A held name that is absent at d gets tier warm (missing), not its last tier.** Its raw price is NaN at d, and the ruling says missing means warm.
4. **Tier diagnostics are also reported for flat books when fields exist.** They are informational; the charge is unchanged.
5. **Financing CSV columns are appended only in the fields matrix.** This keeps legacy CSVs byte-stable.
6. **Summary: added keys only.** The recipe schema id is kept (`atx.dsl-nav-replay/v1`); only keys were added. The recipe hash changes once.

## Concerns

- **Not compiled.** I desk-checked the ATX_TRY forms, the nlohmann comparisons (string compares go through `get<std::string>()`) and aggregate inits. As before, partial aggregate init relies on default member initializers (`NavReplayInput{target, volume}`).
- **The T4 review (`task-T4-review.md`) has landed.** T10 builds on `6a38fe39`. A T4 fix round in the same files would need a rebase; the likely overlap is `plan_decision` and `run_books`.
- **Memory with `--fields`.** The run is 5 books plus 16 B/cell of fields. At 5 books the reserve is about 5/3 of T4's. Default 512 MiB is expected to fit TRAIN, but the root should watch admission on the real role.
- **Tier inputs are declared, not verified.** Tier fees are research priors and `si_shares / shares_out` understates SI versus float. Both are recorded in the recipe.

## Root build / test

- **Targets:** `atx-equity-strategy-targets,atx-impl-strategy-target-tests`
- **gtest filter:** `StrategyNavReplay.*:StrategyTargetReplay.*:StrategyPrice*`
  - Only the NAV file changed; the full set re-checks the shared seams.
