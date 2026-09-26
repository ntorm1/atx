# Independent source review: E6 CPCV callers

Root reviewed source `63352bc4` and follow-up `2217429c`; imported as `29531902`
and `e4c32dd2`. Fixtures `6b1b5eb0` imported as `9af93612`, report `9eef109d` as
`dfa46dcd`. No new caller runtime qualification has occurred.

Reviewed dispatcher/recipe identity, immutable cache keys and checked errors,
all five factory admission stops, SearchDriver resume tags, fidelity geometry,
linear/GBT and sequence inner/outer split selection, retained path provenance,
active discover settings/fingerprints/schema4/manifests and V1 omission rules.
DateV2's learned spans include the actual endpoint price at d+h using exclusive
d+h+1. This is separate from factory one-period stream geometry; extract_streams
retains panel.dates() periods and fidelity uses ceil(D/stride), matching validation.

The first pass found a workspace gap: row-fold expansion and sparse declared date
axes could allocate independently of the plan ceiling. Follow-up `2217429c`
adds checked used-date-ordinal expansion, cumulative path-metadata bounds and
preflight of dense downstream date arrays. Budget scope explicitly excludes
feature matrices, fitted model state and traces; it is per workspace, not a total
RSS guarantee. This resolves the identified gap within that declared scope.

Root additionally rejects an unknown cache rule before span allocation (otherwise
an invalid enum used the uncapped legacy allocation branch before dispatcher
rejection). The existing DateV2 cache case now covers that preallocation refusal.
Focused CMake wiring adds the new learned TU and existing cache-owning TU.

Source approved with those corrections. Existing default/explicit V1 numerical
paths remain; active V2 identities and checked failures are propagated rather
than silently falling back to a full-data fit. Path metadata does not claim
independent path-PnL scoring. Pending: compiler and focused caller/runtime checks,
including owning legacy and sequence checks; full E6 speed/scale gates stay open.
