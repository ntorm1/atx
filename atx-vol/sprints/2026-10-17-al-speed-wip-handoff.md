# AL speed lane: WIP handoff at user stop (2026-10-17)

Branch `feat/vol-al-speed-20261017`. Base `24e01644`. Tier 2 code `5a0a4d21`;
this note sits in the commit after it. Full write-up:
`2026-10-17-al-speed-sprint.md`. All timings are unqualified (loaded host).

## Done (with evidence)

- Profile (base kernel, bulk preset). Per cold price:
  - BAW seed 25%, 2 JN sweeps 45%, premium quadrature 23%, geometry 6% (~45.7k cycles).
  - `std::erfc` / `norm_cdf` costs 20.4 ns vs Cody 6.7 ns, ~47% of the price.
  - Newton takes 6.3-6.5 iterations per node; there are no heap allocations.
  - Tables are in `C:\atx-cache\quote-benchmark\al-speed-20261017-profile\`.
- Tier 1 (`a64dfb83`, bit-identical):
  - Kernel fingerprint sha256 `53c4df68...dcea` is the same at base, tier 1 and
    tier 2. It covers 48,660 doubles over every AL entry point.
  - Keys-off parity: dev200 `c89d0739` MATCH, dev200b `4b273b08` MATCH, graded-v1
    `1348f63d` MATCH (both `al-speed-20261017-parity-tier1-final` and `-parity-tier2`).
  - p50 min over 8 interleaved rounds: bulk 17.7 -> 16.8 us, fast 39.6 -> 37.6,
    accurate 147 -> 127.
- Tier 2 (`5a0a4d21`, opt-in `AlOpts::profile = AlProfile::Fast` /
  `al_fast_profile(opts)`):
  - Same scheme, with Cody Phi/phi, a log-domain boundary ratio and a shared BAW d1.
  - Max error vs Reference over grid + 6,000 random + 10,694 dev200 contracts:
    price 8.0e-13 abs (1.9e-13 rel), IV 0.0054 bp (p99 <= 2e-8 bp),
    Greeks p99 delta 1.8e-13 .. volga 5.2e-7.
  - The bound is documented on `AlProfile` and tested by `AlFastProfile.*` (6 tests).
  - Raw rows: `al-speed-20261017-fast-error-wip1\fast-error.json`.
- Gates on `5a0a4d21`:
  - pricing/simd failed/skipped names equal base (18 names);
  - certification 52/52;
  - fitting 48 and backtest 9 names byte-equal to the 1c sets;
  - hygiene on 21 TUs clean;
  - parity MATCH x3.

## Half-done

- Speed of tier 2: ONE run only (load 30%), `american/ladder_latency`:
  - bulk Reference 18.5 us p50 -> Fast 7.4 us (p99 32.0 -> 9.7);
  - fast Reference 39.7 -> 15.8 us.
  - The 15 us target is met on the bulk rung with Fast in this run. It is not
    yet confirmed by the interleaved A/B.
- The final avx2 benches (`atx-vol-al-preset-ladder-bench`, `atx-vol-american-bench`)
  are BUILT at `5a0a4d21` in `build-vol-rel-avx2\bin` but not run.

## Exact next steps

1. Copy `build-vol-rel-avx2\bin\atx-vol-al-preset-ladder-bench.exe` to
   `al-ladder-tier2-5a0a4d21.exe` (same dir; archived copies lack DLLs).
2. Run an interleaved A/B with ATX_VOL_FIT_WORKERS=1, >= 6 rounds, filter
   `american/ladder_latency/`, over three binaries:
   - `al-ladder-16291525-basekernel.exe` (base);
   - `al-ladder-tier1-final.exe` (tier 1);
   - the tier-2 binary.
   Report p50/p99 min and median for every preset and its `_fastprofile` twin,
   with the load stated.
3. Greeks timing:
   - `american/greeks_latency/<preset>` on the tier-2 binary (Reference vs
     `_fastprofile`);
   - base vs tier 1 through `atx-vol-american-bench`, rows
     `amer/american_greeks_al/fast_cold/put/*` (build it at 16291525 and
     a64dfb83 by checking out `atx-vol/src atx-vol/include` only, then restore HEAD).
4. Re-run `ATX_VOL_AL_FAST_ERR=1 ATX_VOL_AL_DEV200=<al-speed-20261017-dev200-kernel-inputs\dev200-kernel-inputs.csv>`
   on the final binary. Confirm the counters equal the wip1 JSON.
5. Run `--atx-self-check` on the ladder bench.
6. Write a candidate baseline JSON (full default rows) into a NEW folder
   `al-speed-20261017-baseline-candidate\`, never `bench/baselines/`.
7. Fill the sprint note's speed table, then write the sha256 manifests.

## Kernel interface notes for other lanes

- Batch lane: no interface request was received. `american_price_batch_resolved`
  with Fast opts keeps Reference on its AVX2 lanes (`al_init_put_boundary` clears
  `ws.fast`), and the scalar fallback follows the opts. To adopt Fast, use the
  same scalar identities in its vector math.
- Fitting lane:
  - `AlProfile` is not threaded through `quote_slice*` / policy keys (forbidden
    scope here).
  - The carry memo (`fitting/carry_memo.cpp` hash + equality) must add `profile`
    when it is.
  - Records do not persist `profile`; a stored config reads back Reference.
