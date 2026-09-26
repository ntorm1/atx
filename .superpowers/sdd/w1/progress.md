# W1 foundations: implementation in progress, 2026-09-26

The owner prioritized building over 25–45 minute runs and explicitly deferred the
long W0 comparison. This permits source work to continue; it does not pass the
performance gate or qualify W1. Main remains unchanged. No long benchmark or mining
rerun is authorized by this checkpoint.

The original DAG is `docs/plans/2026-09-24-alpha-engine-production-swarm.md`.
Working source base for these lanes is `7e16ed267dcd3f1094c0e504dbe14e78f95eae03`,
the integrated IC-screen implementation before its focused runtime qualification.
Later IC reporting fixes are imported independently; no W1 source is mixed into
the running IC qualification build. Root will commit the minimal CMake scaffold
before importing new translation units.

| Lane | Owner / pool | Scope and current state |
|---|---|---|
| W1-E1 storage, E-11 | g0_evidence / pool-3 | New combine/signal_cube.hpp and src/combine/signal_cube.cpp, bounded chunk storage and original-value statistics; implementation first, focused checks afterward. |
| W1-E1 consumer, E-11 | w0_gate_audit / pool-5 | signal_store.hpp, signal_combiner.hpp/.cpp and walk_forward_combiner.hpp. Reuse cached IC in real GK/EWMA and walk-forward consumers; coordinate immutable cache API with storage owner. |
| W1-A1, A-10/A-11 | w0_replay_integration / pool-4 | Existing alpha time-series headers: block delay, date-major SIMD, finite-window O(1) decay, ResearchFast pair routing. Preserve AuditExact and streaming contracts. |
| Lane0 / integration | root / pool-2 | New-source CMake registration, original DAG SHAs, independent review, bounded owning qualification. Only root compiles; Jobs1. |

All owners reuse their existing leased worktrees. No writes/builds in C:/atx;
no warehouse changes or actual 2020+ rows. No TDD. No agent starts a compiler or
large evaluation without the root's sole compiler slot.

E1 contracts: date-major [t][alpha][instrument] int16 chunks with explicit scale
and missing sentinel; metadata and source/transform/membership/return-treatment/
delay/maturity identities; publish manifest last. Statistics come from original
normalized f64 values before quantization. Exact-f64 cache mode is needed for
legacy bit parity; optional f32 mode is explicit. Planned horizons 5/21/63/126
remain available, and h=1 is supported for existing consumer defaults. Missing
factor-neutral statistics are unavailable, not zero. Full-cache consumers must
mask forward labels crossing each training cutoff, even with an embargo.

The original E1 1000×2520×3000 RSS gate and A1 large-panel speed thresholds remain
pending actual evidence. Use short, bounded synthetic correctness and resource
checks now. Do not infer throughput from Debug tests or claim W1 completion.
