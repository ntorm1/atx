# R20: characteristic replication conventions before measurement

Ruling: interpret Task8's NYSE-breakpoint shorthand as the NYSE size screen
and weight cap used by the specified JKP characteristic portfolios. Reuse
Task7's frozen portfolio construction. Do not introduce a new NYSE-only
characteristic reference population.

The authors' [Global Factor Data Documentation, section2, printed page7](https://jkpfactors.s3.amazonaws.com/documents/Documentation.pdf)
defines characteristic terciles using all nonmicro stocks within each country.
The NYSE20 size percentile defines nonmicro eligibility for those breakpoints;
micro stocks subsequently receive portfolio assignments from the same cutoffs.
Weights use the NYSE80 cap. The source also specifies five names per leg and
sixty monthly observations. Its separate asset-pricing-factor change history
does not replace this explicit characteristic-portfolio recipe.

Root inspected the primary documentation on2026-10-04 through the web tool;
no benchmark return payload was read or measured. The original gap-analysis
requires replication against JKP usa/vw_cap characteristic factors. This
source-based clarification preserves that target, Task7's already frozen
construction, all numeric gates and the full original inventory. It overrides
the proposed NYSE-only characteristic split in the Task8 static plan.
Cost if wrong: a construction mismatch will remain visible in replication
failure and must be investigated without choosing conventions from returns.

Task8 must freeze these separate reference populations explicitly in its
executable config and cover a fixture where NYSE-only characteristic cutoffs
would differ. Do not change Task7, Task6 or Task16 frozen code.

Native benchmark semantics remain independent prerequisites. JKP's admitted
native return column is already direction-signed; do not sign it twice.
OSAP's native percent return conversion and its source-documented portfolio
weighting must be preserved explicitly. A missing native weighting column
does not license assigning vw_cap. Use independently reviewed authoritative
metadata to admit the actual source convention and full overlap; if Task7's
mapping interface assumes an unsupported convention, implement Task8's own
exact native mapping adapter rather than inventing compatibility. This does
not waive correlation/sign thresholds or omit any missing matched member.

All120 requested holding months2013-2022, minimum60 pairs, complete pre2023
windows, independent source/public replay, and original153 outcome inventory
remain mandatory. Root must still pin the exact native metadata/benchmark
artifacts before any actual return measurement. Documentation review alone
does not establish source-file identity or a production PASS.
