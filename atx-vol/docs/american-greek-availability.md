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

Python now carries the same availability without inferring supply from finite
zeros: scalar AmericanGreeks has readonly masks/reasons; American batches,
fitted-chain valuations and priced-surface grids export owning uint32 masks
and uint8 reasons with shape (N,9), including empty (0,9) results. Grid field
validity requires native support, and its new greek_price is the price belonging
to that metadata, distinct from the independently priced fair_value. PriceStatus,
GreekFieldMask and AmericanGreekReason enumerate every native value. This does
not add a Python portfolio API or complete native ServedSession propagation.

The binding tests exposed an older collision: both STATUS_OK and the dedicated
American Unsupported sentinel were -1. Unsupported now exports -2, while success
stays -1 and nonnegative native error codes are unchanged. Price and Greek mixed
batches explicitly pin this distinction and retain every refused row. Partial
Greek failure still preserves supplied price. This is a compatibility correction
for callers that hard-coded the old erroneous unsupported value.

Fourteen metadata regressions first failed at runtime, followed by four explicit
Greek status-collision failures and a separate price-collision failure. All 99
focused Python tests pass, with one expected source-build wheel-metadata skip;
all four owning PCH-off binding objects pass. The staged module is SHA256
b2aa4140009bbde36b217a25950f7fbebbd3170caf5c10fb796c2f026dd31af9 and matches
the actual build output. Receipts are repair-iv/greek-python-{red,green,status-red,
final,hygiene}.*. The hygiene cache initially selected a venv without pybind11;
explicitly selecting the existing Python312 interpreter resolved configuration.
No numerical arithmetic, Greek accuracy budget, or timing claim changed.

Owned and mapped surface fused evaluations now stamp the exact selective
Price/Delta/Vega request. A later dedicated-axis refusal preserves previously
supplied price, Delta and resolved IV. Unrequested zeros have no availability;
invalid-query and price-kernel failures retain their previous poison contract.
A bare Result<double> supplies no numerical reason metadata, so its missing
slot is Nonfinite and the original error remains in status; this does not invent
a StencilCrossing label. Successful dedicated estimates carry ComputedEstimate.
Full bundles preserve their actual producer reasons and reject incomplete
full-Greek seeds. Scalar and laned ordinary refusals now use the short typed
Unavailable error rather than Internal with a heap-allocated message.

The non-AL FD fallback for dedicated Vega now checks the producer's Vega supply.
A frozen BAW put (S=100, K=116.5837, T=.25, sigma=.2, r=.04, q=.01) previously
returned successful NaN from surface.vega although its FD bundle explicitly
refused Vega as StencilCrossing. The pre-change Python module b2aa4140 reproduced
that failure; the native regression pins both owned and mapped surfaces. The
public two-endpoint Vega and center-inclusive full-bundle contracts remain
distinct. No pricing formula, numerical admission or tolerance changed.

Two native runtime REDs reproduced metadata loss and discarded partial prices;
the separate allocation RED counted three allocations for ordinary FD/AL/laned
refusals. Final validation passes 162 surface/portfolio/seed/backtest-guard tests
with two counter-disabled skips, the zero-allocation regression, and all five
owning PCH-off objects. All refused contracts stay in the tests. Evidence is
repair-iv/greek-surface-{red,final,alloc-red,alloc-final,hygiene}.* and
greek-surface-baw-red.{py,log}. Chain-batch and ServedSession result propagation,
plus additive backtest price coverage/carry bookkeeping, remain separate work.

Chain-batch pricing now retains a supplied price and each supplied derivative
when another requested derivative is refused. The appended
`ChainRowStatus::GreeksUnavailable` value is 7; all previous ordinals and IV
statuses are unchanged. `Unpriceable` still means no supplied price, and invalid
or unpriceable rows still poison every requested numeric column. The optional
caller-owned `requested`, `supported` and nine-reason spans are independently
empty or row-sized, checked for byte aliases before any output write, and
overwritten on every row. Only Price and the nonempty Delta/Gamma/Vega/Theta/Rho
columns are requested; Vanna, Volga and Charm remain NotRequested even if a
kernel solve group computed them. Availability remains separate from accuracy.

