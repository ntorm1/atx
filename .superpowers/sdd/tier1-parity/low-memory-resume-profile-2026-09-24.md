# Bounded efficiency experiment — 2026-09-24

LR2 short ledger-recovery trial: the first Python scalar binding was measured
to import pandas/NumPy and add 548,102,144 private bytes. The recovery script now
uses validated internal scalar SQL literals while retaining every full source
count, receipt outcome, precise ledger predicate, transaction and checkpoint.
Its actual helper contract passed at 0.041279GiB without those imports. Run the
short recovery at 256MB/one thread under a 0.5GiB native cap with the existing
guard's cap-plus-2GiB preflight, unchanged 1.5/3GiB host emergency stops and a
3GiB data-disk floor. Actual full recovery peak is still unqualified until the
live receipt succeeds. This does not lower the sustained requirements for long
source/operator jobs or change the full CompanyFacts 4/6GiB,1.5GiB profile.

Operator-only profile refinement: CC1 rebuilds only the small candidate index,
with 256MB/one-thread connections and a streamed table-specific Parquet backup.
SA1's retained-directory probe uses an 8MiB SQLite cache, streamed names and at
most 11 size-capped payloads. Both now have focused evidence below 0.68GiB; the
live read-only candidate inspection used 0.812GiB. Their next actual production
operator trials may use a 1GiB native process cap after 120seconds sustained
3GiB physical/5GiB commit headroom, with a 3GiB disk floor. Worst-case guarded
allocation leaves 2GiB physical/4GiB commit, above unchanged 1.5/3GiB emergency
stops. Guard preflight still requires cap plus 2GiB. A cap or integrity failure
requires diagnosis. This scoped refinement reflects bounded code and smaller
operator work; full CompanyFacts writes retain 4/6GiB and a 1.5GiB cap. It is
not blanket permission to reduce production-stage resource requirements.

Latest actual outcomes supersede the planned trials below: governed migration
0326b completed at 512MB/one thread and 1.209381104GiB native peak. Schema 326,
constraints and backup hash were verified. Archive17 then passed its complete
retained fact/point proof and candidate processing at 1.223964691GiB peak under
the same 1.5GiB cap. It failed a persisted candidate secondary-index integrity
check at COMMIT, with healthy host headroom; no new raw facts were written.
CC1 index repair is required before fresh archive18, whose predecessor is
dd52e571-5786-42c5-bfaa-d7122b033912. The reduced profile is measured for proof
and migration, but full source writes still require runtime acceptance.

SA1 replaces the eager submissions ZIP directory with a bounded disk-backed
index and streaming main-name traversal, preserving batch50 and history scope.
Its separate retained-directory probe may use a 1GiB process cap following the
same 120second 4/6GiB window. This probe cannot qualify actual normalization or
full source writes. A later 512MB/one-thread, 1.5GiB submissions write trial is
conditional on accepted implementation, retained-directory evidence and the
completed CompanyFacts source prerequisite. Emergency stops remain unchanged.

The user authorizes lower memory requirements when supported by more efficient,
incremental code. This overrides the earlier fixed launch floor only for the
following measured trials; data, scope and release thresholds are unchanged.

UM1's real populated0314 upgrade passed at256MB DuckDB/one thread,1GiB process
cap, peak0.858974457GiB, after120seconds sustained4GiB physical/6GiB commit.
SM1 releases fully consumed source proof phases and applies recorded budgets
before reopening. SI1 removes three verified nonunique source ART indexes,
whose memory maintenance contributed a plausible write bottleneck. Actual
source capacity remains to be demonstrated, not inferred from fixture passes.

Next serialized trials use sustained4GiB physical/6GiB commit for120seconds
and a1.5GiB process-tree cap. A fully consumed connection releases buffers at
proof boundaries and each existing checkpointed issuer interval. In the worst
case, a new1.5GiB allocation leaves2.5GiB physical/4.5GiB commit headroom from
the launch floors. Native guard hard stops remain1.5GiB physical/3GiB commit;
the guard preflight remains process cap plus2GiB. No concurrent heavy workload.

1. Changed-source full schema/reentry plus affected lifecycle/connection and
   contradictory-proof acceptance,1GB fixture cap, one thread. Fresh template
   required because actual source fingerprint changed. Root single reviews
   and isolated tests are clean; integration passed17checks in155.07seconds
   at0.840614319GiB native peak, source-memory-integration1.
2. Governed migration0323..0326, MM1's512MB startup budget/one thread,
   backup-keep100, after all changes are reviewed, accepted and committed.
   Inspect actual schema and source index catalog and preserve every backup.
3. Actual full CompanyFacts archive17 at512MB/one thread, force replacement,
   archive_members, no CIK/symbol restriction, snapshot2026-09-20. Predecessor
   remains513cfbbc-096a-4186-9666-b6cc5170c4ad. Its full proof is the capacity
   measurement; avoid a duplicate full read-only SHA scan beforehand.

All runs use new guard receipts and terminal logs. Stop and diagnose any guard
or DuckDB failure; do not silently raise limits, lower hard stops, skip source
verification or narrow the universe. A terminal failure must be inspected and
ledger recovery completed if needed before the next attempt.

Archive13's earlier512MB proof success is a dated baseline over fewer rows;
its first new write failed at the old indexed physical design. It is not a
same-data A/B comparison. Archive17 must demonstrate verified retained data
and actual new commits before claiming that lower source memory is workable.

Other pipeline stages still need measured capacity profiles. No source,
fundamental, market, quality, coverage, PIT or release gate is waived here.

The first production migration failed at final CHECKPOINT with its old1GB
buffer budget and1.5GiB process cap. Recovery restored the pre-migrate backup;
catalog/checksum/estimated-count equivalence and cleared locks were verified.
Backup SHA25682781626397ee01629d6b843f1d9982052bf52efcbacc16faf6121c3b59d63ca
is retained in migration0326-restore-proof2.json; this is not full source-row
equality proof. The full source resume still requires its complete fingerprints.
MM1 keeps the process cap and governance order, reserves more non-buffer memory
by lowering DuckDB to512MB, and bounds restore cleanup before opening as well.
Four focused success/failure recovery checks passed at0.603GiB peak. Retry uses
fresh activation-migrate0326b artifacts; the original failure evidence remains.
