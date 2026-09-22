# EPS schema HEAD contract review

Scope: live diff from `42883bff` in `migrations/__init__.py` and `bodies_0320.py`, with the fix result note. Static review only; the root agent owns imports, tests, and warehouse access.

## Findings

Clean. No Critical, Important, or Minor findings in the bounded diff.

The namespace cleanup now includes the three body modules imported by `registry.py`, matching the established cleanup through 0319. Migration 0320 creates the receipt table before registering its exemption, and refreshes the schema contract pin afterward. The exemption names precisely the two absent canonical PIT columns, has a nonempty reason, and matches the existing `pit_exemption` schema and quality-check parser. The receipt writer uses `INSERT OR IGNORE` keyed by a stable `receipt_id`; the table describes source attempts, with no observation-period or revision-chain semantics. Its `available_at` remains physical and nullable for nonaccepted attempts.

`git diff --check 42883bff --` on the two files passed. Runtime behavior, including the bootstrap PIT presence gate and pin assertion, remains for the root agent's guarded run.
