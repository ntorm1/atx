# 2026-10-19 — L2e: opt-in de-Americanised (Premium) American IV inversion in the de-Am cache

Lane `L2e-deam-inversion` of the vol-sota sprint (`.superpowers/sdd/2026-10-18-vol-sota/`), branch
`feat/vol-L2e-deam-inversion-20261019` on the W3 prebase `53ba6a3f` (checkpoint `d118e6ef`, resume
2 on top). Evidence: `C:\atx-cache\quote-benchmark\L2e-deam-inversion-20261019-evidence\`
(checkpoint binary `fb341d84`) and `...\L2e-deam-inversion-20261019-resume\` (final binary; each
folder carries `sha256.txt`). All timings UNQUALIFIED (shared host, `ATX_VOL_FIT_WORKERS` 1 or 2).

## What

- `IvStrategy {Search (default), Premium}` + `IvOpts::strategy` + `IvOpts::seed_profile`
  (`api/pricing/american_iv.hpp`, Tier-A). Premium inverts by de-Americanisation
  `P_am(sigma) = A(sigma) + D(sigma)`: the seed leg `A` is Barone-Adesi-Whaley where the contract
  has an exercise region (put `r > 0`, call `q > 0`), Black-76 elsewhere; `sigma0 = A^-1(price)`;
  one cold Andersen-Lake solve per iterate gives `D`; the next iterate solves `A + D-chord = price`
  exactly (no American solve); a root is ACCEPTED only when its Newton distance on the cold map
  `|P_am - price| / (A' + D-chord)` is `<= tol` (1e-7 vol, the Search tolerance). Everything else
  FALLS BACK to Search bit for bit (`IvPremiumFallback`: NotApplicable / RangeEdge (identifiable
  range edges and L1c's non-monotone band) / NoEuropeanSeed / NotIncreasing / VolBound /
  PricerFailure / NoConvergence). Budget: 4 cold solves, plus (resume 2) up to 2 extra solves
  spent one at a time only while the Newton distance keeps shrinking by >= 4x per solve
  (`kIvPremiumExtraSolves`, `kIvPremiumShrink`; `src/pricing/american_iv_premium.hpp`).
- Per-contract `IvPremiumMemory` (cold points of an earlier inversion of the same contract): the
  de-Am cache inverts the mid from scratch and seeds bid and ask from COPIES of the mid's points,
  so cache values stay order-independent.
- Cache wiring: `QuoteSliceDeamInversion {Search, Premium}` / `QuoteSliceChannelConfig::deam_inversion`
  (`api/fitting/quote_slice.hpp`), `DeAmModel::implied_vol_premium` (default Search; the dated-cash
  lattice has no Premium route), `DeAmIvView` strategy + mid-first band order, the strategy word in
  the memo key (a Premium cache never answers a Search fit), `validate_channel` refuses Premium
  with the Fast profile. Policy key `graded.deam_inversion = "search" | "premium"` (requires
  `fit_space vol`; premium requires `deam_profile accurate`; provenance only when premium).
- Counters in `--timing-detail`: `deam_inversions`, `deam_inversion_al` (+max), `deam_premium`,
  `deam_premium_fallbacks` (+ by reason), `deam_premium_max_residual_usd`; opt-in `--deam-audit`
  runs Search beside every accepted Premium inversion and records |dsigma| decades, status
  mismatches and both answers' accurate-map Newton distances.

## Why

L1 attribution: 191,511 AL boundary solves inside the SPY graded-v1 de-Am inversions (~8 cold
evaluations per Newton/Brent inversion on full American prices). De-Americanisation needs 1-3.
User directive: anything above 1 s to fit SPY is unusable.

## Finding 1 — acceptance (PM ruling, SHARED-BRIEF resume wave 2, ruling a)

The original "served prices identical to <= 1e-9 USD vs the old path" line is WITHDRAWN: the old
path (Search) is the less accurate one. The acceptance is:

| criterion | measured (checkpoint binary fb341d84, accurate kernel) |
| --- | --- |
| (i) every accepted Premium root has accurate-map Newton distance <= tol (1e-7) | cohort5 88,458 pairs: max 2.44e-10; dev200 185,539 pairs: max 2.71e-11; Search's own distance up to 1.75e-3 (cohort5) / 1.12e-4 (dev200); Search farther on 55,755 / 124,922 pairs; status mismatches 0 |
| (ii) served / refused set identical to keys-OFF | cohort5 served 8,375 = 8,375, dev200 50,348 = 50,348, SPY 1,410 = 1,410; status diff 0; served-set diff EMPTY |
| (iii) keys-ON CSVs recorded as the strategy's OWN hash set with max abs dprice vs OFF | cohort5 `43f95c8b` (4.12e-5 USD, 3,164 rows > 1e-9), dev200 `6a5aa51d` (2.84e-4 USD, 24,433 rows), SPY `3938853d` (2.01e-5 USD, 469 rows); the differences are Search's distance from the root propagated through the fit, not Premium's |

Final-binary keys-ON hash set (resume 2, budget extension on): see "Hashes" below.

## Finding 2 — the dev200 row with |dvol| = 1.14 (resume brief item 1): verdict (a), documented, kept

Row `466851`, AMDY, expiry_days 20742 (T = 0.00940 y, 3.4 days), strike 23 CALL, spot 47.605,
bid 24.10 / ask 25.60 (mid 24.85, spread 1.50), `ddiv` 1.0254 inside the expiry (funding-pv),
`quote_status excluded_side`, `admitted 0`, `status ok`, `vol_status ok`, grade B, profile
ordinary — a SERVED row that is not a fit input. Served price identical to the bit in both runs:
`24.604999542236328` = S - K (the exercised intrinsic; a 3.4-day deep-ITM call ahead of a
$1.03 dividend has zero vega). Served vol: keys-ON `1.1438858429558691`, keys-OFF
`0.0050000000000000001` (= `kIvMin`).

Reproduction (resume folder `row1p14\`, checkpoint binary fb341d84, `ATX_VOL_FIT_WORKERS=2`):
- every expiry of AMDY + IJS + RSSB (`input\quotes-3underliers.csv`, 696 rows, 12 chains):
  graded-v1 OFF `9ee11f5e` = graded-v1-volspace (Search) `9ee11f5e` (byte-identical, so L2a's
  vol-space fit is not the cause), graded-v1-volspace-premium `1c2ed2b5`; row 466851 reproduces
  exactly (Search 0.005 / Premium 1.1438858, |dprice| 0). Only 3 of 608 served rows differ by
  > 1e-5 vol — the three |dvol| rows of the full dev200 comparison (AMDY 1.139, IJS 0.198,
  RSSB 0.178: all deep-ITM 3.4-day rows served at their intrinsic, |dprice| = 0 on each).
- the 3-chain subset alone (188 rows) does NOT reproduce it (both serve 0.5949): the term prior
  from the underlier's other expiries is part of the fixture.
- `--deam-audit` of the AMDY slice: 256 Premium inversions, 46 fallbacks (no_seed 42,
  no_convergence 4), every accepted root at accurate-map Newton distance <= 3.6e-13 while
  Search's answers sit up to 7.9e-6 off (138 of 256 pairs farther than Premium).

Mechanism (serve path, `src/fitting/quote_slice.cpp:407-428`): for a node strike the served
vol is the curve's node volatility (`fit.curve->iv`, line 408) unless the node side carries the
repair's `moved` flag, in which case it is the Search inversion of the served price (line 420 —
Search in BOTH runs; Premium is wired only into the de-Am cache). Strikes 22 and 24-27 serve the
same flat-wing node vol 1.14388 under both strategies; strike 23's call side takes the inversion
route under Search and the node route under Premium, and the Search inversion of an intrinsic
price is L1c's intrinsic-region answer (`LEDGER` "ANDERSEN-LAKE NON-MONOTONICITY": the map is flat
at the intrinsic, the inverter clamps to the `kIvMin` floor). The only state selecting between
those routes for a node strike is the moved-node flag (inferred from the code; the native report
does not list moved nodes), whose repair decisions read the channel bands — which differ between
the runs by Search's own error (this chain <= 7.9e-6 vol). Premium never inverted this served
price, accepted no root for it, and the served set and price are identical: verdict (a)
"Search's intrinsic-region answer; Premium at the accurate root" with (c) "max |dprice| it
causes = 0". Nothing to fix in the inversion; the serve path's treatment of zero-vega intrinsic
rows (an arbitrary vol between the floor and the wing) is a pre-existing property of both paths.

## Finding 3 — fallback reduction (resume brief item 2, bounded)

SPY graded-v1 premium @1 (checkpoint): 1,172 fallbacks of 29,725 inversions = no_seed 381 /
no_convergence 395 / range_edge 239 / not_increasing 157. A temporary stderr trace (not shipped)
classified them:

| reason | count | where |
| --- | --- | --- |
| no_seed | 372 | the remainder-constant step of a deep-ITM PUT mid inverted from scratch (e.g. K 823-910 vs S 773, T 0.21, price 0.14-0.23 above the intrinsic): BAW sits at its intrinsic floor there, the cold price at the BAW inverse is above the quote, so `price - D` lies under the leg's floor and the model has no root |
| no_seed | 9 | 8 memory-seeded (memseed) + 1 cold seed |
| no_convergence | 392 of 395 | the 4-solve budget with the Newton distance still shrinking >= 4x per solve (e.g. 4.9e-3, 6.1e-5, 1.3e-7 at solves 2-4) |
| not_increasing | 157 | deep-ITM put band edges seeded from the mid's memory, non-increasing cold chord at the 3rd solve |
| range_edge | 239 | identifiable-range edges / L1c band (correct refusals) |

Implemented (same key): extra solves while shrinking (`kIvPremiumExtraSolves = 2`,
`kIvPremiumShrink = 0.25`). Tried and DROPPED: a (half) regula-falsi second point anchored on
`(kSigmaLo, zero_vol)` for the no-root case — it turned the 372 cheap first-solve `no_seed`
fallbacks into ~400 third-solve `not_increasing` fallbacks (the cold map is flat at the intrinsic
then convex; the BAW leg is kinked at the exercise boundary) and the counted AL solves per
inversion did not improve (53,376 -> 53,597). The principled fix is a smooth leg in that region
(a floored Black-76 + EEP model), deferred.

Measured (final binary): see "COUNT line" below.

## COUNT line (SPY graded-v1, one surface, `--timing-detail`, UNQUALIFIED)

| run (binary) | fit_and_reprice_s | chain p50 ms | al_total | de-Am AL work (al_total - price - certify) | cache inversions | AL per inversion (max) | fallbacks |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keys-OFF graded-v1 @1 (checkpoint fb341d84; OFF path byte-identical since: parity 3/3) | 103.122 | 3176.7 | 226,234 | 191,511 (`deam` bucket) | - | ~8 evaluations (uncounted warm sweeps) | - |
| vol-space Search @1 (checkpoint) | 54.475 | 1841.4 | 125,935 | 91,215 | 29,725 | 2.31 (6) | - |
| premium @1 (checkpoint fb341d84) | 29.900 | 848.2 | 110,686 | 75,966 = 0.40 of OFF | 29,725 | 1.80 (10) | 1,172 = range_edge 239 / no_seed 381 / not_increasing 157 / no_convergence 395 |
| premium @1 (FINAL 4d53a07e, budget extension) | 15.619 (idle host, load 2%) | 468.9 | 109,873 | 75,153 = 0.392 of OFF | 29,725 | 1.768 (9) | 781 = range_edge 239 / no_seed 381 / not_increasing 157 / no_convergence 4 |

COUNT line OFF -> ON: 191,511 -> 75,153 (0.392; checkpoint 75,966 = 0.40). Fallbacks before -> after:
1,172 -> 781 (no_convergence 395 -> 4; the other reasons unchanged by design). The premium @1
wall 29.9 -> 15.6 s is NOT a like-for-like timing (the checkpoint ran at 59-71 % host load, the
final row at 2 %); the counted AL work moved by -1.1 %.

Target `<= 1/3` of the keys-OFF de-Am AL count is NOT met (accepted DONE_WITH_CONCERNS by PM
ruling). What would close it: (1) band-edge floor/cap prices (`kIvMin` / `kIvMax` prices, 2 AL per
quote, ~23k per SPY surface) priced without a confirm where the band is wide; (2) the
expiry-shared cache under fan-out (`fit_workers 0` doubles inversions: 63,293 vs 29,725) — L2c's,
already on its branch; (3) a smooth seed leg for deep-ITM puts (above). Note the AL counter
counts cold BAW seeds only: Search's warm sweeps are uncounted, so "2.3 per inversion" understates
Search's cost (~8 evaluations).

## Hashes

Keys-OFF parity (final binary `4d53a07e`, resume folder `parity\`, `ATX_VOL_FIT_WORKERS=2`), all
MATCH: dev200 default `c89d0739`, dev200b default `4b273b08`, cohort5 graded-v1 raw `171fa2d2`
(also MATCH 3/3 on the checkpoint binary `fb341d84`).

Keys-ON (the strategy's OWN hash set; NOT expected to be byte-identical to OFF):
- checkpoint binary `fb341d84` (4-solve budget): SPY graded-v1 premium `3938853d` (w1 = w0),
  cohort5 premium audit `43f95c8b`, dev200 premium audit `6a5aa51d`.
- final binary `4d53a07e` (budget extension): SPY graded-v1 premium @1 `2c8b113c`;
  cohort5 premium audit `bb5df194` (88,458 pairs, fallbacks 2,770 -> 1,841 with no_convergence
  959 -> 30, status mismatches 0, max accurate-map Newton distance Premium 2.44e-10 vs Search
  1.75e-3; vs OFF `171fa2d2`: served 8,375 = 8,375, served-set diff EMPTY, max |dprice| 4.12e-5
  USD, max |dvol| 5.6e-7), dev200 premium audit `196387bb` (185,539 pairs, fallbacks 5,628 ->
  4,505 with no_convergence 1,807 -> 684, status mismatches 0, Newton distance Premium 2.71e-11
  vs Search 1.12e-4; vs OFF `fc277413`: served 50,348 = 50,348, served-set diff EMPTY, max
  |dprice| 2.84e-4 USD, max |dvol| 1.14 = the AMDY row of Finding 2, |dprice| 0).
- Row reproduction (checkpoint binary): 3 underliers OFF `9ee11f5e` = vol-space Search `9ee11f5e`,
  premium `1c2ed2b5`; 3 chains OFF `6f26be9e` = Search `6f26be9e`, premium `e3c3153d`.

## Gates (resume 2)

- PCH-off `vol-hygiene`: configure exit 0 (16 s); `check` over the 69 direct `.cpp` includers of
  the six touched headers (`american_iv.hpp` L1c list, `quote_slice.hpp`, `deam_iv_cache.hpp`,
  `counters.hpp`, `quote_bench_policy.hpp`, `american_iv_premium.hpp`; src/tests/tools, bench TUs
  excluded) exit 0, 103 objects, 0 diagnostics (157 s); re-check of the 4 TUs touched by resume 2
  exit 0, 5 objects, 0 diagnostics.
- AL fingerprint (kernel untouched): `vol-rel-avx2` `atx-vol-al-preset-ladder-bench` build exit 0
  (135 steps, 0 diagnostics), `american/ladder_fingerprint` -> `53c4df68...` MATCH.
- gtests `AmericanIvPremium.*` 6/6 and `QuoteSliceVolSpace.*:DeAmIvCache.*` 8/8 (vol-rel); CLI
  pytest 164 passed; native ctest `atx-vol-quote-benchmark-native` with
  `-DPython3_EXECUTABLE=C:/Program Files/Python312/python.exe` 1/1 passed (10.95 s).
- Fail-on-revert: R1-R5 (checkpoint); R6 policy refusal (direct pytest: 1 failed with the check
  disabled, the deeper `validate_channel` refusal still fires); R7 budget extension (the new test
  fails with `kIvPremiumExtraSolves = 0`, the grid's `7=4` no-convergence fallbacks return).
- Keys-OFF parity 3/3 MATCH on the final binary (above).
- Full detail: `.superpowers/sdd/2026-10-18-vol-sota/lane-L2e-deam-inversion-report.md`.

## What L2g (Screen) must assume about `IvOpts::seed_profile`

- Unset (default) or `al_opts`' own profile: every solve is the accurate kernel; nothing changes.
- Another profile (e.g. `AlProfile::Fast`, or a Screen profile): the iterates run `al_opts`'
  scheme on that profile and the accepted root is CONFIRMED by one accurate solve (Newton
  distance `<= tol` on the accurate map, then Newton-corrected); a confirm outside tol falls back
  to Search (NoConvergence), a pricer refusal to PricerFailure. The fallback rules, the
  identifiable-range / non-monotone-band gate and `refuse_non_monotone` are evaluated on the
  ACCURATE map regardless of the seed profile. The de-Am cache passes no seed profile today
  (`SliceDeAmModel::implied_vol_premium`); wiring a Screen profile there is L2g's one-line hunk
  (`IvOpts::seed_profile`), and `validate_channel` must keep refusing Premium + Fast
  `deam_profile` (the Fast profile answers a Fast cache, never an accurate request).
- `IvPremiumMemory` must only be shared between calls with identical S, K, T, r, q, side,
  AlOpts and seed_profile (header contract).

## Hunk table (final SHA vs base 53ba6a3f)

Hunk headers (`git diff -U0 53ba6a3f`, old -> new line ranges). Files L2c also touches are marked (*).

| file | hunks |
| --- | --- |
| `include/atx/vol/api/fitting/quote_slice.hpp` (*) | `-112,0 +113,9` (QuoteSliceDeamInversion enum), `-170,0 +180` (channel field), `-355,0 +366,2` (diagnostics) |
| `include/atx/vol/api/pricing/american_iv.hpp` (Tier-A) | `-144,0 +145,25` (IvStrategy), `-155 +180,7` (IvOpts fields), `-163,4 +194,4` (IvResult doc) |
| `python/tests/test_quote_bench_cli.py` (*) | `-719 +719,12` (strict options), `-795,0 +807,35` (provenance + counters) |
| `src/fitting/counters.hpp` | `-1173,0 +1174,26` (counters), `-1378,0 +1405,19` (audit arming) |
| `src/fitting/deam_iv_cache.cpp` (*) | `-11,0 +12`, `-64,2 +65,7`, `-85 +91,88` (invert + audit), `-109 +202,2`, `-141,0 +236,13` (mid-first band), `-145 +252`, `-149 +256`, `-195,0 +303,2` (memo key word) |
| `src/fitting/deam_iv_cache.hpp` | `-48,0 +49`, `-71,0 +73,4`, `-100,0 +106,12`, `-134,0 +152,3`, `-136 +156,2`, `-147,0 +169,2`, `-149 +172,6`, `-150,0 +179`, `-156,0 +186` |
| `src/fitting/quote_slice.cpp` (*) | `-18,0 +19`, `-691,0 +693,22` (implied_vol_premium), `-721 +744,2` (view strategy), `-1331 +1355,5` (validate_channel) |
| `src/pricing/american_iv.cpp` | `-2,0 +3,2`, `-12,0 +15`, `-14,0 +18`, `-999,0 +1004,369` (Premium strategy; resume 2 added the budget extension inside), `-1002 +1374,0`, `-1004,6 +1376,3`, `-1011,7 +1380,11`, `-1019 +1392` |
| `src/pricing/american_iv_premium.hpp` (new) | `-0,0 +1,76` |
| `tests/american_iv_test.cpp` | `-2,0 +3`, `-4,0 +6`, `-6,0 +9`, `-22,0 +26`, `-1719,0 +1724,346` (6 AmericanIvPremium tests) |
| `tests/quote_slice_vol_space_test.cpp` | `-17,0 +18`, `-272,0 +274,82` |
| `tools/quote_bench_main.cpp` | `-60,0 +61`, `-108 +109`, `-112 +113,4`, `-119,0 +124,4`, `-157,0 +166,3`, `-567 +578,13` (timing JSON), `-1394,0 +1418,10`, `-1565,0 +1599` |
| `tools/quote_bench_policy.hpp` (*) | `-536,0 +537`, `-567,0 +569,18` (deam_inversion key), `-598,0 +618,3`, `-754 +776`, `-814,0 +837` |

Resume 2 changed only `american_iv.cpp` (the loop head and the Newton-distance tracking inside
`-999,0 +1004,369`), `american_iv_premium.hpp` (constants) and `american_iv_test.cpp`.
