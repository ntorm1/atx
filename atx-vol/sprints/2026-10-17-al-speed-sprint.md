# AL speed lane (2026-10-17): final

Branch `feat/vol-al-speed-20261017` from `24e01644`. Resume 5 built tier 1 and
tier 2 (pool-4, run `vol-20261017-al-speed`) and stopped at a user stop before
the timing pass. Resume 6 lane `al-speed-finish` (pool-4, run
`vol-20261017r6-al-speed-finish`) ran the interleaved A/B and the remaining gates,
and keyed the carry memo on the profile. This note replaces the WIP version.

Goal: the cold Andersen-Lake (AL) American price, 25.8 us in the 2026-10-16 perf
audit (`american/ladder/ql_fast` = `al_bulk_opts`), toward 15 us, in two tiers:
bit-identical first, then an opt-in fast profile with a measured bound.
Development cohorts only; no holdout; no default changed.

**Every timing in this note is UNQUALIFIED (loaded host).** Load during the A/B
was mean 27.3% / median 26.4% / p90 44.4% (577 samples, 2 s apart, of
`\Processor(_Total)\% Processor Time`); only 16% of samples were under 15%.
Builds are `vol-rel-avx2`, `ATX_VOL_FIT_WORKERS=1`, bench main thread pinned to
P-core CPU 7.

## Commits

| SHA | what |
|---|---|
| `16291525` | bench only: single-op latency rows, kernel fingerprint mode, profile mode |
| `a64dfb83` | tier 1: bit-identical hot-path hoists |
| `5a0a4d21` | tier 2: opt-in `AlProfile::Fast` kernel + error/Greeks bench rows + tests |
| `c2bf8bb8` | resume-5 sprint note + WIP handoff |
| `d77beb03` | carry memo keys on `AlOpts::profile` + test |
| (this commit) | final sprint note; WIP handoff closed |

## 1. Profile (base `24e01644` kernel, bulk preset, ladder grid of 104 contracts)

alprobe zones (`ATX_VOL_AL_PROBE=z`, rdtsc cycles per cold price, 281,944 ops):

| zone | cycles/op | share |
|---|---:|---:|
| BAW seed (one safeguarded Newton per collocation node) | 11,227 | 25% |
| 2 Jacobi-Newton sweeps | 20,708 | 45% |
| premium quadrature (32 nodes) | 10,407 | 23% |
| static + sigma geometry bind | 2,645 | 6% |
| total | ~45,700 | |

- Unit costs on this host: `std::erfc` / `norm_cdf` 20.4 ns, Cody `norm_cdf_erfc`
  6.7 ns, exp 3.7-4.0, log 3.8-4.6, sqrt 2.0-2.2, div 2.1-2.2 ns.
- One bulk price makes ~433 `norm_cdf` calls, ~47% of its time.
- The BAW Newton takes 6.32 iterations per node (nb 7) and 6.48 (nb 12); 0
  bracket retries, 0 unconverged nodes. No heap allocation on the path.
- The special function, not the iteration count, is the lever, and a
  bit-identical tier cannot touch it.

## 2. Tier 1 (bit-identical, `a64dfb83`)

- Fused BAW residual/derivative evaluator (one d1, Sx-invariant terms hoisted,
  same expressions and FMA shapes); specialised sweeps read K*exp(-(r-q)tau_j)
  and sqrt(tau_j) from the static bind; the JN sweep reuses the eqn_b tip;
  tallies published once per quadrature / bind.
- Kernel fingerprint (`ATX_VOL_AL_FINGERPRINT`, 48,660 doubles over every AL
  entry point) sha256 `53c4df681f70d5906856211f5a35454b12686c657bfd7f37cf7ff1adca18dcea`
  at base, tier 1, tier 2 and the final SHA (section 6).
- Speed: **below this host's resolution.** In the 6-round A/B (section 4) the
  tier-1 / base p50 ratio is x1.002 (accurate), x0.994 (bulk), x1.173 (fast) on
  the min across rounds and x1.003 / x1.014 / x1.030 on the median. The resume-5
  figure "bulk 17.7 -> 16.8 us" (8 rounds, load 13-99%) is NOT reproduced and is
  withdrawn. Greeks (`american_greeks_al`, fast preset, 8 points): x1.075 min,
  x1.044 median.

## 3. Tier 2 (opt-in `AlProfile::Fast`, `5a0a4d21`)

