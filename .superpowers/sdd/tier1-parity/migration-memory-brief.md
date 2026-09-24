# MM1: governed migration checkpoint budget

Question: can the pinned warehouse complete its pending governed migrations within the existing 1.5 GiB process cap while retaining backup, lock, checksum, verification and rollback guarantees?

The production 0323–0326 attempt at `bdbc7d9d` completed apply/verification but failed during its final checkpoint at the 1.5 GiB cap. Its DuckDB buffer budget was 1 GB; recovery's lock-cleanup connection had no startup budget. Root confirmed restoration before assigning this repair.

Owned files: `src/atx_db/migration_admin.py`, `tests/test_migration_memory_budget.py`, and this brief/report. No migration body, registry, schema contract, activation or job integration changes.

Implement a 512 MB, one-thread startup configuration with insertion-order preservation disabled and an absolute spill directory for initial apply, reopen and restore cleanup. Retain checkpoint ordering and all governance operations. Acceptance uses tiny persistent databases with actual migration transactions, schema verification, checksum verification, backup copying and restoration; inspect connection settings before the first session configuration statement. Root owns runtime checks and the live migration retry.

Production acceptance: the real pending migration sequence must complete under the unchanged 1.5 GiB process cap, with source tables preserved and post-apply catalog/ledger verification. Small fixture success alone does not prove that outcome. Preserve all backups and `stash@{0}`.
