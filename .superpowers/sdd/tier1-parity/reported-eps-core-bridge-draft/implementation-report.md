# Reported EPS core bridge (0321) — implementation report

## Delivered static draft

`integration-final.patch` joins accepted SEC Item 2.02 release EPS to the
existing CompanyFacts owner only when the normalized CIK is valid and maps to
exactly one existing owner.  It does not use a ticker/current-security route or
backdate market identity.  The original raw CompanyFacts loader is untouched.

Release EPS is accepted only for GAAP diluted per-share rows with a labeled
fiscal quarter, a conservative source availability clock supplied by 0320, and
one of these exact duration contracts:

- an explicit three-month header and an 89–93-day derived span;
- an explicit period start plus a 91-day 13-week span; or
- an explicit period start plus a 98-day 14-week span.

No annual-minus-YTD, share construction, basic, adjusted, or
continuing-operations path is introduced.

The standardization candidate set adds a later direct CompanyFacts versus
release comparison per exact owner/CIK/item/period.  A difference above 0.005
creates a nullable `reported_eps_conflict` standardized revision.  Its input
code lineage carries both statement-point identities and accessions, and the
existing standardization-exception relation records the conflict reason.  At a
shared clock, a conflict wins; otherwise a direct CompanyFacts EPS wins a
consistent release.  The nullable state therefore supersedes the previous
valid release instead of reviving it, averaging it, or silently selecting a
winner.

Migration body 0321 drops only the existing `fundamental_standardized.value`
NOT NULL constraint, updates the existing table/field catalog text, and refreshes
the schema-contract pin.  No parallel state table or physical status column is
added.  `_derived_pit.py` already preserves whole nullable states and does not
filter standardized `value` before its filing-event selection, so the derived
quarterly EPS YoY expression reconstructs an unavailable state after the
conflict.  `item_coverage.py` and `provider_coverage.py` now select the latest
standardized revision before requiring a finite usable value, preventing an old
release accession from being counted after its later NULL invalidation.

## Static verification

`git apply --check
C:/atx/.superpowers/sdd/tier1-parity/reported-eps-core-bridge-draft/integration-final.patch`
passed from repository root `C:/atx` (the patch paths are rooted at
`atx-db/`).  SHA-256: `9D4BC4F7A147FAA1D73853008D36EAB8685A7CA1BAAA548169BB230E94F1012B`.

No Python import, test, database operation, or production runtime was run
because archive9 is the active guarded writer.

## Focused test plan after the writer stops

Run `tests/test_reported_eps_core.py` plus the standardization/derived fixture
that loads four normal quarters, observes a valid release EPS and YoY state,
then publishes the later conflicting CompanyFacts EPS.  Assert:

1. the release is visible at its conservative availability clock;
2. the direct consistent EPS wins only on equal clock, while an earlier release
   stays visible before the later direct event;
3. the conflicting event writes `fundamental_standardized.value = NULL` with
   both lineages and a `reported_eps_conflict` exception;
4. `eps_diluted_q_growth_yoy` becomes NULL/unavailable at that event;
5. an ordinary CompanyFacts EPS series remains unchanged;
6. cross-CIK records cannot match, malformed CIKs cannot be lpad-truncated,
   exact 13/14-week spans pass, incorrect spans fail, and repeat refresh cleanup
   leaves no stale state.

The source-0320 tests must also confirm the 46-hour conservative filing clock
before this bridge is executed end to end.

## 2026-09-22 repair after independent review

The current `integration-final.patch` supersedes the SHA and test-plan-only
description above. The migrated `fundamental_fact_revisions.as_of_date` is now
explicit in both arms of `source_facts`; release facts carry the document
quarter's `as_of_date`, and statement mapping uses that date. The CTE no longer
anti-matches a direct fact at the release clock. Both source vintages survive,
and conflict pairing covers a later direct fact, an equal-clock direct fact,
and a later release after a direct fact. Conflict lineage projects both
accessions through `visible_pairs`.

Bridge admission now checks `is_preliminary = true`,
`extraction_confidence = 1.0`, and a matching accepted 0320 receipt
(receipt id, raw CIK, accession, source security id, document SHA, and
availability clock). The owner aggregation is scoped by distinct qualified
release CIKs; the bridge still streams the existing CompanyFacts source
relation through SQL and does not fetch the 47-million-row universe into
Python. Its process-tree peak has **not** been measured under the 2 GiB guard.
Full-scale activation remains conditional on the root's later guarded run.

The replacement `test_reported_eps_core.py` uses the initialized migrated
warehouse fixture and its production tables. It covers a valid release,
late and equal-clock direct conflicts, nullable standardized and derived
states, ordinary direct CompanyFacts stability, both accessions in lineage,
source admission, exact 13/14-week spans, malformed and cross-CIK records,
and repeated refresh cleanup. This draft test has not been executed while
the guarded loader is active. 0321 remains unregistered until source 0320 is
integrated first. The patch also pins the new public module in the API
snapshot.
The resulting `integration-final.patch` SHA-256 is
`F6E6712CB81FC9F25FEFBD80372B3CE9313B8B5D0CD4007254D355671D3D777A`;
`git apply --check` passed from `C:/atx`.

For the otherwise unspecified direct-earlier case, both facts remain in
`source_facts`. At a later release clock, a disagreement creates a conflict;
a consistent release becomes the latest standardization event. This policy
does not infer a quarter end from an 8-K report date: the bridge reads the
document-labeled `press_release_facts.period_end` supplied by 0320.

## Integration dependencies

Root must serialize source 0320 before registering 0321.  The issuer-query
draft owner has been notified and has updated its API catalog nullable contract;
it must continue to rank the selected revision before filtering null values.
