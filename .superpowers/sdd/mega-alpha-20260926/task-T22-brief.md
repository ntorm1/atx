# T22 — multiple group fields in the DSL (grp_ prefix) (C++, S)
Spec: T18 report (C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T18-report.md) §5 "DSL/VM operators" and §7 row T22. is_group_field accepts 'sector' and any field named
grp_*; typecheck + VM + IC runner fixtures (group_rank/group_neutralize/group_mean/group_zscore bind and evaluate
with grp_a; NaN group label excluded from group statistics). Read .agents/cpp/agent.md first. Say whether the VM
semantics identity / tripwire (T7: pinned source hash over 29 files) must be bumped and do it if so. Root builds
atx-engine-alpha-tests and atx-impl-strategy-ic-tests.
