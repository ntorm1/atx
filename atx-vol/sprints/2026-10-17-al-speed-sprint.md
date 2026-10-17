# AL speed lane, resume 5 (2026-10-17)

Branch `feat/vol-al-speed-20261017` from `24e01644` (pool-4, run
`vol-20261017-al-speed`). Goal: the cold Andersen-Lake (AL) American price,
25.8 us in the 2026-10-16 perf audit (`american/ladder/ql_fast` = `al_bulk_opts`,
July baseline), toward 15 us, in two tiers: bit-identical first, then an opt-in
fast profile with a measured bound. Development cohorts only; no holdout; no
default changed. Every timing below is UNQUALIFIED (shared, loaded host; load
stated per run), `vol-rel-avx2`, `ATX_VOL_FIT_WORKERS=1`.

## Commits

| SHA | what |
|---|---|
| `16291525` | bench only: single-op latency rows, kernel fingerprint mode, profile mode |
| `a64dfb83` | tier 1: bit-identical hot-path hoists |
| `5a0a4d21` | tier 2: opt-in `AlProfile::Fast` kernel + error/Greeks bench rows + tests |

Stopped at user stop before the final timing pass. See
`2026-10-17-al-speed-wip-handoff.md` for what remains.

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
- One bulk price makes ~433 `norm_cdf` calls, which is ~47% of its time.
- The BAW Newton takes 6.32 iterations per node (nb 7) and 6.48 (nb 12), with 0
  bracket retries and 0 unconverged nodes.
- No heap allocation on the path (stack-bounded `AlBoundary` / `AlWorkspace`).
- Conclusion: the special function, not the iteration count, is the lever. A
  bit-identical tier cannot touch it, because changing erfc changes the doubles.

## 2. Tier 1 (bit-identical, `a64dfb83`)

- The critical-price Newton evaluated put/call residual and its derivative
  separately at every iterate. One fused evaluator now forms both from one d1
  with the Sx-invariant terms hoisted (same expressions, same FMA shapes).
- Specialized sweeps read K*exp(-(r-q)tau_j) and sqrt(tau_j) from the static bind
  (`AlWorkspace::geo_alpha` / `geo_sqrt_tau`). The JN sweep reuses the eqn_b
  tip's v and base for dN/db, dD/db.
- Premium-node and geometry exp / ledger tallies are published once per
  quadrature / bind (same totals).
- Tried and reverted (slower: the extra pass serialised the barycentric
  denominator chain): restructured `al_cheb_eval_t` and the geometry-bind division.

Proof:
- Kernel fingerprint (`ATX_VOL_AL_FINGERPRINT`, 48,660 doubles over every AL entry
  point: ladder rungs, 6,000 random contracts x 4 presets, convergence, Greeks AL/FD,
  delta/vega, IV, boundary/BAW, slices, resolved batch): sha256
  `53c4df681f70d5906856211f5a35454b12686c657bfd7f37cf7ff1adca18dcea` at base, at
  tier 1 and at tier 2 (Reference path).
- Quote bench, keys OFF, on the tier-1 binary: dev200 default `c89d0739...` MATCH,
  dev200b default `4b273b08...` MATCH, dev200 graded-v1 `1348f63d...` MATCH.
- `BawResidualEval.FusedPairMatchesSeparateBits` pins the evaluator bitwise.

Speed (8 interleaved rounds, p50 per op, min over rounds, load 13-99%):
bulk 17.7 -> 16.8 us, fast 39.6 -> 37.6 us, accurate 147 -> 127 us.

## 3. Tier 2 (opt-in `AlProfile::Fast`, `5a0a4d21`)

Policy key: `AlOpts::profile = AlProfile::Fast`, or
`al_fast_profile(opts)`, which returns the same scheme as `opts` (nullopt maps to
its explicit ACCURATE equivalent) with the profile flipped. Reference stays the
default everywhere. The profile is runtime-only: no record format persists it.

Same scheme (nodes, quadrature orders, sweep schedule, BAW seed, guards, clamps),
with three substitutions that are identities in exact arithmetic:
1. Phi / phi by the validated scalar Cody kernels (`pricing/scalar_erfc.hpp`).
2. The boundary ratio in log space. With b = xmax*exp(-sqrt(y)),
   log(b_i/b(u)) = sqrt(y(u)) - sqrt(y_i) and log(S/b(t)) = log(S/xmax) + sqrt(y(t)).
   This saves one exp and one log per fixed-point and premium node.
