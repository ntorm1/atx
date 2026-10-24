# Cold Greek admission: complete 715-contract receipt

The metadata-aware rerun validates the bounded admission change without changing
any option price. It does **not** qualify Greek accuracy or latency. The frozen
corpus still requests 715 contracts x four routes x nine fields = **25,740 field
outcomes**. All requests, unsupported axes, API refusals and reference failures
remain in the denominator.

## Frozen identity

- Source: `ca5dd962dc9d2573b004d1842bc1a59dac21e5f2`, clean worktree.
- Runner: native v2, final verifier `9032a830` + `69101756`.
- Evidence: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/greek-full715-admission-v1/`.
- Full row-level comparison: sibling file `greek-admission-comparison-v1.json`.
- Executable SHA256: `0041168ac9f1474a40cfeab2df96ec80ac37c3f0ea1738f03bddcd291a24b3da`.
- Native report SHA256: `59a7b7a65d1b63fcefe2bc4ef0b05e09582b568bbe4efa22863d3c8559310881`.
- Run manifest SHA256: `3bcd7b4b32676bf95fd2aaf1957b5a1c512540fd058c6a90684cf75e31bf0414`.
- Comparison SHA256: `b0724c2739d88080b2504d19207b5f61d94b86b6558cfc6ac13368205a0142a1`.

The launcher archived the measured executable before running. Every file hash
in the run manifest was independently checked. The 15-case pilot also passed
the actual v2 metadata joins. All original v1 artifacts remain unchanged.

## Exact before/after comparison

Across **2,860 route-price outcomes**, every finite binary64 price bit and every
price status matched the original frozen report. Every reference stencil,
served-map stencil and complete sigma/sigma-q family bundle was unchanged.
The seven pre-existing cold API refusal cases remain refused.

Supplied derivative outcomes decrease from **17,103 to 17,081**: exactly 22
new `StencilCrossing` refusals. Exactly **562** still-supplied numeric values
change; all have reason `Exercised` and become exact payoff derivatives.
**No continuation estimate changes.** Each cold field has 96 exercised outcomes;
many already had the exact numeric value and only acquire explicit provenance.

## Coverage, with every requested field retained

Every cell below has denominator **715**. Before this change each represented
cold field supplied 708 outcomes; cold phi supplied none. Family counts do not
change. A supplied estimate is availability, not an accuracy certificate.

| Field | Cold FD supplied | Cold AL supplied | Sigma family supplied | Sigma/q supplied |
|---|---:|---:|---:|---:|
| delta | 707 | 707 | 386 | 386 |
| gamma | 707 | 707 | 383 | 383 |
| vega | 707 | 707 | 386 | 386 |
| theta | 705 | 707 | 383 | 383 |
| rho | 706 | 706 | 386 | 386 |
| phi | 0 | 0 | 386 | 386 |
| vanna | 707 | 707 | 386 | 0 |
| volga | 707 | 707 | 386 | 0 |
| charm | 705 | 707 | 383 | 0 |

Each represented cold field retains seven `ApiRefused` outcomes. Cold phi is
`NotInApi` on all 715 cases. Each family has 329 preparation/domain refusals;
its detailed unchanged split is preserved in the earlier full-corpus report.
Sigma/q vanna, volga and charm remain `NotInApi` on all 715 requests.

The new crossing refusals are exactly:

| Case | Cold FD | Cold AL | Outcomes |
|---|---|---|---:|
| `g008-put-02` | theta, charm | none | 2 |
| `g011-put-02` | theta, charm | none | 2 |
| `zero_rate_closed_form-put-00` | rho | rho | 2 |
| `zero_rate_closed_form-call-06` | rho | rho | 2 |
| `sigma-family-near-edge-put-00` | delta, gamma, vega, theta, vanna, volga, charm | same seven | 14 |

All five cases retain their original price. The near-edge case retains rho as
a `ComputedEstimate`; no artificial zero is supplied for any refused derivative.

## The two known bad-theta cases

For the near-edge put, cold AL theta changes from `+8.799972502027904` to typed
`StencilCrossing` refusal. The unchanged refined time sequence ends at
`-.3373631080847955` (last-step drift `6.6965e-6`, profile difference `3.7235e-6`).
Cold FD theta `-.33347808489025965` is also refused because its larger production
time stencil crosses. The unchanged price is `38.64967896831942`. This is an
honest refusal; the refined value is not substituted as a supposedly qualified
fallback. The center-inclusive bundle refuses seven crossing fields and preserves rho.

For deep-exercise call `g042-call-06`, cold AL theta changes from
`12.99996879030763` to exact `0`, reason `Exercised`. Cold FD theta changes from
`-2.8975577492929006e-5` to `0`. All six refined time estimates are exactly zero.
The numerical price remains `100.00000622936689`; solved-boundary evidence
recognizes exercise despite that small positive quadrature residual. Derivatives
of the model payoff are distinguished from noise in the numerical price map.

## Unresolved short-time accuracy and other routes

`g010-put-03` retains cold FD theta `-312.5704811476466` versus finest refined
`-307.17081527230624` (absolute gap about `5.39967`). Its cold AL estimate remains
`-307.15642139640937`. Both are labeled `ComputedEstimate`; uniform continuation
does not certify their accuracy. The refined last-step change is `2.8589e-5`.

`g001-call-03` retains cold FD/AL gamma `1.5056756386923202` versus refined
`1.5239342763119224` (gap about `.01825864`, refined last-step change `9.5298e-6`).
These fixed-step defects need separate refinement work. The reference profiles
share the AL method; agreement and observed convergence are not rigorous or
independent-method error bounds.

Cached correction and native adjoint numerical admission are outside this cold
stencil change. Metadata propagation alone does not qualify those graphs.
Cash dividends are outside this continuous-carry corpus. No derivative or P&L
error budget was inferred from the approved SPY price budget.

Implementation and targeted checks are documented in
[the admission review](american-cold-greek-admission-20261024.md). The original
[full-corpus findings](american-greek-full-corpus-20261024.md) retain the baseline
evidence and unresolved reference/coverage details.
