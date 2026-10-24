# Repaired-price family coverage diagnosis

The integrated wave-1 development profile has 1,086 `reimply_no_context`, 472
`reimply_outside_q`, and 3,468 `reimply_outside_window` events. The last name denotes
the native **price-domain** gate near the zero-volatility/nonmonotone band or the
price ceiling. It does not indicate a volatility interpolation box miss.

The profile and raw-input reconstruction identify three distinct cases:

| Expiry | Cause | General next step |
| --- | --- | --- |
| 20741 | `T=0.0058995364` is below `kAlSigmaFamilyMinTau=3/365`; both requested sides report `ShortExpiry`. | Keep the guard until a separately certified short-maturity approximation or direct bounded inversion is tested. Lowering a constant alone is not an accuracy argument. |
| 20742 | Effective yield is entirely negative, `[-0.0076190204,-0.0057994964]`, while effective rate is `0.0448968671`. The put family builds; the call side reports `CallNoCarry`. | These calls are in the kernel's European regime. Use its closed-form European route if beneficial; do not widen an American family into that regime. |
| 20754 | There are 30 positive-bid quotes per side. The adaptive estimate is `5.4*30=162` calls, below the `3*2*16*4=384` build threshold; neither side is requested. | First compare the existing `family=Always` option. Then consider a general cost estimate including publication and repair work, with sparse-chain build-cost regressions. |

Every nonzero `outside_q` counter belongs to 20742 or the global calendar counter
slot. That counter combines a missing side with an out-of-range yield, but the
side refusal and raw carry explain these misses without a range expansion. There
is no evidence here that a repaired yield left a successfully built family.

The shortest-expiry timing slot also accumulates the global calendar publication.
Its 1,026 no-context events and 235 outside-q events cannot all be attributed to
that expiry. The 20754 local slot reports 60 fallback inversions and 48 ms of
Search work. These are development observations on a contended, non-LTO binary,
not qualified latency measurements or a prediction of savings.

Evidence: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`
`integrated-wave1-profile/trial-001.json` and
`k6/family-coverage-diagnostic.{py,json}`. The diagnostic preserves the raw input
SHA-256 `b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e`.
