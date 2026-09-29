# Task P1 report: platform + production code review (read-only)
Deliverable: code-review-v7.md (395 lines) on tree b4ebb30c. No edits, builds, binary/data runs or subagents; nothing dated 2023+ or named VAL read.
Evidence: 212 run receipts, IC-runner summaries, build receipts, and one mechanical count on the v6.1 TRAIN daily CSV.
Findings:
1. v6.1 real-data compute was ≈ 4.2 min of a ≈ 90 min cycle; the rest was per-version generator, script and pin work (9 ladder scripts).
2. One new field invalidated 35/39 cached candidates (keyed by the whole fields-manifest SHA): 0/39 hits. A changed DSL is refused, so every library gets a new 1.9 GB cache.
3. Scalar SHA-256 (~220 MB/s) takes 30/76 s of the u pass and 19/31 s of the w pass. The w pass verifies fields it never loads.
4. Research exes are Debug builds, with /O2 on only 7 TUs.
5. The brief's "3-4 IC passes" premise is v3-era; since v4 every u pass completes in one pass (26-122 s, ≤ 1,112 MiB).
6. There is no production decide path: the replay never decides its last 2 rows and emits no per-name state, and the 2025 seal is hard-coded. The factor model, QP and streaming VM exist in atx-engine but are unused.
Lanes (disjoint files):
- L1: IC cache keys + fast hashing.
- L2: research-cycle driver + fields reuse + shared fit store.
- L3: holdings emission + decide verb + deploy manifest (TRAIN parity).
- L4: DSL ops. Root: Release A/B + identity canary.
