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
wide-carry chain the family grid now serves no price (the key is inert there). Fix round 1
(review-K3 mn-7): a node's box term is `2 z (max(r, 0) K + max(-q, 0) S e^{max(-q, 0) t})`,
t = z^2 - a negative carry's `e^{-qt} > 1` was dropped before - and the sensitivity term
takes `e^{-rt} <= e^{max(-r, 0) t}` and `|r K| + |q| xmax`; each node's term falls with a
put's q below 0 and rises above it, so the q box's ends still carry the worst. Pinned by
`AmericanFamilyError.DomainBoundDominatesThePointBoundOnANegativeCarryBox` (point bound <=
domain bound over a q < 0 put box, every node unverified included). Non-negative carry is
bit-identical.

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
| Confirmed | ConfirmAndFamily | C at the root, the family at the other end (1 solve; 2 for a far root) |
| Confirmed | ConfirmBothEnds | C at both ends (2 solves; 3 for a far root or an open root side) |
| PriceOnly | LowVega | slope W <= 2 eps_C: C cannot certify any bracket of width W; Search serves |
| Interval | BudgetExhausted | a certified bracket wider than W; Search serves |
| None | BudgetExhausted / None | no bracket within 3 confirm solves; Search serves |
| Confirmed | NewtonContract | the established keys-OFF confirm (Newton distance on C), every non-native route |
| Skipped | LegacyBandGate / LegacyNever | the band gate / policy Never on a request WITHOUT a (sigma, q) family |

Under `deam_family_q` the band gate never decides a skip: `DeAmIvView::invert_premium`
turns policy Band into Always for a quote whose side has no family (the B-family arm's
non-family quotes). Measured why, on SPY at 3fb9d6da: 41 of that arm's 748 band-gated roots
sat more than 1e-7 from the bisected accurate root (up to 9.5e-7). The Screen Band arm
without `family_q` keeps the gate, counted as `LegacyBandGate`. Pinned (fix round 1,
review-K3 MJ-1) by `QuoteSliceVolSpace.FamilyQBandArmConfirmsEveryRootOfASideWithoutAFamily`:
a negative-carry chain builds the put family only; with the rule reverted 91 of its call
roots are `LegacyBandGate`, with it 0 and every call root `Confirmed`.

W = min(tol, 1e-7) (1 - 2^-20). Placement of the Skipped enclosure (`family_enclosure`): the
ends x - W/2 and x + W/2, the upper end stepped toward the lower one by an ulp or two until
the COMPUTED width meets b - a <= W (lane K5, section 9). Placement (`confirm_bracket`): C
at the native root x; if C certifies x's side, the other end within W of x (the family at
x +- W first, else C past the chord's Newton point); for a far root (Newton step > W / 4)
both ends at the Newton point +- 0.45 W (family first, else C); if C leaves x's side open
(|C(x) - price| <= eps_C), both ends at x +- W / 2 (family first, else C). No step length and
no secant slope is a bound: a local chord only places an end and the served point. A request
that carried a (sigma, q) family never reaches the band gate (`band_gate_holds` runs only when `gate->qfamily == nullptr`),
so no domain of the family route falls back to a band-based criterion; the audit reads
`LegacyBandGate` = 0 on every family-route arm.

MJ1 (`AmericanIvPremium.FamilyNativeAcceptanceDoesNotRestOnANonLocalSecant`): a remembered
far pair forces the native iterate to accept with a partner 0.3 vol away (56 of 56 roots);
every one is accepted through a certified bracket <= 1e-7 and lies within 7.8e-10 of the
independently bisected accurate root.

The bound term of the enclosure (fix round 1, review-K3 mn-5; `family_enclosure` is declared
in `american_iv_premium.hpp` for this):
`AmericanIvPremium.FamilyEnclosureRejectsAnEndWhoseEnvelopeBoundLeavesItOpen` puts the
price 0.2 W x slope from one end and 0.8 W x slope from the other; a scaled envelope whose
bound lies between the two gaps is rejected by the near end alone, at the lower end and in
the mirror at the upper end (dropping either end's bound from the rule fails it);
`AmericanIvPremium.AnInflatedEnvelopeBoundSkipsNoConfirmOnEitherRoute` scopes a
1e6-inflated envelope and both routes skip nothing.

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

- The bracket confirm spends 2-3 confirm solves where the envelope cannot certify an end
  (no envelope in scope, or a loose one: q < r calls); the old secant confirm spent 1-2.
  K1's fused vega (landed after this lane's base) is not consumed: the bracket rule needs
  no vega bound. A validated vega lower bound from it could replace the second solve.
- `family_domain_bound_usd` is loose; a per-point bound at the grid (quote_slice.cpp,
  out of scope here) would let the family grid serve again on wide boxes.
- (Closed in fix round 1, review-K3 mn-3: the public comments on `deam_family_q`,
  `deam_family_native` and the Family grid kernel in
  `include/atx/vol/api/fitting/quote_slice.hpp` now state this acceptance.)

## 9. Refinement (lane K5, r6): the enclosure's width as computed

