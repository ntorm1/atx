# The (sigma, q) family's error envelope and root acceptance (lane K3, r5)

Status: opt-in keys only (`graded.family_q`, `graded.family_native`, the B-family
`confirm: band` arm). No default reaches the family route. Code:
`src/pricing/american_family_error.{hpp,cpp}` (contract and construction),
`src/pricing/american_iv.cpp` (acceptance: `family_enclosure`, `confirm_bracket`,
`accept_family_native`), `src/fitting/deam_iv_cache.cpp` (`deam_family_certify`, the
scoped envelope, counters, the `--deam-audit` per-root bracket audit).

## 1. What changed and why

Before K3 the family route rested on a SAMPLED statement: `deam_family_probe_worst`
priced the family at a few points against the cold accurate kernel and the grid's bound
was `max(10 x worst, 1e-12)`; the B-family band arm then skipped a confirm on that bound,
and the family-native confirm divided the confirm kernel's residual by the iterate's
secant, whose partner could sit at any distance (R1-A MJ1). A probe is an estimate, not a
bound (deep review F02 / 5.4; master plan 3.2), so neither skip was provably within the
1e-7 absolute-IV requirement.

K3 keeps three quantities apart:

| type | what it is | may it admit a skip? |
| --- | --- | --- |
| `SampledErrorEstimate` | what the build-time probes measured | never (it can only veto) |
| `ValidatedErrorEnvelope` | a per-family bound on the family's boundary error, built from the family's own nodes, with a domain and a class | yes, class `RefinementValidated` only |
| `RootAcceptance` | how one root was accepted: kind, reason, class, bracket | records the decision |

Units: volatility absolute (0.01 = one vol point); prices USD of the contract; y the
Andersen-Lake dimensionless boundary `y = ln(xmax / B)^2`.

## 2. The target map and what the envelope bounds

A(sigma) is the cold accurate kernel the family was built on (`andersen_lake` with the
geometry's `AlOpts`, at the strike's own q). The family and A share the European leg, the
time interpolation and the premium quadrature (same geometry, same tables), so the only
difference is the interpolated boundary: the quadrature contribution is ZERO by
construction (`AmericanFamilyError.OnNodeFamilyPriceIsTheTargetPriceToRounding`: at a node
of a scalar-built family the two prices agree to 3.4e-17 of S + K). A's own
discretisation error against the continuous model is model error, not root error.

## 3. Construction (no solve, no allocation)

Per premium quadrature node i (time t_i = z_i^2, boundary read at tau_i = T - t_i):

1. `Y_i[j][s]`: node (sigma_s, q_j)'s boundary at tau_i, by the pricing path's own time
   interpolation (barycentric on the collocation nodes).
2. Chebyshev-Lobatto coefficients `A_i[n][m]` (DCT-I per axis); column sums `M_m`
   (sigma), row sums `R_n` (q).
3. sigma tail, verified on refinement: pair sums `E2 = M_{N-1} + M_{N-2}`,
   `E1 = M_{N-3} + M_{N-4}`, `E0 = M_{N-5} + M_{N-6}`; the order seen on the coarser
   truncation (E1 / E0) must hold on the finer (E2 / E1): both two-step ratios <= 0.75^2,
   and the tail is extrapolated with the slower one, `E2 r^2 / (1 - r^2)`. A tail at the
   noise floor is resolved.
4. q tail: the a-priori Bernstein ratio of the box against q = r must agree with the
   observed last-coefficient ratio; the slower of the two extrapolates.
5. Pass 2: each node takes 2 x the worst tail of its +-2 quadrature neighbours (a
   coefficient can cross zero as tau moves); a neighbour whose order did not verify makes
   the node unverified.
6. `bound_y[i] = 2 (sigma tail + q tail) [Lobatto aliasing] + Lebesgue_t Lebesgue_sq
   kFamilyBuildParityY + Lebesgue_t kFamilyReferenceNoiseY + (1 + Lebesgue_sq) 64 eps max|Y|`.

Contributions and their pins:

| contribution | value | pinned by |
| --- | --- | --- |
| interpolation (sigma, q tails) | refinement-verified per node; unverified -> +inf | `EnvelopeBoundsTheBoundaryErrorAtEveryQuadratureNode` (worst measured / bound 0.51) |
| boundary solve (node data) | `kFamilyBuildParityY = 1e-12` | `NodeTensorMatchesTheScalarBuild` (AVX2 vs scalar node tensors: 2.5e-13) |
| reference (target off-node noise) | `kFamilyReferenceNoiseY = 1e-12` | `TargetBoundaryIsSmoothBetweenNodes` (the accurate scheme runs 6 sweeps at every point of every box: no early-exit switch, so A is smooth between nodes) |
| quadrature | 0 | `OnNodeFamilyPriceIsTheTargetPriceToRounding` |
| rounding | `64 eps max|Y|` in y, `256 eps (S + K)` per price | - |

Uncovered (`error_class = Uncovered`, every root confirmed): `NotPrepared`,
`UnsupportedShape` (n_sigma < 6), `DegenerateQBox` (a collapsed box of non-zero width),
`SigmaOrderNotVerified` / `QOrderNotVerified` (whole-family statuses are reserved; today
an unverified node falls back to the trivial box bound instead), `ProbeViolation`.

## 4. The price bound

