# Adjusted-return consumers: independent source review

One Codex review pass. Result: **0 Critical, 1 Important**. The Important finding
is accepted by the implementer for a bounded fix in the original focused
validation package. Under the program ruling, accept its fix report; this does
not require a second review pass.

## Reviewed scope and evidence

Reviewed the stable uncommitted task diff against HEAD
`ea657622f09f8ad2258d254e4d5d2df1d7afa901`, after reading
`adjusted-return-consumers-brief.md` and `ar6-price-source-report.md`. The
implementer confirmed ownership and stability of the five source files and five
test files listed below. No implementation commit existed at review time.

Only source and owned test inspection was performed. No tests, warehouse access,
source-data scans, code changes or external LLM calls were performed. Production
price loading retained the sole heavy-workload slot. The focused test execution
remains the implementer's responsibility after that slot is released.

The reviewed working-tree byte SHA256 values bind this report to the pre-fix
content:

| Path relative to `atx-db/` | SHA256 |
| --- | --- |
| `src/atx_db/equity_price_metrics.py` | `e43b969cc6e21c4d26582c3e3915cbdc51b2fde119d755ea22f5d4d799f38f71` |
| `src/atx_db/signal_eval.py` | `ac057a93e74e1cc234cabff43ac9c41c1ecf0945d218872d2d2588246f0b9f38` |
| `src/atx_db/filing_reaction.py` | `274aaefd456371b381ba34fd8849d5ab3aa4d4dfa76d4e3c2903d4b22590a434` |
| `src/atx_db/fundamental_momentum.py` | `1dfc5796a916e242b648043f4717224625d5cc785198c1b602b258676d36053a` |
| `src/atx_db/twin_momentum.py` | `2bd3439461ac1161bd453b1a17772f42a65516ed0223ed69d5e1f79a2c7771f5` |
| `tests/test_adjusted_return_consumers.py` | `02c6ca68020a01cc7e12f07f0d2f513a0e1040cd34e783454e96122d83582f1f` |
| `tests/test_equity_price_metrics.py` | `26578c57496912ed0c15528e8ba523fe0066e65c18873dce4778f6f51f7e63e5` |
| `tests/test_signal_eval.py` | `f948375e0d252261cca80a500e631c8159affe1af81fcf4d834721ac67279c8c` |
| `tests/test_fundamental_momentum.py` | `2a7aa46fc2d8242240b9fe1a5f3ccdc78c3b0eae84d6754ddd71c83e01f9a270` |
| `tests/test_twin_momentum.py` | `0131dbdeb78018e2bcaa3a3f9092f2a7d6504f01259f65cb65b4b897de902845` |

## Important I1: cohort outputs retain clocks earlier than consumed peer inputs

Locations in the reviewed content:

- `fundamental_momentum.py:280` copies each individual
  `decision_available_at` to the output clock. Lines 307-341 then calculate
  cohort ranks, OLS coefficients, winsorization and z-scores without raising
  that clock to the maximum of the cohort's consumed inputs.
- `twin_momentum.py:287` retains the individual clock while lines 302-332
  calculate cohort ranks and standardized output.
- `filing_reaction.py:220` retains each SUE decision clock while lines 232-240
  winsorize and standardize the entire date cohort. The new SQL market-return
  clock correctly covers the price median, but does not cover another cohort
  member's later SUE decision clock.

Concrete failure: three surviving names have the same January 31 `as_of_date`.
A's selected momentum endpoint is revised on February 5, so A's loader output
has `decision_available_at = February 5`. B and C retain January 31 clocks.
The cross-sectional ranks, regression coefficients and standardized values for
B and C consume A's revised momentum, but B and C are still published as
available January 31. A PIT consumer can therefore observe information five
days early. The analogous filing-reaction case uses a later SUE decision clock
for one member of the standardization cohort.

This clock behavior is inherited in the unchanged compute bodies. It is raised
here because the owned loaders deliberately retain later input availability,
and the task explicitly requires output availability to cover every consumed
input. Correct adjusted prices alone do not make these output clocks safe.

Bounded correction: after input validation and cohort eligibility filtering,
raise every participating row's `available_at` to the maximum input clock of
its `as_of_date` cohort before rank/regression/standardization. Preserve factor
IDs, formulas, cohort membership and replacement semantics. Verify each of the
three pure compute paths with a tiny cohort containing one later member and
assert that no peer output has an earlier clock. The implementer acknowledged
this finding and will include these cases in the original focused test run.

## Other reviewed behavior

No further Critical or Important defects were identified in the assigned
adjusted-price changes:

- Positive finite canonical adjusted prices are used directly. The obsolete
  cumulative split-factor reconstruction is removed; missing adjusted prices
  have no raw-price or neutral-factor fallback.
- Adjusted open uses the same row's adjusted/raw close scale. Dollar turnover
  remains raw close times raw volume.
- Missing adjusted observations remain in the date sequence. Production forward
  targets cannot skip a bad endpoint, and the latest invalid revision cannot
  resurrect an older valid price in the corrected adapter.
- Momentum chooses its latest reference session before endpoint validation;
  filing reaction chooses its first post-filing session before return validity.
- Explicit empty scopes cannot accidentally request all bars in the changed
  loaders. The standalone raw-price helper and frozen compatibility definitions
  are unchanged.

The implementation report explicitly discloses the pre-existing full-panel
pandas paths and the retained observed-bar horizon convention. This source
review makes no full-universe memory, source-vintage, historical identity,
split-event, survivorship or economic-adjustment certification claim.
