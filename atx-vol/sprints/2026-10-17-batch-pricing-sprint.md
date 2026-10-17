# Batch-pricing lane: chain-level batch price / Greeks / IV (2026-10-17)

Lane `batch-pricing`, branch `feat/vol-batch-pricing-20261017`, base `24e01644`,
pool-35. Brief: `.superpowers/sdd/2026-10-17-vol-resume5/lane-batch-pricing-brief.md`.

## 1. Design (written before any code)

**Problem.** A caller that re-marks a listed chain (N contracts sharing spot, rates
and dividends; many expiries, many strikes) today loops over contracts and calls
`american_greeks_al` / `andersen_lake` / `american_implied_vol` once per contract,
re-deriving the per-expiry carry each time and running on one thread. The batch API
takes the chain once, as columns, and returns columns.

**Inputs (structure of arrays, caller-owned spans, read only).**
- `ChainExpiryColumns` — one entry per expiry `e`: `T[e]` (year fraction > 0),
  `rate[e]`, `carry[e]` (continuous q: borrow + dividend yield), optional `div_pv[e]`
  (PV of the cash dividends paid before the expiry; empty = none).
- `ChainContractColumns` — one entry per contract `i`: `strike[i]`, `expiry[i]`
  (`uint32_t` index into the expiry columns), `side[i]`, `sigma[i]` (the pricing vol;
  for implied vol, the warm seed).

**Outputs (caller-owned spans, written in input order).** `ChainGreekColumns`:
`price` (required), `delta`, `gamma`, `vega`, `theta`, `rho` (each optional: an
empty span is not computed, and a non-requested vega/rho skips its boundary solves),
`status` (one `ChainRowStatus` byte per row, required). Implied vol writes
`iv_out` + `status_out`.

**Per-row status, never an exception.** `Ok`, `InvalidInput` (the row failed
boundary validation and was never sent to the kernel), `Unpriceable` (the kernel
refused it or returned a non-finite requested output), and for implied vol
`VegaFloor` (ill-posed: an ITM target at intrinsic, a root at the vol floor, or a
slope under the floor), `OutOfBand` (target outside [intrinsic, upper bound]),
`NoConvergence`. Non-Ok rows hold NaN in every requested column. The call itself
returns `Result<std::size_t>` (the row count); only call-shape errors travel there
(mismatched lengths, aliasing outputs, a non-finite spot, a route/option clash).

**Hoisted per-expiry work.** Once per call, serially, before any fan-out: expiry
validity, the effective carry `q_eff = q - log1p(-D/S)/T` (exactly `q` when `D == 0`),
the discount factor and the forward factor. Every contract of that expiry reads them.
The time grids and quadrature nodes of the Andersen-Lake solve are NOT reachable
through the kernel's public API at `24e01644` (they are rebuilt inside every
`andersen_lake` / `american_greeks_al` call); sharing them needs a kernel
interface, requested in the lane report rather than built here (al-speed owns the
kernel).

**Routes.**
- `Scalar` (default): per row, exactly the call a per-contract loop makes —
  `andersen_lake` for a price-only request, else `american_greeks_al(..., need_vega,
  need_rho, need_charm=false)` — at the hoisted `q_eff`. Bit-identical to that loop by
  construction; proven by hashes on the chain-refresh bench chain and a dev200 chain.
- `LanedAvx2` (opt-in): the public laned AVX2 Andersen-Lake Greeks kernels
  (`simd::american_{put,call}_greeks_batch`, `ForceAvx2`), 4 same-side contracts per
  pack, scalar patches inside the kernel. Within the kernel's documented economic
  gate, not bit-identical; measured max |diff| vs Scalar per column.
- Implied vol `PerContract` (default): `american_implied_vol` per row, warm-started
  at the row's own sigma (the serve-path warm-seed rule), behind an intrinsic /
  upper-bound screen that reports `VegaFloor` / `OutOfBand` instead of the scalar's
  "clamp to kIvMin and succeed". (Superseded by the shared refusal rule of §7.)
