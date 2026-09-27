# Task V6-C2 report: industry neutralisation ids, grp_ff12 plumbing, reserve at run geometry

**Status:** DONE_WITH_CONCERNS (I could not compile or run anything in this lane; see Concerns).
**Branch:** `feat/mega-alpha-v6-c2-20260927` in C:/atx-wt/pool-11. **Base:** 04e9d5bc. **Commit:** `720a0066`.
**Files:** 10 files, +991/-93 (7 src, 3 tests). No CMake change. I did not touch v5_train.sh, v51_train.sh or nav_summ.py.

## 1. Deliverables

| # | Deliverable | Status |
|---|---|---|
| 1 | `price-risk-ind-v1` (price-risk-v1 regressors + within-FF12 demeaning, FWL) | done |
| 2 | `price-risk-ind-v2` (ind-v1 with vol 126 / log-ADV 252; the id carries the windows) | done |
| 3 | `price-risk-ind-v1-mkt` (beta against `mkt_ret`) | **skipped**; design in section 5 |
| 4 | grp_ff12 through `load_fields`; workspace reserve at actual geometry (C4) | done |
| 5 | F9 ring buffer for exposures | **skipped**; design in section 6 |

## 2. Changes (file:line at 720a0066)

**`atx-impl/src/strategy_price_exposures.hpp`**
- :19-25 adds `kMinGroupNames = 5`, `kMaxGroupId = 9999`, `kGroupSlots` (one slot per id, plus unknown and fallback) and `kGroupTableBytes` (400,080 B).
- :57-58 adds `NeutralizeScratch::slot/slot_count/slot_sum`. These grow only on the grouped path.
- :79 adds `NeutralizeStats::groups/unknown_group_names/fallback_names`. They stay 0 on the v1 path.
- :140-175 declares the new API and its documented exact order: `neutralize_target_within_groups` and `neutralize_price_risk_within_groups`.

**`atx-impl/src/strategy_price_exposures.cpp`**
- :229-305: `reserve_group_scratch`, `assign_slots` and `demean_within_groups`.
  - `assign_slots` validates the ids and assigns each row a slot: its id, the unknown slot, or the fallback slot.
  - `demean_within_groups` is the FWL demeaning step. It also refuses a z column the groups span (Unavailable): the check is within-group SS ≤ 1e-8 × SS, the grouped equivalent of the Cholesky pivot floor.
- :407-437 `fit_residual` gains `bool grouped`. The target copy into `s.residual` (:416) now comes before the factorization. The grouped demeaning is at :419.
- :440-484: a single shared body for both paths, `neutralize` and `price_risk` (file-local). An empty group span means the v1 path.
- :516-553: the public wrappers. v1 passes `{}`. The within-groups wrappers refuse an empty or wrong-length group span, so they can never silently run v1.

**`atx-impl/src/strategy_target_replay.hpp`**
- :16-28: new enum values `PriceRiskIndV1 = 2` and `PriceRiskIndV2 = 3`, plus `neutralize_by_industry()`, `industry_group_field = "grp_ff12"` and `price_risk_ind_v2_{vol,adv}_window = 126/252`.
- :81 `TargetReplayInput::industry` (optional span; NaN = unknown).
- :95-96 `ConstructionDay::neutralize_groups/unknown_group_names/fallback_names`. These go to summary JSON only; they are never CSV columns.

**`atx-impl/src/strategy_target_replay.cpp`**
- :56 `neutralizing()` now covers all three ids. :60 `neutralize_name()`.
- :105 ind-v2 must carry vol 126 and adv 252; any other pair is refused (InvalidArgument).
- :134 the scratch budget adds the group table for the industry ids only.
- :154 industry span geometry check.
- **:333-345 form_desired dispatch.** An industry id requires the field (InvalidArgument otherwise) and calls `neutralize_price_risk_within_groups` on row d. Otherwise the v1 call is unchanged. The group record is copied into `ConstructionDay`.
- :404-406 `replay_targets` refuses an industry id without the field.
- :642 rule id is `+neutral-<id>`. :666-693 recipe: the id name, the FWL method text, and an `industry` object (field, min_group_names, max_group_id, unknown/fallback semantics).
- :740-758 `industry_summary`: `construction.neutralize_industry` with min/median/max of groups, unknown and fallback over applied decisions.
- :810 `parse_neutralize(value, TargetReplayConfig&)`. ind-v2 also sets its windows.
- :941-944 `run_target_replay` refuses the industry ids: the target replay loads no fields.

