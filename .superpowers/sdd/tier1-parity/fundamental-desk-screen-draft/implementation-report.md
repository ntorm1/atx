# Implementation report

Prepared an apply-ready patch containing only the research SELECT and its
evidence document. `git apply --check` passed against `C:/atx`. No production
file was edited, and no warehouse, test, import, or network runtime was used
while the guarded companyfacts writer was active.

Static source contracts checked: migrations 0302, 0304, 0315, and 0316;
`universe_us_listed.py`; `market_daily.py`; `_derived_pit.py`;
`security_master.py`; the derived-metric seed CSV; and the security/listing
schemas. The query uses the actual `us_listed_v1` ID and declared source names.
It treats `valid_to` on membership as inclusive (the universe accessor's
contract) and identifier/listing intervals as exclusive at the endpoint.

The most relevant source limitation is that the universe builder's `has_cik`
comes from the current `securities.entity_id`. The readout retains that field
as lineage but does not use it to certify a historical issuer join. A dated,
visible `security_identifier_history` CIK interval is required. This may expose
a production identity coverage gap; its size is unmeasured until execution.
The EPS owner check reconstructs the publisher's selected standardized input
state across all CIKs before comparing it with the dated identifier. It
isolates missing input evidence and selected-owner mismatches; the derived
`inputs_hash` remains opaque and is not presented as a direct source-row ID.
The ticker is similarly shown only if a dated, visible exchange listing
qualifies. Missing accounting states, uncertified legacy states, stale periods,
market valuation gaps, and identity gaps receive separate counts.

The readout ranks nullable metric states before validity tests, including
clocked invalidations. `event_reconstructed` history is required, and the
quarterly EPS metric requires quarterly origin. Selected states are also
checked against fingerprints computed from frozen seed literals. It selects
one observed market
session at or before the calendar snapshot and reports its actual date.
The aggregate always exists even for an empty universe; detail output is
bounded to 999 names across three normal dispositions and an overlapping
membership quarantine. The 200-day accounting-age rule uses the actual
selected fiscal operand end as an operational freshness gate. Market fields
have independent missingness counts. The positive-growth filter is
descriptive only.

Remaining root runtime work: apply the patch after the writer releases the
database, execute the frozen SQL once, record its hash and aggregate plus
preview evidence, and repair any *observed* generic production blocker. Do
not describe absent quantities as measured. A short addendum link to
`atx-db/docs/QUANT_DESK_ACCEPTANCE.md` can be added by the root integration
owner after the readout is executed.
