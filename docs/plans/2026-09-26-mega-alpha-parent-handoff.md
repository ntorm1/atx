# Mega-alpha parent handoff — stopped at the owner's request

Prepared September 26, 2026, America/New_York. This is the authoritative current
checkpoint; older entries in the rolling progress file describe earlier states.

## 1. Stop state and objective

The owner said: **“stop here and write a detailed handoff markdown file + goal
prompt for next parent agent.”** Development, builds and experiments are stopped.
The goal tool returned **paused**. All three child agents acknowledged the stop.
No owned compilation or research process remains active. Do not resume from
this document alone; the next owner instruction/goal prompt authorizes resuming.

The active user objective supersedes the original all-DAG objective:

> Build a mega-alpha strategy composed of many subalphas in the atx-engine alpha
> DSL, targeting annualized net Sharpe >= 1, low turnover, and a large
> cross-sectional universe of thousands of stocks, using recent data, especially
> 2020 onward. Target NAV is **$1 billion**.

Working turnover target: one-way turnover <= 30% per calendar month. This and
Sharpe/capacity are **targets, not achieved results**. The mechanical goal record
still has the original DAG wording; its API does not expose an objective edit.
The user's later pivot governs the work.

Latest priority instruction: **core engine runtime, alpha generation, alpha
composition, then detailed realistic backtesting after a working implementation.**
Stop exhaustive corporate-action registration. Preserve prior work, but do not
restart that effort or make it a gate for the current research implementation.

Other persistent requirements:

- Use subagent-driven development to preserve the parent's context; the owner
  explicitly requested it. Production implementation first, **not TDD**.
- Do meaningful focused postimplementation checks. Avoid repeated broad suites.
- No 25–45 minute workloads; real research runs are capped at 180 seconds.
- Optimize iterative C++/CMake compilation; use private implementations,
  incremental builds, warm caches and RAM-admitted **2–4 compiler workers**.
- Keep the original DAG filled with implementation/import/evidence SHAs.
- Do not select signs, weights or policies using validation. TRAIN signs are
  frozen before validation. Keep 2025+ reserved for final evaluation.
- No pushes, main fast-forward, warehouse writes, or broker actions are authorized.

## 2. Workspace ownership and Git state

**Use `C:/atx-wt/pool-2` explicitly as the working directory of every shell call.**
The environment's default `C:/atx` is a separate owned checkout. Do not mutate,
build, switch, stash or commit there. Read-only inspection is permitted.

Root branch: `feat/aes-codex-integration-20260925`.
Root lease: `aes-codex-integ-20260925`.
Last implementation/integration commit before this handoff:
**`6df7cc88c2e89d7c339396d594fc4215f0cb0725`**.
The tracked root tree was clean at the stop. This handoff and its companion
prompt/checkpoint are the only subsequent intended documentation changes.

| Worktree | Agent / responsibility | State at stop |
| --- | --- | --- |
| pool-2 | Parent: integration, all builds and real data runs | `6df7cc88` before handoff docs; clean |
| pool-3 | `/root/g0_evidence`: independent review/evidence | HEAD `ba1a5b1d80a2c1a3318095dc37ecfe23ac02ddf2`; tracked clean; ignored unfinished archive below |
| pool-4 | `/root/w0_replay_integration`: fast IC runner/runtime | HEAD `23f1541b9da14352ecc4ab7d0ec8262186fa22b7`; clean; two unimported commits |
| pool-5 | `/root/w0_gate_audit`: composition/target replay/review | HEAD `8c5dc6bd`; clean; last memory-fix approval is message-only |
| pool-6 | Preserved benchmark baseline | `3ccf012c`; leave alone |

All child agents are stopped. In a resumed session, reactivate existing agents
with `followup_task` if available; `send_message` alone does not start an idle
turn. Otherwise reassign bounded tasks in the established worktrees. Root alone
builds or accesses numerical datasets. Keep source ownership disjoint.

