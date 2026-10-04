# R23: Bounded financial runtime integration after the Task16 repair cap

All85 native extraction replay receipts are independently admitted. Full native
source freeze for r17_20261004_002 passed before and after scratch cleanup, with
the same complete85/144/source snapshot. The first financial batch then failed
at fundamentals.stream_issuers' ordered join: DuckDB exhausted its internal128MB
allowance (121.8MiB/122MiB), while the guarded process peaked307,679,232bytes,
below0.8GiB. This was exit1, not host pressure or a process-cap failure.

The root's isolated one-batch diagnostic passed with256MB DuckDB, one thread,
the same native adapter/kernel and before/after full snapshot checks. Guarded
runtime70.1s, peak557,416,448bytes, output2,919,100bytes. It created no candidate
admission or publication. Evidence: logs/t16-fund-r19-pilot-b0000-v1.* and
logs/t16-fund-runtime-diagnostic-v1.*. Neither changing a budget nor completing
this diagnostic establishes any financial/source-economic gate.

Task16 has exhausted its five repair rounds. Do not edit its frozen producer,
fund_batch_release.py, common.py, extraction/source contract or code inventory;
do not dispatch a sixth local repair or silently rebind existing85 receipts.

Ruling: Task17's release/consumer integration must provide an explicit bounded
runtime adapter using the existing connection-taking native primitives. Use a
file-backed256MB DuckDB connection, one thread, under the unchanged<=1GiB guard
(initial actual runs0.8GiB) and40GiB free-disk floor. Pin the adapter's actual
code and a separate immutable execution contract outside the exact native
generation inventory; require caller-admitted expected identities and recheck
them before/after work. Resource configuration must be visible in provenance.

No monkeypatching/cloning/replacing connection globals, function code, native
contracts or validators to conceal an altered runtime. The adapter may call
the native functions that already accept a connection. It must retain the full
original ordering and obligations: source freeze/admission, snapshot equality,
new-only attempts, complete unit replay and typed validation, all85/144 inputs,
before/after rechecks, full merge/evidence/native manifests, public-generation
validation, immutable publication/recovery and original financial gates. Reuse
the original native helpers and pointer logic; no abbreviated alternate
validator, trusting output existence, legacy import or omitted prior-unit replay.

Preserve the failed pilot's attempt and diagnostic evidence. A fresh native
attempt may use generation002 only after unchanged snapshot identity has been
validated. Do not move diagnostic output into an admitted unit. Every required
financial batch must execute and independently replay under its native protocol.
Consumer validation/recovery must have the same explicit reviewed runtime;
the old128MB CLI is not silently declared operationally accepted.

Independent review and focused equivalence/interruption/stale-runtime fixtures
are prerequisites for an actual adapter run. Task17's full catalog/stagelake/
lake-verify/parity obligations remain required, and its repair counter starts
with that whole task, not a new per-component allowance. Task16 remains5/5.

Cost if wrong: the adapter adds an orchestration surface and duplicate sequencing
that must stay pinned to the frozen native protocol; source/financial evidence
must be regenerated if semantics change. It grants no source-quality, coverage,
statistical or production PASS. All original release gates and R21 remain.
