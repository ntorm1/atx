# R25: Use the existing enforced resource admission rule for one bounded retry

Two native financial attempts stopped for external host low commit, and the
first small observer stopped too. The subsequent v2 observer completed a full
five-minute window:31points, minimumcommit2.2642GiB, physical1.4105GiB; the
guard's faster sampling observed minimumcommit2.1808GiB. Preserve its recorded
3GiB heuristic result as false. No prior result or observation is relabeled PASS.

Source inspection of run_memory_guarded.py:381-401 establishes the existing
enforced admission rule: effective free commit must cover the requested job cap
plus1GiBreserve, after other jobs' unclaimed allocations, with global slot/FIFO
and single-heavy constraints. For the unchanged0.8GiBfinancial job this is
1.8GiB when no other guarded allocations exist. The completed window stayed
above that level; the latest status has no live guarded jobs and2.9758GiBcommit.
This is a new sustained observation compared with the earlier immediate
0.4012GiBstop. It does not guarantee future resource availability.

Ruling: supersede only the controller-added3GiB waiting heuristic with the
existing enforced admission rule for ONE separately receipted max-new-batches1
financial pilotv3. Do not modify the guard, native producer/runtime,0.8GiBcap,
1GiBreserve,0.75GiBcommit stop,0.25GiBphysical stop,40GiBdisk floor or one-heavy
serialization. The native runtime still requires every original source/code/
snapshot/unit/replay check. The guard independently re-evaluates live admission
and continuously enforces stops. This scheduling ruling changes no product
coverage, source-quality, statistical, consumer or release acceptance threshold.

The prior exact orphan locks were archived with verified dead-owner evidence.
Use unique v3 logs and preserve failed attempts. On success, independently audit
the new native financial admission before continuing. On another external stop,
preserve terminal evidence, inspect the dead writer and stop retrying; do not
launch a new observation/retry loop in this turn. No foreign process is killed.

Cost if wrong: external pressure may interrupt the new attempt and consume
bounded compute/scratch without financial progress. The existing enforced
guard and native immutable validation prevent that from becoming an admitted
partial release. A resource-success result would still establish no financial
economic PASS without the original full measurements.