Project instructions prefer codebase-memory graph discovery. No callable graph
tool was available when searched, so targeted `rg` was used. Check availability
again if needed. No skill was applied during the current implementation.

`.superpowers` is ignored: use `git add -f` for intended evidence/checkpoint
files. Exact artifact archives have `.gitattributes` with `* -text` so Git does
not rewrite bytes. Do not add large generated dense payloads indiscriminately.

## 3. Main project records

- Original DAG and completion index:
  `docs/plans/2026-09-24-alpha-engine-production-swarm.md`.
- Current strategy plan and registered policies:
  `docs/plans/2026-09-26-mega-alpha-strategy.md`.
- Rolling checkpoint:
  `.superpowers/sdd/mega-alpha-20260926/progress.md`.
- Existing qualification reports:
  `.superpowers/sdd/strategy/2026-09-26-fast-ic-initial-qualification.md` and
  `2026-09-26-fast-ic-optimized-qualification.md`.
- Source reports:
  `.superpowers/sdd/strategy/2026-09-26-fast-ic-runner-report.md`,
  `.superpowers/sdd/recent-strategy/lane-fast-ic48-composition-report.md`,
  `lane-parallel-research-ic-report.md`, and `lane-target-replay-report.md`.
- Independent reviews:
  `.superpowers/sdd/w2/review-fast-ic-kernel-composition.md` and
  `review-target-replay-source.md`.

The most recent DAG rows supersede old “qualification pending” rows. The full
original sprint is not complete and is no longer the immediate finish criterion.

## 4. What is implemented

### Fixed 48-alpha DSL library and composition

Library: `atx-impl/strategies/slow_price_volume_ic48_v1.json`.
Generator: adjacent `generate_slow_ic_48.py`; `--check` passed exact bytes.

Eight families, each with three templates and two slow variants (21/63):
momentum, trend quality, medium reversal, defensive risk, liquidity state,
flow confirmation, price location, and trading-year seasonality proxy.
Only `close`, `raw_close`, and `volume` are used. Native compilation verified
maximum lookback 314 prior bars and maximum seven VM slots.

The composition fixes equal family/within-family weights (1/48 per candidate).
TRAIN 21-session mean rank IC determines each sign; undefined/zero evidence
gives neutral sign zero. **All 48 candidates enter the blend**, regardless of
diagnostic screen rejection. Missing components contribute zero without weight
redistribution. There has been no validation sign/weight tuning.

Private helper: `atx-impl/src/strategy_ic_composition.hpp/.cpp`. Streams one
candidate, orients/adds/discards it, and retains one blend and support mask.
The existing target proxy uses centered tied ranks, desired gross 1, a five
session cadence and 0.25 partial adjustment. Membership exits immediately go
to zero; survivors are not renormalized. Deployment is included in turnover.
This is a target proxy without price drift, not an executed portfolio NAV.

### Vectorized multi-horizon IC and bounded runner

- New light header: `atx-engine/include/atx/engine/factory/ic_research.hpp`.
- Shared private kernel: `atx-engine/src/factory/ic_screen.cpp`.
- Runner: `atx-impl/src/strategy_ic_runner.hpp/.cpp`.
- Thin CLI: `atx-impl/tools/equity_strategy_ic.cpp`.
- Executable target: `atx-equity-strategy-ic`.

It computes 5/21/63-session labels `close[d+1+h]/close[d+1]-1`, strict observed
entry/exit support, historical decision membership, vectorized Pearson and
exact tied-rank IC, cached label ranks, and overlap-aware chronological HAC.
Coverage distinguishes maturity, missing endpoints, signal support and guarded
price discontinuities. Weak/short/noisy evidence remains KEEP/uncertain.
The inactive fourth horizon cannot reject. General recall/noisy-null pruning
quality is not yet qualified; do not overstate the fast screen's selectivity.