The new runtime regression first reproduced lost partial prices on both sides
and both scalar/AVX2 routes. The nullopt crossing fixture independently supplies
Price and Rho (mask 272), not Delta; this was verified against the frozen
pre-chain Python module b2aa4140 before correcting the test's initial Delta
assumption. Scalar retained prices and Rho are bit-identical to direct kernel
calls. Mixed complete/partial/invalid rows, exact Price+Rho and Price-only
requests, kernel failures after buffer reuse, independently optional metadata,
alias rejection, thread-count determinism and allocation checks pass. The
focused chain/IV/workspace/allocation suite passes all 37 tests, including zero
allocations on reserved partial-result buffers and the existing process-wide
worker checks. Receipts are `repair-iv/greek-chain-{red,probe,green,final}.*`.
All four owning PCH-off objects pass, including the benchmark; its target was
enabled in the hygiene cache only to compile that owning object. The final test
executable is SHA256
9bcd7475634071870ee565264ab703d2fef5f9c7b70148e8539758b815a17876.

The chain benchmark's eight-status histogram includes partial rows. Its
per-column comparison counts retain every finite supplied price/derivative,
including partial rows, and report NaN error when no values were compared.
These are correctness/denominator changes; no chain throughput or Greek
accuracy claim is made. Native ServedSession propagation and additive backtest
price coverage/carry bookkeeping remain separate work.

ServedSession now carries exact five-axis common availability for Price, Delta,
Gamma, Vega and Theta on both scalar answers and chain statements. Cold FD
metadata is narrowed to those axes, while the independently computed declared
price remains supplied even if derivatives fail. Nonfinite prices and malformed
contracts cannot set availability. Book PriceOnly answers explicitly support
only Price; other book derivative bits require both a finite value and the
record's Served/Exercised reason. Route/model/price statuses remain separate
from field availability, and no Greek accuracy certification is added.

Cold and record provenance are not conflated: cold_reasons uses the nine-axis
AmericanGreekReason order; the book answer keeps its six-axis ServeGreekReason
payload, now also copied verbatim into ServedChainStatement.greek_reasons.
NearExerciseEdge is not called StencilCrossing, and the record's Phi axis does
not become Vanna. The inactive payload remains NotRequested. Every chain row
overwrites its metadata, including refusals after a formerly supplied row.

Python exposes readonly scalar masks/cold reasons and owning chain uint32
requested/supported arrays, uint8 cold_reasons (N,9), and book_greek_reasons
(N,6). The book's own chain API exports greek_reasons (N,6). Empty shapes,
refused-row denominators, per-contract equality and buffer lifetime are tested.
Four native runtime REDs and three Python REDs reproduced the missing metadata.
Final scoped checks pass 6 native session tests, 4 book-chain/allocation tests,
33 Python tests and 7 owning PCH-off objects. The actual staged module is
SHA256 00d2012317bfbda0e3664ccc4754f6a37883cbd379ed40af3cb0664fbf603bfc;
receipts are repair-iv/greek-session-{red,green,final,book-final,python-red,
python-final,hygiene}.*. General cached/record chain-tick metadata and additive
backtest price coverage/carry bookkeeping are still separate work.

Raw chain-tick outputs now accept independently optional requested/supported
mask spans for their five fixed Price/Delta/Gamma/Vega/Theta axes. Cached
results use the actual producer's support, reasons and finite values. Raw
ExpiryRecord and spot-ladder fields carry finite-estimate availability only:
their derivative accuracy/admission remains separate research, and a fallback
outside a record box gains no certificate. Ladder metadata is written after
every window exit, rebuild and fallback finishes. Existing prices, derivatives
and TickStatus values are unchanged; absent metadata skips the ladder's extra
per-row pass.