The question (master plan section 7, "Can confirmations be skipped safely often enough?") was
whether the envelope had to be refined or split so that more roots skip the Fast-AL confirm.
Measured first, on the K4 native route at SPY (a temporary instrumented build at 746154d6,
never committed): of 28,117 family-native roots, the validated bound certified BOTH ends of
the enclosure on 25,925 (calls 12,511 of 13,849, puts 13,414 of 14,268), yet only 8,739 were
skipped. The other 17,186 were refused by the width test: the ends were placed as
`a = x - W/2`, `b = x + W/2`, and in floating point `b - a` exceeds `W` by an ulp of x
exactly when frac(W / (2 ulp(x))) > 1/2 - the binade fixes how the half-width rounds (both
ends in x's binade). At `W = acceptance_width(1e-7)`, on the inverse's range [0.005, 5], that
is EVERY x in [0.005, 2^-7), [2^-6, 2^-2) = [0.0156, 0.25), [1, 2) and [4, 5], and no x in
[2^-7, 2^-6), [0.25, 1) or [2, 4) (fix round 1, review-K5 m1: the earlier "every x below
0.25" was false on [2^-7, 2^-6) and omitted [4, 8)). Those roots paid a confirm solve (and a
family price at x +- W). The envelope was not the binding constraint.

Change (`family_enclosure`, `american_iv.cpp`): `b` steps toward `a`, at most
`kEnclosureWidthSteps` ulps (one inside a binade, two where `b` sits on a power of two), until
the COMPUTED bracket meets the unchanged rule `b - a <= W`. `b - a` is exact there
(Sterbenz: W << a), so a skip still means: a bracket no wider than W < 1e-7 whose two ends
the validated envelope certifies, the served point clamped into it. Neither the envelope
(bound, refinement, domain, class) nor the 1e-7 criterion changed. Pinned by
`AmericanIvPremium.FamilyEnclosureMeetsTheWidthWhereRoundingWidensTheEnds` (64 rounding-widened
brackets whose ends the bound certifies: 0 enclosed before, 64 after, each within W) and
`AmericanIvPremium.FamilyEnclosureCertifiesTheEndsItServes` (fix round 1, review-K5 m2: both
ends are priced and certified AFTER the step, at the ends served; at prices on the edge of what
the unmoved end x + W/2 certifies, 18 of 64 points are not certified by the moved end, so a
certificate taken before the step serves a bracket its upper end does not certify). Mutations
(K5 report, fix round 1): removing the step, or removing it and loosening the rule to
`W (1 + 1e-9)`, fails the first; certifying the upper end at x + W/2 while serving the moved
end fails the second; K3's per-end bound pins still catch a dropped lower / upper / both bound.

Measured on the K4 route (policy `graded-v1-famq-grid-native-route.json`):

| SPY native roots | before (746154d6) | after (K5) |
| --- | ---: | ---: |
| skip rate (Skipped / native roots) | 31.1 % (8,739 / 28,117) | 92.2 % (25,925 / 28,117) |
| calls / puts | 18.2 % / 43.6 % | 90.3 % / 94.0 % |
| Fast-AL confirm solves | 22,823 | 5,637 |
| family prices | 156,018 | 126,941 |
| root audit (served roots) | 0 violations, max skipped err 6.25e-9 | 0 violations, max skipped err 6.25e-9 |

The bench cohorts (same policy; skip rate over family-native roots, calls / puts):

| cohort | before | after |
| --- | ---: | ---: |
| cohort5 | 35.4 % (22.5 / 47.5) | 91.9 % (90.5 / 93.2) |
| dev200 | 69.7 % (90.0 / 56.4) | 74.1 % (96.5 / 59.3) |
| dev200b | 53.1 % (62.3 / 45.0) | 53.2 % (62.3 / 45.2) |

On dev200 / dev200b most roots are NOT family-native (no family on the side: the leg's
Newton-contract confirms, 70,878 and 57,864 roots, are unchanged), and the family-native put
roots still confirmed there are envelope-limited (`ConfirmBothEnds`: the bound exceeds W x
slope at both ends), not width-limited.

Per side, the bound itself (SPY, at the enclosure's ends): puts - every one of the 30 put
envelopes verifies all 48 premium quadrature nodes; end bounds p50 2.8e-9, p90 1.3e-6, p99
2.6e-5 USD; the 854 put roots still confirmed fail on the verified sensitivity term (worse-end
bound / gap median 2.1). Calls - none of the 29 call envelopes verifies every node (18 to 48 of
48 unverified: the q < r call's boundary tail does not decay geometrically); an unverified node
takes its trivial box bound g(xmax), which is ~0 where the call's early-exercise premium is
negligible, so 90 % of calls still enclose; the 1,338 call roots still confirmed are all
dominated by that box term. These two populations are what a refined or split envelope (the
section-7 candidates (a) / (c)) could still convert: at most 2,192 roots, ~2.5 k confirm solves.

Cost (SPY, B0 clock, 10 cold trials, CPU 6, development rows on a shared host): the inverse
stage 2,660 -> 1,451 ms. A temporary sub-clock (K5 report) puts the saving on the confirm
bracket (1,664 -> 311 ms; Fast-AL confirm solves ~57 us each) and shows what is left of the
acceptance: the enclosure itself, two bounded family prices per root, ~690 ms (~25 us per
root) - `family_price_bounded`'s per-node bound terms, the next lever on this stage.