Explicit `--workers 1..4`; four workers use one shared DetPool for sequential
VM/IC phases, no nested pools. Candidate order is serial, date-band reductions
are deterministic and HAC remains ordered. One role/cache/signal at a time.
`--plan-only` compiles DSL and checks metadata/resources before loading data.

`--orientations PATH --orientations-sha256 SHA` resumes validation from a pinned
TRAIN artifact and its adjacent recipe without loading/refitting TRAIN payloads.
`--save-combined` writes the exact already-computed blend, member and finite
masks, ordered axes, and a bound manifest. Saved payloads support portfolio
iteration without re-evaluating 48 DSL programs.

### Saved-blend portfolio replay: integrated, not built yet

Files: `atx-impl/src/strategy_target_replay.hpp/.cpp`,
`atx-impl/tools/equity_strategy_targets.cpp`,
`atx-impl/tests/strategy_target_replay_test.cpp`.

Targets registered in root `6df7cc88`:
`atx-equity-strategy-targets`, `atx-impl-strategy-target-tests`.
Implementation is private in `atx-impl-core`; no broad public-header change.

- Reads a SHA-pinned saved blend; optional matching pinned role prices.
- BaselineTargetV1 reproduces existing composition target arithmetic exactly.
- MonthlyTargetBudgetV2 limits discretionary changes against a predeclared
  0.30 calendar-month L1 target-change budget. Deployment and forced exits count;
  unavoidable forced-exit breaches are disclosed. No survivor renormalization.
- O(N) mutable scratch, O(D) diagnostics and one saved blend. No VM/IC rerun.
- Reports total/forced/discretionary/deployment turnover, gross/net, long/short,
  concentration and effective/actual held counts.
- Optional rough return is decision d -> entry d+1 -> endpoint d+2. Missing or
  guarded held returns leave the full day NaN and report observed contribution
  plus missing long/short/gross exposure. No silent zero missing returns.
- Declared linear trading/borrow assumptions are separated. No NAV, fill,
  price-drift, corporate-action accounting or capacity claim.

The source review found and fixed one issue: a return endpoint must be below
the declared `decision_end`, not merely within the underlying padded payload.
Seven postimplementation fixtures cover bit parity, budget/deployment/exits,
missing/guarded signed returns, pin/tamper handling and role-tail isolation.
All source-approved, **runtime still pending**.

## 5. Pending memory fix — import before the next build/run

TRAIN-export v4 stopped on the RAM guard after ten candidates. It started
candidate 11 (`risk_scaled_6_1_s21`, seven slots), without reaching VM-complete.
The old Engine held five slots. `Engine::ensure_pool` constructs the new arena
before releasing the old one; at 6,499,185 cells the old five slots add
**259,967,400 bytes (~248 MiB)** of transient allocation.

Pool-4 has two clean, committed, **unimported and unbuilt** changes:

1. **`8527a839f0b4ad1b9a4d16fcc070dd68a35a7e10`** — runner owns Engine with
   unique_ptr, checks actual retained `pool_capacity()`, destroys it before a
   larger-slot candidate, reconstructs with the same panel/mask/mode/shared
   DetPool, and releases the final Engine before combined scoring/saving.
   The admission formula changes `16*max_slots` to `8*max_slots`; returned
   owning SignalSet, masks and fixed metadata slack remain separately counted.
2. **`23f1541b9da14352ecc4ab7d0ec8262186fa22b7`** — ascending-slot versus
   maximum-first arena fixture. Compares keyed candidate metrics, combined IC,
   target proxy, saved blend and target CSV hashes; growth logs distinguish
   the actual path. Fixture expressions have identical cross-sectional ranks,
   avoiding accumulation-order differences in that comparison.

Pool-5 completed source approval of both. Approval is message-only; **no review
commit was created** before stop. It checked that SignalSet owns its data, each
iteration's spans die before reset, composition retains no borrowed output,
and panel/masks/DetPool outlive each Engine. No formula/sign/order/FP changes.
Expected RAM savings have not been measured. Do not call this runtime-qualified.
The report addendum in pool-4 was not written.

