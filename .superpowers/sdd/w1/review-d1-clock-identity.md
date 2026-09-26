# Independent D1 initial source review

Reviewed source `a2f3f373c4387340f39c23aab4a6f73785a94283` and correction
`59ca094a8624162c35635ca65ebdaeac46675370` in pool3. Reviewer owns no D1 code.
Verdict for this initial slice: **source approved after the correction; C++
qualification pending**. No build, new runtime, production payload read, warehouse
mutation or historical coverage measurement was performed by this review.

## Resolved blockers

1. Initial Python resolution, export audit alignment and C++ alignment allowed
   availability equal to the decision/session key. The new dated path requires
   both identity and filing availability strictly earlier than the decision.
   Correction uses `<`, `bisect_right` and `upper_bound`, respectively. Explicit
   legacy static identity retains its original lower-bound arithmetic. The new
   fixtures independently cover equality of each clock.
2. A correctly hashed ZIP member named for CIK A could contain Company Facts for
   CIK B, which the exporter previously labeled with A's linked owner. Strict
   export now canonicalizes and verifies the body CIK before projecting facts.
   A synthetic mislabeled-member fixture verifies refusal before publication.

Owner logs inspected: `pool-3/build-equity/d1-clock-identity-core-tests.log`
reports 8/8 in 0.019 s; `d1-clock-identity-export-tests.log` reports 5/5 in
0.091 s. Both end in `OK`; the report records native exit 0. Four C++ fixtures
remain uncompiled. No independent runtime rerun is claimed.

## Identity/expiry composition

Source inspection covers the pure evidence adapters, original vendor-line IDs,
canonical CIK/symbols, explicit proof and adjudication clocks, hashed immutable
artifact, exporter, explicit interval-v2 decoder, and actual `PitRecord` aligner.
The dated path uses an exclusive validity end, expires old issuers, withholds
conflicting visible owners even if a competitor supplies no accounting facts,
and allows an older fiscal period from the valid successor. Override priority
is explicit. Mixed legacy/dated versions on one security line are rejected.
The source tests exercise the decoder-to-aligner composition; a production
manifest-verifying file-loader is still absent and is disclosed as remaining
work. Company Facts availability remains the existing modeled policy, not a
verified D3 accounting clock.

## Automatic whole-interval liveness

The original rule requires the right observation's effective date at or after
`valid_to - 1 calendar day`, and delays link availability until both proofs are
public. It is not mathematically empty: for `[Jan 2, Jan 10)`, a Jan 9 proof
public at 18:00 permits a Jan 9 decision after 18:00. Under ordinary chronology,
however, it offers at most that final-day opportunity, and no opportunity when
the confirming proof arrives after expiry. It cannot substantiate a broadly
usable production PIT master.

Root separately authorized a versioned prospective corroboration rule. That
implementation has not been reviewed by this artifact. Its required properties
are no future-proof backfill, two already-public independent observations,
strict decision availability, source-bound known endpoints or open intervals,
and clocked effective conflict/expiry markers that survive export and alignment.
This does not waive the original D1 coverage acceptance.

## Remaining original lane scope

Acquisition/vintage receipts, active loader/schema migration, the production
manifest-verifying loader seam, the original 82 recycled-ticker fixture,
2013–2019 operating-company and survivor-stratum coverage, and D3 publication
clock evidence remain open. No D1 completion or tradeable-alpha claim follows
from this source approval.