**`atx-impl/src/strategy_target_replay_detail.hpp`**: the `parse_neutralize` signature takes the config; doc updates.

**`atx-impl/src/strategy_nav_replay.cpp`**
- :300 `validate_nav_input` requires `x.industry` for the industry ids.
- :1482-1493 `load_field` factors out the per-field load (checks, SHA, binding record). :1516-1530 `load_fields` loads grp_ff12 only for the industry ids, charged as a third f64 field.
- :1539-1551 `role_geometry` reads names and score sessions from the SHA-pinned role manifest only; no payload is read.
- :1605-1620 `nav_workspace_reserve_bytes`. It replaces the file-local `nav_reserve_bytes` and uses the same formula, with `names` and `sessions` in place of `max_names` and `max_dates`.
- :1925-1957 `run_nav_replay`:
  - an industry id without `--fields` is refused;
  - the reserve is charged at role geometry, before any payload, with the same `OutOfRange` refusal;
  - `remaining` also subtracts the industry field;
  - after load, the blend geometry must equal the reserve geometry;
  - `view.industry = loaded.industry`.
- :1986 and :2042: CLI help and the parse call take the config.

**`atx-impl/src/strategy_nav_replay.hpp`**: :372 public `nav_workspace_reserve_bytes`, plus docs on `NavFieldsPin` and `NavReplayInput`.

## 3. Exact ind-v1 order (also in the header, :140-160)

1. Used rows are `member && ok`. The entry gross and the too-few-names refusal are exactly as in v1.
2. Each exposure is z-scored over the used rows (mean, sample SD) and clipped to ±5, exactly as in v1.
3. Each used row gets a slot. A finite id must be an integer in [0, 9999]; anything else is a contract error that aborts the replay. All NaN ids form **one** unknown group. Every group with fewer than 5 used names is pooled into **one fallback group**; the unknown group is included in this rule.
4. Within each slot, the target and every z column are demeaned. Sums run in ascending row order. A z column that the slots span is refused (Unavailable, so the rebalance is skipped).
5. The v1 OLS (equilibrated Cholesky plus one refinement step) runs on [1, demeaned z]. The intercept coefficient is 0 up to rounding, so the residual is the FWL residual of the target on [slot indicators, z].
6. The result is rescaled to the entry gross. `member && !ok` rows are set to 0.

The amplification and excluded-share guards and the skip semantics are v1's, unchanged. By FWL, the result has zero sum within every slot and zero moment on every clipped z column.

**Interpretation of "fall back to the universe mean":** a small group gets no level of its own. Its names load only on the common intercept. With every other group absorbed by its own indicator, that intercept is the mean of all fallback names, so (by FWL) all small groups are demeaned together as one pooled group. This is the only reading that stays an exact OLS residual. Literally subtracting the global mean from small-group names is not a projection: the residual would not be orthogonal to anything, and the gross-rescale guard would lose its meaning. Test (c) pins this reading. Please confirm.

**ind-v2:** `--neutralize price-risk-ind-v2` sets `vol_window = 126` and `adv_window = 252`. The beta window stays 252, because v1 already uses 252. A programmatic config with other windows is refused, so the id always means these windows. The returns block stays at max(252, 126) = 252.

## 4. Field plumbing, reserve at geometry, RSS and time (TRAIN: role 1,155 dates × 5,627 names, score window 756, 5 books)

