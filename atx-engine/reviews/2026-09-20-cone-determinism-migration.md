# Cone determinism checks after the polish correction - 2026-09-20

The iteration-9 broad run reported 179 passes, two historical digest mismatches and one 120-second timeout. The two digest failures shared the same 12-name, no-cone fixture. Both produced `0xcb1ac8b6d01933e0` where the old constant was `0xffed7ec6c177aad2`. This is evidence that an intentionally changed solver changed book bytes, not independent evidence that either book is mathematically correct. No replacement digest has been introduced.

The numerical change is reviewed separately in [the polish review](2026-09-20-qp-polish-review.md): refinement now targets the unregularized KKT system, retains useful auxiliary seed components, and replaces the primal and dual together under explicit acceptance checks. This intentionally changes the old regularization-biased numerical behavior. The previous test comments permitted migration when the box-only algorithm intentionally changed, but copying a newly observed hash would give no independent correctness check.

## Replacement contracts

`RiskCone.ZeroConeMatchesCertifiedBoxOnlyOptimum` replaces `ZeroConeIsByteIdenticalToS84Path`. It preserves the assertion that the assembled problem has zero cones and the bit-for-bit comparison of two repeated solves. It adds an independent dense active-set KKT certificate for the fixed fixture.

`RiskCone.ZeroSectorConeZeroImpactMatchesPlainBookBitwise` replaces `ZeroSectorConeZeroImpactIsByteIdenticalToS85aPath`. It compares actual plain and explicitly inert-feature solves bit-for-bit, instead of comparing only a plain solve against a historical hash. The inert problem enables all-zero impact coefficients and two zero sector-risk specifications; the existing builder omits nonpositive sector cones. Existing structural P/q checks remain, both augmented problems must have no cones, and both returned books must pass the independent certificate.

These contracts detect a zero-effect feature accidentally changing the computation, nondeterminism across repeated solves, and a stable but economically wrong solution. They do not promise numerical identity with a superseded solver implementation.

## Independent analytical certificate

The fixture has 12 names, `lambda=0.7`, a strictly positive diagonal specific variance, alpha repeating `(-0.2, 0, 0.2, 0.4)`, net zero, gross at most 1.5 and absolute name weights at most 0.4. The oracle explicitly constructs the small dense Hessian `P=1.4*(X F X' + diag(D))`; it does not call the production augmentation, ADMM or polish code.

The hypothesized nonzero coordinates are `{0,3,4,7,8,11}`, with signs `{-1,+1,-1,+1,-1,+1}`. A separate 8-by-8 system solves the six weights and the net/gross multipliers:

```text
[ P_SS   1   sign ] [ w_S ]   [ -q_S ]
[ 1'     0     0  ] [  nu ] = [   0  ]
[ sign'  0     0  ] [ tau ]   [  1.5 ]
```

The test verifies that the dense Hessian is positive definite and that this system is nonsingular. It then checks the assumed nonzero signs, strict slack in the name bounds, positive gross multiplier, net and gross equalities, gross complementarity, active stationarity, and the inactive L1 subgradient inequalities. These checks establish the hypothesized active set rather than assuming it from the native result. Positive definiteness makes the certified KKT point the unique optimum.

A read-only NumPy calculation independently gave active weights approximately `(-0.2197756068, 0.2688902458, -0.25, 0.25, -0.2802243932, 0.2311097542)`, `nu=0.0869172741`, `tau=0.2529503644`, and inactive stationarity magnitude at most 0.1140167627, below `tau`. The test computes the oracle from fixture inputs rather than pinning these rounded numbers. The independent dense oracle retains a `1e-10` residual tolerance.

The first migrated 18-case run passed 16 and failed these two new comparisons: the maximum native coordinate difference was `1.7434725885223656e-10`, gross excess was `4.643425643990895e-10`, and gross complementarity error was `1.1745562089355026e-10`. The initial test incorrectly applied the direct oracle's `1e-10` tolerance to a native caller configured for `1e-6` row feasibility and 400 fixed iterations. Those failures and their initial log/XML are retained as evidence; they are not a reason to alter production constraints or iteration budgets.

The corrected test separates native accuracy from oracle accuracy. Native coordinate comparison, net and name checks use the caller's unchanged `cfg.feas_tol`. The coordinate comparison is an additional test requirement at the declared original-unit scale, not a claim that primal row feasibility alone bounds optimizer error. Gross comparison allows `(M+1)*cfg.feas_tol`, derived from M absolute-value split inequalities and their sum row. Gross complementarity scales this bound by the independently solved gross multiplier. Stationarity comparison propagates coordinate tolerance through `||P||_inf`, adding the strict oracle residual tolerance. Plain-versus-inert and repeat-solve comparisons remain exact byte comparisons.

A plausible explanation for retaining the slightly overshooting native point is the preserved objective guard: a point feasible within `1e-6` can have a roughly `tau * 4.64e-10 = 1.17e-10` objective advantage from that overshoot, larger than the guard's `1e-12` allowance. The exact optimum can then be rejected as an objective increase, retaining ADMM. This is an inference from the code and observed errors; these tests do not expose whether polish was accepted, and no measured acceptance status is claimed.

The test-only correction passed whitespace validation. The subsequent focused cone/polish rerun belongs to the root validation record; this note does not claim its outcome before completion. Production solver sources, configurations and iteration counts were unchanged by this migration.

## Dense-oracle timeout scope

`RiskQpAugment.MatchesDenseOracleAcrossBattery` explicitly sets `polish=false` and `ruiz_passes=0`. The changed polish code is not executed. Its 11 cases perform 15,700 outer iterations in each of two solvers. The frozen reference uses 80 PCG steps plus an initial residual application per outer iteration: 1,256,000 PCG steps and 1,271,700 dense KKT applications.

Using the actual reference dimensions, the two dense A/A-transpose products inside those applications alone involve about 142.45 billion scalar coefficient products, excluding the factor-risk products and outer-loop products. The configured build is Debug with `/Ob0 /Od /RTC1`. This quantifies a substantial unchanged workload; it is not a wall-time prediction or a completed differential validation. The test's own comments identify the dense reference as its dominant cost.

The root recorded that this battery had already exceeded five minutes in an earlier interrupted run. The iteration-9 120-second timeout remains incomplete coverage. No iterations, fixture sizes, differential tolerances or reference algorithm were weakened to make it pass, and no further long battery run was requested in this cycle. The bounded polish validation excludes this unfinished test explicitly rather than reporting a full suite pass.
