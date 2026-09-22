# Issuer-content query draft state

Status: complete draft repair; not integrated or runtime-tested.

Owned mirrors are limited to `atx-db/src/atx_db/asof/fundamentals.py`,
`atx-db/src/atx_db/api/catalog.py`, `atx-db/src/atx_db/api/service.py`, the
issuer focused test, and issuer query documentation.  The original
`integration.patch` is preserved unchanged.

The draft fixes the Critical derived-owner CIK collision by excluding any
derived owner whose complete visible raw CIK set at the content cutoff is not
exactly the requested normalized CIK.  It exposes the collision in metadata
and preserves exact-CIK predicates for CIK-bearing tables.  It also repairs
malformed CIK rejection, current-directory candidate ambiguity, shares
first-reported ordering, empty metadata envelopes, and catalog lineage.

The neighboring 0321 EPS bridge makes standardized values nullable for a
reported-EPS conflict.  This draft's issuer catalog exposes that nullable
state and derived reads retain a selected NULL state.  No 0321 source or
migration file was edited here.

Integration remains serialized behind the production loader.  After the
writer is terminal, apply `integration-final.patch` (SHA-256
`c2edf0faadde4e990ab3bfb5a01ca83e0d61c3627971fc43cc4b24e54d426829`), run
`tests/test_issuer_content_query.py` once, and conduct the required
Critical-only review.  The full patch has passed static
`git apply --check --whitespace=error`; no runtime check was performed.
