# AF1 fix1: applied-tree typing repair

Date: 2026-09-20. Status: actual source fix prepared for root verification.

Root applied AF1 after P1/Core commits and preserved its own Ruff adjustments.
This follow-up changes only `atx-db/src/atx_db/_derived_annual.py` plus this
report. The current applied tree is authoritative; the original isolated draft
and integration patch were not recopied or regenerated. No test, import,
database connection, runtime command, lint/typecheck or commit was run here.
Migration/catalog/bootstrap and other AF1/CF5 source files were not touched.

## Reported errors and correction

Root's `annual-cf5-mypy1.log` reported twelve errors in `_derived_annual.py`:
the two starred-generator `Span` constructions could not be assigned a fixed
arity/type, and the `stdev_q`/`lag`/`cagr` period AST arguments were not narrowed
from `Node` before accessing `.value`.

Both `Span` constructions now name their five fields explicitly: `start`,
`end`, `annual`, `coherent`, and `offset`. This preserves each existing SQL
expression, branch-selection condition, field order and offset. Numeric period
arguments now have an explicit `isinstance(..., Number)` guard before the
unchanged integer conversion and CAGR multiplier.

**The starred `Span` construction was not a runtime argument-count or field-
placement bug on static inspection.** Each generator traversed the fixed
four-element tuple `("start", "end", "annual", "coherent")`, followed by one
offset argument. That supplies the five dataclass fields in their defined
order. Mypy's variable-length `Generator[str, ...]` type did not establish that
four-element shape and could not exclude a string reaching the offset slot.
Explicit named arguments remove that ambiguity. No runtime execution was used
to reach this conclusion.

The period guards reject malformed internal AST calls with `TypeError` rather
than an accidental missing-attribute error. Production formula lowering already
validates literal window arguments before metadata lowering. Supported catalog
formulas therefore retain the same period counts, generated SQL, arithmetic,
annual precedence, lineage, clocks, and resource bounds.

Root's existing import-spacing changes were retained. The root-owned raw regex
change in `test_derived_annual.py` was untouched.

## Verification handoff

Applied helper SHA-256 before this fix:
`50eb122e62b96d3f895ce4c52c0e518bd9f45e1724e3c2cbc13489ff50a1aa56`.

Applied helper SHA-256 after this fix:
`4c81e0c469442716d0b7cc70bad51a3f511750d58475c3346198ca48dfb27dae`.

Root can retry the same four-file strict mypy command, then run AF1's focused
tests in its designated guarded slot. Ruff/typecheck/runtime results remain
pending; this report does not claim that the tool errors or AF1 acceptance
cases have been verified as passing.
