# Empirical vector cold-seed component: correctness census

This is an isolated private component on production base
`f30c96a80b89051f6670d425590a23538110ecf4`. It implements the experiment described in
`cold-seed-vector-tier-design-20261024.md`. It changes no public default, policy,
archive format or fitting dispatch. Its vector target price has an explicit
empirical origin; it is never called an exact scalar Reference proof.

The original scalar seed function is unchanged. The new component batches full
four-lane groups under the existing accurate vector boundary scheme, maps calls
to internal puts, captures the actual solved boundary and classification, rejects
the established 0.25 sigma-sqrt-time edge band, and reserves
`1e-11 * max(1,S,K,abs(target),abs(vector_price))` USD/share. This margin is an
engineering allowance, not a theorem about the complete vector boundary solver.
Every declined lane runs the original complete scalar helper. The exact scalar
floor cache remains mandatory; targets in the nonmonotone band retain the scalar
positive-chord check. Scalar tails are evaluated once by that helper.

The source is deliberately narrower than the public price domain: only the
accurate Reference scheme and nonnegative rates/yields can enter vector work.
Unsupported controls/schemes, invalid inputs and degenerate regimes keep the
scalar route. No row identity or SPY-specific whitelist is used. Preparation
origin and immutable-consumer integration are future wiring work, not implicit
behavior of an existing scalar prepared record.

## Complete captured SPY component

The immutable capture contains 496 requests from 11 operations, all originally
accepted. Two repeated candidate evaluations have identical per-request outputs.
The independent report verifier joins every original request and reports:

| Outcome | Count or value |
|---|---:|
| Requests retained | 496 / 496 |
| Empirical vector admissions | 480 |
| Scalar tails/admissions | 16 |
| New refusals or changed scalar decisions | 0 |
| Changed returned IV bits | 0 |
| Exact scalar floor preparations / hits | 11 / 485 |
| Maximum vector/scalar target-price difference | 2.2737367544323206e-13 USD/share |
| Maximum scalar target residual | 6.47291083168966e-5 USD/share |
| Maximum observed vector difference / reserved allowance | 2.9414447017235717e-5 |
| Minimum vector boundary distance | 0.38647412959363164 sigma-sqrt-time |

Evidence: `acceptance/vector-seed-component-spy-v1.json` and
`acceptance/vector-seed-component-spy-summary-v1.json` beneath the shared
`spy-qualified-r7-20261023` evidence root. The summary explicitly denies whole-fit,
generic-bound and timing qualification. Its clocks were collected under host
contention, and exclude caller adapter work; they are not a speed claim.

## Refusal and broader-domain controls

A declared 810-contract grid spans both sides, strikes 60/90/100/110/160 at
spot 100, expiries 0.002/0.5/3 years, volatilities 0.02/0.25/1, rates
0/0.04/0.15 and yields 0/0.012/0.08. All 810 forward calls completed. Original
scalar seed acceptance was 487/810. The candidate admitted 297 vector lanes and
sent 513 lanes to the unchanged scalar route; all final decisions match the
scalar helper. Its 323 refused IVs remain in the denominator. Every admitted IV
meets the scalar price budget and floor check. Maximum vector/scalar difference
among vector admissions is 1.4921397450962104e-13 USD/share. Reversed pack order
preserves all final acceptance and IV bits. This finite census is not a global
bound and does not qualify Greeks.

Five new native tests cover explicit origin, both sides, tails, precise floor
cache model keys, unknown mode/shape, zero/tiny budget, unsupported scheme/rate,
actual near-edge cases and the 0.00499-target/0.00501-seed floor counterexample.
The 28-case owning regression selection passes, including the unchanged scalar
floor/chord and family-economic tests. ForceScalar passes its three applicable
controls; two vector-specific tests are explicitly skipped. Four new Python
adversarial verifier tests and seven previous replay tests pass. The prior four
independent European price controls still pass under two repetitions. PCH-off
checks pass for the owning scalar, SIMD, replay and native-test translation units.

## Timing and next integration gate

The prior archived ThinLTO component campaign was blocked before its first
process: independent preflight found cmake PID8336 and ninja PID20352 after the
shared quiet signal. `cold-seed-replay-lto-v1/blocked-preflight-001.json` retains
zero launched trials. No replacement trial or performance claim was made.

Before whole-fit experimentation, explicit binary/policy provenance, prepared
origin and mode rechecks, named fit counters, and full scalar actual-API accuracy
checks must accompany the wiring. The same frozen 33 slices and 12,866 eligible
rows, raw containment, no disallowed IV demotion, price-arbitrage and floor gates
remain mandatory. A component success never substitutes for those checks.