ServeBook derives the same common masks from its typed status and six-axis
record reasons; PriceOnly supports only Price. ServedSession forwards the
book masks and copies its cold masks without converting reason vocabularies.
Malformed or refused rows overwrite support with None. New metadata spans are
shape checked and byte-alias checked against input/output columns and the
book/session statements and scratch before any writes. Existing calls may
omit both spans. Numeric output alias contracts are otherwise unchanged.

Four raw-route runtime REDs and three consumer REDs reproduce the previous
missing masks and alias guards. Final checks pass 35 raw tick/record/ladder,
6 session and 4 live-book/allocation tests, plus all eight owning PCH-off
objects. Tests retain each invalid/PriceOnly/crossing/fallback row, pin numeric
and status bit parity, and count zero allocations with metadata enabled,
including ladder rebuilds. Receipts are repair-iv/greek-tick-{red,green,final,
record-final,session-red,session-final,book-red,book-final,hygiene}.*.
No numerical formula, admission rule, accuracy budget or timing claim changed.
Python ServeBook mask export and additive backtest price coverage/carry remain
follow-up work.

The Python ServeBook chain result now exports the native fixed-five requested
and supported masks as owning uint32 arrays, preserving its separate six-axis
record reasons. Native enum buffers are converted after reacquiring the GIL;
no enum object aliases NumPy integer storage. One runtime RED reproduced the
missing keys; all 25 focused serve/session/contract tests and the owning binding
PCH-off object pass. Mixed served/refused rows, direct-answer agreement, empty
shapes and array lifetime remain explicit. The staged native module is SHA256
bc64fef5a93f3b7abeb79419d359acd9aa4e6d8b6b30ba008268dab536293c47.
Evidence is repair-iv/greek-tick-python-{red,final,hygiene}.*. This is availability
propagation only; additive backtest price coverage/carry remains separate.


BEV replay now checks supplied Price, Delta and Vega at entry, supplied Price
before the exercise decision, and supplied Delta before a continuing hedge.
A typed refusal returns Unavailable; it cannot silently become a no-trade
comparison against NaN. An exercise exit needs no further hedge Delta. The
strategy risk reducer requires its actual five inputs (Price, Delta, Gamma,
Vega, Theta), permitting unavailable unrelated sensitivities while refusing
finite default zeros without metadata. Manually supplied test bundles now
explicitly declare their supplied fields.

Runtime RED captured entry and rebalance failures for both put and dual call,
and strategy acceptance of unstamped zeros/rejection of a complete five-field
bundle with unrelated NaNs. Five focused consumer tests and five owning PCH-off
objects pass. Evidence is repair-iv/greek-{bev,strategy}-guard-component.* and
greek-risk-guard-hygiene.log. This component does not change numerical pricing
or Greek arithmetic.

Broader checks remain explicitly pending a separate producer correction: a
zero-rate/yield European call at S=118.744054000552, K=100,
T=0.0082135523613963042, sigma=0.25 is incorrectly labeled StencilCrossing when
rounded Black prices straddle intrinsic. The complete 17-operand trace is
repair-iv/greek-bev-operands.log. The down-rate operand genuinely enters an
American regime, so its Rho crossing must not be erased. No contract or test
row is dropped from the denominator. Two other strict numeric pins reproduce
on the pre-guard production source (28b2ad5d): GoldenPathPnlIsPinned returns
1.3832639438392142 versus its historical 1.3832639438393373, and the strategy
cancellation fixture returns Gamma -4.163336342344337e-17 versus zero. Archived
baseline executables and hashes are in greek-risk-guard-baseline-binaries.json;
these pins are retained unchanged in this guard component.


The strategy cancellation fixture now uses binary-exact Gamma inputs
0.03125/0.015625, retaining its strict zero assertion and all other risk inputs.
The prior decimal 0.02/0.01 products allow the final fused accumulation to expose
-4.163336342344337e-17; the original failure reproduces on the archived pre-guard
binary and is independently listed as r6 known issue Q0-F26. All nine focused
strategy/quant integration tests and the owning PCH-off object now pass. The
BEV golden remains unchanged (r6 Q0-F3); its original first-green deterministic
pin is not an economic tolerance. No production arithmetic was changed.
