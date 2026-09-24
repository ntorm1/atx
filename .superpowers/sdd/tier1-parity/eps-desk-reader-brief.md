# ED1: bounded quarterly EPS desk consumer

Question: what are the latest three observed quarterly diluted-EPS YoY states
for an explicitly selected issuer at a fixed accounting cutoff?

Inspection found the IQ2 `WarehouseReadService.issuer_content_range` authority
and the unrelated FQ1 desk screen, but no generic executable EPS desk reader.
ED1 wraps IQ2 without copying publication or fiscal-comparison rules. Frozen
CVX acceptance SQL/results remain untouched. Direct raw recomputation is out
of this task's scope.

Owned files: `atx-db/scripts/read_quarterly_eps_growth.py`,
`atx-db/tests/test_quarterly_eps_desk_reader.py`, an appended consumer section
in `atx-db/docs/CVX_EPS_ACCEPTANCE.md`, and this brief/report pair.

Acceptance: root runs the focused tiny fixtures and one guarded live read of
CIK 0000093410 with cutoff 2026-09-20T22:00:00Z, period range
[2025-10-01,2026-07-01), latest 3. Schema 322 must produce a persisted
`schema_prerequisite_missing` diagnosis and exit 2, naming migration 323's
selected-input columns. Once prerequisites/materialization exist, values
must come only from IQ2 selected-leaf qualification. NULL revisions and all
lineage diagnostics survive the wrapper. The reader must never claim
verified historical vintage or market-security qualification.

Resource contract: one root-authorized runtime slot; 256MB DuckDB, one thread,
2GB maximum spill, immutable read-only transaction; scalar SQL row/byte
preflight before owner/catalog/root payload fetch. Existing memory guard and
sustained-headroom policy remain mandatory. No network or warehouse writes.

Development: Codex implementation, one independent parent review, meaningful
tiny fixtures after implementation, explicit-path task commit after root
evidence. Preserve other sessions and stash@{0}.
