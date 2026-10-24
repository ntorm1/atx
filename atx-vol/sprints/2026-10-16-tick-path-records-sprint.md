# Tick-path records sprint (2026-10-16)

Lane `tick-path-lane`, branch `feat/vol-tick-path-20261016`, worktree
`C:\atx-wt\pool-34`. It was built off `a9ca9dcd` and then rebased onto the root
`feat/vol-resume4-20261016` at `da3a4179` (the build-infra merge). The only
conflict was the README Tier-B count, which the root's implied-forward headers
had already moved to 34; this lane now takes it to 36. It implements audit items
5a, 2, 1+4 and 3 of the pricing hot-path audit
(`C:\atx-cache\quote-benchmark\perf-audit-20261016\2026-10-16-pricing-hotpath-audit.md`),
plus its §4 chain-refresh benchmark. The lane's task items 1-4 map onto them as
1 = audit 5a + §4, 2 = audit 2, 3 = audit 1 + 4, and 4 = audit 3.

**Every timing below is UNQUALIFIED.** Other lanes kept this host at 33-90% CPU
load throughout the sprint (load snapshots are saved beside each receipt), so no
baseline was pinned. Same-run ratios are citable; absolute numbers are not. The
candidate baseline `bench/baselines/i7-1260p-clang18-avx2-chain-refresh-provisional.json`
is provisional by name and must be re-captured on a quiet host before anything
gates on it.

## Commits

| Task item (audit item) | Commit on `da3a4179` | Pre-rebase | What |
|---|---|---|---|
| 1 (5a + §4) | `8042c8b1` | `2ae95c03` | main-thread P-core pinning; `chain_tick.hpp`; chain-refresh bench; pool-free cached tick route |
| 2 (2) | `d6a1da34` | `224e4279` | `ExpiryRecord` per-expiry calibration record; `ChainRecords`; record tick route |
| 3 (1 + 4) | `22b78369` | `532b19e3` | `ChainSpotLadder` per-contract Chebyshev spot ladders (the tick path) |
| 4 (3) | `d8f8772d` | `5b50abfe` | American IV against the record, warm-started from the surface vol |

All timings and the per-item gates below were taken on the pre-rebase commits.
The rebased commits were re-gated under the new lane presets; see "Gates after
the rebase".

## Design

### Item 5a: measurement fix (`bench/bench_main.cpp`, `src/pricing/cpu_topology.hpp`)

The pool's own P-core discovery (`discover_pcores`) and affinity call now live
behind the private `cpu_topology.hpp` seam. The pool workers and the benchmark
main thread both use them. `main()` pins the measuring thread to the
highest-numbered P-core logical CPU (or `ATX_BENCH_MAIN_CPU`), raises it to
`THREAD_PRIORITY_HIGHEST`, opts it out of power throttling, and yields so the
scheduler moves it. It records `atx_main_cpu`, `_observed`, `_pinned`,
`_is_pcore`, `_priority_highest` and `_power_throttling_off` in the JSON context.
It refuses a `baselines/` write when the pin did not take. `--atx-self-check`
asserts the pin. On this host every run reports CPU 7, observed 7.

### §4: the chain-refresh bench (`bench/chain_refresh_bench.cpp`)

The chain matches the spec: S = 600, r = 0.043, 20 expiries from 1 to 730 days,
50 strikes uniform in k over ±3σ_ATM√T, both sides, 2,000 contracts. σ comes
from a fixed eSSVI (ρ = −0.55, η = 0.90, γ = 0.45) and q_eff from a fixed borrow
curve. The tape is fixed-seed 1 bp steps, with 1% of ticks moving 10 bp. One
benchmark iteration is one tick, timed on its own with `UseManualTime`. Counters
are p50/p90/p99/p99.9/max/mean, ns per contract, rebuilds per 1k ticks, the
untimed refit build cost and the build-time certificates.