Policy key: `AlOpts::profile = AlProfile::Fast`, or `al_fast_profile(opts)` (same
scheme as `opts`, nullopt -> its explicit ACCURATE equivalent, profile flipped).
Reference stays the default everywhere; no record format persists the profile.

Same scheme (nodes, quadrature orders, sweep schedule, BAW seed, guards, clamps)
with three substitutions that are identities in exact arithmetic:
1. Phi / phi by the validated scalar Cody kernels (`pricing/scalar_erfc.hpp`).
2. The boundary ratio in log space (b = xmax*exp(-sqrt(y))): one exp and one log
   fewer per fixed-point and premium node.
3. The BAW seed shares one d1 between its Black-76 leg and its smooth-pasting term.

Not adopted on purpose: the AVX2 batch kernel (keeps Reference; batch lane), the
adjoint tape (differentiates the Reference map; its bumped re-solves pin
Reference), truncation knobs (fewer sweeps / nodes; the bulk rung is that trade).

### Error bound (Fast vs Reference, same scheme)

Re-run on the final binary (`al-speed-finish-20261017-fast-error`): all 18 rows,
279 counters, are identical to the resume-5 run (`al-speed-20261017-fast-error-wip1`).
Sets: ladder grid (104), 6,000 random contracts, 10,694 dev200 contracts
(kernel-level inputs, sigma = our own `c89d0739` predicted_vol, never a
SpiderRock mark; inputs sha256 `8fb2f3d3...cb644a`).

| set / preset | price max abs | price max rel (P>=0.01) | IV max bp | IV p99 bp | status mismatches |
|---|---:|---:|---:|---:|---:|
| grid accurate / fast / bulk | 1.28e-13 | 1.29e-13 | 3.6e-11 | 3.2e-11 | 0 |
| random accurate | 4.3e-14 | 1.6e-13 | 4.0e-4 | 1.2e-8 | 0 |
| random fast | 4.3e-14 | 1.6e-13 | 3.8e-5 | 9.6e-9 | 0 |
| random bulk | 4.3e-14 | 1.6e-13 | 1.8e-3 | 2.0e-8 | 0 |
| dev200 accurate | 8.0e-13 | 1.9e-13 | 9.5e-5 | 1.7e-9 | 0 |
| dev200 fast | 8.0e-13 | 1.9e-13 | 1.1e-5 | 6.4e-9 | 0 |
| dev200 bulk | 8.0e-13 | 1.9e-13 | 5.4e-3 | 6.9e-9 | 0 |

Greeks (`american_greeks_al`, p99 of |Fast - Reference|, worst preset per set):

| set | delta | gamma | vega | theta | rho | vanna | volga | charm |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| grid | 7.0e-14 | 5.6e-13 | 3.4e-11 | 2.1e-9 | 4.8e-10 | 4.7e-11 | 2.0e-7 | 1.6e-9 |
| random | 1.4e-13 | 5.0e-12 | 1.4e-11 | 2.6e-8 | 1.4e-10 | 1.1e-10 | 5.2e-8 | 1.5e-7 |
| dev200 | 1.8e-13 | 1.7e-11 | 1.4e-10 | 5.5e-8 | 1.5e-9 | 1.2e-10 | 5.1e-7 | 1.1e-7 |

The table values are measurements rounded to 2 figures; two of them round DOWN
(vega 1.4211e-10, vanna 1.2206e-10), so do not read them as bounds. The exact
measured worst cases and their round-up bounds are in section 8 (MINOR-4) and in
the `AlProfile` comment.

Restated bound: max price error 8.0e-13 abs (1.9e-13 relative); IV max
0.0054 bp, p99 <= 2e-8 bp (the max sits on flat-vega contracts, ill-conditioned
for both profiles); Greeks p99 <= 5.2e-7 (volga, measured 5.12e-7). Tested bound
(`AlFastProfile.*`, tests/american_test.cpp; text on `AlProfile` in
api/pricing/american.hpp): |dP| <= 1e-12 * max(P, 1); |dIV| <= 0.01 bp on
vega-bearing contracts; per-Greek p99 ~100x the measured value; the call slice
stays bit-identical to the scalar Fast call.

## 4. Interleaved A/B — CONFOUNDED (resume 6), superseded by section 8

