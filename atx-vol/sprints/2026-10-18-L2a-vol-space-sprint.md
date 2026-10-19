# L2a-vol-space sprint: opt-in per-quote de-Americanised IV cache (vol-sota W2, 2026-10-18/19)

Lane branch `feat/vol-L2a-vol-space-20261018` (base: PM prebase `7cebbbd3`; WIP checkpoint
`63400aeb`; resumed 2026-10-19 in pool-33). Opt-in only: every default path is byte-identical
(parity below). Plan: `docs/plans/2026-10-17-atx-vol-sota-fit-dag-plan.md` W2 "L2a", re-aimed by the
PM (resume brief, ruling 10 of the sprint handoff) from "vol-space channel solve" to "per-quote IV
cache".

## 1. The refuted premise and the measured cost structure

The plan assumed the channel fit spends its time in "12-19 Newton iterations per chain, each
repricing up to 325 strikes through AL". The code does not work that way:

- The channel objective (`src/fitting/slice_channel.hpp`) is already a volatility-space objective;
  its knot-spline Newton never prices an option. On cohort5 SPY graded-v1 (33 chains, baseline
  report `fit-timing-20261017-cohort5/runs2/graded-v1-SPY-native.json`, per-slice `solve_us`) the
  channel solves sum to 48.9 ms of 54.7 s fit+reprice (0.09%), 12-38 iterations per slice.
- The time is American IV inversions. Point admission inverts every admissible mid; every channel
  fit then builds the bid/ask volatility band of every valid quote: 2 model prices (the kIvMin /
  kIvMax price range) plus up to 3 inversions (bid, ask, mid) per quote. Every graded rung that refits
  the chain (pass A point fit, the channel passes, repair rung, calendar retry) repeats the band.
- Attribution of the WIP checkpoint (temporary stage timers, removed before the commit; one
  instrumented run, UNQUALIFIED, load 51-96%), SPY graded-v1, summed stage seconds:

| path | channel band / observations | channel admission | point admission | certify (incl. repairs) | reprice inside certify | solve |
| --- | --- | --- | --- | --- | --- | --- |
| OFF (price channel) | 93.4 | 14.1 | 6.3 | 10.5 | 9.1-10.4 | 0.34 |
| ON accurate cache | 38.9 (built once; 56/56 channel fits reuse it) | 0.005 | 6.3 | 9.2 | 9.1 | 0.24 |
| ON fast cache | 19.1 | 0.006 | 3.1 | 9.4 | 9.3 | 0.23 |

  What remains with the cache ON: the one-time cache build (about 24.5k inversions and 23k band-edge
  prices for SPY) and the American reprice inside certification (about 9 s over 87 certifications).

## 2. What was built (all opt-in)

- Config (`api/fitting/quote_slice.hpp`): `QuoteSliceFitSpace { PriceChannel /*default*/, VolChannel }`,
  `QuoteSliceDeamProfile { Accurate /*default*/, Fast }`, `QuoteSliceChannelConfig::fit_space /
  deam_profile / deam_cache` (defaults = the historical path), `QuoteSliceDiagnostics::deam`
  (used, reused, inversions, model_prices, accurate_fallbacks: the work THIS fit added).
