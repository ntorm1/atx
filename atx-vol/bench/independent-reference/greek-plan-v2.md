# Second independent Delta/Gamma experiment: prospective declaration

The original216-result pilot and all its unresolved decisions are immutable.
The numerical evidence in `../../docs/independent-greek-pilot-20261024.md`
identifies CN-specific Gamma instability. This follow-up selects the two DIRK
schemes as candidate references; it does not retrospectively requalify the CN
pilot. Both DIRK variants share the spatial discretization and LCP implementation,
so their agreement alone is not an independent error bound.

Use all twelve unchanged IDs in `../american-greeks/refinement-v1.json`, both
`dirk_l` and `dirk_third`, and these six fixed requests:

| Label | nx | nt | Domain multiplier |
|---|---:|---:|---:|
| space-coarse | 4097 | 512 | 1 |
| center | 8193 | 512 | 1 |
| space-fine | 16385 | 512 | 1 |
| time-coarse | 8193 | 256 | 1 |
| time-fine | 8193 | 1024 | 1 |
| domain-wide | 16385 | 512 | 2 |

Exactly144 requests; no new grid is selected after seeing a result. The same
spot-centered domain, sampled payoff, exact stage LCP tolerance and resource
limits apply. No European shortcut replaces a refused American solve. The
separate domain-wide request retains center spacing. All703 unselected contracts
and six other cold fields stay explicit.

Apply the previous decision rule to all12 results per contract: identical
supplied regime, both three-level drift ratios <=0.5 with the same cancellation
allowance, midrange of the two center results, and the same additive maximum
space/time/domain uncertainty plus half the center disagreement. Preserve the
same two shocks,0.0001 USD/share component cap and25% reference reserve. Any
failed solve, missing field, inconsistent regime, failed ratio or insufficient
reserve remains unresolved. No step-window or candidate-error minimization.

As an additional independent check, attach Black-Scholes price/Delta/Gamma for
g000-call-03 (non-dividend call with nonnegative rate),
zero_rate_closed_form-put-03 (r=q=0), and negative_rate_european-put-03
(r<0,q>0). These constant-GBM contracts have no early-exercise premium: the
discounted convex payoff is a submartingale under their respective carry signs.
Their exact derivative must lie within the proposed empirical interval before
that field can be called resolved. Do not use these values to change a grid,
solver stopping rule, extrapolation or reference center. Retain any failed
coverage as a diagnosis of the empirical decision itself.

After reference decisions are frozen, repeat every original48-entry native FD
comparison for both fields and shocks. Keep all2,304 outcomes, the6,912 other
field/shock comparisons uncomputed and any candidate refusal. This experiment
does not change a production Greek default or establish full-bundle accuracy.
