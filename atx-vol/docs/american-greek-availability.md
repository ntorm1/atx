# American Greek availability

`AmericanGreeks` carries eight sensitivities and a price. Each field has an
explicit requested bit, supported bit and reason. Continuous-carry phi remains
in the separate `CarryGreeks` API. The field order is the existing
`GreekFieldMask` order: delta, gamma, vega, theta, rho, vanna, volga, charm, price.
Moving that enum into `american.hpp` preserves its public bit values.

A default bundle supplies nothing. Its legacy numeric members still default to
zero, but both masks are `None` and every reason is `NotRequested`. A caller must
not infer a supplied value from a finite zero. A partial result may supply the
price and delta while refusing theta; refused requested fields are NaN and carry
their reason. Unrequested fields retain their route's numeric convention without
acquiring support.

`ComputedEstimate` and `Exercised` identify supplied finite values. Neither is a
derivative accuracy certificate. The producing model, its exercise rule, and any
independent accuracy evidence remain separate. The other reasons are
`NotRequested`, `UnsupportedAxis`, `StencilCrossing`, and `Nonfinite`.

The private helper `pricing/american_greek_availability.hpp` centralizes four
operations:

- `american_greek_request` converts the existing three solve-need flags to an
  exact field mask, including the base price/delta/gamma/theta group.
- `mark_american_greek_fields` records a producer's supplied or refused fields.
  Nonfinite supplied values become `Nonfinite`; refusal poisons only those fields.
- `narrow_american_greek_request` removes metadata for unrequested fields without
  changing their legacy numeric slots. Expanding a request cannot resurrect an
  old value: newly requested fields become unsupported.
- `american_greeks_support` checks the exact required fields against request,
  support, reasons and finite values. It rejects unknown mask bits and contradictory
  support metadata. A delta-only request does not require an unrequested theta.

This first component establishes the shared schema and helpers. Existing numerical
producers and downstream gates are migrated in follow-up components; their default
empty metadata must not be interpreted as completed propagation. Appending metadata
changes the C++ object layout, so all consumers must be rebuilt. No pricing or Greek
arithmetic changes in this component, and no Greek error budget or latency claim is
introduced.

The new interface tests first failed to compile because the schema/helper was
absent. With the component implemented, six availability tests and nine existing
batch/mask tests pass. PCH-off owning batch and umbrella objects compile. The
umbrella's public-type and inclusion tests pass; its full run remains 7/10 because
of the previously recorded README header-count mismatch and fixture-path checks
in unchanged inputs. Receipts are `repair-iv/greek-availability-*` under the
`spy-qualified-r7-20261023` benchmark evidence directory. No numerical producer
admission or downstream mask enforcement has yet been changed by this component.

The cached correction graph and native adjoint producers now stamp their computed
fields as `ComputedEstimate`, including explicitly computed zeros in the closed-form
European degenerate limits. Nonfinite results are unavailable. The cached graph's
sensitivities still differentiate its fixed-carry correction model; this change does
not claim they differentiate the separate served-price floor or match cold American
sensitivities. In particular, `american_greeks` with a null correction uses the
documented European graph, while `american_price_cached` with a null correction
falls back to the cold American price. Those are distinct existing contracts.

The session record adapter requests all fields and supplies price, delta, gamma,
vega and theta. It explicitly reports rho, vanna, volga and charm as
`UnsupportedAxis`; their existing NaN values remain. The record's supplied fields
are model estimates, not new derivative accuracy certificates. Producer stamping
changes no finite numeric value or pricing decision. Consumer gates and the separate
cold-stencil admission changes remain follow-up work.

Producer regressions first failed at runtime on the missing metadata. The final
focused run passes 29 schema/cached/adjoint tests and all seven session-record
tests, including 104 certified record contracts. Six owning source/test objects
also pass the PCH-off build. Evidence is `repair-iv/greek-producer-*` and
`repair-iv/greek-record-*`. A separate shared-consumer regression remains RED
until the next component: this producer change does not yet enforce metadata in
the old finite-only caller gates.
The first producer component now stamps cold FD, scalar/prepared AL, and both
direct AVX2 kernels as `ComputedEstimate` for their actual computed groups. The
AL-to-FD fallback narrows metadata to the caller's solve needs without changing
the legacy numeric slots. All pricing, stencil and PDE arithmetic is unchanged;
the known near-edge and deep-exercise numerical counterexamples remain open for
the subsequent admission component. This is not completed consumer propagation.

