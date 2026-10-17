# Smooth-dividend lattice lane: WIP handoff at user stop (2026-10-16)

- Branch `feat/vol-smooth-dividend-20261016`, base `a402d615026c327614c1040e383a1e86627c2ed1` (dividend-schedule lane). Lease pool-3, run `vol-20261016-smdiv` (released at stop).
- **State: WIP. This commit has NOT been compiled.** The exploration binaries (below) compiled clean under `vol-rel`, but after them I deleted two control-variate schemes and rewrote the new test file. A build of the new test targets was started and stopped at the user stop (it needed a 131-object rebuild).
- Receipts: `C:/atx-cache/quote-benchmark/smooth-div-20261016-diag/` (per-strike CSVs and the scratch harness `zz_smooth_div_diag_test.cpp.txt`, never committed) and `C:/atx-cache/quote-benchmark/smooth-div-20261016-explore/` (exploration runs, policies, `run_explore*.sh`).

## Diagnosis: why dividend chains fail AmericanPriceSlice certification

There are three separate causes. Columns: certification violations / worst violation in tolerance units; δ is the spliced European price minus Black-76 at the dated forward.

| chain | variant | viol. | worst | finding |
|---|---|---|---|---|
| ASML 20924 | spliced CRR, 301 steps (default) | 48 | 46341 | CRR staircase (lattice noise) |
| ASML 20924 | CRR 300 vs 302 (odd/even parity) | 69 | 68 | parity is part of the noise |
| ASML 20924 | spliced + BBS, 301 | 10 | 487 | noise mostly gone |
| ASML 20924 | spliced BBSR, 4801 | 12 | 178 | converged, still refused |
| ASML 20924 | spliced European, BBSR 4801 | 24 | 178 | model, not lattice |
| ASML 20924 | Black76(F_dated, σ(K)) | 0 | – | the fitted curve itself is convex |
| ASML 20924 | δ = V&N European − Black76 | – | – | 0.023 to 0.235, smooth but concave |
| TSM 20896 | spliced CRR 301 / BBS 301 | 4 / 6 | 14164 / 2984 | noise, then carry |
| TSM 20896 | Black76 at per-strike F(K) | 3 | 725 | 17 distinct vendor per-strike q: genuine kinks |

1. **Lattice noise.** The CRR exercise-boundary staircase and odd/even parity. BBS or a strike-centred tree removes it.
2. **Model mismatch.** The dense fit is convex in Black-76 price space at the dated-cash forward, with tight (zero-butterfly) runs of strikes. The spliced (V&N spot-shift) European is not Black-76 at that forward; the gap δ is concave across those runs. So no amount of smoothing certifies a spliced slice there. The escrowed model's European IS Black-76 at the dated forward.
3. **Per-strike carry.** Vendor per-strike `sdiv` gives kinks on tight runs. This also fails the aggregate route (33 dev200 chains fail on both routes, e.g. META, TSM, V).

## Implemented (uncommitted until this WIP commit)