3. The BAW seed shares one d1 between its Black-76 leg and its smooth-pasting
   term (Black-76's d1 at F = Sx*e^{(r-q)T} is the BAW d1, and F*df = Sx*e^{-qT}).

Not adopted, on purpose:
- The AVX2 batch kernel (`al_init_put_boundary` keeps Reference; batch-pricing lane).
- The adjoint tape, which differentiates the Reference map. Its bumped re-solves
  now pin Reference too, so one bundle never mixes arithmetics.
- Truncation knobs (fewer sweeps or nodes). The bulk rung already is that
  trade-off, and its error is 1e-3, not 1e-13.

### Measured bound (`american/fast_profile_error/*`, Fast vs Reference, same scheme)

Sets:
- the ladder grid (104 contracts);
- 6,000 random contracts (both sides, 1 d to 5 y, vol 4-150%, zero and negative
  carry);
- 10,694 dev200 contracts.

The dev200 set is kernel-level: every American row with a predicted vol in the
`c89d0739` default run. The inputs are spot, strike, years and rate, with
q = sdiv + the aggregate dividend PV as a yield, and sigma = our own predicted_vol.
It is never a SpiderRock mark. Folder `al-speed-20261017-dev200-kernel-inputs`.

| set / preset | price max abs | price max rel (P>=0.01) | IV max bp | IV p99 bp | status mismatches |
|---|---:|---:|---:|---:|---:|
| grid accurate / fast / bulk | 1.28e-13 | 1.29e-13 | 3.6e-11 | 3.2e-11 | 0 |
| random accurate | 4.3e-14 | 1.6e-13 | 4.0e-4 | 1.2e-8 | 0 |
| random fast | 4.3e-14 | 1.6e-13 | 3.8e-5 | 9.6e-9 | 0 |
| random bulk | 4.3e-14 | 1.6e-13 | 1.8e-3 | 2.0e-8 | 0 |
| dev200 accurate | 8.0e-13 | 1.9e-13 | 9.5e-5 | 1.7e-9 | 0 |
| dev200 fast | 8.0e-13 | 1.9e-13 | 1.1e-5 | 6.4e-9 | 0 |
| dev200 bulk | 8.0e-13 | 1.9e-13 | 5.4e-3 | 6.9e-9 | 0 |

IV: one target (the Reference price) inverted under each profile,
`american_implied_vol` tol 1e-10, 100 iterations. The IV max sits on flat-vega
contracts, where the inversion is ill-conditioned for both profiles.

Greeks (`american_greeks_al`, p99 of |Fast - Reference|; the max is in the JSON):

| set | delta | gamma | vega | theta | rho | vanna | volga | charm |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| grid (3 presets, worst) | 7.0e-14 | 5.6e-13 | 3.4e-11 | 2.1e-9 | 4.8e-10 | 4.7e-11 | 2.0e-7 | 1.6e-9 |
| random (worst) | 1.4e-13 | 5.0e-12 | 1.4e-11 | 2.6e-8 | 1.4e-10 | 1.1e-10 | 5.2e-8 | 1.5e-7 |
| dev200 (worst) | 1.8e-13 | 1.7e-11 | 1.4e-10 | 5.5e-8 | 1.5e-9 | 1.2e-10 | 5.1e-7 | 1.1e-7 |

The tested bound is in `AlFastProfile.*` (tests/american_test.cpp), with the
documented text on `AlProfile` (api/pricing/american.hpp):
- |dP| <= 1e-12 * max(P, 1);
- |dIV| <= 0.01 bp on vega-bearing contracts;
- per-Greek p99 at ~100x the measured value;
- the call slice stays bit-identical to the scalar Fast call.

### Speed (PRELIMINARY: one run, not the interleaved A/B)

`american/ladder_latency/<preset>`, single-op p50 / p99 per cold price, load 30%,
tier-2 working-tree binary (same kernel as `5a0a4d21`), ATX_VOL_FIT_WORKERS=1:

| preset | Reference p50 / p99 | Fast p50 / p99 | p50 ratio |
|---|---:|---:|---:|
| bulk (`al_bulk_opts`, the audit's 25.8 us rung) | 18.5 / 32.0 us | 7.4 / 9.7 us | 2.5x |
| fast (`al_fast_opts`) | 39.7 / 60.0 us | 15.8 / 23.7 us | 2.5x |

On this single run the 15 us target is MET on the bulk rung with the Fast profile
(7.4 us p50). The Reference default is not at target (tier 1 alone: 16.8 us p50
min). This is unqualified, and it is one run, not the interleaved base / tier-1 /
tier-2 A/B. That A/B, the Greeks timing and the candidate baseline JSON were
not run (user stop).

## Gates (tier-2 SHA `5a0a4d21`, `-Preset vol-rel`)

- Build of the 7 gate targets: exit 0, 0 warnings.
- `^(pricing|simd)/`: 900 tests, 2 failed. The failed/skipped name set (18) is
  byte-equal to base and to tier 1. The 6 `AlFastProfile.*` tests pass.
- Certification `^(atx-vol-quote-slice$|tick-path:)`: 52/52 pass.
- `^fitting/`: 38 failed of 1027, 48 names, byte-equal to `1c-fitting-failed-skipped-names.txt`.
- `^backtest/`: 3 failed of 646, 9 names, byte-equal to `1c-backtest-names.txt`.
- PCH-off hygiene (`vol-hygiene`): 21 TUs including touched headers, exit 0,
  0 diagnostics.
- Parity keys OFF (`al-speed-20261017-parity-tier2`):
  - dev200 `c89d0739` MATCH;
  - dev200b `4b273b08` MATCH;
  - graded-v1 `1348f63d` MATCH.
- Not run: bench `--atx-self-check` (user stop).

## Follow-ups (not in this lane's scope)

- Fitting adoption: `quote_slice*` / `quote_surface*` and the bench policy do not
  thread `AlProfile` yet (forbidden scope). When they do, the carry memo key
  (`fitting/carry_memo.cpp` hash and equality) must include `profile`. Today
  both ignore it, which is harmless only because no fitting caller sets Fast.
- Persistence: `surface_db` / `surface_archive` / `priced_surface_view` do not
  store the profile. A stored config reads back Reference, as documented.
- Batch: `american_price_batch_resolved` with Fast opts keeps Reference on its
  AVX2 lanes, and the scalar fallback follows the opts. The batch-pricing lane
  can adopt the Fast scalar kernels (`fast_cdf`, log-domain bases) in its vector math.