## 6. Compiler and execution setup

Warm build: `build-equity`, preset `equity-dev`, clang-cl 18.1.8, LLD, ccache,
static libraries, existing stable production/test PCHs and warm dependencies.

Use only the project wrapper for actual builds:

```powershell
& C:/atx-wt/pool-2/scripts/atx-build.ps1 build -Preset equity-dev -Jobs 2 TARGET...
```

Admit at >=1000 MiB free physical RAM and >=2500 MiB commit headroom. Use two
workers below 2200 MiB free, three at >=2200, four at >=3400. Recheck memory at
launch; other owners' processes fluctuate substantially. Do not kill them.
Normal CMake regeneration is okay. Do not change global build flags, rebuild
dependencies/PCH wholesale, use raw Ninja for actual builds, or build all targets.

Existing scoped optimization at `0f618a45`: only `ic_screen.cpp`,
`strategy_ic_runner.cpp`, `strategy_ic_composition.cpp` use
`/O2 /Ob2 /clang:-finline` and `SKIP_PRECOMPILE_HEADERS ON` under clang-cl Debug.
This keeps Debug CRT/assertions/FP/ISA and the rest of the warm environment.
LLVM 18 `/Ob0` otherwise leaves `-fno-inline`; the explicit driver flag clears it.
The actual three-command scope and numerical parity were qualified.
Root `6df7cc88` adds the same local treatment to the new target replay CPP.

Prepared ignored helper, **not run**:
`build-equity/build-recent-strategy-targets-v1.ps1`.
It checks clean Git and RAM, selects 2–4 workers, and builds exactly:

```text
atx-equity-strategy-ic
atx-impl-strategy-ic-tests
atx-equity-strategy-targets
atx-impl-strategy-target-tests
```

It preserves source/configured provenance, cache stats and wall time under
`recent-strategy-targets-v1-*`. The compile-command before snapshot already
exists: `build-equity/recent-strategy-targets-v1-compile-before.json`.
No build receipt for this batch exists. Do not overwrite historical receipts.

Set binary DLL path before native runs:

```powershell
$env:PATH='C:/atx-cache/vcpkg_installed/x64-windows/debug/bin;C:/atx-cache/vcpkg_installed/x64-windows/bin;'+$env:PATH
```

Use `scripts/run_bounded_research.py` for owned-process limits/receipts. It
requires clean Git and an exclusive new output directory, hashes executable
and small bindings, samples descendants/RSS/free RAM, and stops only its own
process identities. It is a sampled guard, not an OS sandbox; no detached jobs.
`--bind` accepts files <=16 MiB, not dense payloads. Real IC caps:
180 seconds, 1536 MiB process-tree RSS, 768 MiB minimum system free memory.
Native checks have used 768 MiB RSS / 512 MiB free. Root alone runs these.

## 7. Data and immutable pins

Read-only raw source: `C:/Users/natha/Downloads/TickerHistory3.parquet`,
3.618 GB, 32,323,644 rows, 2012-03-26 through 2026-09-18.
Raw SHA256: `0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`.

Reuse accepted cache `build-equity/recent-projection-v1`; no raw rescan needed.
Accepted parquet SHA:
`fe0a214eafa6d7b87afedef1976a426365a8cb69cc24a0115d5dc9a7295d4afc`.
Cache manifest SHA:
`25a96be8611bf97eddccea3b254220aa750a66b98953069c51cd392d30f0e446`.

Role producer: `atx-engine/tools/prepare_recent_research.py`. Cohort uses
prior 63 complete raw-dollar ADV, top 3000, price > $5, ADV > $5m and one-session
lag. Historical member mask is distinct from physical source presence.
Adjusted close is widened raw-f32 close times f64 vendor cumulReturnFactor.
Modeled mark/decision clocks are 22:00/23:00 UTC. Common-stock classification
and historical source vintage are unverified: call this a liquidity research
cohort, not a certified point-in-time common-stock universe.

