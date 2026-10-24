# Complete SPY candidate attribution and next batch experiment

The complete capture preserves all 700 monetary-candidate attempts: 684 accept
and 16 refuse. A separate scalar actual-API audit of the same archived binary
passes all 13 gates, and predictions are byte-identical to the prior vector-only
candidate. The main remaining candidate cost is the scalar proof, not finding
the Screen proposal. These are contended, instrumented, non-IPO development
clocks; they do not establish a whole-fit speedup or sub-second latency.

## Exact workload and independent API evidence

Executed clean source: `602c8978535fdcaf00c02c36801b0aedd09d8044`.
The executable numerical/tool code is unchanged from reviewed `bdfeb373`; the
follow-up corrected the independent reader and added the real optional-cache
allocation-refusal control before this full capture was authorized.

Capture directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/price-candidate-capture-v2`.
The earlier `price-candidate-capture-v1` archive remains unexecuted.

| Artifact | SHA-256 |
|---|---|
| Archived quote benchmark | `80d4a19e999514f1740466c7360fd66f1dead891ad3844208ab7e52409e4bdd4` |
| Canonical LF policy | `781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a` |
| Input quotes | `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e` |
| `capture.json` | `ec73d07ad0cb621b1c7dce6f6ca5abf8a1ceb39aac7a0727946ba07d3df55e3e` |
| `native.json` | `6264ea2f01afb2d4947c19d859ef5b85fcf4e869bb67d33a4e6f7e25a7af0c2e` |
| Independent `summary.json` | `3a3c0d22b534e164d25d3fcfa10c1a30c231e50fe936ffbab65957096a972bff` |
| Predictions, capture and API audit | `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f` |
| `full-audit/native.json` | `e616696b17ef3e2fd6322fed3d3f2d6574b13e53cff5b764052a67bdee3797fe` |
| `full-audit-verification.json` | `84582f1d6983d132a2431b9eb9a8ab3716ae372738a91636d0ffbdf78c6cd830` |

The frozen `raw-spread-price-v1` and `reviewed-feasible-floor-v1` contracts give
33 fully served slices and 12,866/12,866 eligible served rows. Of these, 12,851
are inside their original boxes: 99.88341365%. There are 12,850 identified IVs
and 16 genuine floor PriceOnly rows. The maximum independent cold Reference
price residual is $0.0005012966094284366/share, or 50.13% of its raw-spread
budget. All floor, metadata, actual-query, between-node shape/parity/calendar
and coverage gates pass. The strict 1e-7 IV diagnostic still fails for 12,093
identified nodes; it is not the selected monetary-accuracy contract. Neither
this check nor the price budget qualifies Greeks.

## Actual six-phase work

| Phase | Attempts | Boundary solves | Premium evaluations | IV iterations | Diagnostic ms |
|---|---:|---:|---:|---:|---:|
| Screen proposal | 700 | 469 | 57,600 | 1,437 | 30.5886 |
| Scalar Screen proof | 684 | 689 | 42,912 | 0 | 129.0176 |
| Fast proposal | 16 | 0 | 0 | 0 | 0.0021 |
| Fast proof / complete Fast / retry proof | 0 | 0 | 0 | 0 | 0 |

Inclusive observed candidate calls total 159.8243 ms; phase clocks total
159.6083 ms. Clock/observer and surrounding control work account for the
difference. The independent reader reconstructs every conditional phase and
matches existing whole-fit counters. All 16 refusals are retained. They have
no valid seed entering a proof, so they cannot be inserted into a proof-only
replay as fabricated IVs.

The capture owns 107 cache lifetimes, including lifetimes with no candidate
calls. Within the 684 proof calls, floor work is 263 preparations and 421 hits.
The 689 boundary solves cannot be read as 684 target solves plus five floors:
some models take European/degenerate pricing routes, while actual floor
preparations are separately counted. The same finalizer also makes 70 direct
prepared-seed attempts, 61 accepted. Those precede a candidate for nine targets
and share the finalizer floor owner. Their work remains part of the complete
fit and is not included in the 700-candidate phase table.

All 700 calls have distinct full contract/model keys. Their exact floor-handle
keys number 264; for the 684 accepted proofs there are 262, including 250
singletons. A handle key includes side, call-spot/put-strike anchor, T, r, q and
raw optional pricing options. This differs from the 31 exact expiry-geometry
keys, which omit side/anchor, and from SIMD packs, whose four lanes can have
different supported models under the same option scheme. All proofs use raw
null Reference options, resolved as 12/24/48/6, tolerance 1e-10.

Two expiries dominate. T=0.005899536423385143 has 302 accepted proofs,
151 calls and 151 puts, with 85.187 ms of scalar-proof diagnostic time.
T=0.00939783826470375 has 306 accepted proofs, 237 calls and 69 puts, with
24.1308 ms. The latter has 20 exact negative effective yields. The two
refusal-only expiries retain four and twelve attempts respectively.

## Reuse of the already reviewed batch component

`bench/candidate-trace/proof_requests.py` first validates the entire capture,
then extracts every visited proof seed with its exact original request,
raw options and source call/phase identity. No-proof calls remain explicitly
listed. The existing reviewed `atx-vol-cold-seed-replay` and `empirical.py`
evaluate all 684 extracted requests, without changing the vector component.

Replay directory: `acceptance/price-candidate-proof-replay-v1` under the same
evidence root. Archived executable SHA-256 is
`e60cfe8ece15796568c7b9efc0c85f0ae29cd3a4697d4e66ff38987cede0d758`.
Requests, native output and independent summary hashes are respectively
`18d1f7f92c7c6ecf304eb494af1d9f07d999e1c504769a1f02a73b5cfada9aed`,
`d77f7d5a0442d8fbe006f71ab013e6c9ae871fb57b39d2c706a0818f081dab05`, and
`14f27141c61aa6f80ce9a9ddfac619e6bc660974563b24c869bb2f22f06cafef`.

| Outcome | Count | Fraction of all 684 proof requests |
|---|---:|---:|
| Vector target admitted, scalar floor retained | 333 | 48.68% |
| Unsupported vector scope, full scalar proof | 306 | 44.74% |
| Partial-pack tail, full scalar proof | 18 | 2.63% |
| Near-exercise vector refusal, full scalar proof | 27 | 3.95% |
| Final refusal or changed scalar decision | 0 | 0% |

There are 90 full packs/360 vector lanes. Every resulting IV equals the
captured seed, and every scalar full-proof decision agrees. The largest
vector/scalar price difference among admitted vector lanes is 2.4869e-13;
this is at most 3.218e-5 of the already declared empirical allowance. That
observed result is not a generic error bound. The minimum admitted boundary
distance is 0.2540987; the 27 refused puts remain below the existing 0.25
admission band. None of that domain is widened.

All 306 unsupported requests are from the negative-q expiry, with q between
-0.007619020367307835 and -0.005799496384604149. Its 237 calls use the scalar
European route; its 69 puts retain scalar American pricing. This experiment
does not extend SIMD scope to those cases. The largest positive-q expiry
admits 273/302 requests through the vector target path, retaining 27 near-edge
puts and two tails as scalar. The 12 replay groups start with fresh owners;
they reproduce 263 preparations/421 hits in aggregate, but omit interleaved
direct seed operations. Equal counters are not proof of identical owner state.

The component reports 115.2019 ms inclusive, including floor-owner destruction
but excluding caller packing/JSON controls. Its 39.0002 ms vector,
49.4641 ms scalar-fallback and 25.8369 ms scalar-floor subclocks are nested
component attribution. They must not be compared with the earlier 129.0176 ms
as a speedup: execution time, contention, instrumentation and boundaries differ.
Only the route/decision results justify implementing a bounded experiment.

## Approved next implementation, before new output

Keep the existing default-off empirical build selection and an explicit new
candidate-proof provenance field. The scalar audit exporter remains unchanged.
There is no new negative-q vector admission, relaxed boundary band or widened
economic budget. Use these shared pieces rather than another inverse solver:

1. Factor the current re-implication path into a common prefix returning a typed
   completed identification or pending candidate, and a common typed-Search
   tail. The ordinary scalar wrapper executes the same prefix and tail with
   the original arguments. The finalizer executes the prefix once in original
   target order, preserving all cached/native/economic and direct-seed routes.
2. For pending targets, run unchanged scalar Screen proposals. Retain the full
   target, side, S/K/T/r/q, raw options, controls, budget, seed and target index
   in owned records. Collect only actual finite proof seeds. Batch them through
   the existing reviewed `VectorReferenceEmpirical` seed evaluator in exact
   option/control groups, with every fresh floor preparation and owner setup/
   destruction charged. Original prices and raw quote budgets are unchanged.
3. Factor and resume the existing candidate state machine for failed/missing
   Screen proofs. Preserve Fast proposal, early proof, complete-Fast retry,
   final proof and bad-allocation semantics without repeating the Screen
   proposal. Run the common typed-Search tail on remaining candidate refusals.
   Scatter completed identifications in original target order; no pending or
   placeholder identification can enter publication.

This deliberately changes floor-cache order; it is not concealed as an exact
warm-cache replay. The acceptance conditions are bitwise full price/status/IV
parity, all 700 candidate attempts and their 684/16 outcomes, all direct 70/61
counts, complete target coverage, and the separate 13-gate scalar actual-API
audit. New counters distinguish vector origin, scalar fallback, proposal work,
floor preparation/hits and inclusive allocation/setup/destruction.

Before whole-fit wiring, controls must cover failed Screen proof, no Screen
seed, early Fast acceptance, rejected early Fast followed by complete retry,
typed refusal, allocation failures, unsupported options and partial packs.
They must exercise the resumed-state path, not merely the 684 SPY acceptances.
Default scalar dispatch and ordinary behavior remain unchanged. Only after
fresh source review, complete parity and actual-API gates may an archived
ThinLTO binary enter a separately coordinated full-host quiet timing comparison.
The limited 333-lane opportunity is not enough to predict sub-second fitting.
