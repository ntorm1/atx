# Saved LCP failures remain failures in exact arithmetic

The [fixed twelve-request correction control](independent-lcp-control-20261024.md)
retains four refused systems. A separate read-only audit recomputed **all 65,532
rows** of their saved final vectors at 100 decimal digits and with exact rational
arithmetic on the original binary64 inputs. All four still fail the unchanged
`1e-10` tolerance. Rounding in the binary64 residual evaluation alone does not
explain these refusals. No PDE request, policy update or linear solve ran during
this audit; no saved vector, tolerance or historical decision changed.

For each interior row, the audit reconstructs
`continuation = diagonal*x + lower*x_left + upper*x_right - rhs`,
`gap = x-payoff`, and `abs(min(continuation,gap))`. Exact rational arithmetic is
the reference for the word **exact** below; Decimal100 is a separate high-precision
cross-check. The largest Decimal100 residual discrepancy from the rational result
over all rows is `1e-97`. Comparisons use the exact value of the original binary64
tolerance, rather than silently changing it to a different decimal value.

| Scheme / grid | Reported binary64 maximum | Exact maximum, rounded for display | Rows above tolerance |
|---|---:|---:|---:|
| dirk_l / space-fine | 1.1284839729341911e-10 | 1.1322257713671181e-10 | 3 |
| dirk_l / domain-wide | 1.3842793578078272e-10 | 1.3810210484068814e-10 | 1 |
| dirk_third / space-fine | 1.2485656952776480e-10 | 1.2522108297740880e-10 | 1 |
| dirk_third / domain-wide | 1.0894041224673856e-10 | 1.0852915522895374e-10 | 1 |

The binary64, Decimal100 and exact failing-row sets agree in every system. There
are no failures caused solely by binary64 residual evaluation, and no exact
failures hidden by it. Every row is retained in compressed JSONL, including exact
numerators and denominators; the table does not reduce the audit denominator.

## What the saved vectors establish

All six failing rows have `x == payoff` exactly and a negative continuation
residual beyond tolerance. Both binary64 and exact comparisons select the
continuation equation there under the existing `gap < continuation` policy rule.
Their zero-based interior indices are `3984,4443,5925`, `2150`, `6221` and `1460`
in table order. The row-detail artifact retains every original product and the
neighboring values/payoffs. The largest dot-product terms are thousands of
dollars, so the residual is cancellation-sensitive; exact reconstruction still
fails.

There are `1011,294,1034,210` other policy comparisons where binary64 and exact
arithmetic disagree. These are near ties: their maximum exact complementarity
residual is at most `2.551e-12`, below the tolerance. Across all rows the largest
absolute error in the binary64 continuation residual is `3.450e-12`. These
policy disagreements therefore do not, by themselves, account for the six
over-tolerance rows.

The original matrices have nonpositive off-diagonals and exact minimum row
diagonal dominance of one. The most negative obstacle gaps are only
`3.695e-13`, `4.548e-13`, `1.422e-13` and `1.564e-13` in magnitude. This localizes
the decisive failures to the returned vectors' continuation equations. It does
not prove that a particular correction method will converge or establish a
Greek error bound.

Only the initial and final checked vectors were saved. **A policy cycle or
stagnation is not established.** Reaching `n+2` updates is not evidence of a
cycle, and the proven two-state cycle in the older stage60 experiment cannot
be transferred to these different systems. The saved evidence excludes a pure
residual-evaluation explanation; it does not distinguish the remaining policy
iteration and selected-linear-solve mechanisms.

## Bounded next investigation

The next useful diagnostic is an observational replay of only these four saved
stages, retaining identical arithmetic, tolerance and equations while recording
policy and checked-vector equality. Any such replay needs a fixed work/storage
cap and must distinguish an actual repeated state from an unmatched hash or
iteration exhaustion. It must not create a new PDE grid or promote an output.

If that evidence identifies a linear-solve/correction limit, a separate
prospective experiment can compare a more accurately evaluated correction
residual while retaining the final original-equation gate. Another blind
iteration-count increase is not justified. Increasing the reference tolerance
would instead need an explicit accumulated stage-error and differentiated-stencil
budget; a small dollar residual alone is not a Gamma accuracy argument. Neither
change is implemented or executed here.

## Frozen evidence

Under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/`:

- `lcp-zero-rate-residual-diagnosis-v1/report.json` records all four inputs, all-row
  output hashes, exact maxima, denominators and limitations. SHA256
  `e4cb7a69b8b36588a5c06efb73fa10532f184acfd771c28f955ab48dcfc0cbdc`.
- `lcp-zero-rate-residual-diagnosis-v1/row-details.json` retains all six failed
  rows, exact summands, neighboring values, policy-comparison scales and matrix
  dominance checks.
- `diagnose_lcp_residuals_v1.py` and `lcp_saved_row_details_v1.py` are the complete
  read-only analysis sources; their logs and all four compressed row exports
  are pinned by `lcp-zero-rate-residual-diagnosis-receipt.json`.

The original run at reviewed source `8f4f8124` and its four NPZ archives remain
unchanged. Its numerical control still has eight computed requests and four
refusals. Delta/Gamma remain unpromoted; this audit changes no production code,
latency result or reference accuracy decision.
