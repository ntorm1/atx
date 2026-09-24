# UM1: bound the populated legacy-upgrade check

The user permits lower launch headroom when code is made more efficient and
incremental. The pending two-row real-0314-to-current upgrade check should not
need a 1GB DuckDB query budget or retain the bootstrap session throughout the
pending migration phase.

Owned files: only
`test_populated_0314_upgrade_preserves_legacy_contract_and_reentry` in
`atx-db/tests/test_derived_pit_revisions.py`, this brief, and the UM1 report.

Use 256MB and one thread before either database connection opens. Disable
insertion-order retention. Record matching analytical settings on the store
so supported reopens retain the cap. After the genuine 0314 bootstrap and
two-row legacy insert, checkpoint and close that session; open the persisted
warehouse under the same startup settings for real pending migrations.

Preserve every original bootstrap, row preservation, nullable/primary-key,
secondary-index, legacy vintage, schema fingerprint and idempotence assertion.
Do not substitute hand-written legacy DDL, omit migrations, or narrow the
production universe. Root serializes the single focused runtime check under a
1GiB process guard and independently reviews this task. The lower database
budget alone is not measured process-headroom evidence.

Root-approved experimental profile: observe at least 4GiB physical and 6GiB
commit headroom for 120 seconds before the isolated test, then use the lower
1GiB process cap. Existing guard preflight floors of 3GiB/3GiB and runtime
hard-stop floors of 1.5GiB physical/3GiB commit remain unchanged. Root exports
committed HEAD plus this test-only change to isolate concurrent production
code work. This is a fixture capacity experiment, not a production source-job
policy change.

Acceptance command (root owns guarded execution):
`pytest tests/test_derived_pit_revisions.py::test_populated_0314_upgrade_preserves_legacy_contract_and_reentry -n0 -q`

Production outcome: remove the unmeasured 1GB fixture budget and bootstrap
session retention from the outstanding real-schema migration check, enabling
an evidence-based lower-memory release prerequisite. Production migration and
full-universe materialization remain separate uncompleted work.
