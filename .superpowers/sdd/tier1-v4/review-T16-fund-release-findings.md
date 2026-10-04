# Task16 fund release initial independent review

Reviewer: /root/t16_release_foundation; implementation9dfaeb6c; 2026-10-04.
Spec compliance and task quality: Needs fixes. No Critical findings.

Important I1, fund_batch_release.py659-669: recovery fully admits the intended
current target AND the retained previous generation. A legitimate source/code
refresh makes the previous frozen snapshot stale, blocking recovery of a valid
new publication. Offline reproduction admitted new and refused old on its stale
snapshot; calls were [new, old] and the journal remained.

Required correction: fully admit the intended current target; verify the retained
previous pointer's rooted location and manifest identity without admitting stale
inputs. Actual rollback must still fully admit its target. Cover changed-source
recovery and corruption refusals.

Complete85-unit/144-preparation inventory, source/archive replay, private native
v11 adapters, exact numeric/key/clock replay and original gates were inspected.
Actual source qualification, gates, memory/runtime and downstream integration
remain unmeasured: 0 actual PASS /9 pending or failed.

Reviewer ran eleven focused offline cases,16 deselected,10.28s, explicit -n0
-o addopts='' and OPENBLAS_NUM_THREADS=1, plus a temporary-manifest reproduction.
No actual source/hash/data/HTTP jobs, edits or remaining live jobs.

Root dispatched the original author for fix round1/5; no producer edits or
relaxation of the Task5 cap.
