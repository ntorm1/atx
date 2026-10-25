# Direct local cubic-weight successor: fixed24 result

All24 declared requests compute, all12 unchanged two-precision checks pass,
and all168 independent positive-integrand quadrature checks pass. This resolves
the three narrow-cell failures within this bounded component. It does not
complete Stage A, price an option, replace any cash reference, or establish a
global error or latency bound.

The prospective plan is `2a078f20cd743367dfba0793c4632e7e30ca19fc`, the primitive
is `8436a3b978a1dae98cb4c5ec988b07001bfffa5c`, and the exporter/reader is
`a8009c937c9e252611a82aff6ad7f3a68f5d395f`. Root, acceptance and repair approved
the source before execution. Exactly one native fixed24 run was performed,
with no input file, reference price, parameter change or rerun.

## Complete result

Each entry is the maximum relative difference between100- and200-digit outputs
over four value weights, three derivative weights and four diagnostic moments.
The threshold remains1e-40. These are component comparisons, not dollar errors.

| Cell | Original maximum | Successor maximum | Successor gate |
|---|---:|---:|---|
| central | 2.31840439e-98 | 2.31840439e-98 | PASS |
| zero-lower | 9.99651478e-99 | 9.99651478e-99 | PASS |
| adjacent-high | 6.14476055e-37 | 8.30331777e-83 | PASS |
| adjacent-low | 1.78929771e-37 | 1.02897156e-81 | PASS |
| adjacent-unit | 1.05132339e-38 | 1.65349355e-81 | PASS |
| lower-tail | 4.51014378e-94 | 4.51014378e-94 | PASS |
| upper-tail | 6.71761262e-96 | 6.71761262e-96 | PASS |
| positive-near-zero-time | 9.33134975e-46 | 7.58956396e-86 | PASS |
| negative-rate | 2.92674435e-100 | 2.92674435e-100 | PASS |
| negative-yield | 1.16111521e-98 | 1.16111521e-98 | PASS |
| small-variance | 2.48442815e-88 | 2.48442815e-88 | PASS |
| wide | 7.47155558e-100 | 7.47155558e-100 | PASS |

All168 quadrature comparisons retain the original positive-integrand SciPy
rule and tolerance. The largest difference/tolerance ratio is0.000789472.
This binary64 check is much weaker than the precision diagnostic and does not
certify80 digits of accuracy.

Eight outcomes use the local series: both precisions for the three adjacent
cells and positive-near-zero-time. The remaining16 explicitly use original
moments:10 wide-cell,4 density-variation and2 zero-lower. Every original-route
weight exactly equals its historical decimal output. Raw moments, mass and
first stock moment exactly equal the original diagnostics on both routes.
No domain, threshold or conditioning gate was widened.

The old24 native receipt and negative summary remain immutable, with all three
failed precision checks still failed in the historical section. The original50
cash cases,150 PDE outcomes including uncertain DSPYp,32 Gaussian pilot outcomes
and18+5 follow-up results remain unchanged. This component does not resolve the
earlier American refusals or repeated-PWL errors.

## Construction and actual work

The local route integrates Bernstein bases directly against the local Gaussian
density. Raw moments supply diagnostic identities only. The reader reconstructs
orders, truncation formulas, all seven condition ratios and separate
value/derivative prefactors at Decimal250, using direct powers/factorials and
exact rational integrals independently of native recurrence arithmetic.

| Local outcome | Digits | J / N | Selection checks | Products | Local functions | Reserved bytes |
|---|---:|---:|---:|---:|---:|---:|
| each adjacent-high/low/unit | 100 | 4 / 4 | 52 | 782 | 10 | 147,968 |
| each adjacent-high/low/unit | 200 | 11 / 10 | 170 | 25,042 | 17 | 240,448 |
| positive-near-zero-time | 100 | 5 / 5 | 69 | 1,633 | 11 | 147,968 |
| positive-near-zero-time | 200 | 11 / 11 | 171 | 30,301 | 17 | 240,448 |

All24 outcomes total906 selection checks,109,406 coefficient products and121
additional local special-function calls. Separate original-moment work is96
moment attempts,192 normal-function calls and448 original-route polynomial
terms. Selection work before fallback is included. There are no refusals.

Boost1.91 scalar objects occupy64/104 bytes at100/200 digits. Scalar reservation
is256 objects; complete local reservation is2312 objects. These are conservative
construction reservations, not measured heap/stack peaks. All frozen1MiB,
J/N16,262144-product and64-function caps remain unchanged.

All seven local condition ratios are at most approximately1.00000000000000224,
below the fixed4 limit. Relative truncation terms satisfy their frozen
1e-80/1e-180 selection thresholds. These formulas cover real-arithmetic series
truncation only; they exclude multiprecision rounding, transcendental errors,
reconstruction, tails, exercise discretization and accumulated option error.

## Evidence

Directory:
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/cash-cubic-local-weights-v1/`.

| Artifact | SHA256 |
|---|---|
| launch-manifest.json | a2d8333139424d23b23e4e5ff5731b2590f9d70772cbf9c3115d4c2e69cedd87 |
| native.json | f4b1fef6996af4ac9c9d9e6adf50624727c51cc7519e431bbed4e3b74e5c03ae |
| summary.json | 64452536e1c9b5cd1c11f7659a6d1b92b374086167ef3804b5b672501637faf5 |
| completion.json | 05c531aeafc9114c0a61e697ca3f12ae35c2bc447db1ef71a1aab63dc54adbeb |
| original native24 | 70e635b6d7becba2a51d6e9ef71d4e3e7dd5e0b6ee3bf727e4302fefaf0f90bb |

Before launch, the manifest checked a clean exact source, seven source hashes,
the archived binary hash and the reviewed tool receipt. Native exit status is0
with empty stderr. Reader dependencies are Python3.12.2/SciPy1.12.0; reader SHA
`ea3c2afa30b57a6da57ba92a4e64dd5edd907c6992bec186ee1ad4e76f94e682` and unchanged
original reader SHA`808813614103ff6bfb199859214ad4bdd3413360d6762bd8b0d1dd652f3b1205`.

The earlier tool receipt preserves three derivation/two reader runtime REDs,
14 passing Python groups,22 malformed mutations, a deliberately negative
synthetic protocol fixture and owning PCH-off/build checks. This native run
uses no synthetic substitute. It is correctness-only; no host-quiet or ThinLTO
timing qualification is claimed.

## Remaining progression gate

The [cubic plan](../bench/cash-gaussian/cubic-plan.md) still requires a privately
owned continuous representation, full-domain/tail partition identities,
one-step/repeated polynomial value and analytic-Delta controls, exact payoff
kinks, zero/deterministic/positive-near-zero limits, and resource/alias tests.
Cash composition must preserve its derivative kinks and nonconvex pieces.
Exercise requires separately reviewed ambiguity enclosures.

No18-option semigroup run was performed. It remains held for review of this
result and completion of Stage A. General cash price/Greek accuracy, independent
reference qualification and HFT throughput remain open.