**Kept for the record only.** Review v2 (MINOR-1) found this A/B confounded: a
fixed arm order every round, and tier 2 ran 12 rows (53 s) against 3 (12 s) on
base / tier 1. Its tier comparisons are replaced by the counterbalanced A/B in
section 8. Its Fast-vs-Reference ratios come from the same confounded run and
stay unqualified; section 8 says why they were not re-measured.

Protocol: three binaries built in pool-4 from their SHAs (`vol-rel-avx2`, clean
tree each time): base = `16291525` (bench rows on the `24e01644` kernel), tier 1 =
`a64dfb83`, tier 2 = `c2bf8bb8` (tier-2 kernel; `d77beb03` changes only
`carry_memo.cpp`, and its fingerprint equals all three). Six rounds of
base / tier1 / tier2 ladder runs, then base / tier1 / tier2 `atx-vol-american-bench`
Greeks runs, in one host session (18:00-18:19 UTC). Per round, each row's value
is the median of its 5 in-process repetitions; the table gives min and median
across the 6 rounds. Raw output: `al-speed-finish-20261017-ab` (sha256.txt).

Single-op cold price latency (`american/ladder_latency/<preset>`, us):

| preset | arm | p50 min | p50 median | p99 min | p99 median |
|---|---|---:|---:|---:|---:|
| bulk (`al_bulk_opts`) | base | 15.5 | 17.6 | 19.6 | 22.2 |
| | tier 1 | 15.6 | 17.35 | 20.4 | 22.65 |
| | tier 2 Reference | 15.3 | 18.1 | 18.8 | 26.6 |
| | tier 2 **Fast** | **8.5** | **9.35** | 12.1 | 14.0 |
| fast (`al_fast_opts`) | base | 33.9 | 37.65 | 42.7 | 47.45 |
| | tier 1 | 28.9 | 36.55 | 40.2 | 45.3 |
| | tier 2 Reference | 28.6 | 38.4 | 35.9 | 56.85 |
| | tier 2 Fast | 15.2 | 16.8 | 21.9 | 24.5 |
| accurate (nullopt) | base | 110.9 | 137.35 | 171.8 | 214.05 |
| | tier 1 | 110.7 | 136.95 | 168.6 | 179.35 |
| | tier 2 Reference | 122.5 | 141.55 | 148.5 | 190.0 |
| | tier 2 Fast | 59.3 | 65.0 | 89.1 | 110.3 |

Fast vs Reference on the same tier-2 binary and rounds (p50 min / median):
bulk x1.80 / x1.94, fast x1.88 / x2.29, accurate x2.07 / x2.18. (The resume-5
single run said 2.5x; the interleaved figure is lower.)

Cold Greeks, single-op (`american/greeks_latency/<preset>`, tier-2 binary only;
base and tier 1 do not have this row), us:

| preset | Reference p50 min / median | Fast p50 min / median | ratio (min / median) |
|---|---:|---:|---:|
| bulk | 98.0 / 126.45 | 50.8 / 58.45 | x1.93 / x2.16 |
| fast | 176.5 / 204.3 | 68.0 / 79.95 | x2.60 / x2.56 |
| accurate | 703.4 / 811.7 | 253.8 / 346.8 | x2.77 / x2.34 |