- **+1 integer field (grp_ff12, `<f8`, loaded at role geometry):** 1,155 × 5,627 × 8 = **51,993,480 B = 49.6 MiB** of RSS, plus a 0.4 MB slot table and 45 KB of slot scratch. The brief's ~753-date figure would be 33.9 MB (32.3 MiB), but fields are loaded at the full role geometry, warm-up included. Estimated RSS rises from about 338 MiB to about 388 MiB, against the 1,536 MiB cap. The v6 TRAIN manifest's grp_ff12 entry passes the loader contract: I checked the metadata only (one entry, PIT true, `<f8`, date-major, [1155, 5627], receipt 51,993,480 B, SHA d39f645e…).
- **Reserve (admission budget, not RSS).** Struct sizes used: NavReplayDay 584 B (was 560; ConstructionDay grew 24 B) and NavEvent 72 B.

  | | Reserve | Formula |
  |---|---|---|
  | Old (fixed geometry) | 190.6 MB | 16 MiB + 5 MiB + 20,000 × 994 + 20,000 × 2,144 + 5 × (4,096 × 560 + 262,144 × 72) |
  | New, price-risk-v1 | 136.3 MB | at 5,627 names × 756 sessions |
  | New, ind-v1/v2 | 136.7 MB | as above, plus the group table |

  The reserve drops by 54 MB, which about equals the added field (52 MB).
- **Admission at `--max-bytes 1073741824` with ind-v1.** Total ≈ **594 MB of 1,074 MB**; the old v1 total was ≈ 596 MB.

  | Component | Size |
  |---|---|
  | Reserve | 136.7 MB |
  | 3 fields | 156.0 MB |
  | Loader (64 MiB metadata + 36 B/cell + days + names) | 301.7 MB |

  The refusal semantics are unchanged: `max_working_bytes <= reserve` gives `OutOfRange` "budget below NAV workspace reserve" before any payload is read.
- **Time.**
  - ind-v1 adds no `log()`. The demeaning is O(used names) per decision, plus about 0.5 MB of table fills. Estimated **+0.1–0.3 s** per TRAIN run, including reading and hashing the 52 MB field.
  - ind-v2 adds (252 − 63) × 5,627 ADV multiply-adds and 63 × 5,627 × 2 vol passes per decision. That is about 1.3e9 cheap operations, **+1–2 s**, still with no added `log()` (the returns block is unchanged).
  - Both stay well inside 180 s. Today's runs take 28–45 s.

## 5. Skipped (3): `price-risk-ind-v1-mkt` (F6). Design, about 45 LOC plus tests

I skipped this under the brief's own rule: it needs plumbing beyond what item 1 adds. It also is not in the pre-registered C5 grid (only v1 and v2 are), so it would need a pre-registration amendment and adds one trial.

Design:
1. In `load_fields`, load `mkt_ret` for the id.
2. Compress the broadcast field to a per-session series: take the first finite cell of each row, and refuse the field if any finite cell in a row differs bitwise. Then free the cell field. Net RSS is +9 KB.
3. Add `PriceExposureInput::market{}` (dates long). When it is non-empty, `fill_returns` takes `market[j] = market_by_session[w.first + j]` instead of the equal-weight all-instrument mean. The v1 path is untouched when the span is empty.
4. Point-in-time check: `mkt_ret` row d uses closes d−1 and d and membership at d−1, so it is known at the d mark.

## 6. Skipped (5): F9 ring buffer. Design, about 110–140 LOC plus tests

It could be bit-identical, but I skipped it for three reasons. It turns the documented "no hidden state between calls" exposure contract into a stateful one. I cannot build to verify it. And the time budget is not at risk (see section 4).

Design:
1. The scratch keeps the return block instrument-major with capacity 2 × block per instrument, so each instrument's window stays contiguous. When the write head reaches the capacity, the last block − 1 intervals are memmoved to the front (amortised O(1)). The per-interval market is kept the same way, along with the last session's logs.
2. On `d == cached_d + 1`, with the same input spans (pointers and sizes) and the same windows, the scratch appends one interval. That costs 2 × n `log()`. Any other `d` rebuilds with today's path. That includes the same `d`, which the existing tests use after mutating session d.
3. Bit-identity holds because each interval's return and market come from the same expressions on the same operands, and `beta_of`/`vol_of` see the same contiguous spans in chronological order. The ADV sum stays recomputed per decision: a running sum would change the summation order.
4. Effect: `log()` count per TRAIN run falls from about 2.1e9 to about 8.5e6.

