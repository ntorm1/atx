# QP polishing review — 2026-09-20

Worktree: `C:/atx/.worktrees/equity-platform`. Scope: independent mathematical and read-only implementation review of the iteration 9 repair in `include/atx/engine/risk/qp_solver.hpp` and `tests/risk/risk_qp_polish_test.cpp`. **Static review: pass; no actionable blocker found in this repair.** Builds, native replay, and validation receipts belong to the coordinating agent; this review did not execute engine tests or native replay.

Inspected solver SHA-256: `1e06a12311a7637686b622815863eada268af082a23e334a0d7b66e297230922`. Inspected test SHA-256: `c6150b21cebca46eed3f5ce89ed1cfaf82dfb333750af117daebf60b8d404b7a`.

Checkpoint 8 remains frozen at receipt SHA-256 `c417ffdfa6594b94f24712004060a47f848cc0250e17b0f5b5339c03ebc2c5b1`. Its native attempt failed before its first published proposal. The independent checker recorded `no-allocation-evidence`; no completed replay or investment performance exists. This repair addresses numerical correctness, not the source-data gaps or strategy quality.

## Mathematical findings

The prior polish solved a proximal, regularized system, then refined the residual of that same regularized system. Consequently, refinement could remove factorization roundoff while preserving the regularization bias. Its right-hand side also contained `sigma * x_admm`. The intended active-set problem has

```text
K = [ P       A_active' ]       g = [ -q       ]
    [ A_active    0    ]           [ bound    ]

R = K + diag(sigma I, -delta I)
solution_0 = [ x_admm ; selected_y_admm ]
R correction_k = g - K solution_k
solution_(k+1) = solution_k + correction_k
```