Cold Greeks per tier (`amer/american_greeks_al/fast_cold/put/*`, 8 points; per
round the mean over points of each point's median ns/op): base 189,644 min /
198,013 median; tier 1 176,460 / 189,718 (x1.075 / x1.044); tier 2 Reference
174,621 / 194,053 (x1.086 / x1.020).

**15 us target (cold price p50):** MET with the opt-in Fast profile on the bulk
rung in every one of the 6 rounds (per-round p50 8.5-9.6 us; p99 12.1-17.3 us,
under 15 us in 4 of 6 rounds). NOT met by the Reference default (bulk p50
15.3-22.5 us per round on tier 2) nor by Fast on the `al_fast_opts` rung (p50
15.2-20.5 us). Unqualified: loaded host; a quiet-host pin is still owed.

Open observation (unresolved on this host): the tier-2 Reference path is not
faster than tier 1 and may be slightly slower on `accurate` (p50 min 122.5 vs
110.7 us, median 141.55 vs 136.95; slower in 5 of 6 rounds), while bulk and
fast are within the round-to-round spread. Re-measure on the quiet-host pin
before any claim either way.

Candidate baseline JSON (all default rows, final binary `d77beb03`, one run,
load 49.6% before / 22.2% after): `al-speed-finish-20261017-baseline-candidate\
i7-1260p-clang18-avx2-al-preset-ladder.json` (sha256 `c95c0fd3...7e4442`).
`bench/baselines/` is untouched.

## 5. Carry-memo decision: option (a), profile in the key (`d77beb03`)

`DeAmOptions::carry_al_opts` (api/fitting/deamer.hpp) is public, so any caller
can pass `al_fast_profile(...)` to the carry solve; "no fitting path constructs
Fast" (option b) holds for in-repo callers but not for the API. The memo
(`fitting/carry_memo.cpp`) now hashes and compares `AlOpts::profile`. Default
(Reference) keys hit and miss exactly as before; the parity hashes below prove
the default path unchanged.

Test `DeAmer.CarryMemoKeysOnTheAlKernelProfile` (tests/deamer_test.cpp): Fast
then Reference, and Reference then Fast, each cross-profile lookup must solve
(> 0 AL boundary solves) and return the profile's own cold bits; a repeated Fast
resolve must still be a hit (0 solves). With the fix reverted it fails: both
cross-profile lookups were served with 0 solves, and the served borrow differed
from the cold one (bits 4581709936198087010 vs 4581709936198087003, 7 ulp).

## 6. Gates at `d77beb03` (`-Preset vol-rel` unless stated)

- Build of pricing / simd / fitting / backtest slices, quote-bench, quote-slice
  tests, tick-path tests (`-Jobs 3`): exit 0, 225 steps, 0 warning/error lines.
- `^(pricing|simd)/`: 900 tests, 2 failed (`AmericanIv.PricePolishDefaultOffIsBitForBitControl`,
  `SimdNanSafety.BaryEvalPd_ExactNodeHit_ReturnsNodeValue`), the same 2 as a base
  `24e01644` run in this tree (893 tests); the 18-name failed/skipped set is
  byte-equal to base. `AlFastProfile.*` 6/6 and `BawResidualEval.FusedPairMatchesSeparateBits` pass.
- Certification `^(atx-vol-quote-slice$|tick-path:)`: 52/52.
- `^fitting/`: 38 failed of 1,028 (1,027 + the new test, which passes); 48 names
  byte-equal to the wave-1c set. `^backtest/`: 3 failed of 646; 9 names byte-equal.
- PCH-off `vol-hygiene`: the 21 TUs covering every touched header plus
  `deamer_test.cpp`: exit 0, 24 objects, 0 diagnostics.
- Kernel fingerprint (`vol-rel-avx2`): `53c4df68...dcea` at `d77beb03` and on the
  three A/B binaries.
- Parity keys OFF (`al-speed-finish-20261017-parity`, quote-bench sha256
  `a9e90fc3...5021b`): dev200 `c89d0739` MATCH, dev200b `4b273b08` MATCH,
  graded-v1 `1348f63d` MATCH.
- Bench `--atx-self-check` (final avx2 binary): 0 failures.

## 7. Follow-ups (outside this lane)

- Quiet-host pin of the A/B (load < 15%): cold price per tier, Fast vs Reference,
  and the tier-2 Reference vs tier-1 question in section 4.
- Fitting adoption of Fast: `quote_slice*` / `quote_surface*` and the bench policy
  do not thread `AlProfile` (forbidden scope here). The carry memo is now safe for it.
- Persistence: `surface_db` / `surface_archive` / `priced_surface_view` do not
  store the profile; a stored config reads back Reference, as documented.
- Batch: `american_price_batch_resolved` with Fast opts keeps Reference on its
  AVX2 lanes; the batch-pricing lane's four kernel-interface requests are a
  separate later lane.

## 8. Fix round 1 (2026-10-18, lane L0b-fix-al-speed, pool-3)

Closes review v2 (`C:\atx-cache\quote-benchmark\L0b-al-speed-409ee7aa-review-v2\review.md`)
MINOR-1 to MINOR-4. Raw output: `C:\atx-cache\quote-benchmark\L0b-fix-al-speed-20261018-al-ab\`
(manifest `sha256.txt`). Wrapped up early on a PM directive (fewer long benchmarks):
the attribution pass and the Fast-vs-Reference pass were not run.

| SHA | what |
|---|---|
| `683b2ad2` | bench only: `american/ladder_latency_dev200/<preset>` rows (MINOR-2 option (a)) |
| `ebdaccba` | test: adjoint bundle pinned to Reference under Fast opts (MINOR-3) |
| `e9b9b58a` | `AlProfile` comment: measured values plus round-up bounds (MINOR-4) |
| (this commit) | this section |

### Declared timing set (MINOR-2)

- **Latency set: the 104-contract ladder grid** (`grid()` in
  `bench/al_preset_ladder_bench.cpp`, rows `american/ladder_latency/<preset>`).
  Every latency number in this note is on that grid.
- **Error-bound set: the dev200 kernel-input CSV**
  `C:\atx-cache\quote-benchmark\al-speed-20261017-dev200-kernel-inputs\dev200-kernel-inputs.csv`
  (10,694 contracts, sha256 `8fb2f3d30c9e36e20f529350ab864ade704eedaf7f11997fddc55fb725cb644a`),
  with the grid and the 6,000-contract random set (section 3).
- Option (a) was also taken (`683b2ad2`, bench only, fingerprint unchanged):
  `ATX_VOL_AL_DEV200=<csv>` now also registers `american/ladder_latency_dev200/<preset>`,
  the same per-op timing loop over the dev200 contracts. It is supplementary. No
  counterbalanced numbers exist for it yet (only a one-run smoke).

### Counterbalanced A/B (MINOR-1): UNQUALIFIED (loaded host)

Protocol:
- Binaries were built in one tree (pool-3, `vol-rel-avx2`) and copied to SHA-named
  files: base `16291525`, tier 1 `a64dfb83`, an A/A control (a byte-identical copy of
  the tier-1 exe under a second label), and tier 2 `409ee7aa`.
- Before any timing, the kernel fingerprint `53c4df68...dcea` MATCHED on every binary.
- Every arm ran the same filter, `^american/ladder_latency/(accurate|fast|bulk)/`. The
  review's `...)$` form matches no row, because the run names end in
  `/min_warmup_time:0.500/repeats:5/manual_time`.
- Order: a 4-arm Williams square, run twice (8 cycles). Each arm runs once per position
  per square, and each ordered pair of arms is adjacent once.
- `ATX_VOL_FIT_WORKERS=1`. Every kept run shows `-> PINNED` (CPU 7).
- A run was rejected and re-run (up to 4 attempts) when its 2 s load sample before or
  after exceeded 25%. 40 attempts were rejected; 28 of 32 slots were kept. Four slots
  never got a clean run: tier 1 in cycles 2, 4 and 6, and the A/A copy in cycle 2.
  The kept runs are therefore not position-balanced for tier 1 (its five kept runs sit
  at positions 2, 2, 3, 3 and 4); this does not change the "not resolved" verdict
  (integration note, re-review NIT-5).
- Load over 669 continuous 2 s samples: mean 21.7%, median 14.9%, p90 53.5%, 50.2% under
  15%. That gives the label **unqualified (loaded host)**. Window: 2026-10-18
  23:21-23:52 UTC.

Single-op cold price latency, us. Each run's value is the median of 5 in-process
repetitions; the table gives the min and median across kept runs.

| preset | arm | n | p50 min | p50 median | p99 min | p99 median |
|---|---|---:|---:|---:|---:|---:|
| accurate | base | 8 | 106.90 | 107.55 | 121.80 | 134.60 |
| | tier 1 | 5 | 106.20 | 107.20 | 127.20 | 153.80 |
| | tier 1 A/A | 7 | 106.30 | 106.50 | 125.30 | 130.60 |
| | tier 2 Reference | 8 | 106.00 | 106.90 | 123.60 | 132.15 |
| fast | base | 8 | 27.00 | 27.10 | 32.70 | 34.50 |
| | tier 1 | 5 | 26.60 | 27.00 | 31.40 | 36.80 |
| | tier 1 A/A | 7 | 26.60 | 26.60 | 30.80 | 32.70 |
| | tier 2 Reference | 8 | 26.60 | 26.95 | 31.30 | 35.15 |
| bulk | base | 8 | 12.90 | 12.95 | 15.20 | 16.60 |
| | tier 1 | 5 | 12.30 | 12.30 | 14.40 | 15.70 |
| | tier 1 A/A | 7 | 12.30 | 12.30 | 15.00 | 15.60 |
| | tier 2 Reference | 8 | 12.30 | 12.30 | 14.90 | 15.55 |

Paired per-cycle tier 2 - tier 1 on p50, in us (the open question of section 4). The
A/A spread is the largest |A/A - tier 1| over the cycles.

- **accurate (primary): not resolved.**
  - n=5. Per cycle: -1.00, +0.80, -0.90, +3.20, +17.10.
  - Mean +3.84, median +0.80, signs 3 up / 2 down. Bootstrap 95% CI of the mean
    [-0.60, +10.70].
  - A/A spread 4.30 us.
  - The CI includes 0 and the mean is inside the A/A spread. One pair (+17.10, cycle
    8) carries the mean. The arm medians are 106.90 (tier 2) and 107.20 (tier 1).
- **fast: not resolved.** Mean +0.08, CI [-1.86, +1.86], A/A spread 4.00.
- **bulk: not resolved.** Mean -0.02, CI [-0.06, 0.00], A/A spread 0.30.

**No tier-2 default-path slowdown is confirmed, so no kernel change was made (PM
resolution 5).**

By the same rule, tier 1 - base on bulk p50 is -0.68 us: 5 of 5 pairs are negative,
the CI is [-0.74, -0.62], and the A/A spread is 0.30. Tier 1 (and tier 2 Reference, 8
of 8 pairs) is about 5% faster than base on the bulk rung: 12.30 vs 12.95 us median.
This is unqualified (loaded host) and revises section 2's "below this host's
resolution" for bulk only. On accurate and fast, tier 1 - base is within noise.

Not run, on the PM directive of 2026-10-18:
- the alprobe zone attribution pass;
- the counterbalanced Fast-vs-Reference pass on tier 2 (grid and dev200 rows).

The only Fast-vs-Reference ratios in this note are section 4's confounded, unqualified
ones. Do not quote them as qualified. The quiet-host pin of section 7 is still owed.

### MINOR-3: adjoint Reference pin test (`ebdaccba`)

`AdjointGreeksAmerican.FastProfileOptsServeTheReferenceAdjointBits`
(tests/adjoint_greeks_test.cpp):
- It runs the American put grid on the accurate, fast and bulk schemes, once with
  Reference opts and once with `al_fast_profile(...)`.
- It requires the same path, and all 9 fields bit-identical, wherever the adjoint
  path serves.
- Pin removed (`s.profile = AlProfile::Reference` in adjoint_greeks.cpp, blob
  `678c81bd`): the test fails with 276 volga mismatches.
- Pin restored (blob `1bc0c66d`, equal to HEAD): the test passes.

### MINOR-4: `AlProfile` bound text (`e9b9b58a`, comment only)

The values are the measured worst cases from the review v2 fast-error rerun (279/279
counters equal to the lane's), to 5 figures. The bound in brackets is the next 2-figure
round-up.

| quantity | measured | bound |
|---|---:|---:|
| price max abs | 7.9581e-13 | 8.0e-13 |
| price max rel | 1.8534e-13 | 1.9e-13 |
| IV max (bp) | 5.3660e-3 | 0.0054 |
| IV p99 (bp) | 1.9638e-8 | 2.0e-8 |
| delta p99 | 1.7941e-13 | 1.8e-13 |
| gamma p99 | 1.6880e-11 | 1.7e-11 |
| vega p99 | 1.4211e-10 | **1.5e-10** (was printed 1.4e-10) |
| theta p99 | 5.4645e-8 | 5.5e-8 |
| rho p99 | 1.4921e-9 | 1.5e-9 |
| vanna p99 | 1.2206e-10 | **1.3e-10** (was printed 1.2e-10) |
| volga p99 | 5.1159e-7 | 5.2e-7 |
| charm p99 | 1.4977e-7 | 1.5e-7 |

### Gates at `e9b9b58a` (`-Preset vol-rel` unless stated)

- `^pricing/(AdjointGreeksAmerican|AdjointGreeksEuropean|AlFastProfile)\.`
  (`ATX_VOL_FIT_WORKERS=3`): 25/25 passed, 2 disabled. This includes
  `AlFastProfile.*` 6/6 and the new test.
- PCH-off `vol-hygiene` check of american.cpp, adjoint_greeks.cpp,
  adjoint_greeks_test.cpp and american_test.cpp: exit 0, 0 diagnostics.
- Kernel fingerprint (`vol-rel-avx2` ladder bench, sha256 `17eb3ab8...`):
  `53c4df68...dcea` MATCH.
- Parity keys OFF: dev200 `c89d0739` MATCH (quote-bench sha256 `b81588c5...`).