- Cache (`src/fitting/deam_iv_cache.{hpp,cpp}`, internal): `DeAmQuoteIv` per quote (carry
  q_effective / local_forward, floor / cap prices, bid / ask / mid IV, well_posed), filled lazily,
  each value at most once per cache; `DeAmIvView` (one fit's view; counts its work);
  `DeAmModel` (implemented by `quote_slice.cpp` as `SliceDeAmModel`, so the cache uses the fit's own
  forward map: the Accurate kernel IS `model_price` / `contract_iv`).
- Vol-space channel observations (`src/fitting/quote_slice_vol_space.{hpp,cpp}`): the same selection,
  edges, mid anchors and ITM mid weights as `build_channel_observations`, read from the cache, then
  the UNCHANGED family dispatch, `certify_and_serve`, repairs and containment.
- Fast kernel (`deam_profile = fast`): `AlProfile::Fast` inversions; a Fast root is accepted only
  strictly inside (kIvMin, kIvMax) with Black-76 vega >= 1e-5 * max(target, 1) (derivation in
  `deam_iv_cache.hpp`: keeps the Fast-induced shift <= 1e-6 vol, the tested bound); otherwise it is
  redone on the accurate kernel. The dated-cash lattice has no Fast kernel (always accurate).
- Memo (`deam_cache = true`): per thread, FIFO, at most `kQuoteSurfaceMaxExpiries` (128) caches,
  shared_ptr-owned (an evicted cache stays valid for a fit still holding it). Key = the EXACT inputs
  as 64-bit words, compared word for word (the 64-bit `quote_set_hash` only filters): market (6
  fields), the derived forward / discount / effective rate / effective yield, dividend model, lattice
  steps and scheme, AlOpts presence + all 6 fields (profile included), IV tolerance and iteration
  cap, the cache kernel (`deam_profile`), the variance-clock dividends, and every quote (strike, side,
  bid, ask, carry presence and value) in input order. `static_assert`s on the aggregate arity of
  AlOpts / QuoteSliceMarket / QuoteSliceQuote / CashDividend force a key update when a field is
  added. A hit returns exactly what a rebuild computes, so output never depends on hit, miss,
  eviction or thread; a Fast-kernel cache can never answer an Accurate request (kernel word).
- Policy keys (`tools/quote_bench_policy.hpp`, graded only, applied to every profile):
  `graded.fit_space` ("price" | "vol"), `graded.deam_profile` ("accurate" | "fast"),
  `graded.deam_cache` (bool). deam_* without fit_space vol is refused (would be inert); provenance in
  `policy_definition.graded` only when opted in.

L2c (rung memo) reuse: the key deliberately excludes every rung-level fit setting (method, curve
family, repair budgets, quality gates, min quotes, the cache flag itself; pinned by
`DeAmIvCache.MemoKeyCoversEveryInputThatChangesAnInversion`), so any rung - graded or ladder - that
fits the same chain with the same carry and American model computes the same key and shares every
inversion already made. It is quote-set hash + AL profile/opts + carry, as the plan asks. Nothing for
L2c is built here. One design note for L2c: the memo is thread-local, so with L2d's per-expiry
fan-out two rungs of one expiry that run on different threads miss each other (correct, just less
reuse); an expiry-owned cache passed down by `quote_surface.cpp` would make reuse independent of
scheduling.

## 3. Hashes (lane binary `c3e52c11bc0e6185...`, vol-rel, `ATX_VOL_FIT_WORKERS=2`)

Evidence folder `C:\atx-cache\quote-benchmark\L2a-vol-space-20261019-parity\` (`runs.log`,
`sha256.txt`, runner `scripts\run_l2a.ps1`); policies `L2a-vol-space-20261019-policies\`
(`graded-v1-volspace.json` `faf2f3f7...`, `graded-v1-volspace-fast.json` `dc8b2c7a...`).

| run | sha256 | expected | verdict |
| --- | --- | --- | --- |
| dev200 default (auto), keys OFF | `c89d0739...` | `c89d0739` | MATCH |
| dev200b default (auto), keys OFF | `4b273b08...` | `4b273b08` | MATCH |
| cohort5 graded-v1 raw, keys OFF | `171fa2d2...` | `171fa2d2` | MATCH |
| SPY cohort5 slice graded-v1, keys OFF | `087e6a54...` | `087e6a54` | MATCH |
| SPY graded-v1-volspace (accurate cache) | `087e6a54...` | `087e6a54` (= OFF) | MATCH |
| SPY graded-v1-volspace-fast | `45313d61...` | recorded | CHANGES THE CSV, opt-in only |

The binary rebuilt from the final tree differs from `c3e52c11` only in the PE TimeDateStamp
(2 bytes at offsets 128-129; same length 2,375,168).

## 4. ON vs OFF (UNQUALIFIED; shared host, other lanes active; load 24-58% across the three runs)

SPY cohort5 slice, graded-v1, 33 chains, serial expiries (L2d's fan-out is not in this base):

| run | fit+reprice s | chain p50 ms | served rows | containment (two-sided served) | grades A/B |
| --- | --- | --- | --- | --- | --- |
| OFF | 91.1 | 2,759.8 | 1,410 / 13,168 | 926 / 1,368 = 0.677 | 1,040 / 370 |
| ON accurate cache | 52.7 (x1.73) | 1,597.3 | 1,410 (identical CSV) | 0.677 | 1,040 / 370 |
| ON fast cache | 35.2 (x2.59) | 1,065.5 | 1,410 | 0.677 | 1,040 / 370 |

Fast vs accurate, cell by cell: 0 status changes, 835 price cells differ by at most 2.5e-12 USD,
948 vol cells by at most 1.1e-12 vol. The SPY per-expiry target (<= 60 ms) is far off: ~1.6 s per
chain with the accurate cache, single thread. What still dominates is the one-time inversion pass and
the certification reprice (section 1) - the next levers are L2d's fan-out, L2b's cheaper
certification / batched inversion and, opt-in, the Fast kernel.

## 5. Deviations (PM ruling: both ACCEPTED)

1. No separate `solve_channel_vol_space`: the existing family dispatch and `certify_and_serve` run on
   the cached observations. Hash proof: the accurate-cache SPY CSV equals OFF (`087e6a54`), so the
   observation set, solve and certification are the same.
2. No total-variance penalty: w = sigma^2 T is strictly increasing in sigma, so band membership is
   identical in sigma and w; the zero-violation set (all certification and containment see) is
   unchanged. Same hash proof.

Also: the point path (`fit_quote_slice`) reads its mids from the memo when `deam_cache` is set
(identical values on the accurate kernel); only graded-v1 keys-ON policies were written (the
repair-v3 variant of the original brief was not, per the re-aim's "no extra runs").

## 6. Tests (`tests/quote_slice_vol_space_test.cpp` in `atx-vol-quote-slice-tests` + `atx-vol-tests`)

- `QuoteSliceVolSpace.AccurateCacheServesThePriceChannelFitBitForBitOnMissAndHit`: SPY-shaped grid
  (7 cases over 3 tenors, reference / per-strike carry, continuous / funding-PV dividends, one eSSVI
  family case; bidless wings and a deep ITM bid below its floor price): every
  row, price node and curve value bit-identical to the price channel uncached, on a memo miss, a
  partial hit and a full hit; a full hit does 0 inversions and 0 model prices.
- `QuoteSliceVolSpace.PriceChannelIgnoresTheCacheKeys`: deam keys set under PriceChannel: no cache,
  no memo entry, identical fits.
- `QuoteSliceVolSpace.FastKernelCacheNeverAnswersAnAccurateRequest`.
- `DeAmIvCache.FastInversionWithoutAGuaranteedBoundIsRedoneOnTheAccurateKernel` (low vega, root at
  either bound, Fast failure, no Fast kernel, Accurate profile).
- `DeAmIvCache.BandEdgesFollowTheChannelBuilderAndAreComputedOnce`.
- `DeAmIvCache.MemoKeyCoversEveryInputThatChangesAnInversion` (34 inputs change the key, 6 rung-level
  settings do not).
- `DeAmIvCache.MemoIsBoundedAndAHeldCacheOutlivesItsEviction`.
- CLI (`python/tests/test_quote_bench_cli.py`): strict keys; the accurate-cache policy writes
  predictions byte-identical to plain graded.

Fail-on-revert (outputs in the evidence folder `reverts\`): memo off -> 2 tests fail; fit_space guard
off -> 2; kernel word out of the key -> 2; vega guard off -> 1; quote carry out of the key -> 1; policy
refusal off -> CLI strict test fails. Each restored by inverse Edit, blob = HEAD.

## 7. Hunks in shared files (for the W2 integrator; lane line numbers, base `7cebbbd3`)

- `src/fitting/quote_slice.cpp`: includes 14, 16; `admit_point_observations` comment + `deam`
  parameter 486-491; mid-IV ternary 552-553; `SliceDeAmModel` + `VolSpaceCache` 654-734;
  `validate_channel` enum ranges 1320-1324; `fit_quote_slice` memo 1552-1558;
  `fit_quote_slice_channel` dispatch 1596-1604.
- `include/atx/vol/api/fitting/quote_slice.hpp`: enums 91-112; channel keys 162-170; deam
  diagnostics 347-357; `QuoteSliceDiagnostics::deam` 373.
- `src/fitting/curve_fit.cpp`, `src/fitting/quote_surface.cpp`: no hunks.
- `tools/quote_bench_policy.hpp` 531-574, 594-599, 745, 753-755, 806, 812-814;
  `atx-vol/CMakeLists.txt` 107-108; `tests/CMakeLists.txt` 32-33, 39-40, 239; `tests/slices.cmake` 37.
