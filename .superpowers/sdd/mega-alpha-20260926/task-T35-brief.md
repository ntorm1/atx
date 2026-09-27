### Task T35: IC runner headroom (conditional)

**Lane:** E · **Pool:** pool-9 · **Model:** Opus 5.5 · **Depends on:** T34b `--plan-only` refusal · **Peak:** 0.3 GiB

Scope, in order of cost: (1) document `--workers 2` (−56 MiB measured in T7) and the exact plan bytes; (2) if still refused, add `--max-memory-mib` slack accounting so the runner's cap can be 1,400 with the RSS guard at 1,536 (today both are 1,536: zero slack); (3) **do not** attempt f32 panel fields (engine `Panel` tripwire; wipes the candidate cache). Report only; no real runs.

---

