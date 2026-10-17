# Smooth-dividend lattice lane, resume 5 (2026-10-17)

Branch `feat/vol-smooth-dividend-20261016` (WIP `e888791a` -> this tip), merged
with root `feat/vol-resume4-20261016` at `7ca31c95` (merge `4224f97f`). Pool-34,
run `vol-20261017-smooth-div`. Continues
`2026-10-16-smooth-dividend-wip-handoff.md` (diagnosis and scheme design are
there; not repeated). Development cohorts only, no holdout, no default changed.

## What and why

Dated-dividend chains routed to the DatedCash lattice were refused by
`AmericanPriceSlice` certification for two lattice reasons (CRR staircase noise;
a European that is not Black-76 at the dated forward) and one data reason
(vendor per-strike carry kinks). The lane adds opt-in lattice schemes
(`DiscreteDivScheme`, `api/pricing/american.hpp`; bench key `dividend_lattice`,
`dividend_lattice_steps`) with `spliced` the default and bit-identical. This
resume compiled, calibrated and gated that WIP, merged the root, ran the official
benchmarks and closed the dividend-review minors.

## Commits

| SHA | what |
|---|---|
| `9a0fcfe4` | calibrated smoothing tests (27/27) |
| `4224f97f` | merge root `7ca31c95` (2 additive conflicts) |
| `a96e1ae4` | inert lattice keys refused; routes report the scheme only where the lattice ran; QuoteSurface calendar-repair trial copies the scheme |
| `69f64d39` | M1: an overfull dividend increment is kept whole and flagged |
| `adca4a0c` | M2: single events classified against the final schedule |
| `0b9ab63e` | M3: a malformed `ddiv` cell never aborts a run |
| `d131b3f8` | N2: route-window mismatch checked before `applies()`; guard documented |
| `51ef4fc8` | N3 slice stem moved to OWNED_ELSEWHERE; N4 dead `<limits>` include |
| `93b0a97d` | 100-column fix in `slices.cmake` (binary unchanged) |

## Test calibration (measured, each threshold's measurement is in its comment)

Six WIP expectations were wrong, not just loose:

- An ex-date ON lattice step 1 (`tau = T/n`) never converges on a spliced lattice:
  two nodes survive and the splice extrapolates linearly over the whole cash
  (European error -0.252 CRR / -0.239 BBS at the money, S = K = 100, D = 6,
  sigma 0.3, at 300 and 2400 steps alike, about Gamma*D^2/2). The test now fixes
  the date at 0.01 y: max |error| 7.8e-3 at 300 steps, 1.3e-3 at 2400.