| Artifact | SHA256 / geometry |
| --- | --- |
| `atx-impl/strategies/slow_price_volume_ic48_v1.json` | `1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff` |
| Adjacent library recipe | `3b6707f7a46e392391281a9f84585e6c6b1a1b5567cd9ff2656931b72481130b` |
| `build-equity/recent-fast-train-2020-2022-v1/manifest.json` | `3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493`; 1155 dates x 5627 union IDs; scored [399,1155), 756 sessions |
| `build-equity/recent-fast-validation-2023-2024-v1/manifest.json` | `0c757c41a363659664c96359a2d2288e10f792e2b91ab38ca8bf5e064dfbbda7`; 903 dates x 5048 union IDs; scored [401,903), 502 sessions |
| Original `build-equity/recent-fast-ic-v2/orientations.json` | `5106fdc13fc5347c9c2c670714c134a1978e6b7a0d2d2b5dd79bacb7dfb782d6` |
| Adjacent v2 `recipe.json` file | `c2635f5a88e5d76f2f53fce2412193fc81e3323167308b854828370ea8e83b09` |
| Canonical orientation candidate array | `4a3e8004ec15b104e045d06ee255447e5db7df8f1b1f6430668ebab0cbea9081` |
| `build-equity/recent-fast-ic-validation-v3/validation_combined.json` | `7407d7e7b72548e5577fdf94e2f52d238f9ff751a8ebc5640987130c1390d8e1` |
| v3 `validation_combined.f64` | `614d59f3e7bd15a2c5372bf23f7ab498c45bbf3f15856744259b9947e6ce645d` |

TRAIN daily eligible count is 2758–3000; validation effective eligible count
2994–3000. Union IDs are not daily holdings. TRAIN cache prep took 12.266s,
validation 8.016s. **2025+ has not been used for this research.**

## 8. Measured results and attempts

### v1: initial serial path

Source `20bf677b`. Build27.729s Jobs3, all11 focused tests1.344s. Real process
stopped deliberately at105.609s after10 TRAIN candidates, 11th started;
6.091–11.509s/candidate, peak956,231,680 bytes. No completed TRAIN blend or
validation. One final CSV row was truncated. Exact archive committed `76fe5e65`:
`.superpowers/sdd/strategy/fast-ic-qualification-20260926/`.

### v2: optimized VM, completed TRAIN

Source/configured `0f618a45b3bd256eddd3a96f64893746063818d9`.
Build31.631s Jobs3; all13 tests1.618s. Full TRAIN48+blend completed131.666s;
whole run hit180.218s, leaving validation13 complete/14th started, no VAL blend.
Peak896,258,048 bytes. First ten candidates improved84.772s ->24.090s, with
62,376 comparable daily values exactly equal to v1. One truncated row excluded.

TRAIN combined mean rank IC / HAC standard error:

| Horizon | Mean | SE |
| --- | --- | --- |
| 5 | 0.03499213991563447 | 0.01423935195831586 |
| 21 | 0.049930742389850805 | 0.024638057500225415 |
| 63 | 0.05983032291453856 | 0.04886669988234859 |

TRAIN target proxy: mean monthly turnover35.3205%, max80.0826%, deployment25%
included; mean gross0.90121, max abs net0.06453. Turnover target not met.
These are training-fit diagnostics, not a Sharpe result.

Exact optimized archive/report committed **`5ee8cb71`** under
`.superpowers/sdd/strategy/fast-ic-optimized-20260926/`.
Original TRAIN summary, orientations, daily IC and planned targets remain in
`build-equity/recent-fast-ic-v2`. There was no saved dense TRAIN blend then.

### v3: completed frozen validation and saved blend

