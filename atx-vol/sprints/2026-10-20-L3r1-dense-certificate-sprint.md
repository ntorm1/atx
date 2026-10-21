# Sprint note — L3r1 dense-certificate: certify the quote BOX, not the point (2026-10-20)

Lane `L3r1-dense-certificate`, branch `feat/vol-L3r1-dense-certificate-20261020`, base
`be379ad4` (the W2b root), pool-3, RunId `vol-20261020r3-L3r1`. Brief:
`.superpowers/sdd/2026-10-18-vol-sota/lane-L3r1-dense-certificate-brief.md`; evidence
`C:\atx-cache\quote-benchmark\L3r1-20261020-evidence\` (`RESULTS.md`, `sha256.txt`). Fable 5.1.
Checkpoint 1 = lever 1 (the box certificate). Levers 2 (calendar at quote resolution) and 3
(market wing cap) are NOT in this checkpoint; see §6.

## 1. Why

The robustness audit (`audit-robustness-report.md` §1-3) found the single fit routes refuse whole
chains for sub-tick certificate kinks: SPY (cohort5 graded-v1) 28 of 32 chains refused (convexity 15,
vertical slope 9, calendar 4), 89.0 % of rows unserved, served containment 68.2 %; dev200 graded-v1
13,594 unserved rows = 50.8 % calendar crossings + 39.1 % certificate failures. LEDGER L524 names the
mechanism: per-strike carry noise leaves nodes <= 7e-5 above their convex envelope. The exact
certificate asks "is this point curve arbitrage-free to 1e-6?"; a quote can only answer "does an
arbitrage-free curve exist inside the bid/ask boxes?". This lane makes the certificate ask the box.

## 2. What (opt-in: `graded.certificate = "box"`, default `exact` byte-identical)

- `american_price_slice.hpp/.cpp`: `AmericanPriceBox`, `AmericanBoxCertificate`,
  `project_convex_price_box` (the Dykstra projection MOVED here from `quote_slice.cpp`, unchanged
  numerics: uniform weights, Hildreth half-spaces for convexity + end slopes, a full increment for the
  box, 20,000-sweep cap) and `certify_american_box(fitted, boxes, bounds)`: each side's box is
  intersected with the certificate's hard per-node bounds (intrinsic, upper); an EXACT feasibility test
  (the greatest convex minorant of the upper edges must dominate the lower edges — necessary and
  sufficient for "a convex curve exists inside the boxes") refuses before any projection, naming the
  node; then the projection; then the UNCHANGED `AmericanPriceSlice::create` certifies P*. Refusals:
  `Unavailable` = no arbitrage-free curve inside the boxes; create()'s own code/message when P* fails a
  cross-side check (affine call-minus-put); `InvalidArgument` = malformed input.
- `quote_slice.hpp`: `QuoteSliceCertificate { Exact, Box }`, `QuoteSliceRepairConfig::certificate /
  box_half_ticks (0.5) / box_tick (0.01)`, `QuoteSliceDiagnostics::box` (a `QuoteSliceRepairRecord`).
- `quote_slice.cpp`: `quote_boxes` (a node side's own quote channel widened by h = half_ticks x tick
  on each side; bidless [0, ask + h]; a side without a quote held at its fitted price),
  `box_certify_slice`, and the `certify_and_serve` rung: exact certificate -> bounded-move repair ->
  channel repair (if on) -> BOX certificate (if on) -> refuse. P*'s moved nodes go through the
  existing `moved_nodes` path (re-implied; PriceOnly where the served price has no volatility), the
  nondividend joint check and every calendar rule act on P*. No node / volatility budget: the box is
  the budget. Under graded admission the box certificate acts at pass A too (it is the certificate,
  not a fallback rung); a pass-A slice whose served prices moved is relabelled B by the existing rule.
- `quote_bench_policy.hpp`: graded keys `certificate` ("exact" | "box") and
  `certificate_box_half_ticks` (0..100; refused without `certificate: box`); provenance emitted only
  when opted in.
- Tests: `AmericanBoxCertificate.*` (4), `QuoteSliceBoxCertificate.*` (3),
  `QuoteSurface.BoxCertificateServesThePassAFitAsGradeBWithoutARepairRung`, five strict CLI cases and
  `test_native_box_certificate_is_opt_in_with_provenance`; the two new suites added to
  `ATX_VOL_QUOTE_SLICE_SUITES` (the ctest's configure-time filter silently drops unlisted suites).

## 3. Numbers (USER STOP checkpoint, 2026-10-20 ~22:15 EDT; lever 1 + lever 2 at `01b5cfa3`)

SPY-only input (`fit-timing-20261017-cohort5\input\quotes-SPY.csv`, final binary `47d5dc69`,
`spy-final\`): exact graded-v1 served 1,410 / 12,866 eligible (unserved 89.04 %, containment 68.23 %,
MAE/spread 0.773; CSV identity `087e6a54` = the known SPY graded-v1 identity); box h = 0.5 tick
served 5,738 (unserved 55.40 %, containment 87.19 %, MAE 0.184, MAE/spread 0.250, 14 / 33 chains);
box h = 0 served 1,336 (unserved 89.62 %, containment 100.00 %, MAE 0.132, MAE/spread 0.148, 4 / 33
chains). Final binary, cohorts (`box-final\`, `audit-final\`): cohort5 graded-v1 served 8,375 -> 13,071
(unserved 61.58 -> 40.04 %), containment 91.59 -> 92.17 %, MAE 0.083 -> 0.110, MAE/spread 0.210 -> 0.166,
newly served 5,898 rows at 88.0 % containment, 1,202 rows no longer served (calendar displacement; they
had 71.9 % containment); box h = 0: served 8,669 (unserved 60.24 %), containment 97.08 %, MAE/spread
0.108; ladder point -> ladder box: served 10,157 -> 11,607 (53.41 -> 46.76 %), containment 98.51 ->
97.67 %; dev200 graded: unserved 21.26 -> 16.53 %, containment 98.82 -> 98.66 %, MAE/spread 0.1085 ->
0.1183 (served-in-both 0.1088 -> 0.1096); dev200b graded: 12.85 -> 9.46 %, 98.83 -> 98.83 %, 0.1164 ->
0.1163 (served-in-both 0.1161 -> 0.1162). Pooled dev200 + dev200b class / shape rows: 0 worse than the
brief's rule; cohort5 rows worse: high_vol (+4.6 pt unserved: MSTR 245 d lost to a calendar crossing),
long_dated (containment -4.4 pt: SPY long-dated chains newly served at 70-94 %). `check_served.py` on
the lever-1 box CSVs: 0 monotone / 0 convex / 0 calendar violations on all five (final CSVs not
re-checked before the user stop). Remaining SPY refusals are CALENDAR crossings of
0.5-2 cents at far-wing bidless placeholder quotes (`ask 0.01 / 0.02`): the rejected slice cannot absorb
them inside its own exact box because the accepted neighbour is pinned at its ask (a bidirectional
repair, out of scope). Targets (SPY <= 40 % unserved AND >= 98.5 % containment) NOT met in this
checkpoint: h = 0.5 buys coverage at box-edge containment, h = 0 buys containment without coverage.

## 4. Hashes (final binary `47d5dc69fa3b2bdbc6cf539ed831b3a14b9b27da535562df570f124f148d1269`)

Keys-OFF RAW parity at `01b5cfa3`, `ATX_VOL_FIT_WORKERS = 2`: dev200 auto `c89d0739` MATCH, dev200
graded-v1 `fc277413` MATCH, dev200b graded-v1 `46ad6c26` MATCH, cohort5 graded-v1 `171fa2d2` MATCH,
cohort5 ladder point `232d2c0d` MATCH (`L3r1-20261020-evidence\runs.log`). Keys-ON (box-final):
cohort5 graded-v1-box `61721aec`, graded-v1-box-h0 `716ae476`, ladder-point-box `8957d9f7`, dev200
graded-v1-box `92559918`, dev200b graded-v1-box `5eacbfb0`; SPY box `a3859ea1` (reproduced 3 / 3 on the
final binary), SPY box-h0 `05f64ba6`. Open: an identical-source relink (`76cc9303`) gave SPY box
`91f626fd`, differing from `a3859ea1` by <= 4.5e-13 USD on 44 rows of one chain (statuses identical).

## 5. Traps found

- `atx-vol-quote-slice` runs `--gtest_filter=${ATX_VOL_QUOTE_SLICE_SUITES}`: a new suite that is not
  added to that CMake list passes ctest without ever running (the direct exe run caught the only
  failing assertion of the lane's first test draft; the ctest said 100 %).
- Boxes of half-width h close a kink of up to 2h (the neighbours rise by h, the middle falls by h):
  a 2-tick market kink is held exactly by full-tick boxes and refused by three-quarter-tick ones.

## 6. Remaining levers (not in checkpoint 1)

- Lever 2, calendar at quote resolution (`quote_surface.cpp` :841 hunk): a crossing counts only when
  it exceeds h_short + h_long; below that the pair is repaired by a box-constrained calendar
  projection (calendar clamp + quote boxes, no budget) and marked `calendar_channel_repaired`.
- Lever 3, market wing cap (`graded.wing_mult_source = "policy" | "market"`): `mult_ceil` for the
  dense knot wing = max(3.0, market served multiple at the outermost two-sided strike x 1.1), <= 6.0.
