# Sprint note — L3r1 dense-certificate: certify the quote BOX, not the point (2026-10-20 / 21)

Lane `L3r1-dense-certificate`, branch `feat/vol-L3r1-dense-certificate-20261020`, base
`be379ad4` (the W2b root). Checkpoint 1 (2026-10-20, pool-3, RunId `vol-20261020r3-L3r1`, Fable
5.1): levers 1 + 2 at `01b5cfa3` (+ WIP docs `b1bddbc7`). Resume r4 (2026-10-21, pool-34, RunId
`vol-20261021r4-L3r1`, Opus 5.5): lever 3 at `c48519f1`, the both-arm tables, the check_served
gate on the final CSVs, the relink probe. Briefs:
`.superpowers/sdd/2026-10-18-vol-sota/lane-L3r1-dense-certificate-brief.md` and
`...-resume-brief.md`; evidence `C:\atx-cache\quote-benchmark\L3r1-20261020-evidence\`
(checkpoint, read-only) and `C:\atx-cache\quote-benchmark\L3r1-20261021-resume\` (`RESULTS.md`,
`sha256.txt`). Every lever is opt-in; keys OFF the five parity hashes are byte-identical.

## 1. Why

The robustness audit (`audit-robustness-report.md` §1-3) found the single fit routes refuse whole
chains for sub-tick certificate kinks: SPY (cohort5 graded-v1) 28 of 32 chains refused (convexity 15,
vertical slope 9, calendar 4), 89.0 % of rows unserved, served containment 68.2 %; dev200 graded-v1
13,594 unserved rows = 50.8 % calendar crossings + 39.1 % certificate failures. LEDGER L524 names the
mechanism: per-strike carry noise leaves nodes <= 7e-5 above their convex envelope, and the dense
knot wing is capped at mult_ceil 3 x ATM against a market wing near 4.1 x. The exact certificate
asks "is this point curve arbitrage-free to 1e-6?"; a quote can only answer "does an
arbitrage-free curve exist inside the bid/ask boxes?". This lane makes the certificate ask the box.

## 2. What (all opt-in)

- Lever 1, BOX certificate (`graded.certificate = "box"`, `graded.certificate_box_half_ticks` h,
  default value 0.5; `exact` byte-identical). `american_price_slice.hpp/.cpp`: `AmericanPriceBox`,
  `AmericanBoxCertificate`, `project_convex_price_box` (the Dykstra projection MOVED here from
  `quote_slice.cpp`, unchanged numerics) and `certify_american_box(fitted, boxes, bounds)`: each
  side's box intersected with the certificate's hard per-node bounds; an EXACT feasibility test (the
  greatest convex minorant of the upper edges must dominate the lower edges) refuses before any
  projection, naming the node; then the projection; then the UNCHANGED `AmericanPriceSlice::create`
  certifies P*. `quote_slice.cpp`: `quote_boxes` (own quote channel widened by h = half_ticks x tick;
  bidless [0, ask + h]; a side without a quote held at its fitted price), `box_certify_slice`, the
  `certify_and_serve` rung: exact -> bounded-move repair -> channel repair (if on) -> BOX -> refuse.
- Lever 2, calendar at quote resolution: under Box the channel calendar repair projects into the
  QUOTE boxes with no budget (`repair_quote_slice_calendar_in_channel`, `project_in_channel(box_h)`),
  and `fit_quote_surface` opens that repair whenever the box certificate is on (`repair_in_channel`
  guard, the calendar-pass hunk). A crossing the boxes cannot absorb stays refused (0-violations gate).
- Lever 3, market wing ceiling (`graded.wing_mult_source = "policy" | "market"`, default `policy`
  byte-identical): `QuoteSliceWingMultSource` in `QuoteSliceRepairConfig` (the lane's own block, not
  the owner's channel config), applied in `solve_channel_native` after the knot solve:
  `mult_cap = max(mult_ceil, min(6, 1.1 x m))`, m = the larger market multiple (mid IV, else the
  channel centre, over the fit's ATM reference) at the outermost two-sided channel quote of each
  wing. The solve never reads the ceiling, so only the served clamp moves; recorded in
  `QuoteSliceChannelDiagnostics::wing_mult_cap`. Native knot spline only; the term prior's multiples
  keep the policy ceiling (`quote_surface.cpp:157`, not this lane's hunk).
- Tests: `AmericanBoxCertificate.*` (4), `QuoteSliceBoxCertificate.*` (4, incl. the box calendar
  repair), `QuoteSurface.BoxCertificate*` (2), `QuoteSliceChannel.MarketWingCeiling*` (2), strict CLI
  cases + two provenance tests; the two new gtest suites are listed in `ATX_VOL_QUOTE_SLICE_SUITES`.
  Nine revert checks (R1-R6 checkpoint, R7-R9 resume), each failing exactly its tests (lane report).

## 3. Numbers (final binary `90811519`, lane @ `c48519f1`; both certificate half-widths)

Scorer = the audit's definitions (`audit_l3r1r4.py`); SpiderRock `reference_price` is the yardstick only.
`ATX_VOL_FIT_WORKERS = 2`; timings unqualified. Full tables: `L3r1-20261021-resume\RESULTS.md`.

| cohort | arm | served / eligible | unserved | containment | MAE USD | MAE/spread | p99 |
|---|---|---|---|---|---|---|---|
| cohort5 | graded-v1 (OFF) | 8,375 / 21,801 | 61.58 % | 91.59 % | 0.0834 | 0.2102 | 1.561 |
| cohort5 | box h = 0.5 | 13,071 | 40.04 % | 92.17 % | 0.1095 | 0.1662 | 1.594 |
| cohort5 | box h = 0 | 8,669 | 60.24 % | 97.08 % | 0.0636 | 0.1076 | 0.923 |
| cohort5 | graded-v1 + wing | 8,667 | 60.24 % | 91.69 % | 0.0810 | 0.2095 | 1.548 |
| dev200 | graded-v1 (OFF) | 50,348 / 63,942 | 21.26 % | 98.82 % | 0.2343 | 0.1085 | 1.972 |
| dev200 | box h = 0.5 | 53,372 | 16.53 % | 98.66 % | 0.2323 | 0.1183 | 1.954 |
| dev200 | box h = 0 | 53,361 | 16.55 % | 98.91 % | 0.2322 | 0.1181 | 1.950 |
| dev200b | graded-v1 (OFF) | 54,355 / 62,369 | 12.85 % | 98.83 % | 0.2393 | 0.1164 | 1.760 |
| dev200b | box h = 0.5 | 56,466 | 9.46 % | 98.83 % | 0.2389 | 0.1163 | 1.772 |
| dev200b | box h = 0 | 56,366 | 9.63 % | 98.88 % | 0.2392 | 0.1163 | 1.773 |

- SPY (SPY-only input, 12,866 eligible): graded-v1 1,410 rows / 4 chains / 89.04 % unserved / 68.23 % containment / MAE/spread
  0.773; box h = 0.5 5,738 / 14 / 55.40 % / 87.19 % / 0.250; box h = 0 1,336 / 4 / 89.62 % / 100.00 % / 0.148; graded-v1 + wing
  1,702 / 5 (+45 d) / 86.77 % / 72.74 % / 0.673. Box + wing: h = 0.5 byte-identical; h = 0 same rows (only refusal labels change).
- `check_served.py`: 0 monotone / 0 convex / 0 calendar violations and 0 duplicate disagreements on every ON CSV (both h arms,
  every cohort, SPY, the wing arms).
- Ruling (a): the newly served rows sit at 88.0 % (cohort5) / 97.5 % (dev200) / 98.9 % (dev200b) containment under h = 0.5 and
  at 99.9 % / 99.9 % / 99.9 % under h = 0; served-in-both MAE/spread dev200 0.1088 -> 0.1096 (h = 0.5) / 0.1095 (h = 0), dev200b
  0.1161 -> 0.1162 / 0.1160 -> 0.1161 (marginally higher: the strict clause fails by <= 8e-4).
- Pooled dev200 + dev200b class / shape groups worse than the rule (+0.5 pt unserved or -0.3 pt containment): h = 0 -> 0 of 24;
  h = 0.5 -> 2 (long_dated containment 99.15 -> 98.81 %, no_two_sided_near_money 99.11 -> 98.69 %). The checkpoint's "0" was
  per-cohort; pooled by summed counts it is 2.
- cohort5 chains refused: steep_negative_skew 20 / 26 -> 16 (h = 0.5) / 20 (h = 0); high_curvature 14 / 32 -> 11 / 11;
  index_etf_dense 28 / 32 -> 18 / 28; mega_cap 8 / 24 -> 5 / 5.
- Lever 3 effect: SPY convexity refusals 15 -> 5 on the exact certificate (the 3 x ATM clamp kink) and SPY 45 d served; under the
  box certificate no coverage change (the unlocked SPY chains are calendar-refused; dev200 moves 65 prices of 2 chains by
  <= 2.4e-3, all inside their quotes; dev200b byte-identical).
- Brief targets: SPY unserved <= 40 % with containment >= 98.5 % NOT met by either arm (h = 0.5: 55.4 % / 87.2 %; h = 0: 89.6 % /
  100 %). The remaining SPY refusals are calendar crossings at far-wing bidless quotes (L3q).
- Relink probe (concern 4): two forced relinks differ from each other only in the PE TimeDateStamp byte; SPY box h = 0.5 gives
  `a3859ea1` on both — not reproduced.

## 4. Hashes

Final binary `908115192c11b01d6b66f0d74659cc029212180088a996ae730c96607cf2da6b` (lane source `c48519f1`, pool-34
`build-vol-rel`). Keys-OFF RAW parity, `ATX_VOL_FIT_WORKERS = 2`: dev200 auto `c89d0739` MATCH, dev200 graded-v1
`fc277413` MATCH, dev200b graded-v1 `46ad6c26` MATCH, cohort5 graded-v1 `171fa2d2` MATCH, cohort5 ladder point
`232d2c0d` MATCH (`L3r1-20261021-resume\parity-console.txt`). Keys-ON box arms re-proved on the final binary (lever 3
off): SPY box h0.5 `a3859ea1`, SPY box h0 `05f64ba6`, dev200 box h0.5 `92559918`, dev200b box h0.5 `5eacbfb0` = the
checkpoint hashes; cohort5 box h0.5 `61721aec`, box h0 `716ae476`, ladder box `8957d9f7` (checkpoint, hash-verified).
New keys-ON: dev200 box h0 `8a3e66e5`, dev200b box h0 `2693e4ad`; wing arms cohort5 graded-v1 `53754f1a`, cohort5 box h0
`feed5e66` (box h0.5 = `61721aec`), dev200 box h0.5 `c8cb896d` / h0 `540a61f0`, dev200b box h0.5 / h0 = `5eacbfb0` /
`2693e4ad`, SPY graded-v1 `9ab554a0`, SPY box h0 `0f585c22` (box h0.5 = `a3859ea1`). SPY graded-v1 identity `087e6a54`.

## 5. Traps found

- `atx-vol-quote-slice` runs `--gtest_filter=${ATX_VOL_QUOTE_SLICE_SUITES}`: a new suite that is not
  added to that CMake list passes ctest without ever running.
- Boxes of half-width h close a kink of up to 2h (the neighbours rise by h, the middle falls by h).
- A pool tree's `build-vol-hygiene` can be configured at an older source state: `check` then refuses
  newer TUs with "no object target found ... (is its target configured?)" — run
  `atx-build.ps1 configure -Preset vol-hygiene` first (pool-34, 2026-10-21).
- The served-multiple clamp is a CONCAVE kink in the smile, i.e. a convexity failure of the price
  certificate wherever the market wing exceeds mult_ceil x ATM (lever 3's test reproduces it).

## 6. Remaining (out of this lane by PM ruling)

- The SPY far-wing bidless calendar crossings (0.5-2 cents at `ask 0.01 / 0.02` placeholder quotes)
  are lane L3q's (bidirectional repair inside the quote boxes, PM ruling 6a-6).
- h = 0 vs h = 0.5 is decided after L3q on the combined distance-to-market table (ruling 6a-5); the
  committed key default value (0.5) is unchanged.