Actual source/configured **`a9b8814c5797599dd73fd448f5c5d63d88149dc6`**.
Build82.169s Jobs2 under memory pressure: seven CPPs/four links, warm PCH/deps;
CMake regeneration25.7s. All **34 native tests passed in10.484s**. This is the
last built/tested source, not current root HEAD or the unimported memory fix.
Research executable SHA:
`8833a852edec05d7ecaca0d92a5bbe48095c8c76340fa7f0fa169542a3b726d0`.

Run `recent-fast-ic-validation-v3` with original v2 signs, workers4,
min-names2000, memory1536 and `--save-combined`: **48 candidates + one blend,
zero TRAIN evaluations**, completed110.5s, peak660,586,496 bytes (~630 MiB).
Stages: VM38.704s, IC33.847s, composition19.546s, load9.177s, labels1.247s,
save3.115s. Saved payloads total45,631,048 bytes.

| Horizon | Frozen validation mean rank IC | HAC SE | Valid days |
| --- | --- | --- | --- |
| 5 | 0.018033565459307746 | 0.01566902744171605 | 496 |
| 21 | 0.035690280179471075 | 0.027300949553404476 | 480 |
| 63 | 0.05966692932690373 | 0.017772073033932835 | 438 |

Validation target proxy: mean monthly turnover34.4356%, max70.5991%; total
8.2645448796, deployment25% included; mean gross0.90637, max abs net0.01908.
**Positive IC is not portfolio Sharpe; turnover/capacity acceptance remains open.**

Final audit `build-equity/recent-fast-ic-validation-v3-audit.json` verifies:
48 original signs unchanged; all13 prior completed validation candidate metrics
identical; **55,098 daily values exact**, one old unterminated CSV row excluded.
Common13-candidate IC stage24.394s ->8.620s. Other stages varied with host load.
All saved payload lengths/hashes verified. The original failed audit tried to
compare an unterminated last float (`0.027`) as complete; the corrected audit
excludes that incomplete row. This was not an engine arithmetic mismatch.

Root artifacts to preserve:

```text
build-equity/recent-strategy-fast-ic-resume-*
build-equity/recent-strategy-fast-ic-resume-qualification/
build-equity/recent-fast-ic-validation-v3/
build-equity/recent-fast-ic-validation-v3-run/
build-equity/audit-fast-ic-validation-v3.py
build-equity/recent-fast-ic-validation-v3-audit.json
build-equity/recent-fast-ic-validation-v3-audit-run/       # failed audit receipt
build-equity/recent-fast-ic-validation-v3-audit-run2/      # final verified audit
```

Final audit script SHA:
`5c4e80b0a881b6256ae66eb9feef6fce0c8411f9cce20143fcbbc8aa9dc7d2c3`.
Verified audit1.422s/~92 MB. The failed receipt bound the earlier script bytes;
do not pretend that binding hashes the corrected archived script.

### v4: TRAIN export stopped by RAM floor

Same actual source/executable as v3. This was an execution repeat of fixed TRAIN
to save its blend, **not 48 new independent strategy ideas**. Guard stopped it
after41.578s, outcome`system-memory-limit`, exit15. Peak1,101,864,960 bytes;
minimum system free794,648,576 bytes, below768 MiB. Ten TRAIN candidates
complete, 11th started. No completed orientations, saved TRAIN blend or planned
target CSV. Do not use its partial summary as complete.

Artifacts: `build-equity/recent-fast-ic-train-artifact-v4/` and `-v4-run/`.
The exec session87455 has completed; do not poll it again. The discovered arena
growth issue and pending fix are in section5. No repeat was launched after this.

## 9. Unfinished evidence archive in pool-3

Ignored, **not staged or committed**:
`C:/atx-wt/pool-3/.superpowers/sdd/strategy/fast-ic-validation-20260926/`.
46 indexed files, 1,218,507 copied bytes plus index; compact metadata/logs,
receipts/summaries and original orientations, no dense blend. Index file
`sha256-index.json` SHA:
`352ca7b7d98f877a53d6fcc578f39ed7b5f2b3a4f56f90bc0144b2364bca2417`.