The factorization of `R` is reused. It is unnecessary to allocate or factor an unregularized matrix: subtracting the regularization contribution from `R * solution` recovers `K * solution`. One initial correction followed by the configured refinement passes retains the existing total number of linear solves. Existing factor-payload caps can remain unchanged. The correction targets the unregularized system described in [Stellato et al., OSQP, section 4, equations 27–31](https://stanford.edu/~boyd/papers/pdf/osqp.pdf).

The ADMM seed is ATX's initialization choice. A direct regularized solve from `g` can reset unconstrained, zero-curvature auxiliary coordinates to zero, even when their existing values satisfy inactive epigraph inequalities. Starting from ADMM and correcting the unregularized residual retains useful seed information in those free directions. This is not a claim that arbitrary null-space components remain exactly unchanged under every regularization matrix; complete candidate feasibility still decides acceptance.

The old acceptance rule compared a feasible candidate's objective against the objective of a possibly infeasible ADMM point. That comparison can reject a valid repair. For example, minimizing `0.5*x*x` subject to `x >= 1` gives an infeasible `x = 0.99` a lower objective than the exact feasible optimum `x = 1`. Objective nondegradation is meaningful as an additional guard when the incumbent itself is feasible.

Polishing must also update the dual vector. Keeping ADMM duals after replacing the primal vector makes the reported stationarity residual describe a mismatched pair. Lift the solved active multipliers into a full vector initialized to zero. Equalities permit either multiplier sign; lower-active inequalities require nonpositive multipliers, and upper-active inequalities require nonnegative multipliers. A feasible equality-constrained minimizer with incorrect inequality multiplier signs need not solve the original QP.

## Acceptance requirements

The inspected implementation checks the following in original, unscaled units:

- Candidate primal and dual vectors, matrix products, objectives, and residuals are finite.
- Every linear bound and every cone remains feasible within the existing configured tolerance.
- Inequality multiplier signs and the box normal-cone residual `|A*x - clamp(A*x+y, l, u)|` pass; equality multipliers remain unrestricted.
- Candidate primal and stationarity residuals do not deteriorate beyond explicit small numerical floors, with improvement required unless both residuals are already below those floors.
- The objective guard applies when the old ADMM point passes the complete feasibility gate. It does not veto a certified feasible repair solely because the infeasible point had a lower objective.
- Acceptance publishes the coherent primal/dual pair. Ordinary optional numerical rejection retains ADMM; a factor-budget rejection still propagates as an error.

Cone rows are excluded from the active linear system. A candidate with zero cone multipliers is coherent as a solution of the linear relaxation if it also satisfies the complete cone feasibility gate; zero belongs to the normal cone at every feasible point. Copying stale ADMM cone multipliers into that candidate would require a different stationarity equation. The new acceptance comparison includes cone violation in its primal metric.

The residual floors are explicit: `min(cfg.feas_tol, 1e-10)` for primal distance and `1e-10` for stationarity. At least one residual must improve unless both candidate residuals are already within their floors. The separate full feasibility and normal-cone checks retain `cfg.feas_tol`; the floors do not loosen those checks. Resource errors still propagate from the optional polish factorization. The added vectors and cone workspaces are linear scratch, with no second new matrix factorization.

The pinned [OSQP v1.0.0 polishing implementation](https://github.com/osqp/osqp/blob/v1.0.0/src/polish.c) uses an unshifted reduced right-hand side, unregularized refinement residuals, reconstructed duals, and residual-based acceptance. ATX's fixed-count execution, original-unit hard feasibility gate, normal-cone check, and feasible-incumbent objective guard remain its own explicit policy; this review does not claim byte-for-byte OSQP equivalence.

## Verification boundary

The three added fixtures meaningfully exercise binding full-L1 turnover with free gross auxiliaries, a one-step infeasible ADMM equality point replaced by the higher-objective feasible optimum, and rejection/fallback for wrong inequality multiplier signs and cone violations. Compilation and test outcomes must be taken from the coordinating agent's receipts, not inferred from this source inspection.

An existing reporting limitation remains: the public `QpCertificate::prim_res` calculation reports linear-band distance, while this repair's acceptance comparison additionally includes cone violation. Full cone feasibility is still checked before acceptance and return. Do not describe that existing scalar certificate field alone as a complete conic KKT certificate. This does not affect the current equity allocation descriptor, which has no cone constraints.

The independent iteration 9 proposal checker is `build-equity/audits/iteration9_verify_allocation_proposals.py`. It accepts a fresh native attempt through `--root`, defaulting to `C:/atx/data/equity_book_training_2013_qpfix_20260920`. Original context and baseline paths remain fixed. Its APNL decoder is pinned to SHA-256 `9a2052f35d1da6235b0fd0f11ff1222fe7f213a5a64f608cbaffbb9393064ee3` before execution. The frozen iteration 8 checker remains unchanged at SHA-256 `4905a3727af581095f6a3caf9476ff30af1911a15ff9ff2100dffab4dfd14b4c`.

The checker was syntax-inspected at SHA-256 `a8020bc4a20d24472a313cdf73f84cff40c755fbcf7f7cb2dd63509aadeb60c2` and executed only after the new native attempt stopped, as recorded below. It checks recorded proposal economics and observable carry, stops at missing held marks or unrecorded decisions, and cannot establish optimizer global optimality, source correctness, capacity, or investment performance.

## Observed native proposal and independent accounting result

The native attempt at `C:/atx/data/equity_book_training_2013_qpfix_20260920` published one allocation proposal, with `solver_used=true` and `polished=true`, then failed on the required 2013-04-12 close at evaluation period 6, instrument 604, vendor security ID `150340` (archive annotation GNW). It published no completed book/report manifest. The evaluation window, price panels, and missing-mark policy were unchanged.

The independent checker exited 0 and wrote [its immutable proposal receipt](../../build-equity/audits/iteration9-allocation-proposal-verification.json), SHA-256 `1e6ad6eaca57fbe602a40dcbd5141b65ee90d232556dc76448b707ea26835f6a`. Its status is `verified-partial-allocation-evidence`, with `original_replay_failed=true`, `native_constrained_replay_failed=true`, and `completed_performance_result=false`.

The proposal belongs to decision period 0 / execution period 1. All 973 admitted, risk-ready names satisfied the recorded eligibility and original 64-row risk window, context rows 193–256. Decimal-50 reconstruction found full-L1 turnover approximately `0.2`, trade dollars `20,000,000`, fees `10,000`, immediate post-fee gross `0.2000200020002`, maximum name weight `0.00109441991229`, and effectively zero net exposure. Certificate differences were within the declared roundoff allowances; the largest dollar discrepancy was below `9e-8`. All 14 input hashes remained unchanged during verification. The native derived risk digest was retained, not recomputed; the independent raw risk-window digest and adjacent-return readiness were verified.

The checker reconstructed five complete observable intervals with 365-bps ACT/365 short financing and reproduced the same first held-price failure exactly. It stopped before completing period 5→6, did not debit that incomplete interval's borrow, and did not infer later decisions or returns. These are proposal/accounting checks, not a completed performance replay.

The new failure has an important implementability detail: the recorded GNW weight is only `1.051474416021963e-27`. All 973 eligible names are exactly nonzero in the native output, but only 381 exceed absolute weight `1e-12`; the other 592 sum to absolute weight `3.0443545196300967e-25`. These are descriptive measurements, not an applied threshold. The current replay correctly requires marks even for those tiny represented holdings. A future deterministic minimum-trade or zero-support policy must be specified from available decision/execution information and recertified; it cannot be a retrospective edit to avoid this known gap. The archived GNW/MA source gaps remain unresolved.

The iteration 8 checker and receipt were rehashed after verification and retain their previous identities. No engine, implementation, executable, source panel, or frozen checkpoint was changed by this verification.
