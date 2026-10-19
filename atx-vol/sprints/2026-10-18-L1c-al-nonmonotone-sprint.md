# L1c: the non-monotone Andersen-Lake region (map, then refuse; opt-in)

Lane `feat/vol-L1c-al-nonmonotone-20261018`, vol-sota sprint wave 1 (plan
`docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md`, W1 "L1c"). Started 2026-10-18 (checkpoint
`b0a15fb1`), resumed and finished 2026-10-19 (`581160eb` code + tests, this note after it).
PM ruling: refuse only. The kernel is called, never changed (`american.cpp` belongs to L2b). Every
behaviour is opt-in; the default paths are byte-identical.

## 1. What and why

The ledgered trap (LEDGER 2026-10-17 `| trap |`) says that `andersen_lake` is not monotone in sigma
on long-dated deep-ITM puts, and that `american_implied_vol` returns `Ok` answers 0.02-0.18 vol off
on some dev200 long-dated deep-ITM rows. This lane maps where the cold map is not monotone. It then
adds a typed refusal, `NonMonotoneRegion`, to the scalar inverter and to both chain-batch IV
routes. The refusal stays behind a flag that is off by default. It also pins the deferred
batch-pricing minor N2: the PerContract 4-step polish ends in `NoConvergence` on long-dated
deep-ITM puts that Lockstep solves.

## 2. Region map (the kernel is called, not changed)

The probe is `C:/atx-cache/quote-benchmark/L1c-20261019-refusal-counts/nonmonotone_map.cpp`, a
byte copy of the 2026-10-18 probe (sha256 `ba585693...`). It uses the public API only and was
built against the lane library with `-DL1C_HAVE_REFUSAL`.

**Method.** Each cell is priced with `andersen_lake` on a sigma grid: 0.005 to 1.0 in steps of
0.0025, then to 2.0 in steps of 0.01. Three quantities come from that scan:

- **Dip:** the largest drawdown along the grid.
- **Ambiguous band [L, U]:** the targets that have more than one root.
- **Exercise-region flag:** whether the spot is on the exercise side of
  `exercise_boundary(K, T, sigma, r, q, side, opts)` at each point (put S <= B, call S >= B).

**Grid.** Four presets: accurate, `al_fast_profile(nullopt)`, `al_fast_opts`, `al_bulk_opts`.
Puts with K/S, and the mirror calls with S/K, in {1.00..1.10 step .01, 1.15..1.60 step .05}.
T in {0.1, 0.25, 0.5, 1, 1.5, 2, 3, 5}. Eight (r, q) pairs. That gives 2,688 cells per preset.

**dev200.** The 10,694 kernel-input rows from `al-speed-20261017-dev200-kernel-inputs`. Rows
whose target lies within 1e-3·K·max(1,T)² of the zero-vol value are scanned (2,484 rows).

Results (`map/map-summary.txt`; the 2026-10-18 full-scan dev200 file is in
`L1c-20261018-nonmonotone-map/base`):

| preset | cells with a dip | max ambiguous-band top above zero-vol, / (K·max(1,T)²) | decreasing steps not starting in the exercise region |
| --- | --- | --- | --- |
| accurate | 387 / 2,688 | 1.101e-6 (grid); 1.159e-6 (dev200 row 10333, 2026-10-18 full scan) | 0 (2 steps straddle the edge) |
| Fast profile | identical to accurate | 1.101e-6 | 0 (2 straddle) |
| `al_fast_opts` | 927 | 9.08e-5 | 0 (21 straddle) |
| `al_bulk_opts` | 823 | 3.18e-5 | 0 (27 straddle) |

- **Every decreasing step lies in the kernel's own exercise region.** The "outside" steps the
  probe counts (2 / 21 / 27 per preset) are all the single grid step that crosses the exercise
  edge. Its later point is the first point past the edge (`sig_dec_hi == sig_edge` in every
  case). The largest price the kernel prints inside the exercise region equals the
  ambiguous-band top in every cell.
