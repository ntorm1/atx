# Economic family refusal study — 2026-10-24

The remaining 496 bounded economic proposals fail because their error intervals are
too wide. Changing the tangent placement or retrying the existing scalar bounded price
does not address these cases. No numerical change from this study is promoted.

## Frozen scope

The production source was `126447ef614ce5c1f7783eaa5973566eafa91a8d`.
Research commit `75f3b8863772a1a7b3cbd772f7fe0469fd7a984e` observes the unchanged
requests, candidate placements, already computed bounds, and acceptance predicates.
Follow-up `36b17179d40c08563ce15bfb070125f9e63732bd` additionally evaluates the
existing scalar bound for exactly those 496 refused candidates. Both are retained
on `feat/vol-r7-economic-refusal-study-20261024`, outside the production branch.
The observer is serial-only research instrumentation, enabled with
`ATX_VOL_ECONOMIC_DIAGNOSTIC_FILE`; it is not a supported runtime diagnostic API.

Both experiments use the original SPY input, funding-PV carry model, one fitting
worker, and `spy-joint-feasible-economic-family-always-candidate.json` at the frozen
source. Original output, observed output, scalar-shadow output, and the observer's
default-off output have the identical prediction SHA-256:
`04f13351165dfb48943aa19022976100c311ebcfbaae6271e24abed0534db9a3`.
No baseline IDs or fitted targets enter a fitting decision. IDs are joined offline
to the original input and independently audited actual API models and prices.

## Measurements

| Stage | Requests | Finding |
| --- | ---: | --- |
| Native price-window check across final nodes | 12,789 | Only one has edge headroom below half its price budget |
| Economic preparation received | 12,250 | Every captured request maps exactly to its audited model and target |
| Ineligible before bounded pricing | 145 | Every lane reports `SigmaAboveBox` |
| Bounded candidates | 12,105 | 11,609 accepted; 496 refused |
| Refused: upper bound above target and lower bound exceeds allowed error | 435 | Whole interval cannot fit inside the monetary budget |
| Same failures, plus lower bound at/below native edge | 61 | Native edge proof also fails |
| Scalar bounded-price recoveries | 0 / 496 | Scalar price and bound are identical to the fused result |

No priced refusal is caused by invalid status, nonfinite price, malformed bound,
request identity, option identity, or a stale envelope. All 496 are calls from
11 expiries, with strikes from $50 through $470. The 145 ineligible requests are
distinct from these 496. Other final nodes never submitted to this preparation
function are outside this diagnostic's attribution scope.

For all 496 refused candidates, the envelope halfwidth exceeds half the allowed
price error. The ratio is 1.02458 at minimum, 141.739 at the median, and 8,378.956
at maximum. In contrast, the actual family-price decrease divided by the intended
half-budget decrease is 0.999893 at the median (range 0.957616–0.999974).
The original tangent is already placing candidates accurately. At an unchanged
interval width, no centering can make an interval wider than the whole budget
satisfy both `upper < target` and `target - lower <= budget`.

The one small-headroom request is row 27410261, a call at strike $170. Its
headroom is $0.0000720105 against a $0.001 budget. It also fails the interval-width
test. A special placement adjustment therefore cannot recover the wider refusal
population, and none is implemented.

## Scalar bound follow-up

`family_prices_bounded` fuses price and sensitivity-bound evaluation. On finite,
verified boundary nodes it omits the scalar bound's `min(trivial, sensitivity)`;
the resulting interval is conservatively at least as wide. This made the existing
scalar calculation a concrete follow-up worth checking.

The shadow replay calls `family_price_bounded` only for the 496 refused proposals,
at exactly the original sigma. Every scalar center and bound is identical to the
fused result; the trivial cap does not bind for this population. The proof masks
remain 435 failures of target/budget and 61 failures of target/budget/edge. The
496 additional calls consumed 4.1596 ms in one instrumented development run.
This is an attribution measurement, not a qualified latency result or a reason
to add the retry to production.

The existing cold Screen fallback remains necessary for these requests. A future
region-specific envelope would require its own mathematical error argument,
identity/domain checks, cold-reference regressions, and measured whole-fit benefit.
This study provides no evidence for loosening the envelope, changing the native
edge, enlarging the monetary budget, or adding a Newton correction.

## Evidence and limits

Evidence root: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`.

- `economic-headroom-window-v1.json`: read-only full-node edge/headroom analysis.
- `economic-reasons-v1/analysis.json` and `proposals.csv`: all eligibility and proof
  masks, 641 rejected request identities, raw quotes, final API status/model, family
  grouping, and interval/placement distributions.
- `economic-scalar-bounds-v1/analysis.json` and `proposals.csv`: scalar comparison
  for all 496 candidates, with exact row IDs, proof masks and per-call clocks.
- `economic-refusal-provenance-v1.json`: source, binary, input, policy, scripts and
  result hashes. The final instrumented binary is preserved with the scalar receipt.
- `analyze_economic_headroom.py`, `analyze_economic_reasons.py`, and
  `analyze_economic_scalar_bounds.py`: reproducible offline analysis. Python keeps
  the 64-bit chain and row identifiers exact.

The scoped quote benchmark builds and serial diagnostic runs passed; full output
parity was checked. These are research observers, not production changes, and no
new owning-header qualification or independent timing campaign is claimed.
