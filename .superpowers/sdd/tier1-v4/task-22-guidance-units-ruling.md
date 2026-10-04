# R22: Guidance comparator units and the primary surprise field

Before any guidance values or predictive results are measured, the Task10 brief's
`(actual - prior midpoint) / price` shorthand needs an explicit unit contract.
EPS is already per share; total issuer revenue is not. Combining both in one
numeric field would silently change its meaning.

The required `guid_surprise` field is the comparable EPS surprise divided by a
qualified price for the same security/share/currency basis. It is dimensionless.
Select the latest strictly earlier, source-qualified guidance for the same exact
fiscal target and accounting/share basis. A compatible actual and independently
proved split/currency/share bridge are mandatory; absence remains NULL with a
reason. Never substitute adjusted for GAAP or basic for diluted values.

Use the last completed exchange-session close strictly before the actual's
original issue clock, with its genuine availability and basis proof. The daily
feature still obeys GC1; the denominator must not use a later reaction price.
Freeze these choices in the Task10 registry/config and test clock equality and
incompatible comparators before an actual run.

Publish qualified EPS and revenue guidance low/high/point and exact target/basis
metadata. `guid_revision` is the current midpoint minus the latest comparable
prior midpoint in the measure's native units; carry the unit/measure explicitly.
Total-revenue guidance must not fill the EPS surprise field. A revenue surprise
normalized by issuer market value would be a distinct formula and is not silently
introduced by this task. Its unavailable surprise is an explicit unit mismatch,
not an observed zero or an omitted guidance release.

The predeclared evaluation candidate `guid_surprise` therefore refers to the EPS
field above. This clarification precedes all actual results and does not use
outcomes to choose candidates, signs, dates or thresholds. Preserve the complete
guidance release population, the >=90% precision / 300 handchecked release gate,
all yearly issuer denominators and all evaluation tests. R21 and pre2023 complete
forward-label/evaluation limits are unchanged.

Cost if wrong: the first release has no separate revenue-surprise predictor.
Revenue source values and revisions remain available, and any later additional
formula requires its own predeclared registry/trial entry before evaluation.
Independent Task10 review must check this interpretation and its implementation.
