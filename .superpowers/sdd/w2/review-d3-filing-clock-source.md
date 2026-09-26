# Root review: qualified filing-clock exporter and decoder

2026-09-26. Reviewed owner `044185e9`, repairs `17fddf8e`/`3f45713b`,
fixtures `6db088f6`/`3f45713b`, and report `6173acc6`. Integrated as
`6fabb093`/`77bbd4ea`/`98eda926`/`6b3818d9`/`06f7ce20`; registration
and explicit private-CPP standard includes are `f0ffc66d`.
Approved for bounded C++ qualification. Ten owning Python checks already pass
at `f0ffc66d` (0.194s native test time); C++ runtime remains pending here.

The new exporter groups and applies facts by issuer/accession and resolved
availability, preserving separate accepted, observed-public and revision clocks.
Clock evidence is sealed and hash-bound to the exact CompanyFacts payload.
An observed current filing cannot qualify inherited modeled clocks or unverified
fact vintages. The explicit modeled-clock override never grants fact-vintage
qualification. Legacy +24h output remains its explicit reproduction rule;
new fallback arithmetic is exactly filed midnight +46h. Evidence remains
source-declared; the decoder/exporter does not independently authenticate SEC
publication or historical fact vintages.

True DEI entity shares have their own versioned output and exact duplicate /
conflict handling. They are neither summed as security classes nor relabeled
as the existing weighted-average diluted share field. Split-basis, class
allocation and price evidence still block any market-cap claim.

Root review found a concrete NaN-tombstone path into Python3.12
statistics.stdev that raises AttributeError, reproduced without market data.
`17fddf8e` retains authoritative conflict qualifiers while providing a finite
calculation view; a later qualified correction can restore the affected cell.
`3f45713b` additionally guards nonfinite derived seasonal differences through a
V4-only option. Existing arithmetic defaults remain unchanged. Negative dated
identity and link clocks are now rejected in the new decoder.

Qualification is conservatively over all retained accounting cells in the
declared2,928-day lookback. It may withhold an otherwise usable field because
an unrelated retained fact is unqualified. Expiry removes numeric cells and
their qualifiers together. Sparse old dependencies become unavailable; no
coverage claim follows. The producer bounds input sizes and output row counts,
but accumulated Python objects are not a measured byte/RSS guarantee.

V4 decoder parses every row, including absent-axis rows, checks exact clock
arithmetic, identity geometry and finite numeric payloads, and commits audit
counts only on success. Default admission withholds modeled clocks, inherited
unqualified clocks and all unqualified vintages. The old V2/V3 reader bodies
are unchanged apart from the leading V4 dispatch. Ordinary alignment still
retains earlier qualified values with their ORIGINAL availability/period
staleness clocks when a later row is withheld. A V4 conflict is not a revocation
of already-published history; this behavior has an explicit fixture.

Synthetic fixtures cover the actual exporter with hash-bound synthetic ZIP and
clock files; identity/context acquisition is mocked and is not qualified anew.
No real payload, warehouse access, export coverage, true-cap construction,
short-interest identity or full D3 acceptance is claimed.