The put premium is `(sqrt(T)/2) sum_i w_i g(z_i)`, and `dg / d ln B = 2 e^{-rt} phi(d2)
(rK - qB) / sigma`, so g is increasing in B on (0, xmax]: per node the TRIVIAL bound is
`g(xmax)`; with `delta = bound_y[i]`, `|ln B_f - ln B_A| <= d = min(sqrt(delta), delta /
sqrt(y_f))` and the mean value theorem gives the SENSITIVITY bound with phi at the closest
|d2| the interval reaches. `family_price_bounded` sums the smaller of the two over the
nodes, plus the rounding budget. Audit (`EnvelopeBoundsTheMeasuredErrorAtRandomOffNodePoints`,
400 random off-node points per family, ten families): 0 violations, worst err / bound 0.067
(puts), 8.0e-4 (calls: q < r calls are loosely bounded, their y resolves poorly).

`family_domain_bound_usd` (the certification grid's scalar `q_bound`) takes every
point-dependent factor at its domain-wide worst; the grid reads it per unit max(P, 1), a
conservative reading. It is much looser than the per-point bound: on the OIH-shaped
wide-carry chain the family grid now serves no price (the key is inert there).

## 5. Root acceptance (the rule and its proof sketch)

On the domain `premium_root` admits (strictly inside the identifiable range, above the
non-monotone band) A is continuous and increasing in sigma. A bracket [a, b] whose ends
carry CERTIFIED sides, A(a) <= price <= A(b), therefore contains A's root sigma*, and a
served point clamped into [a, b] lies within b - a of sigma*. A side is certified

- by the family: `P_family(s) - eps(s) >= price` (or `+ eps <= price`), eps the
  envelope's per-point bound, `|P_family - A| <= eps`;
- by the confirm kernel C: `C(s) - eps_C >= price` (or `+ eps_C <= price`), eps_C the
  kernel's tested bound against A (`al_profile_price_bound`: Reference 0, Fast 1e-12
  max(P, 1)).

| kind | reason | ends |
| --- | --- | --- |
| Skipped | FamilyEnclosure | both by the family (two family prices at x -/+ W/2, one re-centred retry); no confirm solve |
| Confirmed | ConfirmAndFamily | C at the root, the family at the far end (1 solve) |
| Confirmed | ConfirmBothEnds | C at both ends (2 solves) |
| PriceOnly | LowVega | slope W <= 2 eps_C: C cannot certify any bracket of width W; Search serves |
| Interval | BudgetExhausted | a certified bracket wider than W; Search serves |
| None | BudgetExhausted / None | no bracket within kIvPremiumConfirmSolves = 2 solves; Search serves |
| Confirmed | NewtonContract | the established keys-OFF confirm (Newton distance on C), every non-native route |
| Skipped | LegacyBandGate / LegacyNever | the band gate / policy Never on a request WITHOUT a (sigma, q) family |

W = min(tol, 1e-7) (1 - 2^-20). No step length and no secant slope is a bound: a local
chord only places an end and the served point. A request that carried a (sigma, q) family
never reaches the band gate (`band_gate_holds` runs only when `gate->qfamily == nullptr`),
so no domain of the family route falls back to a band-based criterion; the audit reads
`LegacyBandGate` = 0 on every family-route arm.

MJ1 (`AmericanIvPremium.FamilyNativeAcceptanceDoesNotRestOnANonLocalSecant`): a remembered
far pair forces the native iterate to accept with a partner 0.3 vol away (56 of 56 roots);
every one is accepted through a certified bracket <= 1e-7 and lies within 7.8e-10 of the
independently bisected accurate root.

## 6. Plumbing and audit

`deam_family_certify` (the probe call site in `quote_slice.cpp`) builds the envelope,
audits it with 9 probes independent of the construction nodes (3 random from a seed of
the box, 3 Lobatto cell midpoints of both axes at the grid band's ends and spot, 3 at
0.2 % inside the exercise boundary), retracts it on a violation, and stores it with the
grid's `q_bound`. `DeAmIvView::invert_premium` scopes the side family's envelope around
each inversion (`FamilyEnvelopeScope`, thread-local, identity-checked against the
family). The clean seam is an `IvPremiumGate` field set by `PremiumModel` in
`quote_slice.cpp` - outside this lane's scope (K2 / C0 may move it there).

Counters (`FitTimingCounters`): `deam_accept_by_kind[5]`, `deam_accept_by_reason[9]`,
and under `--deam-audit` `deam_audit_bracket_{roots,unresolved,violations}` and the max
|served - root| of skipped / confirmed roots: every typed root served by the family route
is re-solved by bisection of the accurate map to 1e-9 (never the family). With
`ATX_VOL_FAMILY_ROOT_AUDIT=<path>` the audit also writes one CSV row per typed root.
The slow suite `AmericanFamilyRootAudit.*` repeats the audit over randomized SPY- and
S=100-shaped families at seven expiries.

## 7. Mapping onto C0's identifiability statuses

Confirmed / Skipped -> identified IV; PriceOnly -> price_only; Interval -> iv_interval;
None -> no family-route root (Search's answer).

## 8. Open

- The bracket confirm spends 2 confirm solves where the envelope cannot certify the far
  end (no envelope in scope, or a loose one: q < r calls); the old secant confirm spent 1.
  K1's fused vega (landed after this lane's base) is not consumed: the bracket rule needs
  no vega bound. A validated vega lower bound from it could replace the second solve.
- `family_domain_bound_usd` is loose; a per-point bound at the grid (quote_slice.cpp,
  out of scope here) would let the family grid serve again on wide boxes.
- The public API comments on `deam_family_q` / `deam_family_native`
  (`include/atx/vol/api/fitting/quote_slice.hpp`) still describe the measured bound and
  "every root is still confirmed"; the header was out of this lane's scope.
