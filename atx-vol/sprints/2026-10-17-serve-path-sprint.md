# Serve-path lane, resume 5 (2026-10-16 to 2026-10-17)

This note supersedes the work-in-progress note
`2026-10-16-serve-path-wip-handoff.md` (kept for history).

- Branch: `feat/vol-serve-path-20261016`.
- Base: `dc064a1e`. The root head `7ca31c95` was merged in, not rebased, so the
  reviewed SHAs below do not change.
- Code tip: `b2e09d59`. This note is committed on top of it.
- Worktree: pool-33 (RunId `vol-20261017-serve-path`).

Every timing below comes from `vol-rel-avx2` on a shared, loaded host. They are
UNQUALIFIED, and no baseline was pinned. Correctness gates ran on `vol-rel`.

## Commits

| SHA | Item | What |
| --- | --- | --- |
| `cd427b31` | 1 | Opt-in Records serve mode and the exercise-edge gate (from 2026-10-16) |
| `e695a411` | 2 | Tick-path review minors (from 2026-10-16) |
| `7cf6b38b` | 3 | WIP checkpoint (uncompiled) |
| `291d4601` | 3 | Opt-in nearest-ATM rebuild order, its tests, and the +7% catch-up bench rows |
| `5627cbf6` | merge | Merge of root head `7ca31c95`. The only conflict was the ledger; both sides were kept |
| `25096514` | 4 | Record sigma rows evaluated in lockstep; implied-vol work hoisted out of the loop |
| `b2e09d59` | 5 | Box-end boundary solves reused in certification; opt-in parallel record build; build bench rows |

## Defaults

The default paths are byte-identical:

- The Records serve mode and the exercise-edge gate are opt-in and OFF.
- `SpotLadderPolicy::rebuild_order` defaults to `ContractIndex`, the behaviour
  before this sprint.
- `ChainRecordPolicy::build_workers` defaults to 1, an inline build.

Items 4 and 5 change no record value. Two checks show this:

- **Accuracy counters.** 37 full-precision counters are identical before and
  after the change. They cover:
  - the certificate;
  - price and Greek maximum and p99 errors;
  - implied-vol errors;
  - evaluation counts.
- **Record fingerprint.** It is `1045776371:1697574764` for the default build,
  for the build without the solve reuse, and for the 3-worker build.

## Item 3: catch-up after a large move

When a jump leaves more windows than the per-tick budget can rebuild, the opt-in
`SpotLadderRebuildOrder::NearestAtmFirst` rebuilds the waiting contracts in
order of smallest `z = |ln(K/F)| / (sigma sqrt T)` first.

- The selection uses `std::nth_element` on scratch memory sized at build, so a
  tick still allocates nothing.
- Ties break on the contract index.
- When the budget covers every exit, the two orders are bit-identical.

The rows are `chain/catchup/jump7/<order>_b<budget>/n{600,2000}`. Each one is a
+7% jump followed by the 1 bp tape:

| Row | Catch-up ticks | Tick when every in-box z <= 1 lane is served | Out-of-box lanes |
| --- | --- | --- | --- |
| index_b32 / n600 | 17 | 17 | 52 |
| atm_b32 / n600 | 17 | 6 | 52 |
| atm_b64 / n600 | 8 | 3 | 52 |
| index_b32 / n2000 | 59 | 59 | 116 |
| atm_b32 / n2000 | 58 | 18 | 114 |
| atm_b64 / n2000 | 29 | 9 | 108 |

- The order changes when the near-money lanes are served, not how long the
  catch-up takes.
- Out-of-box lanes stay Fallback until a refit.
- At the tip, a catch-up tick costs 258-266 us at n600 / b32 and 453-496 us at
  n2000 / b32 (514 and 821-832 us before item 4). That is above the 300 us
  tick budget whenever the budget binds at n2000.

Receipts: `C:/atx-cache/quote-benchmark/serve-path-20261017-item3/`.

## Backtest slice: 3 of 646 failures are pre-existing

The same three failures occur at base `dc064a1e` (645 cases) and on the lane
(646 cases, which adds one PricedSurface case):

| Test | Failure |
| --- | --- |
| `StrategyPipelineRisk.AggregatesRiskAndSupportsClampOrReject` | gamma `-4.163e-17` vs `0` |
| `BacktestExec.L2SettlementMarkMemoDropsExpirySolve` | memo ON != OFF at row 3-4 |
| `VarSwapMemo.RowsAreBitIdenticalToTheUnmemoizedPerRowPath` | pv raw bits ...640 vs ...639 |

All three are last-bit floating-point pins under `vol-rel`. The lane does not
touch them, so they were not fixed.

Receipts: `C:/atx-cache/quote-benchmark/serve-path-20261017-backtest-base/`.

## Item 4: record implied vol (target <= 500 ns)

Two approaches were taken:

1. **Lockstep rows.** The 16 x 12 record plane is now stored moneyness-major.
   `eval_rows<Order>` runs the 12 sigma-row Clenshaw recurrences interleaved,
   reading 12 contiguous doubles per term, instead of 12 serial dependency
   chains. Each row's recurrence uses the same operations in the same order, so
   the values are unchanged.
2. **Hoisting.** `implied_vol` computes F, ln(K/F) and sqrt(T) once per
   inversion, through the private `value_and_vega_at()`. Each evaluation is
   bit-identical to `price_and_vega`.

A/B results, interleaved, two rounds, host load 4-96% (all values in ns):

| Row | Before | Lockstep | + hoist | Tip, load 16% |
| --- | --- | --- | --- | --- |
| Implied vol, warm seed (3.51 evaluations) | 939 / 1,047 | 625 / 445 | 601 / 473 | 404 |
| Implied vol, cold seed (4.38 evaluations) | 1,154 / 1,337 | 779 / 519 | 710 / 598 | 508 |
| record_direct, per contract | 360 / 340 | 172 / 219 | 184 / 145 | 141 |
| record_price_only, per contract | 139 / 129 | 143 / 115 | 147 / 131 | 109 |

- Almost all of the gain comes from the lockstep rows. The hoisting is within
  noise.
- The 500 ns target is met on quiet rounds (404-473 ns), but not under load
  (601 ns at 87-96%).
- The price-only route is bound by Black-76 and did not change.
- The serve/records chain went from 0.77 ms to 0.27-0.37 ms.
- The ladder tick at the tip has p50 65.7 us (32.9 ns per contract) and p99
  82.3 us.

Receipts: `C:/atx-cache/quote-benchmark/serve-path-20261017-item4/` (it includes
the three A/B executables).

## Item 5: record build speed

**Profile of one 40-record `ChainRecords::build`.** The probe was
`ATX_VOL_AL_PROBE=z`, with the zone dump added to the new
`chain/records/build/*` rows.

- A certified build takes 279 ms; the sampling pass alone takes 125 ms.
- Cold accurate boundary solves are 60% of the Andersen-Lake cycles, at 27
  solves per record and about 325k cycles each.
- The 192 sampling premiums per record are 18%.
- The certification's cold reference prices are 22%.

Two wins were taken:

1. **Reused box-end solves.** The edge pass already solves the boundaries at
   `sigma_min` and `sigma_max`. With the default accurate sampling scheme, those
   are exactly the certification's two box-end rows, so certification reuses
   them. This takes the record from 27 to 25 solves (3,240 -> 3,000 over three
   builds). A cold solve re-initialises its nodes, geometry and seed, so the
   fingerprint is unchanged. Time: 273-276 -> 266 ms.
2. **Opt-in parallel build.** With `ChainRecordPolicy::build_workers` set,
   `parallel_for_dynamic` runs over the independent record specs, and the bank
   is filled in group order. The records, the contract map and the reported
   error are identical to the inline build. Time with 3 workers:
   266 -> 101-104 ms (90.7 ms at the tip).

The next lever, not taken: warm-starting boundaries from the adjacent sigma node
(`al_solve_put_boundary_warm`). That would change record bytes, because the
solves converge only within tolerance. It would also save only the
Barone-Adesi-Whaley seed (21k of 325k cycles) unless the sweep budget is also
cut.

Receipts: `C:/atx-cache/quote-benchmark/serve-path-20261017-item5/`.

## Gates at `b2e09d59` (vol-rel, `-Jobs 3`)

| Gate | Result |
| --- | --- |
| tick-path | 51/51 |
| `pricing/PricingExecutor` | 23/23 |
| `fitting/(SessionServe\|CorrectionCache\|VolaSession)` | 78/78 |
| `backtest/PricedSurface` | 52/52 |
| fitting slice | 1027 cases: 38 failed + 10 skipped, the same 48-name set as the base list |
| backtest slice | 646 cases: the 3 pre-existing failures above |
| PCH-off check (vol-hygiene) of the touched headers' TUs | clean |
| Bench self-check | 0 failures |
| chain-refresh name coverage | 1/1 (24 required rows) |

The merged tip `5627cbf6` was gated the same way (tick-path 48/48, the other
suites identical) before items 4 and 5.

Receipts: `C:/atx-cache/quote-benchmark/serve-path-20261017-final/`. Each
receipts folder has a `sha256.txt`.

## Open, for review or the next lane

- Ladder rebuilds and record fallbacks during a binding catch-up cost
  0.25-0.75 ms per tick. A per-contract sigma-collapsed series (WIP note
  item 1.2) would cut both.
- Gaps carried over from item 1:
  - PricedSurface has no records tier.
  - PricerFitter's band-IV inversion still uses the tensor.
  - The session Records mode has no spot ladders.
- The record IV target needs a quiet-host pin before it can be claimed.
