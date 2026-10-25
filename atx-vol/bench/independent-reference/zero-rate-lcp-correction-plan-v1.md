# Prospective linear-solve control after the second Greek pilot

The immutable second pilot at a5c8fe79 has six zero-rate-put solver refusals.
This note proposes a separate offline experiment; it changes no production
pricing, pilot decision, solver tolerance or Greek uncertainty rule.

The original dirk_l/time-coarse request was observed without changing its
arithmetic. Its 60th obstacle stage enters a two-state policy cycle: three rows
toggle and successive values differ by about 2.25e-12. The residual alternates
between approximately 1.1461e-10 and 1.5328e-10, above the fixed 1e-10 tolerance.
Re-evaluating the offending binary64 inputs at 100 decimal digits also fails.
Thus this case is not explained solely by rounding in the final residual sum.

Three fixed controls on the single captured stage are retained in
`zero-rate-lcp-diagnostic-v1/linear-controls.json` and its NPZ arrays. Original
iteration still fails. Power-of-two row scaling converges after two linear
solves, with 100-digit residual 9.4469503779e-11. One residual-correction solve
after each policy solve converges after four linear solves, with 100-digit
residual 1.3967060172e-12. All 8,191 rows were checked for each final vector.
The original failed control's recorded floating residual belongs to the last
tested iterate; its saved vector is the following, also-failing iterate. The
two converged controls test and save the same iterate. These controls are not
full PDE solves or evidence that any Greek reference is now resolved.

LAPACK's [tridiagonal solver documentation](https://netlib.org/lapack/explore-html/d6/ddb/group__gtsv_ga6b06a11b8e2dced10bc9cd3c20a09484.html)
describes Gaussian elimination with partial pivoting. Its
[Working Note 165](https://www.netlib.org/lapack/lawnspdf/lawn165.pdf) motivates
residual correction and discusses stronger extra-precision error estimates.
The proposed binary64 correction below does not inherit that paper's error
bounds. The observed improvement is local evidence for testing this alternative.

Before implementation, freeze and peer-review the following fixed experiment:

1. Keep the existing obstacle solver mode and all old receipts unchanged. Add
   an explicit offline mode that computes exactly one residual-correction solve
   after every selected-policy solve, in binary64. No row scaling is combined
   with it, and no tolerance or selected-policy equation changes.
2. Use the original unscaled complementarity residual for admission and the
   original n+2 policy-iteration bound. The new bound is two linear solves per
   policy iteration. Reject nonfinite original or corrected values explicitly.
3. First test the captured failed stage, a small independently solved obstacle
   problem, exercised and continued rows, invalid arrays and resource accounting.
   Record the actual final vector and its residual together; a stale previous
   residual must never be presented as the saved vector's residual.
4. After source review, run all twelve original zero-rate requests: both DIRK
   schemes and all six v2 grids, including the six previously successful rows.
   Preserve the closed-form price/Delta/Gamma checks, all refusals, and complete
   space/time/domain denominators. Do not replace failed PDE values analytically.
5. Only a separate prospectively declared complete 144-request experiment may
   support new reference decisions. Neither this one-stage diagnostic nor the
   twelve zero-rate controls can alter the v1/v2 pilots or qualify the other
   contracts. Full-bundle Greek accuracy and production step changes remain open.

This next control is pending implementation and peer review. No new accuracy
or timing result is claimed by this plan.
