# Lane W0-A0: Alpha kernel correctness

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-2`** ·
Branch: **`feat/w0-a0`** (run id `aes-w0-a0`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-A0 · Alpha kernel correctness
- **Closes:** A-01, A-02, A-03, A-09, A-13, A-18.
- **Owns:** `alpha/cs_ops.hpp`, `alpha/state_ops.hpp`, `src/alpha/typecheck.cpp`, `alpha/oracle.{hpp,cpp}`, `factory/crossover.hpp`, `factory/canonical.hpp`, and in `alpha/ts_ops.hpp` **only** the flat-window guard and the AuditExact ts_sum/ts_mean routing.
- **Build:**
  - Average-rank ties by default. Ordinal ties stay behind `RankTies::OrdinalV1` for old digests.
  - `hump`: a NaN prior takes x; a NaN x emits NaN (with a staleness cap).
  - Typecheck requires a `Literal` in the scalar slots (scale, winsorize, quantile, hump threshold). Crossover refuses non-literal splices there.
  - A relative cancellation guard on the batch path, applied identically in the oracle.
  - AuditExact ts_sum/ts_mean use windowed recompute (oracle-exact). ResearchFast keeps the online path, now Neumaier-compensated.
  - `CanonSet` stores the canonical string and compares it on a hash hit.
- **Suites:** `AlphaCsRankTies_*`, `AlphaHumpWarmup_*`, `AlphaTypecheckScalarLiteral_*`, `AlphaFlatWindow_*`, `AlphaAuditExactParity_*`, `FactoryCanonCollision_*`.
- **Accept:**
  - A tied row ranks to 0.5 for every name.
  - `hump(ts_mean(x,5),0.1)` is finite from t=4.
  - `winsorize(x, close)` returns Err.
  - A 10k-child crossover stress run produces no non-literal scalar slot.
  - `ts_zscore` over a constant 0.1 window gives NaN in both VM and oracle.
  - AuditExact VM output is bit-exact against the oracle for ts_sum and ts_mean.
  - The full VM↔oracle differential stays green.
  - Golden digests are re-baselined, with an old→new table.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| A-01 | H | `alpha/cs_ops.hpp:250-267`; test pins it `tests/alpha/alpha_cs_test.cpp:256-272` | `rank` breaks ties by instrument index. `rank(sign(..))`, `rank(group_count(..))` and tied fundamentals turn into an index/size proxy. `ts_rank` averages ties, so the two are inconsistent. | W0-A0 |
| A-02 | H | `alpha/state_ops.hpp:79-87`, `vm.hpp:1651-1662` | `hump` starting from NaN stays NaN forever. The oracle has the same bug. Stale values carry indefinitely after universe exit. | W0-A0 |
| A-03 | H | `vm.hpp:1284-1288,1653`; `typecheck.cpp:327-402`; `crossover.hpp:67-80` | The scalar operands of scale/winsorize/quantile/hump read panel cell [0,0]. Crossover can splice panels into those slots. | W0-A0 |
| A-09 | H | `ts_ops.hpp:170-182,935-941,1094-1104` | Flat windows give ts_zscore ≈ ±0.97 and noisy corr/slope/skew in AuditExact. This hits every forward-filled fundamental. | W0-A0 |
| A-13 | M | `vm.hpp:301-303` vs `ts_ops.hpp:366-387` | AuditExact ts_sum/ts_mean use an online uncompensated sum, so output depends on the panel start and is not oracle-exact. | W0-A0 |
| A-18 | L | `factory/canonical.hpp` (`CanonSet`) | A 64-bit FNV collision silently reuses another genome's score. | W0-A0 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-engine/include/atx/engine/alpha/cs_ops.hpp`
  - `atx-engine/include/atx/engine/alpha/state_ops.hpp`
  - `atx-engine/src/alpha/typecheck.cpp` (+ `atx-engine/include/atx/engine/alpha/typecheck.hpp` if a declaration must change)
  - `atx-engine/include/atx/engine/alpha/oracle.hpp`, `atx-engine/src/alpha/oracle.cpp`
  - `atx-engine/include/atx/engine/factory/crossover.hpp`, `atx-engine/src/factory/crossover.cpp`
  - `atx-engine/include/atx/engine/factory/canonical.hpp`, `atx-engine/src/factory/canonical.cpp`
  - `atx-engine/include/atx/engine/alpha/ts_ops.hpp` — ONLY the flat-window guard and the AuditExact ts_sum/ts_mean routing
  - `atx-engine/include/atx/engine/alpha/vm.hpp` — ONLY the sites cited by A-02 (hump), A-03 (scalar operand reads) and A-13 (AuditExact ts_sum/ts_mean dispatch); W1-A1 owns the rest of vm.hpp
  - existing tests that pin a cited defect (e.g. `atx-engine/tests/alpha/alpha_cs_test.cpp:256-272` pins A-01) and golden-digest tables that re-baseline
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `alpha;factory` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-alpha-tests`, `atx-engine-factory-tests`.
- Suites: `AlphaCsRankTies_*`, `AlphaHumpWarmup_*`, `AlphaTypecheckScalarLiteral_*`, `AlphaFlatWindow_*`, `AlphaAuditExactParity_*`, `FactoryCanonCollision_*`; must stay green: whole alpha + factory targets including the VM↔oracle differential suites.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-a0-report.md`.

## Lane notes (orchestrator)

- Golden digests: produce the old→new table in the report, each row tied to A-01/A-02/A-09/A-13.
- `RankTies::OrdinalV1` must reproduce the old digests bit-exactly (prove it with a test).

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