A new runtime test failed on the previously empty metadata, then passed across
both sides, full/base masks, prepared geometry, the European fallback and direct
AVX2 kernels. Together with six schema tests and three existing batch tests,
10 tests pass. PCH-off checks pass for both producer sources and the owning test.
Evidence: `acceptance/cold-greek-admission-red.xml`,
`acceptance/cold-greek-metadata-green.xml`, and
`acceptance/cold-greek-metadata-hygiene.log`. The separate numerical-admission
regressions in the RED receipt are intentionally not claimed fixed here.

The shared surface/portfolio caller gate now requires supplied-field metadata as
well as finite numbers. Its existing solve-need groups are unchanged. Normalizing
an unrequested NaN to its legacy numeric zero also removes availability metadata;
request expansion never turns that zero into a supplied field.

The public American Greek batch uses the exact output mask instead of requiring
every field in an internal solve group. A price-only request can therefore succeed
when gamma is unavailable; a full request retains the price but reports
`Unsupported`. Requested and supported masks and nine reasons are available through
the workspace's three new `lane_*_view` accessors, with the same borrow/invalidation
rules as its status and route views. Unknown mask bits fail before any output or
workspace write. A supplied finite mark remains a prerequisite for a successful
lane, including requests that do not write a price column.

`reserve_lanes` preallocates the Greek metadata for repeated Greek batches. The
price-only batch uses `reserve_price_lanes`, which allocates no Greek metadata and
invalidates any old Greek views while retaining their capacity. No pricing options,
stencils, or numerical acceptance conditions change in this caller component.
Portfolio partial-result aggregation and Python propagation remain separate work.

The shared-gate regressions failed at runtime on untouched/default zeros and
normalization that retained stale availability. Missing batch metadata first
failed to compile; a separate runtime regression caught allocating Greek metadata
on price-only batches. With the wrapper metadata prerequisite applied, 29 focused
pricing/batch/gate tests pass. Another 50 surface/portfolio/seed checks pass with
one counter-dependent skip. All six owning PCH-off objects pass. Receipts are
`repair-iv/greek-{gate-red,batch-interface-red,workspace-red,consumer-*}`. The
finite-price/nonfinite-gamma test uses the existing tiny-spot fixture on unchanged
numerical arithmetic; the subsequent exercise-admission component must replace
that fixture with a genuine stencil refusal when it supplies exact exercised
derivatives. The partial-result contract itself is unchanged by that migration.

Portfolio price frames now retain a valid mark and PV under the append-only
`GreeksUnavailable` status. Their requested/supported masks and nine reasons
describe each scaled risk field independently. `price_into` accepts optional
metadata spans, validates their sizes and byte-range separation before any
write, and reuses caller storage. Returning frames allocate three metadata
columns; their allocation and byte counters include those columns.

`PriceTotals` and risk buckets sum all priced rows. `n_priced` reports price
coverage, `n_ok` complete requested bundles, and `n_greeks_unavailable` priced
rows with partial risk. Wholly unpriced rows remain outside the sum, so callers
must compare `n_priced` with the full position count before interpreting PV as
complete book coverage. A missing requested derivative on any nonzero-weight
priced row makes that aggregate NaN/unsupported instead of shrinking its
denominator. Other available fields remain usable. An exactly zero weight
contributes exact zero to aggregate risk while its row retains the refusal.
The aggregate Price bit describes the priced-row PV sum, not an unqualified
claim that every position was valued.

Adjusted Delta computes its internal Vega dependency even when the caller omits
the Vega output group, then narrows outward metadata. A missing slope or Vega
refuses Delta rather than substituting the unadjusted value. Cached raw Greek
bundles remain unadjusted for Taylor P&L; a partial cached bundle is rechecked
against the actual Taylor request, so an unused unavailable rho cannot veto a
no-rate-move explanation. A missing requested Taylor coefficient still refuses
the explanation. Derivative-book combination, backtest coverage reporting and
Python exposure are separate downstream components, not completed by this change.

