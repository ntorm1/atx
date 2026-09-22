# AP1 fundamentals publication capacity review

2026-09-20. One independent **static** pass of the stable 11-path draft against
live `atx-db` source. Read the AP1 brief, activation-memory audit I1, implementer
report, `fundamentals-publication-draft/integration.patch`, and the proposed
full-source artifacts. Graph tools were unavailable; discovery used static text
reads and `rg`. All eight existing-file SHA-256 values in the baseline manifest
still matched live source at review time.

**Findings: 0 Critical / 0 Important / 0 Minor.** No corrective source change is
requested from this pass. The draft is suitable for root integration and focused
verification; this is not a runtime or production-capacity approval.

Only this review file was written. No patch application, live source edit, commit,
Python/import, test/collection, lint/type check, database/probe, network/install,
checkout/reset/restore/clean/stash, or subagent was used. Archive4 retained the
sole runtime slot.

## Calculation, scope, and physical contracts

References below are to proposed files under
`fundamentals-publication-draft/atx-db/`, unless marked live.

The adapter changes retain the existing SQL projections, revision/PIT windows,
provenance fields, source predicates, options, and return queries. Specifically:

- Revisions and statement points retain their selected-concept predicates.
  Statement points still run the REIT addition only for a full refresh; every
  REIT read, existence check, and insert now addresses the same complete stage.
- Periods retain the full projection; statement TTM retains the metric-scoped
  scan and replacement. No reconstruction arithmetic was changed.
- Calendar map retains issuer-wide FYR inference and the 2,048-row Python
  batches; calendar TTM retains its source scope and calculation. The calendar
  temporary tables and registered batch frames are released before recycling.
- Standardized and exception projections retain their shared source/symbol
  replacement predicate and their existing materialization-limit behavior.

`_fundamental_publication.py:207-228` assembles complete replacement stages in
one transaction. The retained-row predicate is `(replace_where) IS NOT TRUE`,
which preserves the old DELETE predicate's NULL behavior. Retained rows copy
all physical values, including clocks. Stage defaults and NOT NULL declarations
come from live `DESCRIBE`; new-row defaults still share the calculation
transaction. Sorting then copies these evaluated values rather than reevaluating
defaults during each shadow insert.

The public replacement is not a CTAS table. `_build_shadow` copies the live
catalog DDL at `:108-121`, retaining physical column order, appended migrated
columns, defaults, nullability, and declared PK/UNIQUE/CHECK constraints. The
existing validator compares column and PK/UNIQUE contracts; CHECK enforcement
comes from the cloned DDL. It does not independently compare CHECK expressions.
No defect in that construction was found for the eight current target schemas.
Public table comments are restored inside the publication transaction.

## Bounded construction and atomic publication

`_fundamental_publication.py:94-167` rejects duplicate/null paging keys before
pagination, counts the complete stage, and performs one ordered disk-stage
replacement. The prefix intervals are contiguous, with open first/last ends;
they do not exclude non-hash keys. The strict previous-key bound and inclusive
page upper bound cover each unique key once. Each successful indexed insert
contains at most 50,000 rows even when one prefix is skewed. Count agreement is
checked before publication.

For configured persistent stores, recycling occurs before indexed insertion,
every four completed inserts, and after each shadow. Live `connection.py` makes
`close()` checkpoint and `reopen()` replay the recorded analytical settings.
The source therefore bounds successful incremental shadow insertion between
reopens at 200,000 rows. It does not establish the resulting peak memory, sort
spill cost, physical pruning effectiveness, or disk headroom at warehouse scale.

The canonical outputs are untouched during stage/shadow construction.
`_bulk_publication.py:84-120` validates all coupled shadows and executes the
completion callback, both swaps, and preceding-table drops in one transaction.
It also attempts rollback when COMMIT raises. Standardization supplies its
completed-build update as that callback at
`_standardization_set_based.py:1003-1037`; the existing failed-build handler runs
after rollback. This preserves the intended joint boundary for standardized
rows, exceptions, and completed ledger status. No per-issuer visibility was
introduced.

Ownership checks precede stale-artifact cleanup. Only marked private stages,
ordered tables, and shadows are eligible for cleanup; reserved caller tables,
views, or an occupied previous-table name cause refusal. The canonical names
are not cleanup targets. Invalidated-connection recovery remains outside this
helper; persistent owned leftovers can be handled by a later explicit retry.

The existing `publish_validated_shadow` signature and complete body were
compared as normalized text with live source and are unchanged. FP1's single-table
API is preserved.

## Session eligibility and migration scope

The same eligibility predicate governs session refusal and recycling: a real
persistent file plus recorded memory and thread settings. In-memory and
unconfigured callers keep their connection and session settings. Configured
persistent writer entry points reject preexisting caller temporary tables or
views before build work. Owned concept filters survive stage assembly and are
released before reopen. Standardization materializes both output stages before
dropping its temporary inputs; its repeated cleanup is qualified to `temp.main`,
avoiding deletion of a persistent table hidden by a temporary name.

0318 enumerates exactly the audit's 27 optional index names, checks table
ownership and UNIQUE/PRIMARY flags, and changes no required key. The 13 current
bootstrap index declarations are removed; historical migration bodies are
untouched. Runtime publication refuses residual secondary indexes rather than
silently dropping them.

The registry/facade artifacts are based on 0316. Root must merge only the owned
0318 lines after FP1/0317 and update the combined module/schema pins. This review
does not treat the unapplied draft ordering as a successfully migrated schema.

## Verification limits and root handoff

The new test file statically contains 25 intended parameterized cases. Its
file-backed helper fixture forces small pages/reopens and checks retained
rows/clocks, default/null/key behavior, CHECK rejection, old/live view readers,
caller-state refusal, retry cleanup, and coupled rollback. The COMMIT case is an
injected exception with an open transaction, not a real engine COMMIT/OOM
receipt. The standardized adapter tests exercise failure before the second
shadow and after the completed-ledger update, plus successful scoped retention.

The all-eight pipeline test checks four revenue quarters, exact 1,000 TTM values,
an unmapped exception, and repeat-build payload consistency. It is not a direct
old-implementation/new-implementation oracle. The report correctly retains the
existing scoped, PIT, calendar, REIT, and standardization fixtures as semantic
oracles; its explicitly named test functions were found in live source.

Root should run the report's focused selectors and combined schema/module/
migration checks only after writer terminal and integration. No tiny test has
been run by this reviewer. Actual writer/production memory remains unmeasured;
the fixed-budget full-universe run, indexed COMMIT behavior, disk demand, and
complete activation remain separate pending evidence. Unrelated P1/AF1 arithmetic
and the rest of the branch were not re-reviewed.