`review-checks.json` is complete. Agent verified receipt logs/bindings, 34 unique
XML passes, seven CPPs/four links, executable identity at the time, frozen signs
and v4 partial status. Sibling prose qualification report was **not written**.
Ignored helper: pool-3 `build-equity/archive-fast-ic-validation.py`.
Do not rerun blindly: it requires an exclusive output directory. On resumption,
finish/report/commit this existing archive in pool-3 and import that commit;
do not regenerate completed results just to archive them.

## 10. Source/import SHA ledger for the current packets

| Element | Source -> root import |
| --- | --- |
| Initial kernel / fixtures | `1dba7035` -> `3048951c`; `ade1e99d` -> `9b65981a` |
| Library/composition / fixtures | `c4c8dc59` -> `0139cb15`; `67b28ba0` -> `2a26e674` |
| Initial runner / fixtures / ID fix | `08940e88` -> `1e67b5c5`; `767d8894` -> `d0c9ade6`; `86671916` -> `9bb58f9e` |
| VM workers / fixtures | `c3942160` -> `c60ee179`; `dcf67f1c` -> `d5d38ea4` |
| Scoped compiler optimization | root `0f618a45` |
| Parallel IC / fixtures | `1485804d` -> `bfabe3a4`; `6e450581` -> `5330a5fd` |
| Resume / fixtures / shared caller | `d7477a24` -> `34d149a2`; `5f8efd39` -> `e72f32da`; `93289822` -> `0230d799` |
| Parallel/resume review / report | `42337ab3` -> `7a2660f7`; `4ddef1c9` -> `7fef1912` |
| Saved blend / fixture | `5eb11cf4` -> `b00ea486`; `40b82835` -> `a9b8814c` |
| Saved-blend review / runner report | `23354229` -> `50a8261f`; `6a738f15` -> `28389363` |
| Target replay production | `64c8f58c` -> `0c5e0d51` |
| Target replay fixtures | `f7a376ea` -> `37ae6624` |
| Role-end fix + fixture | `8005bc08` -> `6c57de30` |
| Pinned optional price-role fixture | `4e72a542` -> `a8bab61f` |
| Target replay report / review | `8c5dc6bd` -> `3c738348`; `ba1a5b1d` -> `8c7dc6a0` |
| Root target registrations + fixed policy | `6df7cc88` |
| VM lifetime fix / ascending fixture | `8527a839`, `23f1541b`: **unimported/unbuilt**, source-approved |

No need to replay old imports. Record new resulting import SHAs in the DAG.

## 11. Exact next sequence after the owner resumes

1. Read this handoff, current plan and Git status. Confirm root ownership and no
   leftover jobs. Do not restart old corporate-action or broad-DAG work.
2. Import pool-4 `8527a839` then `23f1541b`. Capture pool-5's completed source
   review in a short report if useful; don't redo it as a lengthy gate. Finish
   pool-3's existing compact evidence archive in parallel. Record SHAs.
3. Run the prepared RAM-admitted focused build helper once. Inspect incremental
   scope and source/configured provenance. Run the IC suite (expected prior34
   plus new lifetime case) and seven target-replay cases, with bounded receipts.
   Tests may reveal issues; expected counts are not passed results.
4. Make one bounded TRAIN-only saved-blend run in a **new** output directory,
   e.g. `recent-fast-ic-train-artifact-v5`. Use the original full TRAIN and
   library pins, workers4, min-names2000, memory1536, `--save-combined`.
   Admit enough available RAM for the measured working set plus768 MiB floor;
   prior full-run admission used1900 MiB free/2200 MiB commit. Do not blindly
   repeat if the guard stops. No validation arguments are needed.
5. Require the new TRAIN orientation candidate array to equal original v2
   exactly, and compare candidate/combined metrics and target proxy. Save and
   hash the TRAIN blend manifest. Count this as a deterministic export/runtime
   repeat, preserving all attempts; do not label it a new independent strategy.
