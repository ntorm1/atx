# Companyfacts raw-CIK throughput repair

Implementation prepared in the existing `feat/tier1-parity` shared tree on 2026-09-20. Assigned source changes are limited to `atx-db/src/atx_db/fundamentals.py` and new `atx-db/tests/test_companyfacts_cik_spellings.py`. No point deletion, schema, migration, registry, activation, or job changes were made. The existing `a5004bf5` issuer-key transaction repair is preserved.

## Behavior and boundedness

The loader previously applied a regexp/trim/BIGINT normalization predicate twice to raw history for every issuer: once when staging its old keys and once when deleting its old facts. `_companyfacts_cik_spellings` now inventories distinct raw CIK strings once per load, with the same SQL regexp/trim and `try_cast` rules. Python receives only valid distinct spellings and their numeric CIKs. Padded, unpadded, space-padded and extra-zero spellings remain exact bound strings; signed, decimal, non-ASCII-digit, junk, NULL and overflowing values do not become accepted aliases.

The load creates this local dictionary immediately before the first successful parsed/resolved member's replacement. Both raw-fact statements use the same parameterized `cik IN (...)` predicate. An explicit empty tuple uses `FALSE`; the default `None` keeps the previous numeric predicate for independent callers. After the replacement commits, the loader records a newly inserted canonical spelling. It retains old aliases as harmless supersets. Source failures never replace facts, SQL failures remain fatal and do not advance the cache, and every new load builds a fresh inventory. The cache relies on this existing serial writer path; it is not persisted or shared across loads.

Old-key staging, point deletion, raw deletion, candidate cleanup, both inserts and temporary-table cleanup still share the existing transaction. Ticker legacy-ID and candidate predicates retain independent numeric matching, since their spellings can be absent from raw facts. The point DELETE and its exact identity/key semantics are unchanged. Empty successful replacements still remove old evidence, while failed source members preserve it.

Memory scales with distinct raw CIK spelling count, not raw fact count. The controller reports 6,533 distinct existing raw CIKs against roughly 31.6 million facts. This is a cardinality bound, not an unconditional fixed-memory guarantee. The SQL inventory can itself require a scan; replacing per-issuer normalization with direct filters permits storage pruning but does not prove that production row groups will prune. The existing progress option enables one inventory log with issuer count, spelling count and elapsed seconds. No speedup or operator profile has been measured by this agent.

The existing unresolved-candidate accumulation is unchanged: each member's frame has already been reduced to one row per CIK before it is appended. Its accumulated row count scales with issuer count, with per-frame object overhead and a final concat copy. The paired brief records this separate possible follow-up without claiming an archive-wide fact-row leak.

## Tests and validation status

The new file defines eight parametrized test cases covering:

- Cached/default equivalence across mixed numeric spellings and excluded values, repeated empty replacement, duplicate fact spellings, and ticker/candidate spellings absent from raw history.
- Explicit empty inventory versus an independent fresh numeric lookup after mutation.
- Rollback after either raw-fact or point insertion, all three mutated surfaces restored, no temporary-table leak, and successful retry.
- Once-per-load inventory creation, canonical spelling advancement, and a fresh inventory when an existing dataset object loads again after an external spelling change.
- A failed insert leaving the load cache unchanged, then a fresh successful retry.
- A malformed source member preserving prior raw facts/points without starting inventory creation.

Static validation completed: Ruff passed for the two assigned source/test files, and scoped `git diff --check` passed. One initial Ruff complaint about the intentional Arabic-Indic digit literal was resolved by spelling it as `\u0661` in the test. No Python imports, tests, database connections, runtime probes, or live jobs were run by this implementation agent.

The root controller ran `test_companyfacts_cik_spellings.py`, `test_companyfacts_issuer_cleanup.py`, `test_companyfacts_zip.py`, `test_fundamentals_spine_link.py`, and `test_companyfacts_resilience.py`: **54 cases passed**, exit code 0. The process guard recorded **0.6972579956 GiB** peak native memory. Receipts are `companyfacts-cik-spellings-tests.log`, `companyfacts-cik-spellings-tests-memory.json`, and `companyfacts-cik-spellings-tests.err` in this report's directory. These results include the existing issuer-cleanup fixture's separate 64 MB query-memory limit; they do not establish full-archive throughput.

Fresh independent static review (`companyfacts-throughput-review.md`) found **no Critical or Important findings**, with no implementation correction requested. Root-controlled guarded production measurement remains pending. The agent did not stop or modify archive2. The controller reports stopping its owned process at 19:47:59 after 146 loaded CIKs/987,977 inserted rows, 19 empty outcomes and one source error, retaining successful writes. The source error is a separate two-byte `{}` member for CIK 3521; this raw-CIK optimization does not change source interpretation. The reviewed change is ready for the controller's production retry, with the required commit trailer `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
