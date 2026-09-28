# Task L1: fast, incremental IC runner (content-keyed cache + hashing that only touches what is loaded)

**Pool:** C:/atx-wt/pool-10. `git status` must be clean, then `git checkout -B feat/platform-v7-l1-iccache-20260928 <BASE>`
where BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Work only in pool-10. Never build C++ (root builds), never run the
pipeline or any real data, never spawn subagents, never touch C:/atx. You may read C:/atx-wt/pool-2/build-equity/** by
absolute path (TRAIN outputs only; never anything named validation/VAL/2023/2024/2025). Commit trailer:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report: C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/
task-L1-report.md (<= 40 lines). Reply in chat < 15 lines. House style: read .agents/cpp/agent.md before touching C++.
No TDD ceremony: implement, then add post-implementation gtests that root will run.

**Read first:** .superpowers/sdd/platform-20260928/code-review-v7.md findings A1, A2, S3 (timings), S4 Lane 1;
plan-v7.md S2 (the cross-lane contract). Code: atx-impl/src/strategy_ic_runner.cpp (cache at ~987-1075, verify at
~1240-1254, ~1718), atx-core/src/sha256.cpp:157-180, the existing runner gtest, and the candidate cache on disk
(C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v61/*: inspect the sidecar/payload layout).

**Deliverables**
1. Content-keyed cache. Key each candidate payload by (dsl text sha256, semantics version, role manifest sha, the sha256 of
   each field payload the DSL actually references, orientation/label config). Sidecar gets `field_payload_sha256`
   (map field -> payload sha256) and payloads are named `<id>.<dsl_sha16>.{f64,json}`; the old layout must remain
   readable (a hit under the old key is still a hit). A changed DSL under the same id becomes a new entry, not a refusal.
   `--cache-report` prints hits/misses/unreferenced entries with bytes.
2. Verification cost. Verify only the field payloads the library references, hash each file once per process, and keep
   the summary's timing breakdown fields (add `verify_bytes`, `hash_seconds`).
3. SHA-256 speed. SHA-NI (or the fastest portable path) with the scalar fallback; digests bit-identical to the current
   implementation; unit test on the NIST vectors plus a multi-MiB random buffer against the scalar path.
4. Tests: gtests for key derivation (one field change invalidates only referencing candidates), old-layout readability,
   cache-report accounting, hash equality. Keep existing tests green.

**Root acceptance (root runs these; state in the report the exact command lines you expect root to run):** the v6.1 u pass
against a copy of the v6u cache gets >= 38 hits and its orientations.json, train_daily_ic.csv, train_candidates.jsonl
(minus timing keys) are byte-identical to build-equity/mega-v61-train-u-1; incremental u <= 20 s and w <= 15 s under the
180 s / 1536 MiB runner; digests equal the scalar ones on every existing manifest.

Report: changes with file:line, the key derivation spec (exact bytes hashed, in order), expected timings, risks
(e.g. sidecar schema bump), and anything you could not test without a build.
