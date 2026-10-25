# Complete SPY row census

The single approved census of source `6187ab68ace1572f1ce1b1194329d36f9fab2aef`
completed, and its independent reader reconstructed every stored constraint and
native initial slack on the first attempt. A separate actual-API audit of the
same executable passed all thirteen gates under `raw-spread-price-v1` and
`economic-floor-identification-v2`. No reduced solve or timing experiment ran.

| Quantity | Complete problem | Initially violated rows plus all bounds |
|---|---:|---:|
| Free price variables | 12,866 | 12,866 |
| Retained constraints | 54,104 | 32,199 |
| Box barriers | 25,732 | 25,732 |
| Stored lower-triangle matrix entries | 56,010 | 25,718 |
| Off-diagonal graph edges | 43,144 | 12,852 |
| Connected components, including isolated vertices | 225 | 5,023 |
| Largest connected component | 12,210 | 1,789 |

Exactly 6,467 original rows have initial slack below the unchanged `-1e-10`
threshold; 4,200 rows are within `+/-1e-10` of zero. Selection retains every bound,
including bounds satisfied at the initial point. The graph is built from stored
indices, including any merged zero coefficient, just as the native matrix
constructor does. These counts describe a prospective first working set, not
the constraints sufficient for the final solution. Every original row must be
checked after each successful reduced solve.

The full row census has 12,306 convexity, 52 front-slope, 59 back-slope,
768 lower-parity, 1,674 upper-parity and 13,513 calendar rows, plus the bounds.
All 33 slices / 6,433 call-put nodes are retained. Raw midpoint and selected
feasible-box midpoint differ on 3,508 coordinates; the selected target must not
be replaced by the raw midpoint when comparing algorithms. The existing solve
reported 36 sweeps. Its production caller supplies no private projection profile,
so the capture honestly records `profile: null`.

The independent scan of all final **published, clamped** prices finds no row
below `-1e-10`; the minimum normalized slack is `-7.442226674158974e-11`.
The selected-target half-squared displacement is `325.7721705544781` in the
native price coordinates. This describes this feasible result; the solver's
feasibility stop does not certify that this is the exact metric projection.

The separate actual-API proof serves all 12,866 eligible rows and all 33 valid
slices. It contains 12,851 rows in their original boxes (99.88341365%), with
12,850 identified IVs and 16 truthful PriceOnly values. Cold Reference residual
is at most `$0.0005012966094284366/share`, within each original-spread budget.
Floor semantics, shape, parity, calendar, metadata and actual query/prediction
joins pass. Strict `1e-7` IV accuracy remains diagnostic and is not claimed.
Neither this price check nor the census establishes Greek accuracy.

The binary SHA is
`3c7353ac54bf4f26a7e47488a951b52b8333c0e641a972a834649775ac317ff5`.
Input and canonical LF policy retain the frozen SHA values
`b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e` and
`781ddc09f7f26cfbfeb6c32edae1e3591b725a8f8d8d5b85d18eeda00b23f34a`.
Prediction hash `e086fd069a81c2c9eb84bb99e25e8a59e5d78fca8b02b0d51556668eff354d5f`
matches the earlier ordinary output as a diagnostic. Future economically valid
solutions need not reproduce that hash or the old fitted floor targets.

Evidence lives under `acceptance/joint-row-census-v1` in the r7 quote benchmark
directory. `full-spy-v1` holds the capture, native report, exact command, complete
summary and predictions. `full-api-v1` holds the separate same-binary proof;
`full-api-verification-v1.json` contains the thirteen gate decisions. The
component receipt is immutable; `outcome-receipt.json` binds the additional
observations and the exact economic verifier source used for them.

The edge reduction and much smaller connected component justify testing bounded
constraint generation with the existing solver. They do not establish reduced
factor fill, fewer iterations or a latency improvement. The next experiment
retains all variables and original bounds, adds violated rows deterministically,
and falls back transactionally to the original full problem. Its declared caps
and admission remain those in the separately reviewed Stage B plan.
