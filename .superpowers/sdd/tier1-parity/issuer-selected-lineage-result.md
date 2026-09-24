# IQ2 selected-issuer lineage result

## Final parent verification

The independent review's two Important and two Moderate findings were fixed
on the implementer's report. The root-owned exact 1.5GiB guard then completed
23 focused tests across test_issuer_content_query, test_issuer_selected_lineage
and the existing DL1 suite. Receipt iq2-tests2-memory.json records exit0 and
0.686684GiB peak; no schema bootstrap or live query was run. The first attempt
stopped at collection because the new test used a non-package import; the
implementer corrected it before this passing batch.

Scoped Ruff passed in iq2-ruff2-memory.json, exit0 and0.062138GiB peak. Its
first invocation found only the blank line separating the relative test
import; the parent applied that formatting fix. No functional change followed
the passing test batch. These proofs do not close pending full schema/numeric
checks, migrate the live0322 warehouse, or establish CVX production coverage.

The historical implementation and fix notes below are retained as chronology;
the final verification above supersedes their pending-test wording.

Static implementation ready for independent review and the root-owned guarded
tiny test run. No Python, database, tests, network, commit, or full schema
bootstrap was run by the implementer.

The API and as-of derived readers retain coarse fact-owner discovery and rank
whole visible revisions before period filtering. The root-level
`qualify_issuer_derived_page` helper checks retained selected operands with DL1
at each root event (`decision_cutoff=None`), using canonical publisher hashes
for registered definitions. Numeric output requires full requested-CIK
qualification. Verified NULL invalidations remain NULL; legacy, foreign,
missing, and unverified states receive bounded fixed diagnostics. API candidates
are paged in stable order and counted toward the requested limit only after
qualification. As-of checks wide page bytes in SQL before DataFrame conversion.
Both readers expose scan counts and explicit truncation status.

Changed paths: `atx-db/src/atx_db/derived_lineage.py`,
`atx-db/src/atx_db/api/service.py`,
`atx-db/src/atx_db/asof/fundamentals.py`,
`atx-db/tests/test_issuer_content_query.py`,
`atx-db/tests/test_issuer_selected_lineage.py`, and
`atx-db/docs/ISSUER_CONTENT_QUERY.md`.

Static `git diff --check` passed. Runtime verification remains delegated to the
root's exact 1.5 GiB guarded focused batch and scoped Ruff; independent review
may require fixes before integration.

The independent one-pass review's two Important and two Moderate findings are
addressed. Tiny cases now cover a foreign selected metric dependency, source,
definition and clock tampering, pre-0323 missing refs, aggregate batch splitting,
and 65 rejected candidates before an eligible row. Both test modules scope each
DuckDB connection to 256 MB and one thread. As-of retains column names for an
all-rejected page and probes one SQL-bounded row at the output/scan cap before
marking truncation. Existing owned service, as-of, issuer test, and documentation
files were restored to CRLF; `derived_lineage.py` retains LF. Static
`git -c core.whitespace=cr-at-eol diff --check` passed. Runtime remains pending.

The root's first guarded focused test attempt stopped at collection because the
new test imported its sibling as a top-level module. The import now uses the
`tests` package-relative path; the unused service import was removed. Runtime
test and Ruff results are still pending a guarded rerun.