- **Envelope (accurate preset).** Dips need an early-exercise carry advantage of at least
  +0.015 (put r−q, call q−r). The only exceptions are 15 tiny call cells at −0.015. Dips also
  need T >= 0.1 and moneyness >= 1.01.
- **dev200 (resume scan).** 20 of the 2,484 scanned rows have a dip; 12 targets are ambiguous.
  The largest band top among the scanned rows is 1.33e-7 (row 246). No decreasing step is
  outside the exercise region. The 2026-10-18 scan of all 10,694 rows found 506 rows with a
  dip. All targets outside the scan window lie far above any dip, so they have a single root.
- **The ledgered contract reproduces exactly.** Put S=100 K=128.989 T=2 r=.05 q=.015:
  +7.745e-6 at sigma 0.14, exactly the zero-vol value on [0.17, 0.18], +7.4027e-4 at 0.185.
  Six decreasing 0.005 steps from 0.145 to 0.17 all lie in the exercise region. B(T) falls from
  108.5 at sigma 0.14 to 99.77 at 0.185.

## 3. Gate constants (confirmed unchanged)

The gate is `al_non_monotone_gate(K, T, opts) = kappa · K · max(1, T)²`:

| scheme | kappa | largest measured band top | headroom |
| --- | --- | --- | --- |
| accurate, `nullopt` or its explicit Fast-profile equivalent | 5e-6 | 1.159e-6 | 4.3x |
| `al_fast_opts` (wide value) | 4e-4 | 9.08e-5 | 4.4x |
| `al_bulk_opts` (wide value) | 4e-4 | 3.18e-5 | 12.6x |

No grid cell and no dev200 row has a band top above its gate. Any other scheme takes the wide
value. The map confirms the checkpoint constants, so nothing changed.