## 7. Byte-identity argument (price-risk-v1 and default outputs)

1. **Neutralisation math.** v1 and the grouped path share one body. With an empty group span every added branch is skipped. The only v1-visible edit is moving the target copy (`e[r] = target[rows[r]]`) above `factor(normal_matrix(z))`, which never reads `s.residual`. So the operands, operations and order are identical. The old `neutralize_price_risk` reset `stats` twice; the second reset was a no-op, so dropping it changes nothing.
2. **Target replay.**
   - The v1 branch of `form_desired` calls `neutralize_price_risk` with the same arguments.
   - `neutralize_name(PriceRiskV1)` is `"price-risk-v1"` and `neutralize_name(None)` is `"none"`, so the rule id, recipe and summary strings are the old literals. The v1 method text is byte-identical.
   - The scratch budget's group term is 0 for v1.
   - The new `ConstructionDay` fields are never written to CSV. `neutralize_industry` and the recipe's `industry` key are emitted only for the industry ids.
3. **NAV.**
   - Only admission changed. The geometry reserve is ≤ the fixed one, so every previously admitted run is still admitted, and no published value depends on the reserve. The recipe's `max_working_bytes` is the configured value.
   - The v1 field loading reads the same two fields and produces the same binding JSON.
   - `parse_neutralize("price-risk-v1")` leaves `price_risk` untouched.
4. **Pinned test.** `StrategyPriceNeutralizeV6.PriceRiskV1BytesArePinnedOnTheExistingFixture` pins the SHA-256 of the 90 output f64 values, plus `residual_gross` and the 4 coefficients as bits, on the existing orthogonality fixture. I generated the golden with an exact IEEE-754 binary64 Python replica of the base-commit operation order. That operation order uses only +, −, ×, / and sqrt, so the replica has no transcendental-function risk. Every existing v1 test (recipe keys and values, rule ids, CSV headers) is unchanged.

## 8. Tests (GoogleTest; written, not run)

| Test | Checks |
|---|---|
| (a) `StrategyPriceNeutralizeV6.PriceRiskV1BytesArePinnedOnTheExistingFixture` | the v1 golden above |
| (b) `StrategyPriceNeutralizeV6.WithinGroupsZeroesGroupMeansKeepsGrossAndExposure` | two groups of 30: within-group sums ≤ 1e-12; gross 1 (1e-12); all four z moments ≤ 1e-12; the input group bet (> 0.1) is removed; the result differs from v1 (> 1e-3); scratch reuse is stateless |
| (c) `StrategyPriceNeutralizeV6.SmallGroupsPoolIntoOneFallbackAndUnknownIdsAreOneGroup` | groups of 20/20/3/2 plus 15 NaN, interleaved: groups 4, fallback 5, unknown 15; own and pooled sums ≤ 1e-12; the 3-name group's own sum is > 1e-3; relabelling {3,2}→one id and NaN→an id is bit-identical |
| `StrategyPriceNeutralizeV6.WithinGroupsRefusesBadIdsGeometryAndSpannedExposure` | ids 3.5, −1, 10000 and inf are refused; unread rows are ignored; short or empty spans are refused; a between-group-only exposure gives Unavailable; the target is unmodified on every refusal |
| `StrategyPriceNeutralizeV6.WithinGroupsOneCallStepMatchesPrimitives` | the one-call step equals the primitives bit for bit |
| `TargetReplayV6.IndustryIdsParseWithTheirWindowsAndRefuseOtherPairs` | parse and windows; ind-v2 with other windows is refused; all-skipped early v2; short industry span is refused |
| `TargetReplayV6.IndustryConstructionIsTheWithinGroupsPrimitive` | `form_desired` equals the primitive bit for bit with its group record; applied days have gross 1 and net 0; refusals without the field; the pinned target run refuses |
| (d) `NavV6.WorkspaceReserveIsChargedAtTheRunGeometry` | exact per-session increment; a budget of reserve(actual) + 65 MiB runs, and the test asserts that this budget is below the fixed-reserve refusal line; budgets of reserve and reserve + 64 MiB are refused (`OutOfRange`, no output) |
| `NavV6.IndustryNeutralizeRunsWithTheFieldAndIsRefusedWithout` | NAV construction equals the target replay's; lockstep equals single runs; the pinned `--fields` run publishes the id, `industry` recipe, grp_ff12 binding and group diagnostics; v1 on the same fields does not load grp_ff12; refusals without `--fields`, without the entry, or with PIT false; CLI exit 1 |