- `DiscreteDivScheme` in `api/pricing/american.hpp`, plus `to_string`, `discrete_div_scheme_admits` and `discrete_div_coarsest_steps`. It is a new defaulted last argument of `american_discrete_div_price`.
  - Schemes: `spliced` (default, bit-identical), `spliced-bbs`, `spliced-bbsr` (Richardson N, N/2, floored at intrinsic), `escrowed-bbs`, `escrowed-lr`.
  - `escrowed-lr` is an escrowed-dividend tree using the Leisen-Reimer construction (Peizer-Pratt 2, odd steps, strike-centred), which serves as the "strike-aligned nodes" item. It falls back to CRR geometry when p saturates (tiny σ, at the IV bracket's lower end). Exercise is tested against X + escrow(t), with a cum test on the ex-step.
  - The BBS final step handles cash on the last step analytically: call → strike K+D, put → P(K+D) − P(D), American call cum-exercise → K.
  - There is one `LatticeBuffers` per call, and BBSR's coarse pass reuses it.
  - Greeks, bundle and sensitivities stay on the spliced CRR.
  - Certification code and tolerance are untouched.
- `QuoteSliceConfig` / `QuoteSliceFit::discrete_div_scheme`. Validation and IV-bound steps go through `discrete_div_coarsest_steps`.
- Bench policy keys `dividend_lattice` and `dividend_lattice_steps` (strict parsing; emitted only when not the default). The route JSON gains `lattice_steps` and `lattice_scheme`.
- Tests written but never compiled:
  - `tests/discrete_div_smoothing_test.cpp`: bit-identity, typed refusals, independent BBS, expiry and grid-bottom boundaries, convergence to the exact spliced European / Black76 / Roll-Geske-Whaley, parity, and synthetic ASML strike-shape certification;
  - `tests/discrete_div_smoothing_alloc_test.cpp`;
  - the new target `atx-vol-discrete-div-smoothing-tests` (labelled `atx_vol_fast`).
  - Thresholds are UNCALIBRATED. Expect to tune the 2.5e-2, 5e-3 and 5e-5 bounds and the strike-shape expectations.

## Exploration results (unofficial; explore binaries; `--dividends funding-pv`; inferred-schedule policy)

Lattice chains certified / refused:

| cohort | spliced (default) | spliced-bbs | spliced-bbsr | escrowed-bbs | escrowed-lr |
|---|---|---|---|---|---|
| dev200 | 117/63 | 126/54 | 121/59 | 127/53 | **149/31** |
| dev200b | 170/20 | 180/10 | 174/16 | 181/9 | **183/7** |

- **escrowed-lr transitions:**
  - dev200: of 30 chains refused on the lattice but served by the aggregate route, 28 now certify; 4 that failed on both routes now certify; none are lost.
  - dev200b: 13 of 14 recover, and 1 is lost (HSBC).
  - ASML: 11 of the 12 chains the aggregate route can serve now certify, so the optional CN/Rannacher PDE (item 3) was not needed.
- **Dropped:** the control variates (Black76 plus lattice premium), at dev200 130/50 and 141/39. They break tight linear deep-ITM runs, because the BBS mixture's ripple is about 1.4%.
- **Runtime (shared host, not qualified):** lattice 2.06 s → 10.22 s; fit 16.6 s → 24.5 s on dev200.
- **SHA256:**
  - `explore4.exe` `88521968…`;
  - `dev200-x4-escrowed-lr.csv` `2cfda793…`;
  - `dev200b-x4-escrowed-lr.csv` `5a9c1f5f…`;
  - `dev200-none.csv` `abc75955…` equals the dividend lane's opt-in receipt, which confirms the spliced default under opt-in is bit-identical;
  - `dev200b-none.csv` `747d3e6c…`.

## Gates run

None on this commit. Earlier exploration builds of `atx-vol-quote-bench` compiled with no warnings. No ctest, hygiene check or gtest ran on the final scheme set.

## Next steps (in order)

1. Lease a pool tree. `check` `american_discrete_div.cpp`, `quote_slice.cpp`, `quote_fit_policy.cpp` and `quote_bench_main.cpp`. Build `atx-vol-discrete-div-smoothing-tests` and `atx-vol-discrete-div-tests`, then calibrate the test thresholds.
2. Gates:
   - `atx-vol-discrete-div-tests` (the default pins must not move);
   - `atx-vol-quote-slice-tests`;
   - `atx-vol-dividend-schedule-tests`;
   - anchored `-Ctest -R`;
   - PCH-off `vol-hygiene` `check` for the owners of `american.hpp` and `quote_slice.hpp`;
   - `git diff --check`;
   - the 100-column check.
3. Rebase onto `feat/vol-resume4-20261016` if it has moved (the dividend lane was being merged there). Commit as `feat(vol): ...` with the attribution line.
4. Official benchmarks through `tools/quote_benchmark.py` with the policy `inferred-schedule` + `"dividend_lattice": "escrowed-lr"`: dev200 → `20261016-auto-divsmooth-v1`, dev200b → `20261016b-auto-divsmooth-v1`.
   - Default parity must stay `c89d0739…` / `4b273b08…`.
   - Compare on exact intersections against the dividend lane's runs: all rows, payers, tenor, ASML, anti share; dollars and spread units.
   - Also report certified-chain counts and runtime per chain.
5. Re-run the diag harness (receipts `.txt`) with `escrowed-lr` on ASML 20924 and TSM 20896 to fill the table's final column. Write the sprint note `2026-10-16-smooth-dividend-lattice-sprint.md` and the ledger lines.

## Dividend-review minors (review `dividend-a402d615-review-v1/review.md`): NOT STARTED

All of these go in a separate commit after the main work.

- **M1:** fix the claim in the header comment at `dividend_schedule.hpp:41-42`; add the wide-bracket probe test (0.25 at days 10 and 101, a 1.00 special at day 150 in (110, 200], which today becomes two `SumSplit` events at 111.0 and 189.19).
  - Note: the special is 4 whole units, so a "split only near whole units" gate would NOT catch this probe.
  - A better signal is round(inc/unit) > the number of events that fit in the window. Report that as ambiguous rather than guess, and measure it on the 59 dev200 split increments first.
- **M2:** in `place_events` (`dividend_schedule.cpp:307`), re-solve count==1 increments from the final `rest`, and mark Exact iff within `day_tolerance` of an in-window integer. Run the schedule tests: classification can move both ways.
- **M3:** `quote_bench_main.cpp:307`. Parse `ddiv` only on the opt-in path, and turn a bad cell into a typed row rejection, not an abort.
- **N2:** compare the life/instant counts before `applies()`, and document the 0 return at `dividend_schedule.hpp:276`.
- **N3:** move the stem to `ATX_VOL_SLICE_OWNED_ELSEWHERE` (`slices.cmake:61`).
- **N4:** drop `<limits>` from `dividend_schedule.cpp:5`.
- **N5:** append a ledger correction: 17 ASML chains have an ex-date in life (13 certification failures + 4 insufficient quotes); the dev200b anti share is 0.0435; 64 of dev200's +1,322 rows come from a lattice chain leaving `arbitrage_failure`.