The scalar certificate inside the band uses three settings: a chord over [x(1−1e-4), x], a
slope floor of 1e-8·K (the chain batch's default `vega_floor`), and a Newton correction of at
most 1e-4 vol.

## 4. The refusal (opt-in)

- **Scalar.** `american_implied_vol(price, S, K, T, r, q, side, const IvOpts&)` returns an
  `IvResult` with `IvStatus`. With `refuse_non_monotone` false it is the positional overload
  bit for bit. With the flag true, the cold Andersen-Lake route refuses an `Ok` answer x only
  when the target is in the band, and then only if either:
  - x lies in the kernel's exercise region (`al_non_monotone_region`), or
  - x > kIvMin and the cold map does not certify it.

  BAW and the cached maps never refuse.
- **Chain batch.** `ChainIvOptions::refuse_non_monotone` turns on rule 3 in `batch_chain.hpp`.
  A row whose target is in the band becomes `ChainRowStatus::NonMonotoneRegion` in either case:
  - **(a) Falling chord:** a resolved chord falls faster than vega_floor·K (PerContract's
    verification chords, Lockstep's iterate chords).
  - **(b) Ok in the region:** its `Ok` vol satisfies `al_non_monotone_region`.
- **Resume fix (`581160eb`): trigger (a) is now gated on the band too.** Without the gate,
  Lockstep refused 7 dev200 calls (kernel-input rows 9104-9116: S=152.32, T=0.254,
  K=68-74). Their targets were 0.43-0.63 USD above the zero-vol value, about 1,000x the gate.
  Lockstep's first iterates had crossed the low-vol dip on the way to the one root.
  `dev200/dev200-rows.csv` holds that run; `dev200-rows-v2.csv` is the gated run.

## 5. dev200 flag-on counts

Source: `dev200/counts-v2.txt`. Run: one run at `ATX_VOL_FIT_WORKERS=2`, 333 s, unqualified;
host load 35-48% before, 1-31% after. Target = cold AL at the row's sigma; batch seed 0.3. Every
status flip is X -> NonMonotoneRegion, and no `Ok` value changes.

| route | NonMonotoneRegion | flag-off Ok > 1e-4 vol off: caught / left | refusals of a flag-off Ok within 1e-4 | 12 multi-root targets: refused / Ok left |
| --- | --- | --- | --- | --- |
| scalar (`IvOpts`) | 660 | 660 / 43 | 0 | 11 / 1 |
| PerContract | 1 | 0 / 0 | 1 (row 8732) | 1 / 0 (11 already refused flag-off) |
| Lockstep | 1 | 0 / 3 | 1 (row 8732) | 1 / 3 |

What remains:

- **Scalar, 43 rows still Ok and off:**
  - 40 are kIvMin clamps at the zero-vol value outside any exercise region. These are the
    documented positional contract; the chain routes call them VegaFloor.
  - 1 is row 10649 (below).
  - 2 are 1.1e-4 / 6.4e-4 vol warm-search misses on short-dated deep-ITM calls (rows 2447 and
    10022), outside the region.
- **Row 8732 (call K=80, T=0.029).** Its target is ambiguous and the true sigma lies in the
  exercise region. It is refused by design.
- **The three long-dated puts 10649 / 10651 / 10653** (T=2.26, r−q=.032):

  | row | scalar | PerContract | Lockstep |
  | --- | --- | --- | --- |
  | 10649 | keeps 0.1563 | NoConvergence (both flags) | keeps 0.1562 |
  | 10651 | refuses (was 0.1418, in region) | NoConvergence (both flags) | keeps 0.1800 |
  | 10653 | refuses (was 0.0794, in region) | VegaFloor (both flags) | keeps 0.2022 |

  Every kept answer is the root just past the exercise edge. That is the root a kernel pricing
  intrinsic in its exercise region would return (section 8). The generating sigma (0.147) is
  itself in the exercise region, so its target is an artifact price.

On the grid round trip (`map/counts-grid-roundtrip-pre-gate.txt`, 16,128 targets), the scalar
refuses 3,457 answers. 3,456 of them were off by more than 1e-4 vol, and 1 is a false refusal
whose true sigma lies in the exercise region. The batch columns of that file predate the
section-4 gate.

## 6. Tests (implementation first, then tests; each fails when its behaviour is reverted)

- **`pricing/AmericanIvNonMonotone.*`** (5 tests, `american_iv_test.cpp`):
  - the ledgered-contract pin (dip only inside the exercise region);
  - the gate values;
  - flag-off identity with the positional overload over 432 deep-ITM targets, with flag-on
    changing nothing but Ok -> NonMonotoneRegion, inside the band;
  - refusals inside the region (both triggers, plus the in-region kIvMin clamp);
  - answers outside the region kept bit-identical (including a European-regime clamp and BAW).
- **`chain-batch:ChainBatchIv.*`** (4 new tests, `batch_chain_test.cpp`):
  - the N2 pin `PerContractPolishRunsOutWhereLockstepSolves`. Two rows, the ledgered contract
    at AL(0.14) and K=140 T=5 at AL(0.2): PerContract returns NoConvergence, Lockstep returns
    roots of the cold map;
  - `NonMonotoneRegionSurfacesOnBothRoutesWhenOptedIn`: both triggers on both routes;
  - `NonMonotoneFlagOnlyEverTurnsRowsIntoRefusals`;
  - `NonMonotoneFlagNeverRefusesTargetsAboveTheBand`, which uses dev200 rows 9100-9118.

Revert checks. Each was an Edit, then a build of the owning target, then a run, then the
inverse Edit. Restored blobs: `american_iv.cpp` `b4dddae8`, `batch_chain_iv.cpp` `43840ecc`.

| revert | test that fails |
| --- | --- |
| `ok_in_region` disabled | SurfacesOnBothRoutes: PerContract rows 0 and 2 become Ok; Lockstep rows 2 and 4 become Ok |
| falling chord disabled | SurfacesOnBothRoutes: PerContract rows 1 and 5 become VegaFloor; Lockstep row 1 becomes Ok |
| band gate removed | NeverRefusesAboveTheBand: 7 Lockstep refusals at K=68-74 |
| N2 polish exit changed to VegaFloor (mutation) | PerContractPolish: rows 3 and 4 become VegaFloor, expected NoConvergence |
| scalar refusal disabled | FlagOff...OnlyRefuses (0 refused); RefusesWrongOk... (5 cases return Ok) |

## 7. Gates (lane SHA `581160eb`, `-Preset vol-rel`, `-Jobs 2`)

- **pricing/ and simd/ name sets:**
  - Cap 3: 906 tests, 2 failed. The failed/skipped name set is byte-equal to the base set
    `52324ef8` (W0 root = prebase source tree).
  - Cap 2: the same 2 failures. The skip set adds 3 `PricingExecutor` tests that skip when the
    worker count is that low. The base could not be rebuilt at cap 2 in the lease tree,
    because the branch switch was refused.
- **chain-batch:** 31/31 (27 at base, plus 4).
- **`AlFastProfile`:** 6/6.
- **`atx-vol-umbrella`:** 7/10. The 3 failures are pre-existing and none is a file in the lane
  diff: README Tier-B count 38 vs 42 headers, and fixture-path literals in
  chain_export / fit_archive / oracle_bench tests. The `api/` header count is 101 at base and
  at head.
- **PCH-off (`vol-hygiene`):** the 19 direct includers of `american_iv.hpp` / `batch_chain.hpp`
  (library, tests and tools; 26 objects) compiled with exit 0 and 0 diagnostics.
- **AL kernel fingerprint (`vol-rel-avx2`):** `53c4df68...dcea` MATCH.
- **dev200 default parity:** `c89d07393c90ad86...` MATCH (`parity/runs.log`).

## 8. Follow-up hypothesis for L2b / L3b (not changed here)

The cold put price never tests the exercise region. Every site computes
`price = euro + prem`, then floors it at the intrinsic and at the European value:

- `atx-vol/src/pricing/american.cpp:2290-2296`, in `al_put_price_from_boundary_cached`, which
  starts at :2275;
- `atx-vol/src/pricing/american.cpp:2548-2554`, in `al_put_price_from_boundary`, which starts at
  :2541;
- the laned AVX2 kernel `atx-vol/src/simd/american_boundary_avx2_kernel.hpp:621-626`, which
  Lockstep prices with.

Calls go through the same put kernel by symmetry.

**Hypothesis.** When S <= B(T) (put), the exact American value is the intrinsic K − S. Pricing
intrinsic there would make P(sigma) flat up to the exercise edge and monotone after it. Every
in-region root would disappear, and an ambiguous target would get the past-edge root that
Lockstep already returns on the 3 long-dated puts.

That change moves default prices by up to 1.2e-6·K·max(1,T)² (accurate). It must therefore be
opt-in, or fingerprint-gated, in all three sites at once; otherwise PerContract and Lockstep
diverge. L3b wires `NonMonotoneRegion` into the fitter. This lane only makes the refusal exist.

## 9. Ledger candidate

`2026-10-19 | trap | ANDERSEN-LAKE NON-MONOTONICITY LIVES ONLY IN THE KERNEL'S OWN EXERCISE REGION: every decreasing step of andersen_lake(sigma) starts with S on the exercise side of exercise_boundary (put S<=B), where the kernel prints euro+premium floored at intrinsic instead of intrinsic; the ambiguous band top is <= 1.16e-6*K*max(1,T)^2 (accurate = Fast profile), 9.1e-5 (al_fast_opts), 3.2e-5 (al_bulk_opts); opt-in refusal IvOpts/ChainIvOptions::refuse_non_monotone with gate 5e-6 / 4e-4 refuses 660 / 1 / 1 dev200 rows (scalar / PerContract / Lockstep), 0 fine scalar answers | C:/atx-cache/quote-benchmark/L1c-20261019-refusal-counts/sha256.txt`