The lost-price/partial-total regression first failed at runtime; a second RED
case caught overly broad rejection of a cached partial bundle. The combined
exercise-admission build passes all 98 portfolio/seed/target-seed tests and 33
focused batch/shared-gate tests. Five owning PCH-off objects pass. Existing
overflow fixtures were replaced by the frozen real crossing and its call dual;
the old tiny-spot put now has a positive exact-exercise Gamma regression. One
synthetic reducer fixture now supplies explicit metadata for its manufactured
finite mark. Receipts are `repair-iv/greek-portfolio-{red,first,scoped,final}.*`,
`greek-batch-migration.*`, and `greek-portfolio-hygiene.log`. No fit or Greek
latency claim, derivative budget, or native-adjoint/cached accuracy qualification
is inferred from these checks.

The aggregate-overflow follow-up normalizes nonfinite sums to NaN and refuses
their availability. Signed and gross Vega share one bit: if either overflows,
both become unavailable, even when offsetting signed contributions cancel.
Independent PV and risk remain usable. A runtime regression reproduced the
bucket-grand false support; the final tests also cover actual large finite
positions and signed overflow. All 100 portfolio/seed tests and three owning
PCH-off objects pass. Receipts are `repair-iv/greek-portfolio-overflow-*`.

Derivative books now propagate availability for the nine axes shared with
`PriceTotals`. Standalone `DerivGreeks` pricing, its extra carry/smile diagnostics
and its numerical methods are unchanged. A finite PV with an unavailable
requested shared derivative reports `GreeksUnavailable`; its price and other
supplied derivatives remain in the totals. `second_order=false` leaves Vanna
and Charm unrequested, while Gamma and Volga remain requested. Missing time
operands use `UnsupportedAxis`, not an American exercise-crossing label.
The legacy exclusion counters still count missing row columns, including
disabled axes. Exactly zero quantities contribute zero aggregate risk while
their row retains any refusal.

`combine_totals` unions requests and requires both inputs to supply each field.
An empty marks-only block still refuses the combined Greeks, preserving the
previous NaN convention; default finite zeros carry no availability evidence.
All three coverage counts saturate independently. Their `n_ok` values describe
the original input requests; the combined supported mask describes the union.
Signed/gross Vega overflow shares a refusal, and independent PV survives.
The derivative scenario caller admits partial priced rows to its existing
shock-specific check: an unused unavailable theta cannot exclude a no-time-roll
scenario, while a requested missing theta remains a named missing sensitivity.

Four derivative-book regressions and the scenario missing-time classification
first failed at runtime. All 129 portfolio/derivative/memo tests and 18 derivative
scenario tests then pass. The memo test retains every row's bit-exact comparison
and exact fixed-order PV sum. Its old independent `sum += qty * raw_pv` oracle
was a contracted multiply-add recurrence, one ULP away from the sum of rounded
published rows. An older pool3 binary (SHA256
`0587890fdc472edebf29e1898206817d74a512dab87b3fa061bf149d79aa7c44`, exact source
revision unknown) independently reproduced the same failure. A test-only
volatile product now enforces separate rounding; no tolerance was relaxed and
the production PV sum remains unchanged. The diagnostic source, hashes and
exact integer bit patterns are frozen in `repair-iv/greek-deriv-book-fma-*`.
All six owning PCH-off objects pass; final receipts are
`repair-iv/greek-deriv-book-final2.*`, `greek-deriv-scenario-final.*` and
`greek-deriv-book-hygiene*.log`.

Backtest Error policy now checks aggregate availability as well as the existing
complete-row risk count. A finite row population can still overflow an aggregate;
that is an Unavailable error, without fabricating excluded rows. Partial priced
rows retain their PV under report mode and produce a requested-risk diagnostic
under Error mode, rather than claiming their surface is absent. This component
does not add persistence columns or change the complete-risk coverage policy.

Two runtime regressions first reproduced the aggregate false pass and misleading
partial-risk diagnostic. All 81 backtest/shape/mark tests and eight declarative
strategy/tolerance tests pass; three owning PCH-off objects pass. Receipts are
`repair-iv/greek-backtest-risk-{red,green,policy,hygiene}.*`.
