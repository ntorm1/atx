# SM1: bound source resume connection lifetime

User instruction, 2026-09-24: launch memory requirements may be lowered when
code becomes more efficient or incremental. This task changes connection
lifetime and opening behavior; it does not change any memory guard threshold.

Research question: can the complete retained CompanyFacts history be verified
and resumed on this machine without retaining the memory of earlier proof
phases or opening replacement connections outside their recorded budget?

## Owned scope

- `src/atx_db/_companyfacts_resume.py`: consume each proof result completely,
  then recycle persistent configured connections between inventory, fact
  fingerprints, point fingerprints, and unresolved-summary recovery.
- `src/atx_db/connection.py`: `close` read-only handling and `reopen` only;
  apply recorded analytical caps at `duckdb.connect`, before database opening
  or WAL recovery can allocate under defaults. Preserve public signatures and
  unconfigured behavior.
- Focused new connection/resume tests plus necessary lifecycle expectation
  adjustments in `tests/test_companyfacts_archive_repair.py`.

## Invariants and acceptance

Every archive identity, allowlist, run lineage, receipt timing, issuer count,
source ownership, row fingerprint, multiplicity, and point-symbol check remains
unchanged. Both complete source surfaces are scanned, including other owners
sharing a retained security. No source universe or historical scope is reduced.
Only issuer/security aggregates survive in Python across phases.

Focused verification must exercise actual file-backed close/reopen, preserve
committed facts and filing availability, reject shared-security corruption,
and prove the memory cap is present at connect time. A read-only run must never
request CHECKPOINT or modify the file. The normal loader's separately reported
`connection_reopens` stays distinct from `resume_proof_connection_reopens`.

Root owns live qualification: execute `verify_companyfacts_resume` within the
actual full production resume against the complete current warehouse and
terminal lineage with bounded resources; record verified issuer/row totals,
elapsed time, native peak memory, and host headroom. A separate read-only full
proof is unnecessary duplicate work. Old archive13 evidence is a dated baseline,
not a paired comparison. Proof success alone does not qualify the source
replacement write budget; the actual write phase must also supply evidence.

No Python process, tests, heavy query, or live mutation may run from this agent
until root assigns the single workload slot. One independent review by root.

## Acceptance checkpoint

Root's serialized validation passed six new SM1 checks within a combined
nine-test batch (0.595832825GiB native peak), scoped Ruff, and 17 integration
checks (0.840614319GiB peak). Integration includes existing connection behavior,
both affected archive lifecycle paths, every contradictory proof case, real
0326 source-index bootstrap, and the public API snapshot. Root's independent
review is clean; no repeat review is required. Production full-resume capacity
and actual source completion remain separate, pending runtime evidence.