Accuracy is measured untimed on 1% of ticks × a rotating 5% of contracts. It is
compared against `american_greeks_al` with the accurate AL preset, computed once
per process. The tick seed and the chain-spec hash land in the context. Names are
pinned by the `atx-vol-chain-refresh-name-coverage` CTest. Routes never touch the
pricing pool. `cold_al_avx2_batch` runs `american_greeks_batch` with
`executor = nullptr`.

### Item 2: `ExpiryRecord` (`api/fitting/expiry_record.hpp`)

One record per (expiry, side) at the expiry's exact (r, q). It replaces, as an
opt-in path, the per-underlier 16×8×12 tensor baked at one `q_rep`.

- **What it stores.** The Andersen-Lake early-exercise premium is recorded
  forward-normalized: c(k, σ) = premium / F. It is a 16 (k) × 12 (σ) Chebyshev
  plane of 192 coefficients, with F = 1 at sampling. One record prices every
  (S, K) pair of the expiry by homogeneity.
- **The exercise edge.** The record solves the boundary at every σ node anyway,
  so it records the current exercise edge k*(σ) as a 12-term series (from the new
  `al_boundary_now` seam). Each σ row's moneyness axis is mapped onto the
  continuation side only. The American price's second-derivative jump at the
  boundary is therefore never interpolated; past the edge the record serves
  intrinsic. The domain endpoint is the unclamped edge, which is smooth in σ. It
  is clamped only in corners: a domain narrower than 5% of the box, or more than
  one box width beyond it.
- **Cost.** A price is one 192-term Clenshaw plus Black-76. Greeks use the
  differentiated recurrences. Theta comes from the continuation PDE (the
  `american_greeks_al` definition), because the record has no T axis.
- **Storage.** Arrays are fixed-size, so a record owns no heap memory, and every
  query is `noexcept` and allocation-free. `ExpiryRecordBank` reserves its
  capacity once and refuses inserts past it.
- **Dividends.** `ExpiryCarry::dividends` is a typed hook. `build()` refuses a
  non-empty list with `NotImplemented`. Still missing: a dividend-to-spot axis
  (cash dividends break spot homogeneity) and a Vellekoop-Nieuwenhuis lattice
  reference for those nodes.
- **Defaults.** `CorrectionCache`, the session, `PricedSurface` and every
  default are untouched. The record is not wired into the session serving path.

### Items 1 + 4: `ChainSpotLadder` (`api/pricing/chain_tick.hpp`, `src/pricing/spot_ladder.cpp`)

- **Series.** At refit every contract gets a 16-node Chebyshev price series and
  a vega series in spot. They cover a window of half-width
  clamp(2σ√T, 0.25%, 4%) in ln S around the build spot, and are sampled from the
  contract's record. A tick is two Clenshaw recurrences per contract with no
  transcendental call. Delta and gamma are derivatives of the price series, so
  they are consistent with the served price. Theta is the PDE identity on
  (V, Δ, Γ), so no third series is stored.
- **Exercise split.** The record's edge gives each contract's exercise spot. The
  window's exercise side is served intrinsic (`TickStatus::Exercised`); the series
  covers the continuation side only.
- **Layout.** Contracts are packed into 4-contract blocks in input order, with
  coefficients lane-interleaved ([node][lane]) and window, affine map and carry
  beside them. The lane loops are branch-free and auto-vectorized under
  `/arch:AVX2`. The working set is 656 KB for 2,000 contracts, which is
  L2-resident. Outputs are one status byte per contract, with no `Result` per
  contract.
- **Window exits.** When the spot leaves a window, the tick rebuilds that
  contract's ladder from its record, centred on the new spot. The budget is 32
  rebuilds per tick; past it the record serves that tick (`Fallback`) and the
  contract rebuilds on a later tick. The tick never allocates and never touches
  the pool.
