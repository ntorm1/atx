# Local main research checkpoint, 2026-10-24, r8

This user-requested checkpoint advances local main from `a037b1e4`. The tested
combined source is `9b3a6aac0da881c08f6bf0529ea60d49b42bd5c5`; this record is a
subsequent documentation commit. It combines reviewed centrality, exact cubic
coefficient and LCP observation work. No remote push or performance promotion
is included. All new research paths remain default OFF.

## Ordinary solver and full-SPY validation

A finite-input overflow regression exposed a Newton-system defect: Eigen could
report success after a Hessian entry overflowed. The ordinary solver now refuses
nonfinite assembled Hessians before factorization. This is the intended default
behavior change; finite arithmetic, constraints, stopping rules and final
certificates are preserved. No SPY mispricing caused by the overflow was observed.

Fresh combined-source validation passes 435 native tests, with six explicit
fixture/environment skips: 361 quote tests, 65 cash controls and nine standalone
centrality controls. There are also 67 independent-reference Python tests and
three exact-rational test groups passing without skips. Nine owning translation
units retain their component PCH-off checks; their reviewed source identities
match the integration. No broad release pass is claimed.

A newly executed full ordinary SPY audit passes all thirteen actual-API gates.
It serves all 33 valid expiries and 12,866 eligible rows, with 12,851 inside the
original quote boxes (99.8834136484%), 12,850 identified IVs and 16 truthful
PriceOnly rows. Its complete prediction bytes match the prior accepted hash:
`e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`.
The price budget remains `min($0.001/share, 1% of original spread)`. Coverage,
identification, containment and arbitrage requirements are unchanged. This audit
includes proof/repricing work and supplies no cold-fit latency claim.

## Retained research results

- The actual centrality callback and whole-QP experiment have fifteen independent
  solver/dispatch controls. Its full SPY projection saves six factorizations
  (35 to 29) but increases direction solves from 70 to 124. Of 66 extra correction
  solves, 51 are accepted and 15 fail to improve the step bounds. Development
  component time increases; no qualified timing result or promotion follows.
- The experimental full API audit remains false: twelve of thirteen gates pass.
  The same two historically reviewed floor targets move by approximately
  -4.53e-9 and +1.51e-9 USD/share beyond their exact evidence pins. Classification
  counts and IDs, economic price accuracy, containment and arbitrage gates are
  unchanged. The fixture is not widened. See the complete
  [negative result](joint-centrality-result-20261024.md).
- The isolated cubic research owner now proves exactly zero middle coefficients
  from the six original binary64 input patterns. Fixed checked integer storage,
  bounded split products and a backend compile guard prevent hidden unbounded
  multiplication scratch. Exactly nonzero values that round to zero at decimal
  precision retain the original refusal. Full affine tails, complete-domain
  propagation, endpoint-projection accounting and the 324/18 studies remain open.
- The LCP observer validates refusal phases and independently reconstructs the
  initial residual before accepting a missing state. The four saved-stage runner
  is still incomplete and no saved-stage replay occurs in this checkpoint. The
  archived twelve-request study verifies unchanged: eight computed, four refused.
  No PDE solver arithmetic, tolerance or historical Greek decision changes.

All components received fresh exact-source peer approval. The combined CMake
change is a clean union; the sole ledger conflict preserves both append-only
entries. Root build files, public headers, presets and dependency manifests are
unchanged. A clean-source no-op build confirms the final linked targets.

## Evidence and continuing work

The receipt pins 27 changed source files and 178 artifacts, including native
binaries/dependencies, ordinary SPY audit, original runtime failures, component
PCH-off checks and the failed centrality experiment:

`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/integrated-research-r8-receipt.json`

SHA256: `c55d37a3dd006c4b2b63924a635d6a7f33c872a021deefcd47736933963b2a6e`.

The separately frozen [one-secant plan](../bench/joint-quasi-newton-plan-v1.md)
authorizes an isolated inverse-action primitive. Whole-QP reuse still needs a
numerical admission/storage supplement, independent actual-factor controls and
all thirteen full-fit gates before any timing study. The newly supplied volfi,
SANOS paper and sanos-superfast implementation are being assessed as additional
fundamental alternatives; no external benchmark result is adopted here.

Previously reproduced pricing golden, umbrella and BEV failures remain open.
Quiet-host latency, the 100-process release gate, subsecond single-core SPY and
complete American pricing/Greek leadership are not established by this merge.
