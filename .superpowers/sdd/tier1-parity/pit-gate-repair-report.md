# PG1 PIT gate repair report

Implemented within the owned test file; no production code or live warehouse
changed. Static inspection tied both failures in `quant-platform-head6.log` to
fixture integration drift:

- The fixture's `reversed_raw` temporary table survived after its input-copy
  purpose ended. Configured persistent test stores now exercise the production
  connection recycler, whose preflight correctly rejects caller-owned temporary
  objects. The fixture now drops its staging table in `finally` before refresh.
  Both refreshes and their different event chunk sizes/run IDs still execute;
  the exact state/lineage equality and all event-aware API assertions remain.
- The populated 0314 test actually applied migrations 0315 through 0325 but
  expected only 0315/0316. It now derives all pending versions from the restored
  current registry, explicitly requires 0315/0316 first, compares the complete
  applied list and checks the resulting migration head. Every existing legacy
  row, provenance, constraint, API vintage, coverage, schema-contract and
  idempotent reentry assertion remains.

No production defect was found from this bounded investigation. Acceptance
remains the complete `tests/test_derived_pit_revisions.py`, including the two
failed cases, under root's unchanged serialized resource guard. Implementer
performed static inspection only. This repair does not qualify source coverage,
reconstructed history, research outputs or a production release.

Root's single independent static review is clean, recorded in
`pit-gate-repair-review.md`. Root's `pit-gate-chunk1.log` records the repaired
chunk-equivalence selector passing in 4.19 seconds against the isolated
`2e0d738f` HEAD export plus the candidate test file, using the completed exact
schema-fingerprint cache. The candidate test SHA-256 is
`6be6adfd02148dd6b3334d343da931e998b234cf07931fcba5063b8773d6e0a8`.
`pit-gate-chunk1-memory.json` records a completed exit 0 with peak job memory
0.707485 GiB under the unchanged 1.5 GiB guard. Full-file scoped Ruff also
passed in `pit-gate-ruff1.log`.

The populated 0314 upgrade selector remains **pending**, because it builds its
own older schema and needs the sustained 6 GiB physical / 8 GiB commit launch
window. The third window terminated without sustained headroom (physical free
memory approximately 4.0-5.7 GiB). No upgrade test was launched in that window.
Retain the unchanged passing checks from the completed exact-source head6
receipt; only the two repaired selectors require rerun, and the chunk selector
is now complete. This reviewed repair is a checkpoint commit, not a closed PIT
prerequisite gate or a full-file runtime pass. Root records the next exact
guarded upgrade command in the current handoff.