- **Record box (review fix round 1).** Every window is clipped to the spot range
  that keeps the contract's k inside its record's [k_min, k_max], and a contract
  whose σ is outside the record's σ box gets no window. A spot outside the box is
  served from the record, flagged `Fallback`, without spending the rebuild
  budget. So an `Ok` or `Exercised` lane is always inside the box its
  certificate covers. At `dcb6e3f1` a rebuilt window could run past the box
  edge; the clamped record values then went out as `Ok` (see "Review fix
  round 1").

### Item 3: IV against the record

`ExpiryRecord::implied_vol` runs Newton on the record's own price and vega,
seeded at the surface vol and safeguarded by a bisection bracket inside the σ
box. It is capped at 16 evaluations. A one-byte `RecordIvStatus` refuses
ill-posed targets instead of guessing:

- `BelowIntrinsic`.
- `Below/AboveBox`; the box ends are tested directly.
- `VegaFloor`: the price is flat in σ. This includes an in-the-money target at
  intrinsic, which every σ up to the exercise edge reproduces. The cold-seeded
  bench row found that case: Newton had returned the far end of the flat
  interval as "Ok".
- `NoConvergence`.
- `Invalid`.

`implied_vols_from_records` is the chain-level entry.

## Error certification

- **Record.** `build()` evaluates the finished record against the cold accurate
  AL solver on an off-node grid, with a fresh accurate boundary per check σ. The
  grid crosses both box ends and every σ midpoint between nodes with every k
  midpoint on the continuation side, the box ends, and four exercise-side points.
  That is 221-273 points per record. The maximum price error is stored in forward
  units. This is a measured bound on the grid, not a proof between grid points.
  - Debug test grid (T = 0.02 / 0.25 / 1.5, both sides): ≤ 1.3e-8 forward units.
  - The 40 records of the bench chain: ≤ 3.3e-7 forward units, about $2e-4 at
    F ≈ 600.
- **Ladder.** At build and at every rebuild, the price series is compared with
  its record at all 15 node midpoints and both domain ends. The certificate is
  that maximum plus F_max × the record's certificate, with F_max taken at the
  window's top. When it exceeds `max_cert_usd` ($5e-4), the window halves down
  to 0.25%. A contract still over is refused and served from its record every
  tick, and so is any contract whose record is uncertified (`certify = false`).
  The certificate holds only inside the record's (k, σ) box, which is why
  windows are clipped to it. In the bench chain no contract is refused; the
  worst certificate is $2.3e-4. The series reproduces its record to price
  2.9e-9 and delta 1.8e-8 (test chain).
- **Greeks are not certified per contract.** They are measured against
  `american_greeks_al`, which is a finite difference with a fixed
  hS = 1e-3·S. Near the exercise boundary that stencil straddles the kink, and
  the reference itself is wrong there.
  - A 1-day put 3.3% in the money has reference gamma 1.37e-3 against the
    ladder's one-sided 1.80e-3, so the PDE theta flips sign: −0.89 against
    +3.21. That one contract is the 4.1/yr theta maximum.
  - Exercised contracts show reference gamma ≠ 0 where both sides price at
    intrinsic.
  - The Greek maxima are therefore reference-limited; the p99 values are the
    meaningful figures.

## Provisional numbers (unqualified; one process, same run, 2026-10-16)

Each figure is the median of 5 repetitions; the main thread was pinned to CPU 7.
The receipt is `C:\atx-cache\quote-benchmark\tick-path-20261016-item3\`; host
load was 90% by the end of the run.

| route | p50 µs | p99 µs | max µs | CV p50 | price max $ | price p99 $ | Δ p99 | Γ p99 | vega p99 | θ p99 /yr |
|---|---|---|---|---|---|---|---|---|---|---|
| cached_scalar | 5,368.5 | 6,063.6 | 6,786.9 | 6.3% | 0.577 | 0.545 | — | — | — | — |
| cached_ladder | 5,015.9 | 5,916.3 | 9,646.8 | 4.8% | 0.577 | 0.545 | — | — | — | — |
| cold_al_qlfast | 150,621 | 165,399 | 165,927 | 8.3% | 6.4e-3 | 3.6e-3 | — | — | — | — |
| cold_al_avx2_batch | 361,454 | 413,189 | 413,717 | 14.3% | (this route *is* the reference) | | | | | |
| record_direct | 696.0 | 977.6 | 1,626.8 | 11.1% | 1.07e-4 | 3.7e-5 | 1.4e-4 | 1.2e-5 | 6.4e-3 | 0.138 |
| record_price_only | 283.2 (142 ns/contract) | 460.2 | 648.0 | 10.8% | 1.07e-4 | 3.7e-5 | — | — | — | — |
| **record_ladder** | **94.3** (47 ns/contract) | **172.5** | 442.4 | 3.3% | 1.08e-4 | 3.9e-5 | 1.3e-4 | 1.0e-5 | 6.1e-3 | 0.145 |

record_ladder p99.9 is 218.8 µs. It made 62 in-tick rebuilds over 10,000 ticks
(6.2 per 1k ticks), with 0 fallbacks and 0 refused contracts. Ladder build takes
23 ms and record build takes 299 ms for 40 certified records; both are untimed
refit costs. The cached rows' Greek maxima against cold are Δ 0.076, Γ 2.8e-3,
vega 60 and θ 4.1.

**Correction (review of `dcb6e3f1`, finding 3).** An earlier version of this
note blamed the cached route's $0.577 on q_rep / wide-box drift. That is wrong.
The reviewer's one-factor probe over all 2,000 contracts gives:

- the session recipe: $0.969 max;
- exact per-expiry q: $1.13;
- a tight T box: $0.97;
- exact q plus tight T, k and σ boxes: still $0.534 max, with 561 contracts
  over $1e-3.

The dominant error is the 16×8×12 CorrectionCache tensor interpolating across
the exercise boundary's kink. The record avoids it because its k axis stops at
the edge.

Other runs on the same day (receipts `…-item2`, `…-item4`):

- record_ladder p50 69-113 µs across runs.
- record_price_only 118-129 ns per contract.
- IV (`…-item4`): warm 0.90-1.25 µs per IV at 3.5 evaluations, cold 1.5 µs at
  4.4.
  - Worst |iv − σ_true| is 1.4e-6 vol (p99 2.3e-7) over 1,720 Ok contracts;
    280 were refused as ill-posed (deep ITM / at intrinsic).
  - On the Debug test grid it is 1.7e-8.

Against the audit targets, unqualified:

- **Chain refresh ≤ 300 µs: met.** p50 ~70-113 µs and p99 ~125-181 µs, with
  ≥1.7× headroom at p99.
- **Cached price ≤ 150 ns: appears met.** ~118-142 ns per contract in-chain. This
  needs quiet qualification and an `amer/american_price_record/*` micro-row.
- **American IV ≤ 500 ns: NOT met.** ~0.9-1.25 µs warm.
- **Cold AL ≤ 15 µs** was out of scope.

Same-run ratios:

- record_ladder is ~57× cached_scalar and ~1,600× cold_al_qlfast, at
  ~5,300× / ~60× better price accuracy.
- record_direct is ~7.7× cached_scalar.

## Gates

All runs used the `vol-dev` preset for correctness, `vol-hygiene` for PCH-off and
`vol-rel-avx2` for timing, at `-Jobs 2`. Only focused executables were built,
never the monolithic `atx-vol-tests` (coordinator rule).

| Item | TU checks | Focused ctest `^tick-path:` | PCH-off owners | Umbrella |
|---|---|---|---|---|
| 1 | 4 (rel-avx2) | 31/31 (ChainTickCached 5, CpuTopology 2, PricingExecutor 24) | 5/5 | 9/10 |
| 2 | 3 (dev) | 47/47 (adds ExpiryRecord 11, ExpiryRecordBank 1, ExpiryRecordSeam 1, ChainTickRecords 3) | 6/6 | 9/10 (Tier-B 33 → 34) |
| 3 | via build | 57/57 (adds ChainSpotLadder 10) | 6/6 | — |
| 4 | via build | 61/61 (adds RecordImpliedVol 4) | 5/5 | 9/10 |

- The one umbrella failure, `VolUmbrella.NoFixturePathResolvedOutsideTheSharedResolver`,
  is pre-existing. It flags absolute path literals in `chain_export*` /
  `oracle_bench*` files this lane never touched.
- Bench: `atx-vol-chain-refresh-name-coverage` 1/1, and `--atx-self-check` 0
  failures with the main thread pinned.
- The allocation tests use a thread-local counting global `operator new` that
  is linked only into `atx-vol-tick-path-tests`. They prove 0 allocations for
  the cached route, the record route, record queries, the ladder tick
  (including rebuild ticks) and the IV chain entry.
- Bit-identity of untouched paths:
  - `refresh_chain_cached` reproduces `american_greeks` bit-for-bit.
  - The record route reproduces `ExpiryRecord::greeks`, and a ladder fallback
    reproduces the record bit-for-bit.
  - Through the new seam forwarders, the price-from-boundary path is still
    bit-identical to `andersen_lake`.
  - The `american.cpp` / `american_boundary.hpp` edits are additive forwarders.
  - `CorrectionCache`, the session and `PricedSurface` are unchanged, so their
    suites in the monolithic exe were not re-run here.

### Gates after the rebase (code HEAD `d8f8772d` on `da3a4179`)

These gates were run under the root's new lane rules: vol-only presets and
`-Jobs 3`.

| Gate | Preset | Result |
|---|---|---|
| Build `atx-vol-tick-path-tests` + `atx-vol-umbrella-tests` | `vol-rel` (fresh configure) | OK, 249 steps in 43 s |
| `-Ctest -R "^tick-path:"` | `vol-rel` | **61/61** |
| `-Ctest -R "^atx-vol-umbrella$"` | `vol-rel` | 9/10 cases. The failure is the same pre-existing `NoFixturePathResolvedOutsideTheSharedResolver` (chain_export* / oracle_bench* only). `TierCountsMatchTheReadmeTable` passes at Tier-B 36 |
| PCH-off build of `atx-vol-tick-path-tests` | `vol-hygiene` (reconfigured) | OK, 156 steps |
| `-Ctest -R "^tick-path:"` | `vol-hygiene` | **61/61** |
| Build `atx-vol-chain-refresh-bench` | `vol-rel-avx2 -Bench` | OK |
| `--atx-self-check` | `vol-rel-avx2` | 0 failures; main cpu 7, observed 7, P-core |
| `^atx-vol-chain-refresh-name-coverage$` | `vol-rel-avx2` | 1/1 |

There was also a confirmation timing run, UNQUALIFIED, with host load 79% before
and 63% after; the receipt is `C:\atx-cache\quote-benchmark\tick-path-20261016-rebased\`.

- record_ladder: p50 77.5 µs (38.8 ns per contract), p99 106.4 µs, p99.9
  131.6 µs, max 388.4 µs, CV 4.3%.
- record_price_only: p50 257.8 µs, p99 430.6 µs.

The accuracy counters match the pre-rebase run to the printed digits: price max
$1.0753e-4, p99 $3.94e-5, certificate $2.27e-4, 62 rebuilds, 0 fallbacks.

## Review fix round 1 (review of `dcb6e3f1`: BLOCK)

The review is `C:\atx-cache\quote-benchmark\tick-path-dcb6e3f1-review-v1\review.md`.
The fix was made in the leased tree pool-4 (run `vol-20261016-tickfix`).

**MAJOR (finding 1), fixed.** A rebuilt ladder window was never intersected with
the record's (k, σ) box, so past `spot_move` the ladder served box-clamped
record values as `Ok` under an in-box certificate. Against the cold solver that
was $6.0e-3 at +6% and $1.9e-2 at +8% on the test chain, and $4.5e-3 at +6% on
a 600-contract bench subset. `build_one` now does three things:

- It clips the window to S ∈ [K e^{−k_max}/g, K e^{−k_min}/g], pulled in by a
  relative 1e-12.
- It gives a contract whose σ is outside the record's σ box no window.
- It leaves the window empty when the centre spot is outside the box.

On the tick side, a pending lane whose spot is outside its box is served from
the record as `Fallback` and does not spend the rebuild budget. When the spot
comes back inside the box, the lane is rebuilt.

`WindowExitRebuildsWithinBudgetThenCatchesUpNextTick` used to assert the bug at
+6%. It now moves +4.5%, which leaves every window but stays inside every box.
The new `WalkPastTheRecordBoxNeverServesAnUncertifiedLane` walks ±10% and back
in 0.25% ticks with an unlimited budget. At each tick it asserts:

- no `Ok` / `Exercised` lane is outside its box;
- every out-of-box lane is `Fallback`;
- every lane strictly inside its box is served.

At every 1% it also checks each served lane against cold accurate AL, within its
certificate plus 1e-6. Over 1,260 cold checks the served lanes are at worst
$1.58e-6 off, and the largest error minus certificate is −2.8e-14. There were
540 out-of-box lane-ticks, all `Fallback`; their clamped-record error reaches
$3.4e-2 and carries no certificate.

The reviewer's probe, re-run on the fixed code (receipt
`C:\atx-cache\quote-benchmark\tick-path-20261016-fix1\probe-ladder-box-after-fix.log`),
shows 0 out-of-box `Ok` / `Exercised` lanes at every checkpoint: ±1..8% on the
test chain and ±1..6% on the bench subset. The served-lane maximum is
$9.5e-7 on the test chain and $1.17e-4 on the bench subset. Out-of-box lanes
are `Fallback`, at up to $1.9e-2 (test chain, +8%) and $4.4e-3 (bench subset,
+6%), the same numbers the record route reports.

**MINOR, fixed:**

- **Finding 4.** An uncertified record (NaN) now refuses its contract. Its
  certificate is NaN and the record serves it, flagged `Fallback`.
- **Certificate tightening.** The record's dollar certificate now uses F at the
  window's top rather than at its centre, so the bench's worst ladder
  certificate moved from $2.27e-4 to $2.36e-4.
- **Finding 5.** `pricing_executor_test.cpp` was removed from
  `atx-vol-tick-path-tests`. PricingExecutor is owned by
  `atx-vol-pricing-slice` / `atx-vol-tests`, so `atx_vol_fast` runs each case
  once.
- **Finding 3.** The attribution of the cached route's error is corrected here,
  in the `expiry_record.hpp` header comment, and by an appended LEDGER
  correction line.

**NITs, fixed:**

- **Finding 6.** Documented that the cached route's refusal path allocates.
- **Finding 7.** `OutOfWindow` is documented as reserved, since no route emits
  it.
- **Finding 8.** Documented that the record routes read only strike and σ from
  the chain.
- **Finding 9.** `refresh_chain_records` now applies the out-of-box `Fallback`
  test before `Exercised`; the test asserts `Fallback` exactly where the record
  box excludes the contract.
- **Finding 14.** The provisional baseline JSON now carries an
  `atx_baseline_status` context key.

Findings 10-13 (`build()` length, skipped cert rows, unclamped Greeks under a
clamped price, and main-thread / pool CPU sharing) are left open.

### Gates (fix round 1, vol-only presets, `-Jobs 3`, focused targets only)

| Gate | Preset | Result |
|---|---|---|
| `-Ctest -R "^tick-path:"` | `vol-rel` | **38/38**. That is 61 − 24 PricingExecutor cases (deduplicated) + 1 new walk test: ChainTickCached 5, CpuTopology 2, ExpiryRecord 11, ExpiryRecordBank 1, ExpiryRecordSeam 1, ChainTickRecords 3, ChainSpotLadder 11, RecordImpliedVol 4 |
| Allocation tests (`--gtest_filter=*Alloc*`) | `vol-rel` | 5/5: ChainTickCached.HappyPathTickAllocatesNothing, ExpiryRecord.QueriesAllocateNothing, ChainTickRecords.ShapeErrorsAreRefusedAndTheTickAllocatesNothing, RecordImpliedVol.ChainEntryMatchesPerContractAndAllocatesNothing, ChainSpotLadder.TicksAllocateNothingEvenWhenTheyRebuild (its +7% tick now also covers the out-of-box record path) |
| PCH-off build + `^tick-path:` | `vol-hygiene` | OK, **38/38** |
| `--atx-self-check` / `^atx-vol-chain-refresh-name-coverage$` | `vol-rel-avx2 -Bench` | 0 failures / 1/1 |

There was also a `record_ladder` confirmation run, UNQUALIFIED, with host load
39% before and 85% after:

- Timing: p50 96.7 µs, p99 159.4 µs, p99.9 196.3 µs.
- Counts: 62 rebuilds, 0 fallbacks, 0 refused.
- Accuracy: price max $1.075e-4, p99 $3.94e-5; Greek p99s unchanged from the
  earlier runs.

The bench tape stays inside every record box, so the fix does not change its
accuracy.

## Unfinished

1. **Quiet-host qualification.** Nothing is pinned. Re-run
   `atx-vol-chain-refresh-bench` best-of-3 processes on a quiet host, then
   replace the `-provisional` candidate with
   `i7-1260p-clang18-avx2-chain-refresh.json`.
2. **`compare_baseline.py` named-counter comparison (§4).** Not extended, so
   `p50_us` / `p99_us` are not yet gateable by the script.
3. **§4 variants.** `/div2` (two dated dividends, V&N reference),
   `/spy_real`, and the opt-in `markov_batch` row are not implemented.
4. **Dated dividends in records.** There is only the typed hook, which refuses.
   The dividend axis and the V&N certification are missing.
5. **Serving integration.** Records and ladders are not wired into the session
   / `PricedSurface` / `QueryAccelerator` path; they are opt-in APIs. The
   default cached route is unchanged, including its tensor-across-the-kink
   error. The session's `cache_serves` gate checks box containment only, so
   in-box contracts near the boundary are still served cached; the serve path
   needs a follow-up.
6. **IV ≤ 500 ns.** ~3.5 evaluations × ~250-350 ns each. Candidate fixes:
   - Halley, or a first step from the surface vega.
   - Collapse the k axis at the strike when the record has no edge in its box.
   - An AVX2 pass across contracts.
7. **Explicit AVX2 kernel.** The ladder relies on auto-vectorization under
   `/arch:AVX2`, which was not verified by disassembly. Under the SSE2 `rel`
   preset the lane loops are at most 2-wide.
8. **Greek certification and reference.** There is no per-contract Greek
   certificate. A sharper Greek reference near the exercise boundary (one-sided
   or smaller-step FD) is needed before the Greek maxima mean anything.
9. **Record build cost.** 299-356 ms for 40 records, dominated by 26 accurate
   boundary solves plus 13 certification solves per record. It is not yet
   packed through the AVX2 boundary batch (audit item 7). It also does not use
   a cheaper sampling scheme.
10. **Micro-rows.** The audit's `amer/american_price_record/*` and
    `american/iv/record_{warm,cold}` micro-rows in `american_pricing_bench` /
    `american_shootout_bench` were not added. The in-chain rows above stand in
    for them.