- Implied vol `Lockstep` (opt-in): 4 contracts at a time run a safeguarded
  Newton-secant in lockstep (lane arrays for iterate, bracket, residual and slope;
  the first step's slope is the closed-form Black-76 vega at the forward), each lane
  pricing through its own warm-started `AloPricer` from the workspace, no cold
  polish. Measured max |diff| vs PerContract.

**Allocation and exceptions.** The call allocates nothing once the
`ChainBatchWorkspace` is sized (`reserve(max_expiries, max_contexts)`; an
undersized workspace grows once inside the call). The workspace owns the per-expiry
arrays and, for Lockstep, 4 `AloPricer`s per execution context. Kernel `bad_alloc`
is contained per row (`Unpriceable`); a kernel refusal still builds its own
`Error` message (the kernel's allocation, not the batch's).

**Chunking and determinism.** Rows are cut into fixed chunks of 32 in input order
(independent of the thread count). Inline, chunks run in order on the caller; with
an executor, `PricingExecutor::run_dynamic(n_chunks, n_threads, ...)` hands whole
chunks to contexts (caller + pool workers pinned by the existing
`configure_pricing_executor` topology) and the per-context scratch is indexed by the
stable worker id. Each row is written by exactly one context, and a row's value
depends only on its own inputs (Scalar, both IV routes) or on its fixed chunk's
contents (LanedAvx2 pack composition), so outputs are bit-identical for any
`n_threads`. Nested calls inline (executor rule).

**Out of scope here.** Kernel internals, the default quote-bench path (the batch API
is only used when a caller opts in; `c89d0739` must reproduce), storage, pins.

## 2. What changed from the design, and why

- **Lockstep IV no longer uses warm `AloPricer`s.** Measured first: the warm map is
  not the cold map (a warm-started boundary stops its truncated sweep budget at a
  different point), which moved roots by up to 1.1e-6 vol on the test chain, and the
  warm search plus one cold correction measured 1,607 ms on the 2,000-contract chain
  against the loop's 1,776 ms (1.1x; a smoke run of the superseded code, not in the
  receipts; the warm price is not much cheaper than the cold one at the accurate
  preset). The shipped Lockstep prices
  each 4-lane pack in ONE call of the public laned AVX2 put kernel
  `simd::american_put_boundary_batch` (calls through `C(S,K,r,q) = P(K,S,q,r)`; packs
  always 4 wide so the kernel never takes its scalar tail). That map matches
  `andersen_lake` to ~1e-12 USD, needs no cold correction, and is 2.3x faster than the
  loop single-thread. The workspace therefore holds only the per-expiry constants
  (`reserve(max_expiries)`), and `for_each_chunk` needs no per-context scratch.
- The kernel success paths turned out allocation-free (the per-contract loop allocates
  nothing beyond its own output vectors), so "zero allocations" is a measured property
  of the whole Scalar call, not just of the batch layer.

## 3. API (`api/pricing/batch_chain.hpp`, Tier-B)

`price_chain_batch(spot, ChainExpiryColumns, ChainContractColumns, ChainGreekColumns,
ChainBatchOptions, ChainBatchWorkspace&)` and `implied_vol_chain_batch(spot, expiries,
contracts, target, iv_out, status_out, ChainIvOptions, ws)`, both `Result<size_t>`;
`chain_effective_carry(spot, T, carry, div_pv)`; `ChainRowStatus` {Ok, InvalidInput,
Unpriceable, VegaFloor, OutOfBand, NoConvergence}. Opt-in keys: `ChainBatchOptions::route
= LanedAvx2`, `ChainIvOptions::route = Lockstep`; fan-out via `executor` + `n_threads`.
Sources `src/pricing/batch_chain.cpp`, `batch_chain_iv.cpp`, private
`batch_chain_detail.hpp`. Nothing existing calls the API; no default path changed.

