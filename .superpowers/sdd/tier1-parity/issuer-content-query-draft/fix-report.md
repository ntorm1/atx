# Issuer-content query repair report

## Completed repairs

The draft-only issuer surface now proves the full visible raw CIK set for each
discovered source owner at `content_as_of`.  A derived row is eligible only
when its owner belongs exclusively to the normalized requested CIK.  Shared
owners are excluded, and the response carries the machine-readable
`ambiguous_owner_cik_collision` status, `issuer_owner_ciks`,
`derived_issuer_owner_ids`, and `excluded_derived_owner_ids`.

CIK-bearing statement, standardized, TTM, ratio, and share reads retain their
independent exact normalized-CIK predicate.  Input and stored CIKs must have
one to ten digits; longer values raise an error and cannot be truncated by
`lpad` into another issuer.

Ticker lookup still leaves the strict security resolver untouched.  Its
separate issuer lookup reports every co-visible directory candidate, never
selects the first duplicate candidate, and always carries
`historical_security_qualified: false` with an explicit reason.  Empty and
unresolved responses use the same issuer metadata envelope.

The issuer catalog now retains immutable statement, standardized, and TTM
lineage fields.  Its standardized value contract explicitly permits NULL for a
visible reported-EPS conflict from migration 0321.  Derived selection ranks
the state before its period filter and does not filter NULL values; this is
compatible with 0321's conflict-invalidated EPS state and introduces no source
priority rule for other items.

`first_reported` and `latest` use matching directions for shares, including
their availability/load and identifier tie breakers.  No broad NULL-first rule
was introduced.

## Restored focused fixtures

`test_issuer_content_query.py` is restored in the draft.  It covers a reused
owner that has raw CIK A and B, verifies that B's derived value cannot reach an
A query, verifies the later NULL derived state wins, exercises exact-CIK
statement filtering plus content-clock and first-reported selection, checks
shares vintage direction, checks repeated directory candidates remain
unqualified, and rejects an eleven-digit CIK.

## Static verification only

No Python, imports, tests, warehouse, network, or database commands were run
while the loader was active.  The integration owner should apply the final
draft patch and run only the focused issuer selector after the writer is
terminal.  `integration-final.patch` applies cleanly in a static
`git apply --check --whitespace=error` validation.  SHA-256:
`c2edf0faadde4e990ab3bfb5a01ca83e0d61c3627971fc43cc4b24e54d426829`.