- The BBS European error is one-signed (CRR's flips sign at the money) but does
  not halve per doubling (300 -> 600 grows): pinned as one sign over
  300..4800 steps, factor 3.0-3.1 from 1200 to 4800, 5.7e-4 at 4800.
- American errors against RGW and a 4801-step reference oscillate with the step
  count: end points pinned (RGW 3.3e-3 at 301, 2.7e-4 at 3311; spliced family
  2.2e-2..3.2e-2 at 301, escrowed 3.3e-4..2.2e-3).
- BBSR parity needs the Richardson combination of its two lattices' rounded-step
  dividend PVs (constant 4.9e-5 residual otherwise); measured 3.6e-11 after.
- On the synthetic ASML chain the spliced CRR certifies a smooth smile: the
  staircase alone breaks nothing. Measured three regimes instead (also with the
  cash removed): smooth smile, every single-rollback scheme certifies; runs that
  keep 2% of their butterfly, CRR refused (node 48), BBS / escrowed-BBS / LR
  certify; exactly tight runs (zero butterfly), only escrowed-LR certifies, so the
  remaining failure is the smoothed CRR lattices' own O(1/N) European error
  (8e-4..1.8e-3 at 301 steps) against LR's O(1/N^2) (4.4e-6), not only the
  spliced model's departure from Black-76. BBSR is refused on 10-wide strikes
  even on the smooth smile (its Richardson step amplifies the ripple).

## Gates (final code tip `93b0a97d`; vol-rel, `-Jobs 3`)

| gate | result |
|---|---|
| `atx-vol-discrete-div-smoothing-tests` | 27/27 |
| `atx-vol-discrete-div-tests` (default lattice pins) | 88/88 |
| `atx-vol-dividend-schedule-tests` | 21/21 (was 18; +3 minors tests) |
| `atx-vol-quote-slice-tests` | 84/84 |
| anchored ctest (the 6 lane/bench targets + the 4 discrete-div suites) | 94/94 |
| native pytest (`test_quote_bench_cli.py`, `test_common_markov_bench_cli.py`) | 146/146 (+2 lane tests) |
| PCH-off `vol-hygiene` check, 13 TUs including every touched header | 13/13, 0 diagnostics |
| `git diff --check`, 100 columns on added code | clean |

Receipts: `C:/atx-cache/quote-benchmark/smooth-div-20261017-gates/`.

## Parity (final binary `076263f4...`, byte for byte)

dev200 default `c89d0739...`, dev200b default `4b273b08...`, dev200 opt-in
spliced `abc75955...`, dev200b opt-in spliced `747d3e6c...`, graded-v1
`1348f63d...`, R104 common `6b129f64...` (336 s, unqualified;
`smooth-div-20261017-parity/r104-93b0a97d/`).
The opt-in spliced hashes also held after M1 and after M2 separately
(`smooth-div-20261017-minors/`).

## Official results (`smooth-div-20261017-official/RESULTS.md`)

escrowed-lr vs the spliced default, both on the inferred schedule:

| | dev200 | dev200b |
|---|---|---|
| lattice chains certified / refused | 117/63 -> **149/31** (0 lost) | 170/20 -> **183/7** (1 lost: HSBC 2027-09-17) |
| payers MAE, exact intersection | 0.1969 -> **0.1470** (n 5,406) | 0.2093 -> **0.1862** (n 6,230) |
| payers MAE/spread, intersection | 0.0636 -> 0.0579 | 0.0720 -> 0.0677 |
| payers p99, intersection | 1.942 -> 0.923 | 2.003 -> 1.898 |
| payers containment, intersection | 98.91% -> 99.06% | 99.50% -> 99.49% |
| payers coverage (scorer) | 19.73% -> 22.60% | 29.28% -> 28.53% |
| newly served / lost rows | +786 / 0 | +114 / -364 (calendar check) |
| non-payer underliers | identical (0 of 26,594 rows) | identical (0 of 26,898 rows) |
| lattice step (UNQUALIFIED) | 1.15 s -> 3.75 s | 1.58 s -> 4.31 s |

ASML: 11 of 13 refused chains certify; intersection MAE 0.448 -> 0.271, p99
2.27 -> 1.00; 418 rows newly served. TSM: 3 of 12 certify; of the other 9, 8
fail the aggregate route too (the carry-kink class). On dev200b the calendar check rejects more newly certified
rows than the certifications add (OIH, FDX). The candidate CSVs equal the
exploration's bytes (`2cfda793...`, `5a9c1f5f...`).

## Dividend-review minors

- **M1 closed** (`69f64d39`). Measured first: 0 of the 59 dev200 and 0 of the 48
  dev200b split increments hold more units than their split count, so the rule
  moves nothing on real data. The review's optional integrality gate
  (|inc/unit - n| <= eps) was NOT adopted: dev200b's split increments reach
  |inc/unit - round| = 0.5.
- **M2 closed** (`adca4a0c`): the new test fails on the old rule (565.99999999999693,
  NonIntegerDay at 30 and 80 bp) and passes; 0 events move and 0 change kind on
  either cohort.
- **M3 closed** (`0b9ab63e`): malformed `ddiv` -> NaN, the snapshot's schedule is
  refused `non_finite_input`, rows counted; default bytes unchanged (pytest).
- **N2 closed** (`d131b3f8`). **N3, N4 closed** (`51ef4fc8`; marketdata slice 17 ->
  16 stems). The `-L atx_vol_fast` double run via the monolith remains, as for
  every OWNED_ELSEWHERE stem.
- **N5 deferred to the PM's ledger pass**: lanes report ledger lines rather than
  append them (avoids end-of-file conflicts between four lanes). Text in the lane
  report, ledger candidate 1.

## Open

- Certification tolerance untouched. dev200's 31 still-refused chains are META 11,
  TSM 9, V 6, ASML 2, IEF 2, QLD 1, and 29 of them fail the aggregate fallback
  too: the both-routes class the WIP diagnosis traced to vendor per-strike carry
  kinks on TSM 20896 (not re-diagnosed for META/V). dev200b's 7: IGV 3, OIH 2,
  HSBC 1, KHC 1.
- Newly certified slices can lose to the snapshot calendar check (dev200b
  -250 rows net). A calendar repair for lattice slices is out of this lane's scope.
- The escrowed vol column is the escrowed stock's vol, not spot vol; vol MAE vs
  srVol is not like for like (dev200b payers 205 -> 217 bp).
- Runtime is unqualified; nothing pinned.
