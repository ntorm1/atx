# Serve-path lane: work-in-progress handoff at user stop (2026-10-16)

- **Branch:** `feat/vol-serve-path-20261016`
- **Base:** `dc064a1e` (not rebased; the root head has moved, to `9fc3934b` or later).
- **Worktree:** pool-4. The lease is released with this commit.

All bench numbers below come from `vol-rel-avx2` on a loaded host. They are UNQUALIFIED and provisional; no baseline was pinned.

## Done

### Item 1: serve-path gate — commit `cd427b31`

**What changed:**
- `SessionServePolicy` is a new Tier-A field (`SessionInputs` arity is now 25).
- Records serve mode (`SessionServeMode::Records`, opt-in): ExpiryRecords are built at fit and refit time. Each served price carries a per-contract certificate. A contract that is uncertified, out of its box or at an off-grid expiry is served cold.
- Opt-in exercise-edge gate on the CorrectionCache (`CorrectionCache::exercise_edge_distance`, `CorrectionBlend::clear_of_exercise_edge`). Also available on PricedSurface via `with_exercise_edge_gate`.
- New query routes: `ExerciseEdgeFallback` and `Record`.

**Bench, 2,000-contract chain, error against cold accurate AL:**

| Route | Max error | ms per chain | Flips |
|---|---|---|---|
| cached tensor | $0.969 (775 contracts over $1e-3) | 6.05 | — |
| gated, band 1 | $0.453 | 63.0 | 417 |
| gated, band 3 | $0.070 | 121.5 | 832 |
| records | $1.08e-4 (p99 $4.0e-5; 0 over $1e-3) | 0.85 | — |

- **Decision:** the gate stays OFF by default; it is in the ledger. The tensor's kink error is global, so a distance gate cannot retire it.
- **Receipts:** `C:\atx-cache\quote-benchmark\serve-path-20261016-item1\`

**Gates:**

| Gate | Result |
|---|---|
| Fitting slice | 1027 tests; 38 failed + 10 skipped, identical to the failure set at base (ledger trap) |
| `session_serve` | 13/13 |
| `backtest/PricedSurface*` | 52/52 |
| Full backtest slice | 646 tests, 3 failed — **not yet confirmed at base** |
| tick-path | 38/38 |
| umbrella | 9/10 (`NoFixturePath`, pre-existing) |
| vol-hygiene | 7 TUs clean |

### Item 2: review minors — commit `e695a411`

**What changed:**
- **Certificate threshold:** the ladder accepts at `max(max_cert_usd, max_cert_rel x F_max)` (rel = 5e-7), and every certificate carries an 8-ulp floor. At $50,000 the fixed threshold refused 7 of 42 contracts; the relative one refused 0. The $100 chain certifies bit-identically either way.
- **Fallback counters:** `SpotLadderCounters` splits fallbacks into budget / out_of_box / sigma_out_of_box / refused.
- **Floor Greeks:** they now match the served price, on both the ladder and the record (`RecordGreeks::floored`).
- **Certification:** a sigma row the cold solver cannot solve is now refused rather than skipped (`uncertified_rows()`).
- **Bench pool:** the pricing pool excludes the main thread's physical core (pool `0x3f`, main core `0xc0`). A baseline write also requires that the main thread is on a P-core.
- **Tests:** refusal of uncertified records; sigma outside the box; the scaled threshold; served-Greek consistency. The walk and cold-tracking tests now pass with no allowance (worst `err - cert` is -2.1e-13 and -2.3e-11).

**Gates:**

| Gate | Result |
|---|---|
| tick-path | 43/43 |
| `pricing/PricingExecutor` | 23/23 |
| `fitting/(SessionServe\|CorrectionCache\|VolaSession)` | 78/78 |
| vol-hygiene | 7 TUs clean |
| bench self-check | 0 failures |

**Bench, n2000:**

| Row | Median | Notes |
|---|---|---|
| `record_ladder` | 88.3 us per tick (42.9 ns per contract) | CV 7.6% |
| `record_direct` | 753 us | |
| `cold_al_avx2_batch` | 531 ms | pool is now 3 P-cores, so not comparable with earlier numbers |

- Host load was 46%, then 40%.
- **Receipts:** `C:\atx-cache\quote-benchmark\serve-path-20261016-item2\` and `serve-path-20261016-item2-*`

## In progress (in this WIP commit; NOT compiled, NOT tested)

### Item 3: catch-up after large moves

- `SpotLadderRebuildOrder` (default `NearestAtmFirst`) and `SpotLadderPolicy::rebuild_order` are in `chain_tick.hpp`.
- `refresh()` now collects the in-box candidates. When there are more candidates than the budget, it ranks them by `|ln K - ln g - ln S| / (sigma sqrt T)` with `std::nth_element` on scratch sized at build, so the tick stays allocation-free.
- The header doc on the $5,000 threshold claim was corrected.

## Unfinished: exact next steps

1. **Finish item 3:**
   - Run `check src/pricing/spot_ladder.cpp -Preset vol-rel`.
   - Add tests:
     - With a budget of 8 and a +4.5% jump, the rebuilt set equals the 8 smallest z.
     - `ContractIndex` order rebuilds contracts 0..7.
   - Add bench rows `chain/catchup/jump7/{index_b32,atm_b32,atm_b64}/n{600,2000}`. The 600-contract chain is the strikes with j % 10 in {0, 3, 6}. Each row measures:
     - ticks to catch up (no budget fallback)
     - the ATM catch-up tick (|z| <= 1)
     - per-tick time during catch-up
     - out-of-box lanes
   - Add those row names as `--required` entries in `bench/CMakeLists.txt`.
   - Then: tick-path ctest, hygiene, commit.
2. **Optional speed-up:** collapse the record at the contract's sigma into a 16-term moneyness series plus its sigma derivative. Use it for ladder rebuilds and record fallbacks: an estimated ~3x cheaper rebuild and ~4x cheaper fallback. Rounding is covered by the 8-ulp floor.
3. **Item 4 (IV toward 500 ns): not started.** Halley step using volga, a seed from the previous tick or the surface vol, a per-contract 1-D sigma series.
4. **Item 5 (record build speed): not started.** Currently 271 ms for 40 records:
   - reuse the box-end edge solves in certification
   - warm-start the boundary solves
   - parallelise `ChainRecords::build`
5. **Wrap-up:**
   - Confirm the 3 backtest-slice failures at base: `StrategyPipelineRisk.AggregatesRiskAndSupportsClampOrReject`, `BacktestExec.L2SettlementMarkMemoDropsExpirySolve`, `VarSwapMemo.RowsAreBitIdenticalToTheUnmemoizedPerRowPath`.
   - Write the sprint note `2026-10-16-serve-path-records-sprint.md`.
   - Rebase onto the root head, then run the final gates.
6. **Known gaps carried over from item 1:**
   - PricedSurface has no records tier.
   - PricerFitter's band-IV inversion still uses the tensor.
   - Session records mode has no spot ladders.