## 4. Results (UNQUALIFIED: loaded host, 42-95 % CPU before the runs)

2,000-contract chain-refresh chain (spec hash `5414dd10c5a59ebd`) and the dev200 TSM
chain (2,498 contracts, 19 expiries, dated-dividend PVs). Greeks = price + delta +
gamma + vega + theta + rho, accurate Andersen-Lake (5 boundary solves per contract).
`t<k>` = calling thread (pinned CPU 7, P-core) + k-1 workers of the pool (mask 0x3f, 3
P-cores). Median of 5; per-row CV 4-43 %. Run 1 = same library code before commit,
run 2 = committed `0a42510e`; ratios are within one run against that run's loop row.

| Row | chain run 1 ms | x loop | chain run 2 ms | x loop | dev200 run 2 ms | x loop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| greeks loop (baseline) | 1905.5 | 1.00 | 2846.5 (CV 35 %) | 1.00 | 2159.2 | 1.00 |
| greeks Scalar t1 | 1848.5 | 1.03 | 3219.4 (CV 34 %) | 0.88 | 1902.9 | 1.13 |
| greeks Scalar t2 | 1097.1 | 1.74 | 1218.3 | 2.34 | 949.6 | 2.27 |
| greeks Scalar t3 | 844.1 | 2.26 | 856.3 | 3.32 | 893.2 | 2.42 |
| greeks Scalar t4 | 612.3 | 3.11 | 696.8 | 4.09 | 573.8 | 3.76 |
| greeks LanedAvx2 t1 | 928.6 | 2.05 | 1119.0 | 2.54 | 956.2 | 2.26 |
| greeks LanedAvx2 t3 | 320.3 | 5.95 | 462.3 | 6.16 | 347.6 | 6.21 |
| price loop (baseline) | 406.2 | 1.00 | 462.9 | 1.00 | 463.3 | 1.00 |
| price Scalar t1 / t4 | 395.8 / 110.0 | 1.03 / 3.69 | 470.1 / 137.1 | 0.98 / 3.38 | 475.2 / 119.1 | 0.97 / 3.89 |
| price LanedAvx2 t1 / t3 | 207.9 / 55.1 | 1.95 / 7.37 | 201.5 / 78.1 | 2.30 / 5.93 | 186.5 / 65.6 | 2.48 / 7.06 |
| IV loop (baseline) | 1822.9 | 1.00 | 3693.2 (CV 22 %) | 1.00 | 2121.1 | 1.00 |
| IV PerContract t1 / t3 | 1802.1 / 926.0 | 1.01 / 1.97 | 3123.6 / 771.6 | 1.18 / 4.79 | 2084.0 / 729.9 | 1.02 / 2.91 |
| IV Lockstep t1 / t3 | 791.8 / 264.0 | 2.30 / 6.91 | 776.7 / 315.4 | 4.75 / 11.7 | 934.4 / 380.8 | 2.27 / 5.57 |

Run 2's synthetic loop rows were taken at 79-95 % host load (CV 22-35 %); read the
synthetic ratios from run 1 and dev200 from run 2.

**Target >= 3x single-thread over the naive loop: NOT met.** Single-thread the
bit-identical Scalar route is the loop (1.0x within noise by construction: it makes
the same kernel calls, which are the whole cost, and the public kernel API shares none
of their boundary solves or tables between contracts); the
opt-in SIMD routes reach 2.0-2.5x (Greeks, prices) and 2.3x (IV). 3x is reached only
with fan-out: Scalar t4 3.1-3.8x (Greeks), LanedAvx2 t3 6.0-6.2x (Greeks), Lockstep
t3 5.6-6.9x (IV). Contracts/s: Greeks loop ~1,050 (chain) / ~1,160 (dev200) ->
LanedAvx2 t3 ~6,240 / ~7,190; IV loop ~1,100 / ~1,180 -> Lockstep t3 ~7,580 / ~6,560.

