# Lane W0-D0 report — Data-layer PIT leak fixes

## Outcome

- **Outcome:** DONE (every acceptance item MET; every cited ID CLOSED, D-06 tie+rebase part
  closed here with the PIT-shares source left to W2-D3 as briefed). One golden digest could not
  be re-pinned inside the lane (it needs 2024 real data); see "Golden digests".
- **Branch:** `feat/w0-d0` · **Pool:** `C:\atx-wt\pool-4` · **Run id:** `aes-w0-d0`
- **Base:** `458d0bef480a624e258070c9d45174a9984466bf` (W0 base; `merge --no-ff feat/w0-integration`
  at start: "Already up to date").
- **Final SHA:** the commit that adds this report (see `git log -1` on `feat/w0-d0`); code commits
  `115cfb57` (implementation) and `02d34323` (tests + follow-up fixes).
- Build tree: `build-equity\` reconfigured once with `-Groups "data;alpha"` (it held `factory`).

## Files changed (all owned, or new test files)

Source / headers:
- `atx-engine/include/atx/engine/data/history_panel.hpp`, `src/data/history_panel.cpp` —
  `LevelBasis` enum, `level_basis_name`, `history_field_level_basis(name)`,
  `HistoryPanel::field_basis` (filled for every field; unknown tag is an `Internal` error).
- `atx-engine/include/atx/engine/alpha/augment.hpp` — only the dollar_volume/adv/vwap derivation
  in `with_alpha101_fields`: new `DollarVolumeBasis {CloseV1, RawCloseV2}` parameter, default V2.
- `atx-engine/include/atx/engine/data/finra_short.hpp`, `src/data/finra_short.cpp` — NYSE session
  calendar (`nyse_is_session`, `nyse_add_sessions`), `FinraLagRule`, `FinraPublicationLag`,
  `finra_first_usable_day`, new default overload of `load_finra_features`.
- `atx-engine/include/atx/engine/data/adjust.hpp`, `src/data/adjust.cpp` — `TriGapRule`.
- `atx-engine/include/atx/engine/data/align.hpp`, `src/data/align.cpp` — `AlignColumnRule`,
  `AlignOptions`, options overload of `align_onto`.
- `atx-engine/include/atx/engine/data/corporate_actions.hpp`, `src/data/corporate_actions.cpp` —
  `SharesPitRule`, `CorpAlignRule`, `corp_action_align_options`, rebased row-dated PIT shares.
- `atx-engine/include/atx/engine/data/context.hpp`, `src/data/context.cpp` — as_of mismatch and
  moved-from use return `Err`.
- `atx-engine/include/atx/engine/data/universe.hpp`, `src/data/universe.cpp` — `NanFloorRule`,
  NaN-safe top-N ordering, traded-price membership test (V2).
- `atx-engine/include/atx/engine/data/real_panel.hpp`, `src/data/real_panel.cpp` —
  `RealPanelPriceBasis`, `restate_on_tri_basis`, `real_panel_field_level_basis`,
  `RealPanel::field_basis`, config fields `price_basis` / `corp_align` / `tri_gap_rule`.

Tests:
- New: `atx-engine/tests/data/data_w0d0_{future_perturb,finra_lag,adjust_gap,align_event,
  corpact_rebase,context_asof,nan_floor,real_panel_basis}_test.cpp`.
- Existing tests edited (comments only, no expectation changed):
  `data_adjust_test.cpp:393` (pins D-04; the value 105 still holds under the ratio rule because
  no dividend accrued before that gap) and `data_universe_test.cpp:347-366` (pins D-09's old
  reason "NaN >= 0 is false"; expectation unchanged, exclusion now comes from the traded-price
  test).

No CMakeLists edits, no new `src/*.cpp`.

## Acceptance table

| Plan acceptance item | Test(s) | Measured result | Status |
|---|---|---|---|
| Future-perturbation invariance: mutating every row (factors included) dated after t leaves every history_panel output row ≤ t bit-identical, for all fields | `DataHistoryPanelFuturePerturb_W0d0.EveryRowUpToTIsBitIdenticalForAllFields` (+ `FixtureExercisesWarmupAndTopNCap`) | t ∈ {4, 11, 12, 20}; prices, volume, shares, gics, earnings/IV fields and `cumReturnFactor` all changed after t. 3,060 history-panel cells and 5,355 Alpha101-augmented cells (dollar_volume, vwap, adv2, adv5, returns, cap, IndClass.*) compared bit-for-bit, 0 differences; universe mask and `field_basis` identical; control: row t+1 differs every time. Fixture has ADV warm-up (row 0: 0 members) and a binding top-4 cap (row 10: 4 of 5). | MET |
| FINRA Thu/Fri/holiday fixtures land on the correct session | `DataFinraLag_W0d0.ThursdayFridayAndHolidaySettlementsLandOnTheEighthSession`, `LoaderPlacesValuesOnTheUsableSessionNotBefore`, `NyseCalendarKnownSessionsAndClosures`, `OutOfRangeSettlementAndNegativeLagFailClosed` | Thu 2019-03-14 → Tue 03-26; Fri 2019-03-15 → Wed 03-27; MLK (Thu 2019-01-17) → Wed 01-30; Thanksgiving (Thu 2018-11-15) → Wed 11-28; Bush closure (Fri 2018-11-30) → Thu 12-13; Good Friday (Mon 2019-04-15) → Fri 04-26; Mon 2019-03-11 → Thu 03-21. Each is exactly the 8th NYSE session after settlement. Legacy +10 calendar days was early by 1, 2, 2, 2, 3, 1, 0 sessions; loader: legacy exposed 11 session-cells early, new rule 0. 2019 has 252 sessions in the calendar. | MET |
| A 3% payer shows no phantom drop across a gap | `DataAdjustGap_W0d0.ThreePercentPayerShowsNoPhantomDropAcrossGap` (+ 4 more DataAdjustGap tests) | Flat 100 price, 0.75/quarter, 5 years, one missing session: TRI step across the gap = 0.000000 (RatioChainV2) vs −0.132351 (legacy ReanchorV1, all 19 dividends dropped). | MET |

Lane "Build" items (brief) and their proof:

| Build item | Test(s) | Measured result | Status |
|---|---|---|---|
| dollar_volume / adv{d} from raw_close × volume | `DataLevelBasis_W0d0.DollarVolumeAndAdvAreBuiltFromRawClose`, `FactorResnapshotLeavesRawLiquidityInvariant` | dollar_volume == raw_close·volume and adv5 == rolling mean of it, bit-for-bit; field order identical to legacy. Re-snapshot (all factors × 0.5): raw_close, volume, market_cap, dollar_volume, adv2, adv5, returns, cap and the mask unchanged bit-for-bit; legacy CloseV1 dollar_volume halved on 112/112 finite cells. | MET |
| vwap | same | vwap stays the typical price on close's basis (identical under both rules); tagged `adjusted_level`. See Deviations. | MET (with deviation) |
| Tag every field with its level basis | `DataLevelBasis_W0d0.HistoryPanelTagsEveryFieldWithItsBasis`, `RealPanelFieldTagsFollowThePriceBasis` | All 12 history fields and every Alpha101-derived field tagged; unknown names → nullopt. | MET |
| FINRA lag in NYSE sessions, +1 after close | DataFinraLag_W0d0 (4 tests) | see acceptance row | MET |
| TRI gap uses prev_tri·S_t/S_last | DataAdjustGap_W0d0 (5 tests) | ratio carried (103·90.9/101), split inside a gap continuous, leading gap anchors, V1 reproducible | MET |
| Event columns exact-date, staleness cap, coverage failure | `DataAlignEvent_W0d0` (7 tests) | dividend total 2.00 (legacy, counted 4×) → 0.50; factor NaN after cap instead of frozen; pre-axis rows fail a finite cap; price dates past plug coverage → `Err(OutOfRange)`; corp preset = dividend event + cap 5 + coverage guard | MET |
| Shares rebased by cum_adj ratio; same-filed-date tie uses rows ≤ d | `DataCorpActRebase_W0d0` (5 tests) | market cap step at a 2:1 split 0.000 (V2) vs −0.500 (legacy); legacy tie showed post-split shares on 3/3 pre-split days, V2 on 0; restating rows after day 2 moved 3 legacy cells, 0 V2 cells; unknown basis → NaN | MET |
| DataContext as_of mismatch returns Err | `DataContextAsOf_W0d0` (3 tests) | earlier and later as_of → `Err(InvalidArgument)`; cache intact afterwards; moved-from accessors → Err (no null dereference) | MET |
| NaN-floor semantics match the documentation | `DataUniverseNanFloor_W0d0` (4 tests) | disabled floors admit NaN cap / NaN ADV (legacy excluded them); enabled floors still fail NaN; NaN ADV ranks last in top-N; no traded price → never a member | MET |
| real_panel O/H/L scaled by TRI/raw | `DataLevelBasis_W0d0.RealPanelCandleSharesTheTriBasis`, `RestateNeverFabricatesAPrice` | open/high/low ÷ TRI close equals the raw bar ratio to 2.22e-16 over 5 years with dividends, a gap and a split; legacy mixed basis off by 0.99 | MET (restatement unit-tested; full build path see Deviations) |
| si_publication_lag default via W0-I0b | — | Integration note below | handed to I0b |

## Defect table

| ID | Status | How / where |
|---|---|---|
| D-01 | CLOSED | `augment.hpp` `with_alpha101_fields`: derived dollar_volume/adv{d} = raw_close × volume (`DollarVolumeBasis::RawCloseV2`, default; `CloseV1` reproduces). Level-basis metadata: `history_panel.hpp` `LevelBasis`, `history_field_level_basis`, `HistoryPanel::field_basis`; `RealPanel::field_basis`. Consumer `stage_equity_mine.cpp:1375` calls `with_alpha101_fields(span.panel, adv_windows)` on a history panel that carries `raw_close`, so it picks up the fix with **no edit** (the file compiles unchanged against the new header: `check atx-impl\src\stage_equity_mine.cpp` OK). |
| D-02 (engine side) | CLOSED | `finra_short.{hpp,cpp}`: NYSE calendar (atx-core rule holidays + unscheduled closures 1994-2025 + NYSE MLK start 1998), `FinraLagRule::NyseSessionsV2` default (7 sessions + 1 after close), legacy `int` overload keeps `CalendarDaysV1`. The atx-impl default is W0-I0b's (see Integration notes). |
| D-03 | CLOSED | `real_panel.cpp`: open/high/low/vwap restated per cell by TRI/raw_close (`RealPanelPriceBasis::TriScaledV2`, default; `MixedV1` reproduces); volume/dollar_volume/adv stay raw. |
| D-04 | CLOSED | `adjust.cpp`: `TriGapRule::RatioChainV2` (default) resumes TRI = prev_TRI·(S_t/S_last); `ReanchorV1` reproduces. `data_adjust_test.cpp:393` still passes (comment updated). |
| D-05 | CLOSED | `align.{hpp,cpp}`: per-column staleness caps in canonical sessions, event columns (cap 0: join once, never forward-filled), `require_coverage` → `Err(OutOfRange)`. `corporate_actions` `corp_action_align_options(CorpAlignRule::EventOnceCappedV2)` (dividend = event, other columns cap 5 sessions, coverage required) is used by `build_real_panel` by default. The 2-argument `align_onto` keeps the legacy join for feature/signal plugs. |
| D-06 (rebase + tie) | CLOSED | `corporate_actions.cpp` `SharesPitRule::RebasedRowDatedV2` (default): the resolving filing is the greatest filed_date ≤ d among rows dated ≤ d (ties → latest such row), and its count is rebased by cum_adj_factor(d)/cum_adj_factor(row); unknown basis → NaN. `AsFiledV1` reproduces. `universe.cpp:102` (market_cap = shares × raw close) is correct once shares are on d's basis — measured continuous across a split. |
| D-06 (PIT shares source) | DEFERRED → W2-D3 | As briefed: the vendor-shares vintage (history panel `shares` has no filing date) is W2-D3's. |
| D-08 | CLOSED | `context.cpp`: `signal_admit_candidates` returns `Err(InvalidArgument)` on any as_of different from the cached one, in every build type; moved-from accessors return Err instead of a release-mode null dereference. |
| D-09 | CLOSED | `universe.{hpp,cpp}`: `NanFloorRule::DisabledFloorPassesV2` (default): a floor ≤ 0 passes every cell (NaN included), an enabled floor still fails NaN, a cell with no traded price (NaN / ≤ 0 raw close) is never a member, NaN ADV ranks last in the top-N sort (the old comparator was not a strict weak order once NaN could reach it). `NanFailsV1` reproduces. |

## Evidence (verbatim tails)

Configure (once): `scripts\atx-build.ps1 configure -Preset equity-dev -Groups "data;alpha"` →
`-- Build files have been written to: C:/atx-wt/pool-4/build-equity`.

Build (after the final source change):
```
build -Preset equity-dev atx-engine-data-tests
[24/25] Linking CXX executable bin\atx-engine-data-tests.exe
build exit=0
build -Preset equity-dev atx-engine-alpha-tests
exit=0
```

New suites, direct run (`atx-engine-data-tests.exe --gtest_filter=*W0d0* --gtest_brief=1`, first run):
```
[tri-gap] 3% payer, 5y: RatioChainV2 step=0.000000  ReanchorV1 step=-0.132351
[align-event] dividend total legacy=2.00 event-join=0.50 (true 0.50)
[corp-tie] legacy leaked post-split shares on 3 pre-split days; V2 on 0
[corp-rebase] market-cap step at split: V2=0.000 legacy=-0.500
[finra-lag] Mon 2019-03-11               usable=17976 legacy_early_sessions=0
[finra-lag] Thu 2019-03-14               usable=17981 legacy_early_sessions=1
[finra-lag] Fri 2019-03-15               usable=17982 legacy_early_sessions=2
[finra-lag] Thu 2019-01-17 MLK           usable=17926 legacy_early_sessions=2
[finra-lag] Thu 2018-11-15 Thanksgiving  usable=17863 legacy_early_sessions=2
[finra-lag] Fri 2018-11-30 Bush closure  usable=17878 legacy_early_sessions=3
[finra-lag] Mon 2019-04-15 Good Friday   usable=18012 legacy_early_sessions=1
[finra-lag] loader: legacy leaked 11 cells, NYSE rule leaked 0
[perturb] history cells compared=3060 augmented cells compared=5355
[resnapshot] CloseV1 dollar_volume cells halved=112/112; RawCloseV2 moved=0
[real-candle] max |open/close - raw ratio|: TriScaledV2=2.22e-16 MixedV1=0.99
[==========] 35 tests from 8 test suites ran. (2033 ms total)
[  PASSED  ] 35 tests.
exit=0
```
(`NoTradedPriceIsNeverAMember` was added afterwards: the suites now hold 36 tests.)

Anchored ctest runs (`scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'`), all exit 0:
```
^DataLevelBasis_                 100% tests passed, 0 tests failed out of 6
^DataFinraLag_                   100% tests passed, 0 tests failed out of 4
^DataAdjustGap_                  100% tests passed, 0 tests failed out of 5
^DataAlignEvent_                 100% tests passed, 0 tests failed out of 7
^DataCorpActRebase_              100% tests passed, 0 tests failed out of 5
^DataContextAsOf_                100% tests passed, 0 tests failed out of 3
^DataHistoryPanelFuturePerturb_  100% tests passed, 0 tests failed out of 2
^DataUniverseNanFloor_           100% tests passed, 0 tests failed out of 4
```

Whole data executable (`atx-engine-data-tests.exe --gtest_brief=1` with the real-data filter below):
```
test exit=0
[==========] 235 tests from 37 test suites ran. (27505 ms total)
[  PASSED  ] 235 tests.
```
The first whole run (before the traded-price rule) failed exactly one test,
`DataUniverse.MembershipExcludesNanPriceCells` (it relied on `NaN >= 0` being false); the V2 rule
now excludes cells with no traded price, and the test passes with its expectation unchanged.

Excluded from the whole run by data discipline (they resolve `C:\atx\data\...` through the git
worktree marker and read real on-disk smoke data dated up to 2026, which W0 lanes must not read):
`DataRealPanel.*` (5), `DataCorporateActions.{LoadsSmokeMasterRowShapeMatchesManifest,
DividendZeroFilledOffExDates, SharesOutstandingPitForwardFill, SymbolInterningDeterministic,
PartitionedLoaderOrdersBySymbolArgument}`, `DataAdjust.SplitAdjustedNoDiscontinuityAtKnownAaplSplits`,
`OratsE2ESmoke.{RealPartitionRunsUnchangedRobustPipeline, OperatorOratsZip}`,
`DataUniverse.SurvivorshipCaveatDocumentedOrDeferred` (reads a doc from the main checkout).
The orchestrator must run these at G0.

Whole alpha executable (`atx-engine-alpha-tests.exe --gtest_brief=1`):
```
exit=0 secs=50
[==========] 678 tests from 255 test suites ran. (49540 ms total)
[  PASSED  ] 678 tests.
```
Anchored alpha augment suites
(`-R '^(MultiFamilySmoke|LiquidityFields|IvFields|Datafields|StreamingEngine_|ModesAndWidths/StreamingEngine_Corpus101)'`):
`100% tests passed, 0 tests failed out of 28`, exit 0.

atx-impl consumers of the changed headers type-check unchanged (`check atx-impl\src\<f>.cpp` for
`stage_augment`, `append_history_panel`, `stage_equity_mine`, `stage_panel`: all compiled).
The logged `CHECK failed:` lines in both whole runs are the output of existing death tests.

## Golden digests (old → new)

| Digest | Old | New | Defect | Note |
|---|---|---|---|---|
| `data_real_panel_e2e_test.cpp:169` `kGoldenDigest` (smoke real panel, 2024-07) | `0x2a22a873483d9157` | **not measured in lane** | D-03 (candle on TRI basis), D-05 (corp join), D-04, D-09 | The test reads 2024 real data under `C:\atx\data`, which lanes may not read. D-03 multiplies O/H/L/vwap by TRI/raw_close; wherever the chained TRI differs from raw_close by one ulp the bytes move. **G0 must run `DataRealPanel.*` and re-pin (or confirm) this literal.** |
| History-panel digests | none pinned (tests only compare two builds) | — | D-01 | `build_history_panel` output values are unchanged (field_basis is metadata outside the digest); only Alpha101-derived dollar_volume/adv change downstream. |

No other golden literal in the data/alpha targets moved (all 235 + 678 tests green).

## Deviations from the brief

1. **vwap is not built from raw prices.** The brief lists vwap with dollar_volume/adv. A raw-basis
   vwap next to an adjusted `close` would recreate D-03 (two bases in one panel) and would make
   `close/vwap` carry the snapshot factor, i.e. introduce a look-ahead. vwap therefore stays the
   typical price on close's basis and is tagged `adjusted_level`, so the W2-A3 lint rejects
   cross-sectional use of its level. On the real panel vwap is restated with O/H/L (D-03).
2. **D-03 is proven at the restatement function, not through `build_real_panel`.** The real
   build reads a databento hive whose `volume` is UInt64; the in-process parquet writer only
   writes i64/f64/string/timestamp, so a synthetic hive cannot be produced inside a test.
   `restate_on_tri_basis` is public and is exactly what `build_real_panel` applies; the full path
   is covered by the G0 smoke run (see golden digests).
3. **Extra suite `DataUniverseNanFloor_W0d0`** for D-09 (the brief's suite list has no universe
   suite). Prefix is lane-unique.
4. **Traded-price membership (V2).** To keep `DataUniverse.MembershipExcludesNanPriceCells`'s
   intent without the NaN-cap side effect, `DisabledFloorPassesV2` also requires a finite,
   positive raw close. Zero-price cells, previously members under a disabled cap floor, are now
   excluded under V2.
5. **Moved-from DataContext** accessors now return Err too (same release-no-op-assert class as
   D-08; owned file, no API change).
6. **Event join semantics:** "exact date" is implemented as staleness 0 in canonical sessions —
   the row joins its own session, or the first session after an off-axis date, exactly once.
   Rows older than the first canonical date never pass a finite cap (fail closed).

## Integration notes

- **W0-I0b (D-02 default):** `atx-impl/src/config.hpp:399` `si_publication_lag = 2` and
  `stage_augment.{hpp,cpp}` (`augment_panel_with_finra(..., int lag_days =
  kFinraDefaultPublicationLagDays)`) still use the legacy calendar-day overload. New default:
  **7 NYSE sessions with the after-close +1** (`FinraPublicationLag{}` =
  `{FinraLagRule::NyseSessionsV2, 7, true}`), i.e. a value is first usable on the 8th NYSE session
  after settlement. Suggested change: `long si_publication_lag = 7;` meaning **sessions**, and
  `stage_augment` calls `load_finra_features(..., FinraPublicationLag{FinraLagRule::NyseSessionsV2,
  static_cast<int>(cfg.si_publication_lag), true})`. Keep the old behaviour reachable as
  `FinraLagRule::CalendarDaysV1` (the `int` overload). The NYSE calendar covers 1990-2040;
  settlements outside it fail with `OutOfRange`.
- **W2-A3 (lint):** read `HistoryPanel::field_basis` / `RealPanel::field_basis` (parallel to
  FieldIds) or call `history_field_level_basis(name)` / `real_panel_field_level_basis(name)` for
  augmented / loaded panels; `nullopt` must be treated as unknown, not clean.
- **Universe behaviour change (D-09):** `stage_panel.cpp:394` keeps `min_mktcap_usd = 0`, so
  names with no share count (NaN market cap) now pass the (disabled) cap screen and can enter the
  production universe; the ADV floor still applies. If that is not wanted, set a positive
  `min_mktcap_usd` (or `nan_floor_rule = NanFloorRule::NanFailsV1`). atx-impl tests were not run
  by this lane; G0/I0b should expect universe-count movement here.
- **Real panel:** `RealDataConfig` gained `price_basis`, `corp_align`, `tri_gap_rule` (V2
  defaults). `build_real_panel` now fails with `OutOfRange` if the price window extends past the
  security master's last date (D-05 coverage guard).
- **`with_alpha101_fields`** gained a defaulted third parameter; existing call sites compile
  unchanged. Panels with `raw_close` (history/research panels) get raw liquidity automatically.
- **D-06 source (W2-D3):** the history panel's `shares` still come from the archive with no
  filing date (`kNoDate`), so no rebase is possible there; W2-D3 supplies the PIT shares source.

## Ledger candidates

- W0-D0: FINRA short interest is usable on the 8th NYSE session after settlement (7th-business-day
  release after the close + 1); settlement + 10 calendar days leaks 1-3 sessions on Thu/Fri/holiday
  settlements (11 session-cells over 7 fixture dates).
- W0-D0: history-panel dollar_volume/adv from adjusted close scales with the vendor's snapshot
  factor (a later 2:1 split halves every past value); raw_close × volume is invariant.
- W0-D0: TRI re-anchoring after a single missing session drops accumulated dividends: −13.2% on a
  flat 3% payer after 5 years; ratio resumption fixes it (step 0).