6. Run exactly two saved-blend TRAIN policies: baseline then fixed monthly
   budget. No parameter grid, no DSL rerun. Both policies were preregistered in
   root `6df7cc88` before target outputs. Use the same pinned role and hypothetical
   cost assumptions: **6 bps one-way target change, 300 bps annual short borrow**.
   These are scenario assumptions, not estimated $1bn impact/borrow/locate costs.
7. Require exact baseline daily parity against the original planned target CSV.
   Compare turnover together with exposure/concentration/name counts. Lower
   gross is not a free win. Keep forced breaches and deployment visible.
8. The same budget policy is frozen for validation regardless of TRAIN results.
   Apply to the already-saved v3 validation blend (no alpha rerun), optionally
   baseline as a fixed comparison. Do not tune to observed validation outcomes.
9. Report actual findings and update the original DAG with implementation,
   import and qualification SHAs. Continue portfolio/alpha work toward a
   measured combined net strategy; do not claim Sharpe1 or $1bn capacity from IC
   or a target proxy. Keep2025+ reserved until the strategy is truly frozen.

### Command shapes

TRAIN export (use a new directory and guard; line continuations omitted):

```text
atx-equity-strategy-ic.exe
 --library atx-impl/strategies/slow_price_volume_ic48_v1.json
 --library-sha256 1ec75242f0328a459ac114256f99f534eb0b8ec2e642794d3f4ad846779a09ff
 --train build-equity/recent-fast-train-2020-2022-v1/manifest.json
 --train-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493
 --output build-equity/recent-fast-ic-train-artifact-v5
 --max-memory-mib 1536 --min-names 2000 --workers 4 --save-combined
```

Target replay; replace placeholders with actual saved TRAIN pin and new paths:

```text
atx-equity-strategy-targets.exe
 --combined TRAIN_COMBINED_JSON --combined-sha256 TRAIN_COMBINED_SHA
 --output NEW_BASELINE_DIRECTORY --rule baseline-v1
 --cadence 5 --trade-fraction .25 --monthly-budget .30 --max-bytes 536870912
 --role build-equity/recent-fast-train-2020-2022-v1/manifest.json
 --role-sha256 3f53ee9aa1b674d3f5022cbb22d40e5c043e7add8c9422cd2456299ce3662493
 --one-way-bps 6 --annual-borrow-bps 300
```

Second run changes only new output path and `--rule monthly-budget-v2`.
CLI outputs `recipe.json`, `daily.csv`, then complete `summary.json`.
Turnover is decision-calendar target change, not actual filled turnover.

Ignored draft audit prepared in root, **not executed**:
`build-equity/audit-target-replay.py`. Args:
`--source ORIGINAL_PLANNED_TARGET_CSV --baseline BASELINE_DIR --budget BUDGET_DIR --output NEW_JSON`.
It checks exact IEEE-f64 baseline turnover/gross/net, month reconciliation,
declared policy/costs, NaN full-day missing-return handling, and reports common
complete-day sums without calling them full-period returns/Sharpe. Inspect it
before use; it was written as an execution audit, not yet runtime-qualified.

## 12. Deferred work and interpretation limits

Earlier strict-book attempts failed on MDCO then JAG; three attempted, zero
completed. Cash claims and JAG/WCG stock transitions are retained; the last
stock batch passed45 native checks at `b036fa32`. No fourth full-book rehearsal
ran. Do not redo event registration or treat those failures as alpha selection.

The fixed48 ensemble and fast evaluation are real working implementations,
with measured frozen validation IC and runtime. Saved-target replay and its
turnover budget are implemented but unbuilt at this stop. There is no complete
costed portfolio Sharpe, no qualified common-stock/vintage inventory and no
measured $1bn execution capacity yet. These are concrete remaining tasks, not
reasons to resume exhaustive realism before portfolio construction works.

The companion `2026-09-26-mega-alpha-next-parent-goal.md` is a paste-ready
resumption prompt. This handoff itself leaves the goal paused.