**Identity.** Every Scalar / PerContract row, at t1 through t4, has
`identical_to_loop = 1` and `identical_to_t1 = 1`; every LanedAvx2 / Lockstep row
`identical_to_t1 = 1`. Output hashes (FNV-1a over every output bit and status byte),
identical in both runs:

| Set | Loop = Scalar/PerContract t1..t4 | LanedAvx2 / Lockstep t1 = t3 |
| --- | --- | --- |
| chain greeks | `9f41b247880203d7` | `85384e266ca20d12` |
| chain price | `80a5e844496da9c6` | `7451f5ede4a614a2` |
| chain IV | `b9a8099a9c1ae13a` | `16bf3ea73549928a` |
| dev200 greeks | `e3e9bb01908a6013` | `1815597d519b6ee6` |
| dev200 price | `8f3fed7017bdcfd8` | `58800063b37082ba` |
| dev200 IV | `f2edd4997a78248c` | `3d9795712f012058` |

**Opt-in bounds (max |diff| vs the loop over Ok rows).** LanedAvx2: price 1.1e-12,
delta 7.7e-13, gamma 3.8e-12, vega 2.7e-10, theta 2.4e-7, rho 1.6e-9 (dev200 / chain
maxima). Lockstep IV vs PerContract 1.8e-5 vol; vs the true vol 1.6e-8 on the chain
ONLY (dev200 worst 2.8e-3, the row below), where PerContract itself is 1.8e-5 off
(1.3e-5 on dev200). One dev200 status differs (row 1295): an ill-conditioned call
(American fd vega 9.34e-6 at the root, above the 1e-8 K = 1.85e-6 floor; its whole time
value above the zero-vol American value is 8.47e-8 USD) that PerContract refuses
through american_implied_vol's zero-vol band (|target - zero_vol| <= 1e-9 S + 1e-12 =
4.37e-7 -> Ok(kIvMin) -> VegaFloor) and that Lockstep accepted at 0.306194 against a
root of 0.308999998 (2.8e-3 off) on the price residual alone. 280 / 299 VegaFloor rows
(chain / dev200) are ITM targets at intrinsic, identical on both routes.

*[Corrected in fix round 1 (§7): the original text said "vega ~2e-5", "admits ~1e-3 of
vol" (observed 2.81e-3), "the kIvMin clamp" (the rule is the zero-vol band) and gave the
Lockstep 1.6e-8 unqualified. The IV hashes in the table above are the fe6a516f ones;
§7 has the fixed ones.]*

Receipts: `C:/atx-cache/quote-benchmark/batch-pricing-20261017-bench/` (RESULTS.md,
both JSONs, tables, binary, `sha256.txt`), dev200 sample
`batch-pricing-20261017-dev200-sample/` (`tsm_chain.csv` sha256 `a9b88432...`).

## 5. Gates (vol-rel unless noted; full commands and output in the lane report)

| Gate | Result |
| --- | --- |
| `-Ctest -R "^chain-batch:"` (new focused target) | 20/20 |
| `-Ctest -R "^pricing/(PricingExecutor\|ConfigurePricingExecutor)\."` | 24/24 |
| `-Ctest -R "^atx-vol-umbrella$"` | 9/10: `TierCountsMatchTheReadmeTable` passes with Tier-B 38; the failing `NoFixturePathResolvedOutsideTheSharedResolver` names only chain_export / oracle_bench files this lane does not touch (pre-existing) |
| PCH-off `check` (vol-hygiene) of batch_chain.cpp, batch_chain_iv.cpp, batch_chain_test.cpp | clean |
| bench `--atx-self-check` (vol-rel-avx2) | 0 failures |
| `-Ctest -R "^atx-vol-chain-batch-name-coverage$"` (vol-rel-avx2) | 1/1 |
| Default parity, quote-bench `auto` with binary `0a42510e` | dev200 `c89d0739...` MATCH, dev200b `4b273b08...` MATCH |

