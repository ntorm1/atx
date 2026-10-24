# Complete American Greek diagnostic corpus, 2026-10-24

The frozen 715-contract run exposes incorrect successful cold Greek outputs.
It does **not** qualify any Greek route: derivative budgets remain unset,
references share the production Andersen-Lake method, and timing was contended.
The [machine-readable report](american-greek-full-corpus-20261024.json) records
every denominator, reference drift, selected counterexample and receipt hash.
The [earlier review](american-greek-qualification-review-20261024.md) describes
the model conventions and broader independent-reference/latency requirements.

## Frozen evidence and reproduction

Native source was clean `e62d092d2827ddf794a866a51acbf17905681d64`. The archived
executable SHA256 is
`83cd04cdec766951a307b60e18f65e29d67be9e41f7faf18fc97684b54dcc115`;
raw native JSON is
`017f73de1e495f7cdd2a879e915bfb08800c17d72e171e641a6f8ba73ec7a778`.
Receipts live under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/greek-full715-v1`.
The original native prices, summary and manifest remain unchanged. Revalidation
produced `summary-hardened-v2.json`, requiring all 715 frozen IDs, independently
reconstructing every intrinsic-band flag at its bumped spot, and including the
center in each derivative's exercise-regime evidence. A missing center leaves
otherwise computable first differences visible but marks regime evidence
incomplete. Three adversarial tests failed before these fixes; all ten tests now
pass, including 228 independent European Black price checks.

```powershell
python atx-vol/bench/american-greeks/greek_report.py --report <receipt>/native.json --require-all --output <new-summary.json>
```

The native run requests 715 contracts x 4 routes x 9 fields = **25,740 field
outcomes**. All 715 contracts and all requests survive reporting. No low-vega,
exercise, preparation-failure, unsupported-API or reference-unavailable row is
removed. The summary reports 17,103 available values; availability is not an
accuracy pass. All prices use continuous carry and exact frozen model clocks;
cash events, fitted surfaces and portfolio aggregation are outside this run.

## Coverage

Each table entry below is available/requested. Both cold routes return finite
estimates on 708 contracts; the seven unsupported double-boundary puts remain
in the 715 denominator. The cold API has no phi field.

| Field | Cold FD | Cold AL | Sigma family | Sigma/q family |
|---|---:|---:|---:|---:|
| Delta | 708/715 | 708/715 | 386/715 | 386/715 |
| Gamma | 708/715 | 708/715 | 383/715 | 383/715 |
| Vega | 708/715 | 708/715 | 386/715 | 386/715 |
| Theta | 708/715 | 708/715 | 383/715 | 383/715 |
| Rho | 708/715 | 708/715 | 386/715 | 386/715 |
| Phi | 0/715 | 0/715 | 386/715 | 386/715 |
| Vanna | 708/715 | 708/715 | 386/715 | 0/715 |
| Volga | 708/715 | 708/715 | 386/715 | 0/715 |
| Charm | 708/715 | 708/715 | 383/715 | 0/715 |

These family routes use the declared fixed diagnostic boxes, not a composite
library dispatcher. Both refuse 168 contracts below their near-expiry guard.
Sigma-only refuses another 161 outside its American orientation; sigma/q
refuses 133 whose declared q box crosses the orientation domain and 28 whose
geometry has no American orientation. These are preserved route-domain limits,
not a claim that the contracts cannot be priced by another route. Of the 386
prepared families, 64 report exercised derivatives, 322 continuation values;
three continuation contracts refuse gamma/theta (and sigma-only charm) under
the existing near-edge band. Sigma/q vanna/volga/charm are absent from its API.

## Actionable counterexamples

1. **Cold AL applies its continuation PDE across the exercise boundary.** The
   fixed put `sigma-family-near-edge-put-00` has S=772.99, K=811.6395, T=0.0682,
   sigma=0.12, r=0.04496, q=0.00162193. Cold AL returns theta
   **+8.7999725020**, while the finest refined calendar estimate is
   **-0.3373631081**. Refined32's final step change is 6.70e-6 and the two
   profiles differ by 3.72e-6. Its T-only stencil does not cross intrinsic.
   The production spot step 0.77299 does cross: the lower spot price equals
   intrinsic even though the center premium 0.0001789683 passes the old
   center-only test. Cold AL charm is -23.03749089 versus finest -1.59271924;
   the mixed reference stencil crosses at its first two levels and still has
   0.0953 final-step drift, so that last value is not a settled charm oracle.

2. **Tiny default-profile price error misclassifies a deeply exercised call.**
   `g042-call-06`: S=200, K=100, T=3, sigma=0.2, r=0.03, q=0.08. Default price
   is 100.0000062294, enough to miss the center-only intrinsic tolerance.
   Cold AL returns theta **+12.9999687903** and charm **+0.0800011782**.
   Both refined profiles return intrinsic 100 and zero theta/charm at all
   three levels. All reference stencil points are exercised; there is no
   crossing to detect. The family routes classify the base as exercised and
   return exact zero derivatives. A crossing-only patch therefore cannot
   resolve this case. Use the solved boundary/active exercise evidence or
   explicitly refuse an unresolved classification; do not widen an arbitrary
   price tolerance to conceal it.

3. **A fixed time bump gives substantial short-expiry FD truncation error.**
   `g010-put-03`: S=K=100, T=1/365, sigma=0.8, r=0.03, q=0.08. Cold FD theta
   is -312.57048115 versus refined -307.17081527: absolute difference 5.39967.
   Both refined profiles agree; final-step drift is 2.86e-5. Cold AL is
   -307.15642140. This is separate from the exercise-edge failure and needs
   step-size/refinement treatment before FD is considered a qualified fallback.

4. **Fixed spot steps also limit gamma.** `g001-call-03`: S=K=100, T=1/365,
   sigma=0.05, r=0.03, q=0.01. Both cold routes return gamma 1.50567564 versus
   refined 1.52393428, absolute difference 0.0182586. Refined final-step drift
   is 9.53e-6 and profile difference zero. Merely preventing exercise crossing
   does not establish accurate continuation derivatives.

5. **Rate-boundary references are unresolved.** At `zero_rate_closed_form-call-06`
   (S=200, K=100, T=1, sigma=0.2, r=q=0), refined32 rho changes
   58.6466 -> 62.4907 -> 65.5662; phi changes -109.418 -> -117.052 -> -123.762.
   The phi stencil crosses the exercise regime. These are not established
   reference values, so candidate/reference differences here cannot be turned
   into accuracy pass/fail counts.

The existing AL-vs-FD theta relative-error accumulator excludes |FD theta|<=1,
which hid the first counterexample. The fix requires a direct sign/absolute
regression. The scalar and SIMD put/call implementations duplicate the same
center-only PDE admission, so all routes and prepared wrappers must be covered.

## Complete numerical diagnostic maxima

Absolute differences below use the finest refined32 estimate, regardless of
its stability, and **are not accuracy bounds**. The JSON preserves each worst
case's ID, full refinement series and refusal counts. Unsupported fields are
shown as a dash rather than zero error.

| Field | Cold FD | Cold AL | Sigma family | Sigma/q family |
|---|---:|---:|---:|---:|
| Delta | 0.00085122 | 0.00085122 | 0.000011222 | 0.000011223 |
| Gamma | 0.018259 | 0.018259 | 0.0000046775 | 0.0000046780 |
| Vega | 0.070177 | 0.070177 | 0.0055769 | 0.0055769 |
| Theta | 5.3997 | 13.0000 | 0.00034296 | 0.00034296 |
| Rho | 6.1567 | 6.1567 | 0.0061129 | 0.0061128 |
| Phi | - | - | 0.010515 | 0.010455 |
| Vanna | 1.2152 | 1.2152 | 0.039417 | - |
| Volga | 158.85 | 158.85 | 23.622 | - |
| Charm | 0.71204 | 21.445 | 0.000049223 | - |

All nine reference fields have 708 complete numerical stencils and seven
unavailable contracts. Center-inclusive intrinsic-regime changes occur on one
contract for delta/gamma/rho/vanna/charm, two for phi, and zero for
vega/theta/volga. Step-change growth counts vary from 7 to 85 and include
floating-point cancellation near zero; they are diagnostics, not a binary
stability standard. The JSON retains magnitudes and case identities.

Served-map consistency is a separate comparison. For example, near-edge
sigma-family volga is 1339.4231 versus finest own-map 1339.6044, but refined
cold-model volga is 1363.0448. Own-map agreement cannot establish model accuracy.
Time/rate are absent axes in both fixed-expiry families and q is absent from
sigma-only; the runner keeps those map stencils `NoModelAxis`.

## Required implementation sequence

Freeze a shared additive availability contract before changing callers:
default-empty requested/supported masks, explicit per-field reasons, and finite
checks. An available `ComputedEstimate` is not accuracy-certified. Preserve the
finite base price independently of refused derivatives. Then gate scalar and
SIMD numerical formulas on their actual field-specific exercise/stencil
evidence, including the solved boundary rather than center price alone.
Unrequested normalized zeros must never become supported outputs.

Propagate masks/reasons through batch, chain, prepared, portfolio, record and
Python adapters; add allocation and requested-mask tests. Freeze new numerical
receipts on the same 715 IDs and show both improvements and remaining defects.
Subsequent refinement or alternative formulas need explicit derivative budgets
and a stronger independent reference before any accuracy-qualified latency
ranking. The SPY monetary price budget supplies no derivative tolerance.
