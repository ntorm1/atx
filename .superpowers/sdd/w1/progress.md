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

Source integrations completed before compilation: Lane0 `95d0773d`; E1 storage
`0d7a45ec`, consumer `4e49679e`, checks `e2ea5e6d`/`43d5ad98`; A1 `aeea9b55`,
`65a7210c`, `6cc30e5f`, `59e3e199`, `0bdec034`, `70ccafdc`, `acfae169`, `5a64fa2d`;
report `e7fa0e3c`. Independent review accepted source after raw and derived
co-moment overflow fixes. The consumer fixture's missing parent directory was
also fixed before compilation. Root's focused W1 target contains ten test TUs,
including existing oracle/conformance/streaming and combiner/WF consumers.

The IC statistical review found no demonstrated noisy-null pruning (0/18) at
the original V2 rule. A new explicit EquivalenceV3 uses a protected full-window
IC floor0.002 and multiplier3.5; it removes the arbitrary one-SE veto while
retaining the additional heuristic quarter safeguard. Old V2 remains explicit.
This still passes underpowered candidates; it makes no general recall or speed
claim. Kernel import `b9e55206`, mine import `d7b11bc5` and discovery/defaults
`8ccf0982` are independently source-approved. Bounded qualification is complete
in `123163e2`: W1 foundation 83/83, IC 63 passing plus one corrected fixture
passing, and impl 42/42, 189 distinct checks with no unresolved failures/skips.
Production rank reuse/source-local JSON fix is `7e9f6f4a`; test correction
`e2aaea7e`. The build reused PCH/dependencies, Jobs1, and completed in 324.500s
(initial missing include) plus 375.350s; final fixture-only rebuild 8.531s.
Full wave, performance, scale and shared causality-harness gates remain open.

The next independent implementation lanes are active under the same owner
build-first instruction. Pool-3 owns W1-D1 dated security links, with the minimum
exporter and PitRecord consumer wiring needed to enforce expiry and acceptance
availability. No migration or loader runs are authorized. Pool-4 owns W1-A5
compact library records, absolute-correlation retrieval, segments and lifecycle.
Pool-5 owns the W1-B1 immutable cost-surface core and consistent fitness,
optimizer and replay adapters. The full B1 spread/calibration/borrow work remains
separate. These agents do source work while root owns the only compiler slot.

B1 core imports: a3ee26f0 -> e5f8ea8d, checks7cad09c9 -> a3aed92c, report3a9ae917 -> 86e50ae8. Root independently approved source and registered cost_surface.cpp plus an EXCLUDE_FROM_ALL focused cost target. No compilation yet. Calibration63ea1ef4/cdddaaab requires the identified near-singular raw-fit correction before import. A5 source d9925eec/10f0d46e awaits G0 independent review and focused fixtures. D1 source freeze forthcoming. Packet123163e2 is independently approved by the audit owner after XML, binary-hash and source-diff verification.

Calibration repair is now source-approved and imported: 63ea1ef4 -> 416961a3, cdddaaab -> 168cb34c, cdde0a55 -> b0ddb715, report514b1732 ->11268bb2. The near-singular raw-design finding is resolved by centered/scaled V2 fitting; fixed delta has no raw-slope dependency. Root cost target now contains six TUs including existing calibration, cost-chain, replay and optimizer checks; no configure/build has run for these new imports. A5 and D1 still await coherent source reviews before the next combined build.


## Current W1 qualification batch (2026-09-26, supersedes older pending-source notes)
Root pool2: source through bee493a6; original DAG indexes all imports. A5 through5d37aa27 + fixture9c4a2fd8; B1 core/calibration as previously recorded; D1 through592aa045 with independent reviewa5610cf0; E6 breadtha5bb66ac, PBOkernel20f22155/bcd96bfb andcallerbb86d9de, combinedsourceb17356ad/a53dbb63/dce7312c, regimes6c4c9006/7c919551. All source reviews approved; runtime remains pending. Integrated D1 synthetic Python15+7 passed (0.025s+0.154s native test time), logs build-equity/w1-d1-integrated-*-tests.log.
The first stable-object batch atdce7312c used Jobs1 and existing PCH/deps, stopped after88.563s on11 incomplete Provenance aggregate initializers in the new library fixture under -Werror. Test-only9c4a2fd8 supplies explicit fields; no production algorithm fix. Successful objects are retained. Log/start/receipt build-equity/w1-stable-objects-*. No compiler currently active after that stop.
Next final bounded targets: atx-engine-w1-{cost,eval,library,data}-tests, atx-engine-ic-screen-tests, atx-impl-ic-screen-tests. Focused eval now includes PBO,breadth,regimes,combined source,NNgate and factory caller/consumer checks; impl adds discover/provenance for rule binding. Only root compiles, Jobs1. 2020+ remains sealed; no warehouse writes, long benchmark or mining run. New source-only next lanes: pool4 R1 sparseconstraints, pool5 remaining B1 spread/FIM/borrow, pool3 D5 qa/universe; no imports during this qualification batch.