Parity receipts: `C:/atx-cache/quote-benchmark/batch-pricing-20261017-parity/`.

## 6. Kernel interface requests (for al-speed; not built here)

1. A prepared per-expiry Andersen-Lake geometry (the sigma- and strike-free time grid,
   Chebyshev nodes and Gauss-Legendre tables) accepted by `andersen_lake` /
   `american_greeks_al` / the laned kernels, so a chain builds it once per expiry.
2. A public boundary handle (`solve` once, `price_from_boundary(S, K)` many): calls of
   one expiry at equal sigma share all five Greek boundaries (their internal strike is
   the spot), and IV could warm-start neighbouring strikes.
3. A warm-seeded laned put kernel (per-lane seed boundary): Lockstep IV spends most of
   each iteration in the per-lane scalar BAW seed of a cold solve.
4. A laned price + sigma-bumped price in one pack (exact Newton slope for IV).
5. (fix round 1) `andersen_lake` is not monotone in sigma on long-dated deep-ITM puts:
   S=100, K=128.989, T=2, r=.05, q=.015 prices intrinsic exactly for sigma <= 0.070,
   rises to intrinsic + 7.7e-6 at 0.140, falls back to exactly intrinsic on
   [0.170, 0.180], then rises (laned map identical). A target in that window has
   several Ok roots; the routes can return different ones.

## 7. Fix round 1 (review of fe6a516f: BLOCK, 2 major) — code `5da11835`

**Rule now shared by both IV routes** (header "One implied-vol refusal rule"):
american_implied_vol's own screen, replicated (price ceiling, immediate intrinsic,
zero-volatility American value, band = max(max(intrinsic_tol, 1e-9) * ceiling,
price_tol * max(1, P)) + 1e-12); Ok only at an evaluated sigma with residual <=
price_tol * max(1, P), a chord slope of evaluated American prices > vega_floor * K and
a Newton correction <= sigma_tol (Lockstep: or both bracket ends evaluated within
sigma_tol). Lockstep never accepts on the Black-76 seed slope or on the residual alone;
PerContract verifies american_implied_vol's answer on the cold map (flat chord ->
VegaFloor; a miss -> up to 4 secant steps, then NoConvergence).

**Measured (vol-rel-avx2 smoke run of the fixed binary, UNQUALIFIED timings omitted;
the full bench rerun was stopped at the user stop, partial output kept in
`C:/atx-cache/quote-benchmark/batch-pricing-20261017-fix1-bench/`):**
- Greeks hashes unchanged: chain `9f41b247880203d7`, dev200 `e3e9bb01908a6013` (the
  dev200 fixture's pricing sigma is frozen at its fe6a516f definition).
- New IV hashes (values of PerContract Ok rows that missed the rule are now polished on
  the cold map; Lockstep rows follow the new acceptance; statuses below): chain
  PerContract `22ce29207191495b` (was `b9a8099a9c1ae13a`), chain Lockstep
  `c0dd99622b63d825` (was `16bf3ea73549928a`), dev200 PerContract `5471894473c1a004`
  (was `f2edd4997a78248c`), dev200 Lockstep `144a6b11b337506e` (was `3d9795712f012058`).
- Status counts: chain 1720 Ok / 280 VegaFloor on both routes; dev200 2199 / 299 on
  both (Lockstep was 2200 / 298). dev200 PerContract-vs-Lockstep status disagreements:
  1 before (row 1295), 0 after; chain 0 / 0.
- max |iv - true| over Ok rows: PerContract chain 2.1e-9 (was 1.8e-5), dev200 3.8e-9
  (was 1.3e-5); Lockstep chain 1.6e-8, dev200 3.7e-8 (was 2.8e-3).
- Scalar answer vs the rule before polishing (probe): chain 459 / 1720 Ok rows missed
  the residual and 66 the sigma_tol correction (max 1.8e-5); dev200 648 / 163 of 2199.
