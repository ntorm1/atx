# Empirical vector seed build: full SPY correctness audit

The explicitly enabled research build passes all 13 existing SPY correctness
gates. Its complete prediction file is byte-identical to the previously audited
scalar candidate. This establishes this workload's correctness under the declared
monetary contract; it does not establish a generic vector error bound, Greek
accuracy or a latency improvement. No qualified timing trial was run here.

The component census in `cold-seed-vector-component-20261024.md` describes the
earlier isolated component at `1419fa95`, before fitting dispatch existed. The
subsequent default-off wiring at `701f96de` is the subject of this report.

## Frozen execution identity

| Item | Identity |
|---|---|
| Source, clean before archival | `701f96de69845eed5c3ef550a12ff913608916b5` |
| Archived executable SHA256 | `f5f8c59792106a3ca5eeb4886463b82f4730d5bbc2def0e43284c3c1183456f9` |
| Raw SPY CSV SHA256 | `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e` |
| Canonical policy SHA256 | `781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a` |
| Final predictions SHA256 | `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f` |
| Native audit report SHA256 | `e2d25b69ed30ec7d45a1fbbd16a3ab16fb93efea4c64e88c88c2885e2d023c8e` |
| Evidence manifest SHA256 | `669faf33017ba02755bf0af3df49bcf3b82dac30171ec680ae24f99af9499559` |

Evidence root:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/vector-seed-wiring-enabled-v1`.
The archived executable is `bin/atx-vol-quote-bench.exe`; final native and Python
receipts are in `canonical-full-audit/`. `canonical-audit-evidence-v1.json` hashes
the executable, policy, native output, predictions, verification and earlier
receipt. The build uses `vol-rel`, precise arithmetic, AVX2 and
`ATX_VOL_EMPIRICAL_VECTOR_SEEDS=ON`. The linked library reports
`VectorReferenceEmpirical` in both effective-policy and build provenance. The
audit's cold price exporter remains scalar `AndersenLakeReference`.

The initial run used the worktree's CRLF policy bytes, SHA256
`b78ba4e71b63bb61acd34063ef6ea26466996368e2818e87f99c77fbec228073`.
`policy-byte-comparison.json` records that replacing its 73 CRLF sequences with
LF produces exactly the canonical bytes and identical parsed JSON. Both initial
receipts remain in `full-audit/`; they are not overwritten. The final run uses the
archived canonical LF policy itself, so no byte-equivalence assumption is needed
to join it to the canonical comparator policy. `source-and-binary.json` retains
its original CRLF policy hash; the final receipt supplies the canonical binding.

## Complete workload result

| Required result | Observed |
|---|---:|
| Fully served valid expiries | 33 / 33 |
| Eligible rows with a served price | 12,866 / 12,866 |
| Prices inside original raw quote boxes | 12,851 / 12,866 (99.8834136484%) |
| Identified IV rows / PriceOnly rows | 12,850 / 16 |
| Scalar cold monetary checks passed | 12,850 / 12,850 |
| Scalar cold target-floor checks passed | 12,850 / 12,850 |
| Maximum absolute cold price residual | $0.0005012966094284366 / share |
| Maximum fraction of each row's raw-spread budget | 0.5012966094284366 |
| Actual-API arbitrage, row-join or metadata violations | 0 |

The monetary contract is explicitly `raw-spread-price-v1`:
`min($0.001/share, 1% of the original raw bid-ask spread)`, with separate numeric
roundoff accounting and the tightest duplicate-row budget. The identification
contract is `reviewed-feasible-floor-v1`, retaining the two previously reviewed
floor-related PriceOnly transitions. The strict no-transition diagnostic remains
false; the experiment creates no new transition and has identical predictions.

All 13 checks pass: successful execution; frozen input; native-report and
prediction hashes; all eligible rows served; all valid slices fully served;
99.5% containment; actual-API shape/parity/calendar checks; approved identification
coverage; identified target-floor semantics; scalar cold monetary accuracy;
every eligible prediction joined to the actual API; and every eligible metadata
record joined to that API. The shape checks include analytical between-node
terminal-floor extrema and the actual PWL representation, rather than only
stored knots. These are finite-domain checks, not a general model-realizability,
extrapolation, dated-cash, Greek or live-tick theorem.

The older strict sigma +/- 1e-7 diagnostic remains visible: 757 strict straddles
and 12,093 failures out of 12,850 identified nodes. The user-approved monetary
contract deliberately permits these IV differences. No strict-root result has
been relabeled a pass.

## Work actually selected

| Seed preparation outcome | Count |
|---|---:|
| Eligible prepared seed requests | 496 |
| Vector packs / vector lanes | 120 / 480 |
| Vector accepted roots / consumed witnesses | 480 / 480 |
| Scalar tail fallbacks / accepted roots / consumed witnesses | 16 / 16 / 16 |
| Exact scalar floor preparations / hits | 11 / 485 |
| Other vector retry reasons | 0 |
| Remaining ScreenThenFast attempts / accepted roots | 700 / 684 |

All 684 remaining accepted roots were accepted at the Screen proposal's scalar
cold proof, and all 684 Screen proposals stopped early. The 16 refusals remain
in the denominator. The older `reimply_fast_attempts/roots` names count the whole
ScreenThenFast candidate invocation, not exclusively the Fast inverse branch.

The counts are sums over all 33 slice timing records. Audit wall clocks are
diagnostic and host-contended. Separate vector, floor and scalar clocks must not
be added to the inclusive candidate clock. There is no extrapolated full-fit
speed claim from these counters. Earlier component timing preflight was blocked
before any trial; its failed preflight remains archived.

## Reproduction and next decision

Run `bench/spy_acceptance.py run-audit` with the archived executable, original CSV,
`canonical-policy.json` and a new output directory. Then run `verify` with
`--accuracy-contract raw-spread-price-v1` and
`--identification-contract reviewed-feasible-floor-v1`. The existing receipt
contains the exact native command and all input hashes. A future timing campaign
requires a separately archived ThinLTO binary, the same scalar API audit, fresh
processes, one physical core, and the existing host/sibling admission checks.

The next structural investigation is specified in
`cold-price-candidate-attribution-design-20261024.md`: capture the 700 remaining
candidate invocations, separate proposal inversions from their original scalar
proofs, and retain every refusal before choosing another batch architecture.
