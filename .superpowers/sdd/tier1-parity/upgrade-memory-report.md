# UM1 implementation report

Implemented the owned two-row populated upgrade check with a 256MB, one-thread
DuckDB startup configuration and insertion-order retention disabled. The
`DuckDBStore` records matching analytical settings before initialization.

The real 0314 bootstrap still runs in full. After all legacy schema checks,
row insertion, and capture of the original columns, the test explicitly
checkpoints and closes its initial connection. The pending real migrations run
against the persisted database through a second connection with the same
startup bounds. This releases bootstrap-session buffers before the upgrade.

Every original assertion remains: actual 0314 migration maximum and legacy
NOT NULL/index state; original row fields; new provenance defaults; one public
table; nullable value; primary key and duplicate rejection; retired secondary
index; both legacy IDs across latest/first-reported reads; zero qualified
legacy coverage; catalog nullability; schema fingerprints; nullable update
rollback; empty second migration application; initialization reentry and row
preservation. No fixture, migration, production code or registry was edited.

Validation: root ran the single focused acceptance under the serialized 1GiB
process guard. `upgrade-lowmemory-test1-memory.json` records completed/exit0
and native peak job memory 0.8589744567871094GiB. The test log reports one
passed in 154.60 seconds, with a 147.92-second test call. This proves the
256MB real-0314-to-current fixture path passes within the lower process cap;
it does not establish production readiness or authorize lowering headroom
for unrelated workloads.

The experimental launch followed `upgrade-lowmemory-window1`, which measured
120.015 sustained seconds at the explicit 4GiB physical/6GiB commit floors.
Guard preflight and runtime hard stops remained unchanged. Runtime used an
isolated export of committed `7e0bcd02` plus this test change, excluding
concurrent source-path edits. The candidate test SHA256 was
`9c564c5439310b01e28af1651bc3c62d8e0cee5f993dee5509414dbc24848dfd`;
the exact schema fingerprint remained `19b2c3a07814e600498c7d79`. This evidence applies to the
smaller, recycled fixture session; production source policy is separate.

Independent review: root's one static review is clean in
`upgrade-memory-review.md`; all original assertions remain. Root also ran
scoped Ruff successfully on `tests/test_derived_pit_revisions.py`.
Implementer ran `git diff --check` successfully and launched no runtime work.
Root accepted the task and authorized an explicit-path commit of the owned
test, brief and report only.
