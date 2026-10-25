# Constraint reduction: next algorithmic option after the bounded prototype

Research note, 2026-10-24. This does not change the current experiment or authorize a performance claim.

[Laiu and Tits, constraint-reduced MPC](https://arxiv.org/pdf/1810.09243), sections 2.6 and 3, select near-active constraints using a slack threshold that contracts when the residual measure improves. Their method changes the working set within a primal-dual iteration sequence. Its convergence analysis has assumptions including strict feasibility and constraint qualifications; those assumptions do not automatically hold for our boundary-only price faces. The paper primarily benchmarks dense, highly imbalanced problems. Its reported speed ratios are not forecasts for SPY.

[QOCO/QOCOGEN](https://arxiv.org/abs/2503.12658v4) separately motivates custom linear algebra for known sparsity. Our earlier cached LDLT experiment did not produce a useful gain, and the SPY graph changes with the input strikes and reduced row set. Generated fixed-pattern arithmetic therefore remains a later research option, not the next integration step.

## Consequence for the current code

The Stage B wrapper solves bounded subsets from the original target, adds all discovered omitted violations, and validates the full published surface. It preserves the existing solver and provides a clear first test of whether smaller factor graphs help. It does not implement Laiu–Tits or inherit their convergence proof.

If the first complete SPY result spends most of its cost restarting reduced solves or reaches the original full fallback, inspect the retained per-round counts before tuning a threshold. A more fundamental follow-up would preserve the current primal/dual iterate across row additions, initialize only newly added slacks/duals, and recompute the required symbolic structure. That change needs a new numerical contract and failure tests: a previous reduced iterate is not necessarily feasible for added rows, and its multiplier state must not be treated as valid full-problem dual evidence.

The immutable full row set, full scans at publication, original economic admission and an original-target full fallback remain the acceptance boundary. Near-active selection is a prospective method for reducing work, not permission to omit a constraint from final validation. No code or third-party dependency is adopted by this note.

