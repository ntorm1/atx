# SEC filing index retry repair

Patch: `sec-index-retry.patch`  
SHA-256: `FD9B24F274C7EC56F09E54BAD9D802B3ED8EE718E3C2F6D800FEABD11E0DA93C`

The filing index parser now requires a complete `Document Format Files` table with `Document` and `Type` in the expected columns and complete five-cell document rows. A malformed HTTP 200 index raises `ValueError`, which the existing refresh handler records as `fetch_failed` and can retry. A valid table without EX-99 still returns an empty selection and receives the terminal `ex99_document_not_found` outcome. EX-99 selection remains based on the fourth-column filing Type and a flat document link within the accession directory. The nested href conditional was folded for the existing Ruff SIM102 warning.

The draft tests cover a malformed 200 response followed by a successful retry with an accepted fact, a valid no-EX-99 terminal outcome, and direct invalid or incomplete table cases. `git apply --check` passed against live source and tests. Python, tests, database access, and network were not run under the current memory guard.

Live source SHA-256 before patch: `A3ADB1F7137A30B375A81EAFC4AB41091EB8FE89065C10B90DD464C44B0A8F42`  
Live test SHA-256 before patch: `E17FC26DCA9AAF7301CB79CD9A069DBDD9270A09865293C00561486A601597BA`
