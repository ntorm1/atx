# Task8 scoped fixround1 independent review — 3b896c5f

### Spec Compliance

**I1: PARTIALLY ADDRESSED / NOT FULLY ADDRESSED.** Expected-byte checks for retained snapshot/report/manifest, sealed-generation full native validation, idempotent publication and a journalled two-pointer transaction replace the original unrecoverable finalization path. Interrupted report/manifest creation can now continue without overwriting immutable evidence. Prior pointer restoration precedes source admission for the supported stale-source case. However, the new recovery order still leaves mixed pointers unrecovered when the target manifest changes (I2 below).

**M1: ADDRESSED.** `summarize` now emits a distinct `source_admission` FAIL gate with NULL measurement and pending-independent-acceptance reason. It ignores caller source-quality/PASS assertions; statistical success cannot certify it. The manifest inherits the four-gate inventory.

The changed code preserves original 120/153 axes, economic/native adapters, full public replay, statistical thresholds and R20/R21 evaluation boundaries. No new production/source qualification is claimed. Actual evidence remains 0 PASS / 4 FAIL-unmeasured; source and statistical acceptance cannot be established by this scoped engineering review.

### Strengths

Retained final bytes are compared to independently reconstructed canonical bytes. Sealed restart invokes full native validation. Publication intent captures exact prior pointer bytes and identities, validates the prior manifest/report pair, and ordinarily restores it on exceptions. The focused fixtures cover final-file boundaries, each pointer, stale-source rejection, changed replayed economics and idempotence. The report honestly discloses the initial 80.47s per-file budget violation and subsequent split bounded runs.

### Issues

#### Important I2 — Target manifest rejection occurs before safe prior-pair rollback

`atx-db/src/atx_db/alpha_panel/char_replication.py:137`, `:289`–`:290`.

`load_pending` requires the pending target manifest's current identity to match its intent before returning the authenticated journal. `build` restores the prior pair only after that call succeeds. If a hard stop occurs between pointer writes and the target manifest is subsequently changed/corrupted, retry raises `publication target manifest changed` while leaving `current.json` on the target and the validation pointer on the previous generation. The captured prior pair and its immutable old manifest/report can still be valid; the target's corruption need not prevent restoring those exact prior bytes.

This is a recovery hole in the new transaction path, rather than acceptance of corrupted target evidence. Fail-closed target rejection is correct, but leaving the published pair inconsistent means the rollback requirement remains incomplete.

Fix: separate authentication of the pending intent and independently retained prior pair from validation of the proposed target. Under the writer lock, restore a recognized old/desired mixed pair from authenticated prior bytes before rejecting changed target evidence. Target identity/full native replay must still be mandatory before any target commit. Keep unrelated pointer or journal changes fail-closed.

Evidence: one new narrow synthetic test constructed a valid unqualified prior pair and pending target transaction, emulated an uncatchable interruption after only the target `current.json` write, then appended one byte to the target manifest. `build` rejected its identity, but the current pointer remained target while validation remained prior. No actual sources, native return computation or DuckDB job were involved.

### Validation

Only the new target-corruption rollback repro was run; author and unchanged suites were not rerun. Command from `C:/atx`: `OPENBLAS_NUM_THREADS=1`, `PYTHONPATH=C:/atx/atx-db/src`, `atx-db/.venv/Scripts/python.exe -m pytest <temporary test_corrupt_target.py> -n0 -o addopts='' -p no:cacheprovider -q`. Result **1 passed in 0.58s**, asserting the defective mixed-pointer state. No product edits, commits, subagents, actual source/data/hash/HTTP jobs or live jobs.

### Assessment

Spec: **Needs fixes**. Quality: **Needs fixes**. M1 is closed and the original final-file resume boundaries are corrected, but I1's recovery contract remains incomplete because the new target-validation ordering can prevent rollback of an independently authenticated prior pair.
