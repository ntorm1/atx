# Knot-ladder sprint (2026-10-17)

Lane `knot-ladder`, branch `feat/vol-knot-ladder-20261017`, frozen base
`24e01644c2a68d54f4c02de860b848d7dc86dbc6`, pool lease `pool-3` (run id
`vol-20261017-knot-ladder`). Requirements:
`.superpowers/sdd/2026-10-17-vol-resume5/` SHARED-BRIEF and
`lane-knot-ladder-brief.md`; design from `2026-10-16-knot-ladder-wip-handoff.md`
(`0e5f393a`, read-only).

Commits:

| SHA | What |
|---|---|
| `2bec949f` | `PriceOnly` row status, bench key `price_only`, scorer/composer, tests |
| `1ada69a0` | Opt-in knot-first ladder (`graded.ladder: "knot-first"`), tests |
| `6216978c` | Containment-first calendar order, test |
| `8a4caeb4` | Test of the held-point calendar rung |
| (this commit) | `docs/quote_benchmark.md`, this note |

Artifacts (new folders under `C:/atx-cache/quote-benchmark/`, each with
`sha256.txt`): `knot-ladder-20261017-{bin,policies,scripts,diag,parity-v2,bench-v2}`.
Superseded and kept as written: `knot-ladder-20261017-parity` (complete keys-off
runs of the pre-containment binary `1a116c6d`) and `knot-ladder-20261017-bench`
(an aborted batch, receipts only, no CSV). Final binary
`knot-ladder-20261017-bin/final2/atx-vol-quote-bench.exe`, sha256
`dfe3f1b807645887425b771f0de85cc8bb373c868b2b4042705dad7d88fe8d11` (vol-rel,
built from `6216978c`'s tree). Every run: `quote_benchmark.py run --model
quote-slice --dividends funding-pv`, `ATX_VOL_FIT_WORKERS=2`. Timings are
unqualified (loaded host). srPrc/srVol are scoring labels only.

## 1. PriceOnly (commit 2bec949f)

A repair (bounded convexity, channel, or calendar) can move a served node to a
certified price that has no identifiable American volatility under the row's
own carry, typically below the zero-volatility exercise value. Before, that
row became `InvalidVolatility` with neither price nor vol. Now it is
`QuoteSliceEvaluationStatus::PriceOnly`: the certified price is kept,
`fitted_iv` is NaN, `evaluate_quote_slice` still refuses the contract, and
`prices_produced` counts Success + PriceOnly. Both calendar repairs refresh and
re-imply PriceOnly rows, so a PriceOnly row can become Success again (tested).
**Superseded in fix round 1 (section 9):** a PriceOnly row is final; a later
calendar repair refreshes its price only, and `evaluate_quote_slice` refuses it.

Bench: top-level policy key `price_only` (boolean; static and graded). On, a
PriceOnly row is `status=ok_price_only` with its price, blank vol and
`vol_status=unidentifiable`, never counted in `served_rows`; the report adds
`price_only_rows` (per grade and per method under the ladder). Off, the row
stays `status=invalid_volatility` with no price; its `vol_status` now reads
`unidentifiable` instead of `not_evaluated`. That one cell is the only keys-off
output change (PM ruling; proven in section 4). Scorer: price coverage over
`ok | ok_price_only`, vol coverage over `ok`. Composer: `--served-status` takes
several values; the default stays `ok` only (strict opt-in; the design note had
proposed `ok ok_price_only`).

## 2. Knot-first ladder (commits 1ada69a0, 6216978c)

Opt-in `graded.ladder: "knot-first"` (default `off` = point-first, bit for
bit). Requires point profiles; refused with a `knot-spline` profile or with the
dominance gate (`channel.point_max_outside_fraction`), both at policy load and
in `fit_quote_surface` (Err). The A rule is unchanged.

1. **Pass A, both rungs eagerly.** The knot slice (term-prior-free channel fit,
   with the profile's bounded-move convexity repair) serves A when it
   certifies, is inside the market and has own point evidence; else the point
   fit serves A when it certifies and is inside the market. `ladder_fallback`
   names why the knot slice did not serve: `certificate`, `knot_failure`
   (too few channels / numerical), `containment`. A knot-served slice keeps its
   in-market point fit for the calendar rung.
2. **Channel passes** (B1/B2 refits) unchanged for expiries neither rung
   served; a refit outside the market is refused `outside_market`.
3. **Repair rung** (`graded.repair: channel`, the slice-repair lane's step):
   the knot fit with the repair, then the point fit; first certified in-market
   fit serves (point B; knot B/C by mid strikes).
4. **Market containment.** No rung serves prices outside [bid, ask] on more
   than `ladder_max_outside_fraction` (point fits) or
   `ladder_channel_max_outside_fraction` (knot slices, refits) of the strictly
   two-sided quotes and on at least `ladder_min_outside_quotes` of them
   (defaults 0.05 / 0.05 / 1). Every calendar repair or retry must end inside
   too. The point-fit rung therefore "sees the channel": a point fit with more
   than 5% of its quotes outside is never served unless the policy relaxes
   `ladder_max_outside_fraction` explicitly. The channel limit is separate
   because a channel fit floors each channel's half-width at
   `channel.min_half_width_vol`, so it can sit outside a market narrower than
   that floor; relaxing it is an explicit policy choice (the `point` policy
   below does, the `strict` policy does not).
5. **Calendar, single pass, confidence-ordered.** Every served slice is visited
   ONCE in a total order on intrinsic properties, and each is resolved against
   the slices accepted before it, before the next is visited:
   1. containment under the smaller of the two limits first: a slice served
      only under a relaxed limit never out-ranks one inside the market (in v3,
      kept gated point fits held the slots: AAPL 65 d for AAPL Nov 20, and
      SPY 7/8/12 d plus 65 d, 61% outside, for SPY 23 d);
   2. grade tier (A before B before C: own evidence first; a pass-A slice keeps
      its A tier until the final relabel);
   3. prices that are the fit's own (or moved only by the bounded convexity
      repair, within `repair_half_spreads` of each half-spread) before prices
      certified by the channel repair or a repair rung (which can move a quoted
      side across its whole channel);
   4. more strictly two-sided quotes (more market information);
   5. nearer maturity (distinct maturities make the order total).

   Because the order depends on no processing state and each visit sees only
   higher-ranked slices, a lower-confidence slice can no longer block a
   higher-confidence slice's repair. That was the v3 displacement mechanism: the
   two-pass calendar pass accepted every clean slice first, so evidence-0 repair-
   rung point fits (META 20749/20752/20784) blocked META 20770's repair.
   A crossing slice gets, in order: its own bounded-move calendar repair; the
   held point fit (clean, then repaired; `ladder_fallback=calendar`); the
   stronger-prior retry under the accepted slices' prior (clean, then
   repaired); the channel calendar repairs of its own prices, then of the
   retry's; else it is refused.

Provenance per slice: `method` (`point`/`channel`/`channel_refit`),
`ladder_fallback`, `knot_status`, `knot_error`, `point_error`, `repair_rung`,
`outside_fraction`, `outside_quotes`; report counts per method and fallback.

### Ordering variants measured (dev200, `point` policy, vs graded-v1)

| binary | third key | vol cov | int MAE | lost (containment / displacement) | cohort5 AAPL Nov 20 |
|---|---|---|---|---|---|
| dev2 `dc629fc9` | two-sided quotes | 85.95% | -2.76% | 2,764 (1,726 / 1,038) | served |
| dev3a `342d5bb1` | own evidence (graded-v1's key) | 84.66% | -2.59% | 1,948 (1,324 / 624) | **refused** |
| final2 `dfe3f1b8` | containment first + two-sided | 85.97% | -2.75% | 2,764 (1,726 / 1,038) | served |

Own evidence lowers the displacement count (graded-v1 itself ordered by
evidence, so its winners win again) but loses vol coverage below v3's 84.89%
and refuses AAPL Nov 20; it was rejected.

## 3. Tests and gates (code at 6216978c, tests at 8a4caeb4)

New gtests (`tests/quote_surface_test.cpp`): PriceOnly rows below the
zero-volatility value; calendar repair re-implying a PriceOnly row; repair flags
ORed; knot slice served at pass A and equal to the standalone fit; knot failure
falls back to the point fit (A); point-rung containment and its two explicit
relaxations; channel refit refused `outside_market`; single-pass calendar order
(H repaired, E refused; the reverse with the ladder off); containment-first
order; a crossing knot slice served by its held point fit; validation
(channel profile, gate, bad limits). CLI pytest: key
strictness (`price_only`, ladder keys), keys-off byte identity
(`price_only` + `ladder: off`), ladder provenance. Scorer/composer pytest:
price-only rows price-served not vol-served; composer multi-status routing and
duplicate refusal.

| gate | result |
|---|---|
| build `atx-vol-quote-slice-tests atx-vol-implied-forward-tests atx-vol-quote-bench` (vol-rel, -Jobs 2) | exit 0 |
| gtest quote-slice | 108/108 |
| gtest implied-forward | 27/27 |
| ctest `^(atx-vol-quote-slice\|atx-vol-implied-forward\|atx-vol-quote-benchmark-tools\|atx-vol-quote-benchmark-native\|atx-vol-quote-benchmark-campaign)$` | 5/5 |
| pytest test_quote_bench_cli (native) / test_quote_benchmark (scorer, composer) | 56/56, 97/97 |
| PCH-off `vol-hygiene` check: quote_slice.cpp, quote_surface.cpp, quote_bench_main.cpp, quote_bench_common{,_seed,_policy}.cpp, quote_surface_test.cpp, quote_slice_test.cpp, quote_slice_carry_test.cpp (covers quote_slice.hpp, quote_bench_policy.hpp) | 9 TUs / 13 objects, exit 0, 0 diagnostics |
| commit 2bec949f alone (working tree stashed): build, gtest, pytest | exit 0, 100/100, 150/150 |

## 4. Parity, keys off (final2 binary, `knot-ladder-20261017-parity-v2/`)

| run | output sha256 | normalised (vol_status cell only) | reference |
|---|---|---|---|
| dev200 `--policy auto` | `c89d07393c90ad86860a6bcacd3d1a84ede85f4647d9cce136567e34763d5b7c` | - | identical |
| dev200b `--policy auto` | `4b273b084231f8a3811c7d6505d42500355ae29c864a5c2e1c2b964a7aab70fb` | - | identical |
| dev200 graded-v1 | `fc2774134e108d356f1a8cdb1dcdc51aab595d3ef8f4b01ec1241d20a0be4138` | `1348f63dba576874653663be67b69389d8360efd2339bf48215587be0b1fe377` (564 rows) | identical |
| dev200b graded-v1 | `46ad6c2681806daaab024c2b8b02d753ab99ad7c912e79d02f2a1b8b37c91b80` | `4c70d2bce706cd75c189cd08b65829c7c64a962e7f75f9cc1eb0b3e10f41da79` (429 rows) | identical |
| cohort5 graded-v1 | `171fa2d2d03138ac9814d61f9369664e25aee5b4efae4111a9cf328f6285edd5` | `7fa3c415c4d2f13427e498249d660bd420b291ce2a142f5eb0a429af855bb577` (2 rows) | identical |

`normalise_vol_status.py` (in that folder) rewrites only `vol_status`
`unidentifiable` -> `not_evaluated` on `invalid_volatility` rows and proves the
rest byte-identical ("differ outside vol_status: 0; normalised == reference:
True" on all three).

## 5. Benchmarks, keys ON (final2 binary)

Full tables: `C:/atx-cache/quote-benchmark/knot-ladder-20261017-bench-v2/RESULTS.md`.
Policies: `strict` = v3's repair keys without the dominance gate + `price_only` +
ladder with both limits 0.05; `point` = the same with the channel limit relaxed
to 1.0 (explicit).

| dev200 (63,942 rows) | vol-served | price-served | MAE $ | MAE/spread | containment | p99 $ | int. MAE vs graded-v1 | graded-v1 rows lost (displacement) |
|---|---|---|---|---|---|---|---|---|
| graded-v1 | 78.74% | 78.74% | 0.2343 | 0.1085 | 0.9880 | 1.972 | - | - |
| slice-repair v3 | 84.89% | 84.89% | 0.2250 | 0.1137 | 0.9894 | 1.948 | -1.49% | 719 (719) |
| ladder point | **85.97%** | 86.98% | 0.2233 | 0.1081 | 0.9934 | 1.885 | **-2.75%** | 2,764 (1,038) |
| ladder strict | 84.65% | 85.64% | 0.2230 | 0.0963 | 0.9974 | 1.880 | -2.70% | 3,413 (1,038) |

Paired vs v3 (dev200): intersection MAE -2.00% (point), -1.99% (strict).
Price-only rows: point 646 at MAE $0.295 (MAE/spread 0.076, containment 0.994);
strict 634 at $0.297 (containment 1.000).

| dev200b (62,369 rows) | vol-served | price-served | MAE $ | MAE/spread | containment | p99 $ | int. MAE vs graded-v1 | graded-v1 rows lost (displacement) |
|---|---|---|---|---|---|---|---|---|
| graded-v1 | 87.15% | 87.15% | 0.2393 | 0.1164 | 0.9882 | 1.760 | - | - |
| slice-repair v3 | 90.49% | 90.49% | 0.2367 | 0.1112 | 0.9926 | 1.774 | -2.07% | 286 |
| ladder point | 89.30% | 90.25% | 0.2358 | 0.1059 | 0.9965 | 1.847 | -2.12% | 1,862 (1,323) |
| ladder strict | 88.52% | 89.45% | 0.2359 | 0.1008 | 0.9980 | 1.848 | -2.05% | 2,332 (1,348) |

Paired vs v3 (dev200b): -0.10% (point), -0.06% (strict). Price-only: point 592
rows at $0.265; strict 577 at $0.236. New dev200b comparison run
knotspline-v1 (`2f635356`): 82.71% coverage, MAE $0.2382, MAE/spread 0.1071,
containment 0.9971, p99 $1.838, intersection -1.70% vs graded-v1, 4,499
graded-v1 rows lost; graded-v1 dev200b is the parity run above.

Five-symbol 10:30 cohort: **AAPL Nov 20 served** (B, channel refit after a knot
certificate failure, calendar-repaired, 2.2% of its quotes outside) and **SPY
23 d served** (B, knot slice, 3.7% outside) under both policies; **0** monotone,
convexity or calendar violations (point 10,157 rows / 77 chains, strict 9,647 /
76). Coverage 46.6% / 44.3% vs graded-v1 38.4% and v3 62.7%, at MAE $0.051 /
$0.030 vs $0.083 / $0.130 and containment 0.985 / 0.989 vs 0.916 / 0.876 (v3
served 22 slices, 6,479 rows, with more than 5% of quotes outside).

Containment per rung (served slices with > 5% of two-sided quotes outside):
strict 0 on every rung on all three cohorts; point 0 on point rungs and 20 / 12
/ 1 slices (dev200 / dev200b / cohort5) on its explicitly relaxed channel rungs;
graded-v1 37 + 10 (dev200), v3 30 (dev200).

## 6. Criteria

| criterion | result |
|---|---|
| gates exit 0 with counts | met (section 3) |
| c89d0739 reproduced | met (also dev200b default 4b273b08) |
| graded-v1 reproduced modulo the one-column normalisation | met: 1348f63d, 4c70d2bc, 7fa3c415 |
| dev200 vol-served coverage >= 84.89% with intersection MAE within +1% | met by `point` (85.97%, -2.75%); `strict` 84.65% (-0.24 pp) |
| price-only rows reported separately with their own MAE | met |
| dev200b table present (+ graded-v1 / knotspline-v1) | met |
| AAPL Nov 20 and SPY 23 d served, 0 shape/calendar violations | met (both policies) |
| no rung serves > 5% outside unless the policy says so; count per rung | met (strict 0; point only on its relaxed channel rungs, counted) |
| intersection MAE not worse than v3 by more than 0.5% | met (dev200 -2.00%, dev200b -0.10% paired) |
| dev200 rows lost to displacement <= 200 | **NOT met**: 1,038 (point and strict) |

## 7. Concerns and follow-ups

- **Displacement.** 1,038 dev200 rows (24 chains) that graded-v1 served inside
  the market are refused by the ladder (v3: 719). None is refused against a
  lower-ranked slice (single pass, by construction): for 14 chains (730 rows)
  at least one blocker is a slice the ladder newly serves (mostly knot slices,
  A tier), for 5 (209 rows) the blockers are slices graded-v1 also served but
  now at their knot prices, and 3 (51 rows) cross only between quoted strikes.
  Most failures are the repair primitive, not the order: of 87 calendar
  refusals, 62 needed at most 0.5 half-spreads by the bounded repair's own
  measure and still failed certification or the calendar re-check (the channel
  calendar repair reports `not_certified` on 51). Both calendar repairs clamp a
  slice at its own nodes only, so a denser neighbour (a knot slice) still
  crosses between them. Follow-up: clamp on the union of the slice's and the
  accepted neighbours' knots (and give inserted nodes an interpolated
  half-spread), which needs changes in `quote_slice.cpp`'s repair primitives.
- **Third ordering key.** "Own evidence" (graded-v1's calendar key) gave 624
  displaced rows but 84.66% vol coverage and refused AAPL Nov 20 (dev3a); the
  two-sided quote count was kept.
- **Containment losses** (dev200 1,726 / dev200b 539 / cohort5 1,921 graded-v1
  rows) are the rule working: graded-v1 served those prices with 5.06%-60.6% of
  their strictly two-sided quotes outside bid/ask (dev200, re-measured by the
  full review; corrected in fix round 2, this bullet said "6-61%").
- **dev200b vol coverage** (89.30%) is below v3's 90.49%: -1.19 pts net. Of the
  2,311 vol rows v3 serves and the ladder does not vol-serve (3.71 pts gross),
  1,593 rows (2.55 pts, 29 chains) are displacement (calendar 1,349,
  certificate 244), 599 (0.96 pts, 6 chains) are v3's out-of-market slices
  (containment), and 119 (0.19 pts) moved to `ok_price_only`; 1,567 ladder-only
  vol rows (2.51 pts) offset part of it. The gap is mostly displacement, the
  same class as dev200's 1,038 rows that follow-up L3a targets. (Corrected in
  fix round 2: this bullet attributed the gap to v3's out-of-market point
  fallbacks, which explain about a quarter of the gross loss; figures from the
  full review, `L0a-knot-ladder-2e60186c-review-v1/review.md` section 5 (b).)
- **Vol accuracy (bp) is not reported above**: sections 5-6 give USD MAE only.
  On the official paired intersections the vol MAE in bp is worse while the
  median improves: dev200 vs graded-v1 1549.0 -> 1590.7 bp (+2.69%; median
  174.8 -> 159.9 bp; tail-driven, p95 7400 -> 7961 bp), vs v3 +4.71%; dev200b
  vs graded-v1 +4.31%, vs v3 +3.43% (full review, NOTES). The W2/W3 lanes
  should report vol-bp MAE beside the USD headline.
- Untested path (closed in fix round 1, section 9): the knot-fit branch of the
  ladder repair rung (knot slice certified only with the channel repair) has no
  unit test; it serves one
  cohort5 slice. The held-point calendar rung is tested (`8a4caeb4`) and serves
  one slice on each benchmark cohort (the other `calendar` fallbacks are
  stronger-prior retries).
- The bench's per-slice `outside_fraction` is measured on the final prices
  before a refused slice is stripped.

## 8. Deviations

- Commit trailers say `Claude Opus 5.5` (the session's attribution instruction),
  not the brief's `Fable 5.1`.
- Composer default kept `ok` only (design note proposed `ok ok_price_only`), to
  keep every default byte-identical.
- A second containment limit (`ladder_channel_max_outside_fraction`) beyond the
  brief's single 5% rule; its default equals the rule, and only an explicit
  policy relaxes it.
- The ladder refuses the dominance gate and knot-spline profiles instead of
  composing with them.
- Commit `2bec949f` was staged with `git apply --cached` of hunks selected from
  the Edit-authored working tree (a VCS split; no source text was generated),
  then verified alone with the rest stashed.
- The early policy files written by a Python one-liner were deleted and
  recreated with Write; evaluation scripts in the output folder were written
  with Write.
- `-Jobs 2` and `ATX_VOL_FIT_WORKERS=2` (PM message) instead of the shared
  brief's 3.

## 9. Fix round 1 (lane `knot-ladder-fix1`, resume 6)

Fresh implementer, pool `pool-33` (run id `vol-20261017r6-knot-ladder-fix1`),
base `901757ec`. Findings from the partial review
(`C:/atx-cache/quote-benchmark/knot-ladder-901757ec-review-v1/review.md`);
report `.superpowers/sdd/2026-10-17-vol-resume6/lane-knot-ladder-fix1-report.md`.

| SHA | What |
|---|---|
| `b627d0cc` | PriceOnly is final (library), None/strip docs, gtests |
| `81f5a69d` | Native bench tests producing `ok_price_only`, bench/doc text |
| `90d12701` | gtest: a surface calendar repair that removes a volatility flags PriceOnly |
| (this commit) | this section |

What changed and why:

- **PriceOnly is final (MINOR-1).** With `price_only` off a PriceOnly row could
  be re-implied to Success by a calendar repair and published `ok` (0 cases on
  the cohorts, but not behind a key). Now `commit_calendar_repair` refreshes a
  PriceOnly row's certified price and never re-implies it, and the per-node-side
  mask `QuoteSliceFit::price_only_nodes` (set wherever a row becomes PriceOnly,
  carried through the surface's repair trials) makes `evaluate_quote_slice`
  refuse the same contract. A price-only outcome is therefore `ok_price_only`
  (key on) or `invalid_volatility` (key off), never a vol row, in the library
  and in the bench alike. This restores the base (`24e01644`) row-status rule
  (a row that lost its volatility never regains it) while keeping the price.
- **Knot branch of the repair rung tested (MAJOR-1):** a micro-kinked expiry
  whose prior-pulled refits exceed the channel repair's node budget reaches the
  rung; its prior-free knot fit with the repair serves (method `channel`).
- **Bench PriceOnly end to end (MAJOR-2):** deep in-the-money puts with a
  per-row yield zig-zag produce `ok_price_only` rows in the native CLI (static
  and graded), the key-off mapping, the calendar-refusal reset, and a
  prepare -> run -> score test of the scorer's price/vol rule.
- **`ladder_fallback=none` (MINOR-4):** the knot slice served, or it certified
  inside the market without own point evidence and the channel refits decide.
  The point fit can never serve such a slice at pass A (it shares the knot
  fit's admission and is refused `insufficient_quotes`), so `none` never labels
  a pass-A point fit. Documented (header, `graded_semantics`, docs) and tested.
- **String mappings (MINOR-3)** all tested; a refused slice's PriceOnly rows
  become NoFit (documented, tested); `prices_produced` counts PriceOnly rows
  (documented diagnostic, not emitted).
- **Normalisation proof (MINOR-2):** a strict checker
  (`knot-ladder-fix1-20261017-scripts/normalise_vol_status.py`) fails on any
  deviation from the claim; self-test 6/6; the lane's original script accepted
  two of those violations with exit 0.

Gates, parity and cohort runs (binary `knot-ladder-fix1-20261017-bin`, sha256
`2a67075a5c4f95fa591a634ca585b525c271e7bc8f872c9de7b31d20c8699b38`, built from
`81f5a69d`; `90d12701` changes only `quote_surface_test.cpp`; vol-rel,
`ATX_VOL_FIT_WORKERS=3`):

| gate | result |
|---|---|
| gtest quote-slice / implied-forward | 113/113, 27/27 |
| ctest quote-slice, implied-forward, quote-benchmark-{tools,native,campaign} | 5/5 |
| pytest test_quote_bench_cli / test_quote_benchmark | 61/61, 97/97 |
| PCH-off `vol-hygiene`, 9 TUs covering `quote_slice.hpp` | 13 objects, 0 diagnostics |

| run (keys off) | sha256 | vs `901757ec` |
|---|---|---|
| dev200 `auto` | `c89d0739...` | identical |
| dev200b `auto` | `4b273b08...` | identical |
| dev200 graded-v1 | `fc277413...` -> normalised `1348f63d...` (564 rows) | identical |
| dev200b graded-v1 | `46ad6c26...` -> normalised `4c70d2bc...` (429 rows) | identical |
| cohort5 graded-v1 | `171fa2d2...` -> normalised `7fa3c415...` (2 rows) | identical |

| run (keys on, lane policies) | sha256 | vs `knot-ladder-20261017-bench-v2` |
|---|---|---|
| cohort5 point | `232d2c0d...` | identical |
| cohort5 strict | `21f4ee58...` | identical |
| dev200 point | `6ebd63c5...` | identical |
| dev200 strict | `858d6512...` | identical |
| dev200b point | `d53d2a0d...` | identical |
| dev200b strict | `c5ab9121...` | identical |

Cohort5: AAPL 2026-11-20 168/168 `ok` and SPY 2026-11-06 334/334 `ok` under
both policies (graded-v1 0/168, 0/334); 0 monotone / convexity / calendar
violations over 47,188 (point) and 46,318 (strict) pairs. Every number of
sections 5-6 therefore stands unchanged (byte-identical outputs).

Not addressed (stated in the report): the pre-existing `make_prior` sort tie
(a default-path change), the ladder vs dominance-gate/knot-spline combination
and the union-of-knots calendar clamp (follow-up lanes).

## 10. Fix round 2 (lane `L0a-fix2-knot-ladder`, sprint 2026-10-18-vol-sota)

Fresh implementer, pool `pool-4` (run id `vol-20261018s-L0a-fix2-knot-ladder`),
base `2e60186c`. Findings from the full review
(`C:/atx-cache/quote-benchmark/L0a-knot-ladder-2e60186c-review-v1/review.md`:
MAJOR-1, MAJOR-2, MINOR-1); report
`.superpowers/sdd/2026-10-18-vol-sota/lane-L0a-fix2-knot-ladder-report.md`.
Tests and this note only; no library change.

| SHA | What |
|---|---|
| `a8810c1e` | Six `QuoteSurfaceLadder` gtests (below) |
| (this commit) | Section 7 corrections (dev200b attribution, 5.06%-60.6%, vol-bp note), this section |

Each check now has a test that fails with it removed (temporary edit of
`quote_surface.cpp`, build, full `atx-vol-quote-slice-tests` run, inverse edit,
restored blob `5913374a` == HEAD every time):

| removed | test that fails | run |
|---|---|---|
| repair rung, knot: `served_in_market(*knot, i)` | `RepairRungSkipsAKnotFitItsRepairCertifiesOutsideTheMarket` | 118/119 |
| repair rung, point: `served_in_market(*point, i)` | `RepairRungSkipsAPointFitItsRepairCertifiesOutsideTheMarket` | 118/119 |
| clean retry: `&& in_market(*fit, i)` | `ACleanRetryOutsideTheMarketIsRefused` | 118/119 |
| bounded calendar repair: `ladder() && !in_market` | `ABoundedCalendarRepairThatLeavesTheMarketIsRefused` | 118/119 |
| channel calendar repair: `LeftChannel` block | `AChannelCalendarRepairThatEndsOutsideTheMarketIsRefused` | 118/119 |
| calendar key 3 `channel_repaired` (review revert RH) | `AnUnrepairedSliceKeepsTheCalendarSlotOverAChannelRepairedOne` | 118/119 |
| all five containment checks (review revert REI) | the five containment tests | 114/119 |

Fixtures (synthetic chains, each with a relaxed-limit positive control that
serves the same rung): deep in-the-money calls quoted off-model and narrower
than the channel half-width floor put a certified repair-rung fit outside the
market; a bounded repair allowed 10 half-spreads lifts a cheap later expiry
above its asks; on the bounded-repair pair, the own fit's channel calendar
repair exceeds a node budget of 3 while the stronger-prior retry's fits it
and ends a third outside; a wide half-width floor and a strong prior make a
clean retry that misses every one-cent quote. Finding: the channel calendar
repair of a slice's own served prices can never leave the market (its box
keeps an inside node inside), so the `LeftChannel` check binds only on the
retry's prices.

Gates at `a8810c1e` (vol-rel, `-Jobs 4`): gtest quote-slice 119/119; ctest
quote-slice, implied-forward, quote-benchmark-{tools,native,campaign} 5/5;
dev200 `--policy auto` with the lane binary `c89d0739...` (identical;
`ATX_VOL_FIT_WORKERS=3`). Evidence:
`C:/atx-cache/quote-benchmark/L0a-fix2-knot-ladder-20261018-evidence/`
(`sha256.txt`).