I desk-checked the expected numbers with the replica:
- (b) group sums about 5e-18, amplification 1.75.
- (c) group-5 sum 0.035; the relabel is bit-identical.
- Role(70, 12, 21) with ind-v1 applies at every decision from 20 onward (48 decisions), with the worst amplification 2.16 against the cap of 5.

**Root commands:**
```powershell
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_price_exposures.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_target_replay.cpp
powershell scripts\atx-build.ps1 check atx-impl\src\strategy_nav_replay.cpp
powershell scripts\atx-build.ps1 build atx-impl-strategy-target-tests
build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=StrategyPriceNeutralizeV6.*:TargetReplayV6.*:NavV6.*
build\bin\atx-impl-strategy-target-tests.exe --gtest_filter=StrategyPriceExposures.*:StrategyPriceNeutralize.*:StrategyTargetReplay.*:StrategyNavReplay.*:TargetReplayV5.*:NavV5.*
```
The last line is the unchanged regression set. The focused target is `EXCLUDE_FROM_ALL` and not registered with ctest, so run the exe directly. `atx-impl-tests` globs the same files if the ordinary suite is preferred.

**TRAIN cell (the v51 nav line with only the id changed; run under `scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536`):**
```
build-equity/bin/atx-equity-strategy-targets.exe nav --combined $C --combined-sha256 $CS --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage $LEV --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-ind-v1 --max-bytes 1073741824
```
For v2, use `--neutralize price-risk-ind-v2`. `--fields` is mandatory for both ids (`FD = build-equity/recent-fast-train-2020-2022-v2-fields-v6`). The summary carries `construction.neutralize_industry`, and the recipe carries `industry` and `financing_fields.fields_used.grp_ff12`.

## 9. Concerns

1. **Nothing was built or run.** The code and tests are desk-checked only (C++20, `/W4 /WX`, 100 columns, CRLF preserved). The first `check` or `build` may surface trivial compile errors.
2. **Golden (a) comes from a replica, not a base-binary run.** It is exact for the dev preset: SSE2 with no FMA contraction. It would fail under `rel-avx2` (`/arch:AVX2` allows FMA contraction) because `sha256` would differ. To validate the golden itself, cherry-pick the test onto 04e9d5bc; it must pass there too.
3. **Merge overlap with V6-C1** (locate-in-aim, `--order-basis`, `--exit-rate`). The lanes touch the same places:
   - `form_desired` (:326-345), where C1 adds special-tier zeroing before the call;
   - the `TargetReplayConfig` and `ConstructionDay` structs;
   - the NAV help and `--neutralize` parse lines (:1986, :2042);
   - `run_nav_replay` around the base config and reserve (:1925-1957);
   - the end of the test files.

   All of these are textual adjacency. There is no semantic conflict: C1's zeroing happens before my dispatch.
4. **Coverage.** grp_ff12 member coverage is 0.60, so about 40% of used names form the single unknown group (about 1,200 names). That follows the brief ("one residual group"), but it weakens the industry hedge for those names; grp_ff49 is equally sparse. The fallback reading in section 3 needs root confirmation.
5. **Naming.** The fields binding key is still `financing_fields` even though it now also records grp_ff12. I kept it to avoid a schema change.
6. **Pre-registration.** Only ind-v1 and ind-v2 are pre-registered (C5). Each TRAIN cell is +1 trial. Items 3 and 5 are not implemented.
