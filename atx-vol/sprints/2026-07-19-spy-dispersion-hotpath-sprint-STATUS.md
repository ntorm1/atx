# SPY Dispersion Hot-Path Sprint — Status & Next Steps

**As of:** 2026-07-20 — **FINISH REPORT** (supersedes the mid-sprint banners below)
**Integration branch:** `feat/disp-hotpath` @ `07950ec` — **SOUND & GATED**
**Base:** `main` @ `8cb4576` (main has since advanced under an active peer — see "Deferred")
**Sprint plan:** `atx-vol/sprints/2026-07-19-spy-dispersion-hotpath-review-sprint.md`

> ✅ **The build freeze (old §7) is moot** — all sprint agents exited with the
> prior process; nothing is parked. The four workstreams have been merged,
> verified, and gated. §1–§9 below are the detailed mid-sprint record; the
> FINISH REPORT immediately below is the current authoritative state.

---

## FINISH REPORT (2026-07-20)

**Core deliverable complete.** All four frozen workstreams merged onto a corrected
integration tip; independently gated; byte-identity preserved. **One decision is
the user's** (final merge to `main`). **Two items are documented follow-ons**, not
bundled into this merge.

### Final merged chain (`feat/disp-hotpath` @ `30809a0`)

```
07950ec  merge     content-derived created_ts_ns (byte-reproducible corpus builds)
30809a0  fix       strict-config enum-reject probe -> inverse_vega  (blocker-1 fix)
5db00d2  WS-GATE    laned SIMD-invariance + ListedDispersion entry-mark + tolerance re-band
8773352  WS-BATCH   corpus fan-out batching + phase-split instrumentation
f2b0f68  WS-X-B     X4/X5 tearsheet (friction-regime-first) + strike-param-by-presence
e0da68b  WS-ZCFIX   seal only the snapshot cache run_backtest privately owns  <-- fixes old §3
b79ac15  WS-ZC      zero-copy borrowed replay surfaces (was defective; corrected by e0da68b)
...      (fdfc418 <- 3e57edb <- 5812200 <- 1ca3122 <- d0cec36 <- 8929bea <- 8cb4576)
```

Merges done by PM `--no-ff`; all four were textually clean (only a trivial
`.gitignore` union between xb and batch). WS-BATCH's uncommitted `corpus.cpp`
instrumentation was committed (`dbfb1e0`) before merging so it could not be lost.

### The old §3 soundness blocker is RESOLVED

`b79ac15` flipped a **caller-owned** snapshot cache to `Sealed` unconditionally,
orphaning the caller's `Mutable`-keyed preloads and silently downgrading the
pricing tier. Fix `02c5e64` binds backing to construction (immutable; setter
removed); the private cache is `Sealed`, a caller cache is used as configured.
**Independently verified before merge:** dev serial gate returned to the exact
17-failure `fdfc418` baseline, the 3 ZC-attributable failures gone, both new
regression tests present/passing/**non-vacuous**, byte-identity 82/135 preserved.

### Final gate: 4 pre-existing failures, ZERO sprint-introduced

Authoritative serial run on `30809a0` (single process, correct cwd,
`gate-final-30809a0.log`): **1963 ran / 1922 passed / 37 skipped / 4 failed.**
The tolerance re-band cleared all genuinely-WS-P1a numeric failures. The 4
residuals are **all proven pre-existing on base main `8cb4576`** (built in a
fresh worktree and run 3×, deterministic):

| Residual failure | Verdict |
|---|---|
| `SurfaceV2Qualification.RiskBuild…/Latency` | pre-existing (FAIL on 8cb4576) |
| `SurfaceV2Qualification.RiskBuild…/Balanced` | pre-existing (FAIL on 8cb4576) |
| `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily` | pre-existing (FAIL on 8cb4576) |
| `PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration` | pre-existing (FAIL 3/3 on 8cb4576) |

### Byte-identity (rel-avx2 replay) — RE-PINNED post-merge 2026-07-21 (WS-M M2)

The golden pin is `sha256(surface_backtest.tsv)` — the surface-only replay payload
(`dispersion_run_surface_backtest`; see the in-code note at `dispersion_run.cpp`
"the reproducibility pin is measured on exactly the bytes it always was"). The two
X5 companion artifacts the same replay writes (`surface_pnl_track.tsv`,
`surface_tearsheet.tsv`) were also 3×-identical and are recorded below as
supporting determinism evidence. `backtest_profile.tsv`/`backtest_counters.tsv`
carry timing/counter data and are written only under `-DATX_VOL_PROFILE/COUNTERS`
(OFF for the golden build) — NOT part of the pin.

**Current pins** (re-pinned post-merge 2026-07-21, engine = main A9 kernel + branch
tolerance re-band, 3×-stable; measured on `build-rel-avx2`, run-to-run identity
verified via `sha256sum` + `cmp`):

- 82-session (`bt-sota-baseline`, final_nav 247.4062412)
  `5e7ca06514dfe121308643cc431c90858827c180fdbc9c84e906a49fe4715af4` — **3×-stable**
- 135-session (`bt-sota-full`, final_nav 1283.615746)
  `141173fdc35eed9fbb0263c87729c547e9f0eac144c1c336173c932ac69f2835` — **3×-stable**

**They moved ONCE from the pre-merge pins** (82: `0737660775601f1609690568d930c62c46a1dddd0d97784036916ba4c5484c3a`,
135: `ac97a643851fa9988880d85af3201ef39158aa7e054f8c74062f0c4e68970b33`) because the
M1 keystone merge repriced the golden replay through the peer "pricing/greeks SOTA
sprint" A9 American-greeks kernel — the deferred-integration note below predicted
exactly this legitimate break. This is a one-time re-pin, NOT engine
nondeterminism: the M2 gate ran each replay 3× serially and every artifact was
byte-identical. Supporting artifact SHAs (each 3×-stable):
82 pnl_track `94afbf8668d37053245e60e6fd52f665d6fd230f540813c929be83eda55c875b`,
82 tearsheet `584a6ff4569ea5888210ba7c4f32b6922a9a4f4ed74c8a7ca54efcaab39d63cd`,
135 pnl_track `20e6ee2c3319f37ea4c689f294c2c4a7559486545448c3d26bd0a375ce1d4619`,
135 tearsheet `c41f7e07c19d6e068a18538e0efe183eda296eff87d2a704e7db1887040daa79`.

WS-X-B's `dispersion_run.cpp` edits and the strike-param-by-presence fix (`ac8758e`)
did **not** move the payload (the golden `run_spec` sets no strike field; `ac8758e`
gates only on `strike_log_moneyness` / `strike_abs_delta`).

### 🔧 Correction to the prior record: PricerFitter was mis-bucketed

The mid-sprint record counted `PricerFitterTest.LocalRiskRefitPublishesCopyOnWrite‑
Generation` among the "13 WS-P1a" failures. **It is not WS-P1a-caused — it fails
identically (3/3, deterministic) on `8cb4576`, which does not contain WS-P1a**, and
`session.{cpp,hpp}` + `pricer_fitter.{cpp,hpp,test}` are byte-identical across the
whole sprint. Mechanism (`session.cpp:2075-2243`): the test rescales quote spreads
by 0.45 **while holding mids constant**; `cached_refit_observations` derives the
carry forward from *mids* (a mid-based borrow solve, `resolve_chain_forward`), so
`forward_shift ≈ 0 < 1e-5` and the cache correctly **does not** invalidate. The only
greeks-derived term in the decision (`obs.vega`) appears solely in gates that a
spread-shrink *loosens*, so the laned kernel cannot flip `has_value()`. **No stale
data is served** — the reused IVs remain correct for a mid-preserving update. The
failing *expectation* (that a mid-preserving spread rescale must invalidate) is the
stale/incorrect part; the test is pre-sprint WS-F-owned (see follow-ons).

### Blocker-1 (the ONE genuine sprint-introduced failure) — FIXED

WS-X4 (`bb5a144`) shipped `weighting=equal_vega` as a real scheme, so the X1-era
strict-config test that used `equal_vega` as its "unimplemented → must reject" probe
stopped rejecting. Fixed (`30809a0`) by swapping the probe to `inverse_vega`
(outside the legal set `{vega_neutral, equal_vega, gamma_neutral, theta_neutral}`),
keeping the reject-and-list-the-legal-ones assertion **non-vacuous**. `equal_vega`
acceptance is already covered (`dispersion_run_config_test.cpp:521`,
`DispersionX4.EqualVega_ChangesAllocation…`).

### ⏸ Deferred — final `disp-hotpath → main` integration (USER DECISION: defer)

**This is a 57-commit cross-sprint engine reconciliation, not a localized conflict.**
During this session `main` advanced from `8cb4576` to `3f7ba3f` — an **entire peer
"pricing/greeks SOTA sprint" landed** (tip `76ec4bf` = "sprint COMPLETE"). Scope of
`8cb4576..main` (measured):
- **57 commits**, **~48 `atx-vol/src`+`include` engine files** changed — including the
  exact SIMD/greeks/American-pricing code this sprint modifies:
  `american_greeks_avx2.cpp`, `greeks_batch_avx2.cpp`, `american_boundary_batch.cpp`,
  `calib.cpp`, `session.cpp`, `parallel_for.hpp`.
- A dry merge yields **7 textual conflicts**: `listed_dispersion_strategy.{cpp}` +
  test, `spy_dispersion_backtest.cpp`, **`simd/american_boundary_batch.cpp`** (SIMD
  kernel), **`backtest_test.cpp`** (our zcfix regression tests vs peer changes),
  `american_batch_test.cpp`, `tests/CMakeLists.txt`. The earlier "3 conflicts" reading
  was against `main` @ `a0997cd`, before the bulk of the peer sprint landed, and it
  masked a large **auto-merged** semantic merge.
- **Byte-identity will legitimately break**: the golden dispersion replay reprices
  options through the American greeks kernel the peer rewrote, so the pinned SHAs
  (`0737660…`/`ac97a643…`) will not reproduce post-merge — a *peer* engine change, not
  a regression here. A **new post-peer golden baseline** must be established as part of
  the reconciliation.

**Decision (user): defer.** `feat/disp-hotpath` @ `30809a0` is complete, gated, and
byte-identical to its base `8cb4576` — that is the shipped deliverable. The
main-integration is a dedicated follow-up: with the peer sprint now reading COMPLETE,
merge (or rebase) `disp-hotpath` onto the settled `main`, resolve the 7 conflicts
against BOTH sprints' intent (watch for double-applied SIMD fixes — e.g. our
`095be4a` greeks-ledger invariance vs the peer's own AVX2 kernel work), re-establish
the golden on the post-peer engine, and re-gate. Not attempted this session because
the approval to "merge now" had been given on a materially wrong scope (5 commits).

### Corpus batching lever — measured NEGATIVE (WS-BATCH, now settled)

Phase-split on the golden corpus spec (`ATX_VOL_CORPUS_PHASE_TIMING=ON`, 82 fan-out
dates, 16 workers): **fan-out is ~97% of the build, ingest only ~1% (1–4 s).** So the
batching lever targets the right phase, but `batch 1→8` (`fanout_calls` 82→15,
**payload bit-identical** — manifest + quality TSV incl. the content-fingerprint row
show 0 diff) produced **no end-to-end win above ±40% host-load noise** (sign of the
delta flipped with contention). **The lever is not worth pursuing**; the real
bottleneck is fan-out worker utilization (~9/16), not fan-out invocation count. The
proposed "overlap ingest with fitting" follow-on also caps at ~1% → also not the
lever. The instrumentation is merged; the measurement re-confirmed the only container
non-reproducibility is `created_ts_ns` (see follow-on 1, now being implemented).

### `created_ts_ns` corpus reproducibility (old §5.2) — ✅ DONE, landed at `07950ec`

Implemented and gated. When `SurfaceArchiveWriteOptions::created_ts_ns` is the `0`
sentinel, the writer now fills it from **`crc32c` of the archive content payload**
(span `[sizeof(header), file_size)` folded with `file_size`), not the system clock —
deterministic across identical builds, distinct for distinct content, so
`SnapshotCache` staleness eviction is preserved (a constant would have broken it).
Header excluded from its own hash (no circularity); explicit-nonzero stamps honored
unchanged. **Consumer survey confirmed no site renders the field as a wall-clock
date** (the one display site, `db_stats.csv`, emits a raw integer by design), so a
content-derived value is safe. Files: `surface_archive.cpp:606-627`,
`surface_archive_v1.cpp:665-682` (+ removed the now-dead file-local `wall_clock_ns()`
to satisfy `-Werror`). Two non-vacuous reproducibility tests added
(`SurfaceArchiveV2.ContentDerivedCreatedTsIsReproducible`,
`CorpusBuildSession.DefaultStampBuildsAreByteReproducible`), both pass. Corpus policy
fingerprint (`corpus.cpp:483`) was already stable (folds the config sentinel `0`);
the container bytes are now reproducible too. Golden 82/135 replay unaffected
(write-path-only; reader + prebuilt archives untouched). Bonus: `cached_artifacts`'
cross-process "a losing rename means a byte-identical archive was already published"
claim, previously false under wall-clock stamps, is now true.

### Follow-ons (documented, deliberately NOT bundled into this merge)

1. **PricerFitter test over-assertion (WS-F-owned, pre-existing).** The test asserts
   a mid-preserving spread rescale must invalidate the certified cache; the design
   correctly does not (no stale data served). The *test* should be corrected to
   assert reuse-with-correct-IVs for a mid-preserving update. Pre-existing on `main`,
   out of this sprint's scope; route to the WS-F owner.

### Accepted, documented drift (unchanged from mid-sprint; re-confirmed)

Tolerance re-band values verified present in `isa_golden_tol.hpp` and confirmed to
clear all WS-P1a route-parity failures: `kAccumRelBand = 1e-11` (measured path),
`kAccumCloseRelBandFma = 1e-9` (unmeasured FMA path, deliberately unchanged),
`kLanedGreeksRelBand = 1e-9`, `laned_greeks_close` with a `scale==0 → exact` branch.
See the drift table in §4. The golden `final_nav` appears only in **comments/docs**
(no hard assertion); the current tip is self-consistent (gate green + byte-identity
match), so **no test re-pin is required** — at most a cosmetic comment-digit sync.
The stale `surface_backtest.tsv` (`76bdff4e…`) lives under `C:\atx-data\…` (reference
data, **must not be mutated**) and is not repo-tracked, so it is left as-is.

---

## 1. Standing authorization (from the user, verbatim)

> "Note you are allowed to break byte identical or tolerance tests if the
> performance gains are real, algorithmically correct, and differences are not
> economically meaningful as long as you document them."

Documentation is the *price* of that permission, not an afterthought. Every
relaxation this sprint carries measured old value, measured new value, the delta,
and an economic-materiality argument.

Other constraints in force:
- Databento real-data spend cap **$100** (unused, $0 spent).
- **Never mutate `C:\atx-data\...`** — reference data. Copy to scratch first.
- Merging to main was authorized in a prior session for that session's work only.

---

## 2. What is merged and sound

`feat/disp-hotpath` first-parent chain:

```
b79ac15  WS-ZC   zero-copy borrowed replay surfaces   <-- DEFECTIVE, see §3
fdfc418  WS-X    C1 activation, typed config, frictions/financing/costs/risk-limits
3e57edb  WS-P3   fit-scheduler affinity tier (default-off) + determinism guards
5812200  WS-P1v  unify batch seam (view gets laned path) + WS-P2 rho-drop tier
1ca3122  WS-C    dispersion correctness cluster (C1-C4)
d0cec36  WS-M    extract dispersion driver into a library seam
8929bea  WS-P1a/b laned AVX2 analytic greeks under Auto ISA
8cb4576  main
```

**Verified at `fdfc418`:** 17 serial failures (13 attributable to WS-P1a + 4
pre-existing). Every merge from `8929bea` through `fdfc418` added **exactly
zero** new failures.

### Wins banked

| Lever | Result |
|---|---|
| Laned greek bundles | **1** Andersen-Lake boundary solve instead of **5** (observed 12→2, 6→1, 7→2). Plan projected 5→3; actual is 5→1. |
| WS-ZC borrow (mechanism) | `snapshot_load` 1008.5 → 44.0 ms; `archive_map` 992.4 → 10.3 ms. Counters **identical** owned vs sealed (`cnt_boundary_solves = 1324` both) — proves load work removed without touching pricing work. |
| WS-P2 | rho solve-pair skipped when `dr == 0`; free, `pnl_rho` was already 0. |

### Biggest economic finding (WS-X, reproduced independently by WS-X-B)

With impact coefficients `k=0.02, β=0.6, participation=0.02`, retail preset:

| Regime | Return | Cost |
|---|---|---|
| Frictionless | **+247.41** | 0 |
| Retail frictions | **+12.81** | 234.60 |
| + Almgren √-impact | **−64.60** | 312.01 (**126% of gross**) |

**The pinned headline result is ~95% friction-dominated and flips sign under
modest impact.** Any report that shows only the frictionless number is
misleading. WS-X-B makes regime the first key of every artifact, with a
colour-coded banner, per-tile captions, and a renderer that hard-refuses a track
with no `friction_regime`.

---

## 3. 🔴 `b79ac15` IS NOT SOUND TO BUILD ON

An independent gate failed the WS-ZC merge. **Do not merge to main.**

**Defect:** `run_backtest` calls
`snapshot_cache->set_archive_backing(ArchiveBacking::Sealed)` **unconditionally**,
including on a cache the **caller** supplied and owns
(`backtest.cpp:1587`, `backtest.cpp:1994`).

WS-ZC correctly made backing part of the snapshot cache key — so flipping a
caller-owned cache mid-flight **orphans every entry the caller preloaded** under
the default `Mutable` backing.

Blast radius:
- Loads go **3 → 6**; follow-up `ReuseOnly` misses and **silently downgrades**
  `RepresentativeFast(02)` → `ColdReference(01)`, `query_cache_pair_count` **1 → 0**.
- Reaches **production** at `dispersion_run.cpp:1561` — shared cache flipped to
  Sealed, then used for 82–135 post-run loads that now hold mappings open.
- **Not** covered by `ATX_VOL_ZC_BACKING` / `ATX_VOL_ZC_BORROW` — both gate the
  loader, not the key.

Three tests entered the failing set (all confirmed pass@`fdfc418`, fail@`b79ac15`;
`git blame` shows the assertions predate WS-ZC — they are correct and must **not**
be relaxed):
- `Backtest.StrictPolicyValidatesPreloadedCacheInBothRunOverloads`
- `Backtest.ReuseOnlyRunUsesColdOnMissAndPreparedFastAfterExplicitPreload`
- `Backtest.AdaptiveStrategyHandlesFreshFullAndPartialFastResidency`

**WS-ZC's core claim survives intact** — byte-identity confirmed at both scales
under `cmp`, counters identical, all 57 contract tests green and unweakened
(the `Sealed`/`Mutable` split *does* hold the `8627ccb` line). Only the *scoping
to caller-owned caches* is broken.

**Fix in flight:** `feat/disp-zcfix` (worktree `wt-disp-zcfix`, off `b79ac15`).

---

## 4. Accepted, documented drift

| Item | Old | New | Delta | Status |
|---|---|---|---|---|
| Golden `final_nav` (WS-P1a) | `247.4065016443293` | `247.40650164556075` | 1.2e-9 abs, ~5e-12 rel (~1 nano-dollar on $247) | Accepted; confirmed twice independently. **Re-pin once at sprint end.** |
| `kAccumRelBand` | 1e-9 (FMA branch) | **1e-11** | *tightened* ~20× above two independent measurements (5.13e-13 dev, ~5.5e-13 rel-avx2) | Accepted |
| `kLanedGreeksRelBand` | bit-equality | **1e-9** | 7.46× over measured worst (gamma 1.34e-10) | Accepted |
| `golden_accum_close` FMA path | 1e-9 | **unchanged** | — | Deliberately left alone as *unmeasured* |

**Third nav value observed:** WS-X-B measured `247.40650164561367` on the **dev**
preset. Leading hypothesis is preset (FMA contraction), since the pins were taken
on `rel-avx2`. **Unconfirmed — must be verified under rel-avx2** (§6, WS-X-B item 3).
Its *inertness* claim is unaffected (same-preset before/after, identical hashes).

### Route-parity reframing (WS-FIX)

Several tests used `bits_equal`/`EXPECT_EQ` on doubles to assert what is really a
**route-parity** claim (batched vs single-contract re-query; analytic vs FD), not a
golden value pin. Since WS-P1a ships the laned kernel under **Auto ISA** — selected
at *runtime* by CPU capability, not by how the TU was compiled — the old
"reference ISA is byte-exact" premise is void. These are now relative bands with
measured worst cases in `atx-vol/tests/support/isa_golden_tol.hpp`.

---

## 5. Cross-cutting findings (higher value than any single lever)

### 5.1 🔴 Vacuous verification — 3 confirmed instances, sweep in flight

Checks that appear to assert a property but **have no way to observe it**. They
report green having checked nothing — worse than a missing test, because they read
as protection.

1. `ArchivedSnapshotDefaultsColdAndPreparesEverySurfaceForRequestedTier` —
   `query_pricing_route` loop iterates `surfaces()`, **empty on a borrowed load**.
   Body never executes.
2. Strict-config contract check compared parsed **value** vs default, so
   `strike_abs_delta=0.25` under the default rule was **silently inert**. Could not
   distinguish "set to default" from "never set."
3. `DefaultPolicies_ReproduceShippedBookBitForBit` — **the test pinning a
   workstream's bit-identity claim** — guarded with `ASSERT_EQ(size(a), size(b))`.
   Asserts counts *agree*, not that either is non-zero. Two empty baskets pass.

**The tell:** *the check never had a way to observe the difference it was
asserting about.* Sub-shapes: empty subject; value indistinguishable from its own
default; symmetric guard both sides satisfy at zero.

`WSVAC` is sweeping the whole suite read-only, ranked by blast radius.

### 5.2 🔴 Corpus builds are not byte-reproducible run-to-run (pre-existing)

`SurfaceArchiveWriteOptions::created_ts_ns` defaults to 0 = "fill from system
clock", and the **production** path never pins it. Unit tests miss it because they
set `created_ts_ns = 1`. Two back-to-back identical builds produce 82 differing
archives (same total bytes; manifest/quality TSVs SHA-identical — **the fits agree,
the containers differ**).

**DO NOT "fix" this by pinning to a constant.** `created_ts_ns` is load-bearing:
`surface_archive.hpp:136-146` defines `ArchiveContentIdentity{file_size,
created_ts_ns, header_crc32c, metadata_crc32c}` and `SnapshotCache` "keys/evicts
on it so a rewritten archive never serves a stale cached snapshot" (R-19/F6).
A constant would make two *different* builds at the same path share an identity →
cache serves stale surfaces indefinitely. **WS-ZC's borrow holds mappings against
these archives, so identity-based eviction matters more now, not less.**

**Correct resolution:** a **content-derived** stamp (hash of payload), which is
deterministic across identical builds *and* differs when content differs.
Blast radius includes `corpus.cpp:481`, which folds `created_ts_ns` into the
corpus fingerprint — **so the corpus fingerprint also varies run-to-run today.**

**PM-owned. Not yet scheduled to an agent.**

### 5.3 Six plan premises falsified by measurement

| # | Premise | Reality |
|---|---|---|
| 1 | P1 packing target | Entered ~32× in 82 sessions. Declined. |
| 2 | P3.1 idle E-cores | `build_corpus` uses `FitAffinity::None`; E-cores never idle. Lever *regresses* 9.94→12.25→15.45 s. |
| 3 | P3.2 SIMD de-Am | Kernel feeds a bisection; reassociation shifts σ and surface bytes. Declined. |
| 4 | P4 / ZC2 prefetch | **Already implemented and enabled** (`backtest.cpp:1925/1997`). |
| 5 | Warm-start chain couples dates | `warm_start_chain` defaults **false**; only set true in a unit test. Cross-date batching is byte-preserving. |
| 6 | Corpus parallelism 3.4/16 | **Does not reproduce** — measured 8.78/16 (dev, loaded host; both caveats acknowledged). Phase split will settle. |

Also retracted mid-sprint: WSBATCH's own board-skew explanation (SPY is 6× the
median single name but only **9.3%** of a date's bytes — cannot pin parallelism at
3.4/16). **I had endorsed that premise, which made it harder to retract.**

### 5.4 🔴 Tooling: `rtk` — 5 confirmed incidents

- **4× output truncation** (including hijacking `grep -h`).
- **1× working-set destruction.** `rtk` intercepts `git diff` and writes a
  *compacted human summary* into the target file — right filenames, right ±counts,
  plausible length, **zero `diff --git`/`@@` headers**. An agent parked work with
  `git diff > wip.patch`, ran `git checkout --` then `git apply`, got
  `No valid patches in input`, and lost 5 files.

**This was a PM error** — `git diff > file.patch` was the park protocol *I*
mandated after banning `git stash`. Corrected protocol:

- **PRIMARY: commit.** `git add -A && git commit -m "wip: ..."` on your own branch.
- **SECONDARY:** `cp` to scratch (rtk does not intercept `cp`).
- Any patch file: validate with `grep -c "^diff --git"` **before** relying on it.
- **`git stash` remains BANNED** — repo-global stack shared across all worktrees;
  6 live entries from other workstreams were confirmed present.
- Byte comparisons: **`cmp` or SHA256, never `diff`.**

> Transferable lesson, in the affected agent's words: *"I had a verification
> artifact and no actual backup, and I did not notice the difference."* A SHA256
> baseline proves a restore worked; it cannot perform one.

Work was **fully recovered** — reconstruction verified by identical SHA256
(`5d1324f3…`) across all 17 pre-loss measurement lines.

### 5.5 Test-harness instability, not engine instability

- `ctest -j 16` inflates the failing set by **26** (48 vs 22 serial, full 5453-test
  sweep). My earlier estimate of "up to 7" was badly low. **Always attribute serially.**
- 8 `Earnings*` failures were an agent artifact — running from `build\bin`, where
  fixture paths don't resolve. All 10 pass from the correct CWD.
- The "3 new backtest failures" from an earlier agent **do not reproduce**.

**Every apparent failure this sprint has traced to harness or environment, never
the engine.** Carry that prior.

### 5.6 Infrastructure gaps

- **Shared vcpkg lock.** `FETCHCONTENT_BASE_DIR` isolates spdlog per worktree, but
  nothing isolates `C:\atx-cache\vcpkg_installed`. N worktrees serialize; 25–40 min
  configure waits observed. **Do not work around with a private vcpkg root while a
  byte-identity claim is pending** — keep dependency provisioning out of that
  claim's causal history.
- **Worktrees do not inherit submodule checkouts.**
  `git submodule update --init atx-core/third-party/databento-cpp` per worktree.
- **Examples are behind `ATX_BUILD_EXAMPLES`** (dev preset leaves it OFF).
  `atxvol_spy_dispersion_backtest` won't exist without it — reads like a build failure.
- `atx-build.ps1` verb quirks: `configure` hardcodes `cmake --preset dev` (drops
  isolated deps) and `build` hardcodes `--target`. **Use the raw pass-through form**
  (it already prefixes `cmake`); build with raw `--build <dir> -j 6`.

---

## 6. Agents in flight

| Agent | Branch / worktree | State |
|---|---|---|
| **WSFIX2** | `feat/disp-gate` / `wt-disp-gate` | **RUNNING** — owns the quiet window. Flaky study: 20 serial + 20 `-j 16`, interleaved. |
| **WSZCFIX** | `feat/disp-zcfix` / `wt-disp-zcfix` | **FROZEN** — designing the §3 fix. |
| **WSBATCH** | `feat/disp-batch` / `wt-disp-batch` | **FROZEN, staged** — `6fddfba` phase instrumentation, *uncompiled*. Next in bench queue. |
| **WSXB** | `feat/disp-xb` / `wt-disp-xb` | **FROZEN** — 8 commits, functionally complete. |
| **WSVAC** | `wt-disp-hotpath` (read-only) | **RUNNING** — vacuous-verification sweep (read-only, permitted under freeze). |

### Per-agent outstanding work

**WSFIX2** — reconstruction verified; `1e-11` ship-blocker resolved by *scoping*
(`kAccumRelBand` tightened where two independent measurements agree;
`golden_accum_close` keeps 1e-9 on the unmeasured path; constants split so
tightening one cannot drag the other). Outstanding: the flaky verdict itself —
CPU contention vs memory starvation vs genuine engine nondeterminism, with
failure **mode** recorded per failure. Early datum argues *against* its own memory
hypothesis (~0.03 GB per test process → 16 ≈ 0.5 GB; the ~3.6 GB free was the
other worktrees' `clang-cl`). **If value mismatches appear under `-j 16`, that
falsifies the documented `parallel_for.hpp:43` bit-identity invariant → named
engine bug, escalate immediately.** Scope: atx-vol-tests only (1932 tests).

**WSZCFIX** — fix shape is open: scope Sealed to privately-owned caches, or
save/restore the caller's backing. Latitude granted to improve the *contract*
(backing requested per-load, or a cache handle carrying its backing) rather than
keep a setter that mutates caller state. Must also fix the §5.1-instance-1 vacuous
assertion and add a regression test that **fails without the fix**.

**WSBATCH** — measurement plan approved: (1) compile, (2) **phase split at
batch=1 — this decides everything**, (3) phase split at batch=8, (4) best-of-N,
(5) payload identity batch=1 vs 8 *plus a batch=1-vs-1 control* so the timestamp
confound stays visible, (6) worker-count invariance via `fit_workers` in the run
spec (not an env var — that's the field that reaches `CorpusConfig::n_threads`).
Instrument prints unattributed time as `other_s` rather than folding it into a
named phase. **A correctly-sized negative result is the expected and acceptable
outcome.**

**WSXB** — runbook approved: (1) build+verify `ac8758e`/`7491f16` (**unbuilt,
blocking**), (2) measured baseline delta *serially* via `git checkout b79ac15 --
atx-vol/` for sources *and* tests, plus re-run the 82/135 replay since `ac8758e`
touched `dispersion_run.cpp` after the byte measurement, (3) **rel-avx2 replay to
resolve the third nav value** — must reproduce `0737660775…` / `ac97a643851f…`
exactly, else there is real drift between WS-P1a and `b79ac15`.

**WSVAC** — read-only sweep; may build only after `CLEAR` to promote SUSPECTED →
CONFIRMED. Must not fix anything without routing (files are owned by active agents).

---

## 7. ⚠️ IMMEDIATE — the freeze must be resolved

A host-wide build freeze was issued so WSFIX2's oversubscription study would run on
a quiet box (it measures a contention effect, so background compilation lands
directly in its dependent variable).

**WSBATCH, WSXB, WSZCFIX are all parked waiting for a `CLEAR` message.**

Options:
1. **Wait for WSFIX2's study**, then send `CLEAR` — bench queue order is
   **WSBATCH first** (phase split), then WSXB / WSZCFIX / WSVAC.
2. **Lift the freeze now**, accepting that the flaky study is contaminated and must
   be re-run later.
3. **Stop all agents** and resume from this document.

Doing nothing strands four agents indefinitely.

---

## 8. Next steps, in order

1. **Resolve the freeze** (§7).
2. **Land WSZCFIX** and re-gate. `b79ac15` must return to **17 serial failures**
   (matching `fdfc418`) with byte-identity preserved at both scales and the Sealed
   perf win intact on the private replay path.
3. **Collect WSFIX2's flaky verdict.** If it names genuine engine nondeterminism,
   that outranks every remaining perf item.
4. **WSBATCH phase split.** If ingest dominates, say so and size the batching lever
   honestly; the follow-on lever would be *overlapping ingest with fitting*
   (scoped, ~1 day, **not started** — order-preservation of `session.finish()` and
   the input fingerprint is the primary invariant and needs its own byte gate).
5. **WSXB rel-avx2 replay** — resolve the third nav value before anything merges.
6. **WSVAC findings** — route fixes to owning worktrees.
7. **Schedule the `created_ts_ns` content-derived stamp** (§5.2). PM-owned.
8. **Re-pin the reference golden ONCE** (1.2e-9) and fold §4, §5.1, §5.3, §5.4 into
   the sprint doc.
9. **Final:** quiet-host `rel-avx2` best-of-7, determinism proof, PnL track
   regeneration, merge to main.

---

## 9. Reference data & repro

- **Corpus/archives:** `C:\atx-data\spy-dispersion\opra` (7021 parquet, 1.15 GB).
- **Golden run:** `C:\atx-data\spy-dispersion\runs\bt-sota-baseline\` —
  `run_spec.tsv` + `archives\` with **85 prebuilt `.atxvsa`** → replay-only (~0.2 s),
  **no corpus rebuild needed**.
- **Run spec:** date_lo 2026-01-02, date_hi 2026-04-30, snapshot suffix
  `T19:55:00Z`, flat_rate 0.043, min_names 10, min_weight_coverage 0.8,
  target_dte_days 30, min_dte 21, max_dte 60, roll_dte 7,
  gross_index_vega 10000, delta_band 0, fit_workers 0, core_mode 0.
- **Universe:** single `effective_date` block (2026-01-02), 10 equal-weight names
  (AAPL AMZN AVGO LLY GOOGL JPM META MSFT NVDA XOM). **No reconstitution in-window**
  → PIT activation cannot move the golden on this fixture.
- **Expected SHAs (rel-avx2, `sha256(surface_backtest.tsv)`)** — re-pinned post-merge
  2026-07-21 (WS-M M2), engine = main A9 kernel + branch tolerance re-band, 3×-stable
  (pre-merge values in strikethrough moved once via the peer A9 greeks kernel):
  - 82-session `5e7ca06514dfe121308643cc431c90858827c180fdbc9c84e906a49fe4715af4`
    (was `0737660775601f1609690568d930c62c46a1dddd0d97784036916ba4c5484c3a`)
  - 135-session `141173fdc35eed9fbb0263c87729c547e9f0eac144c1c336173c932ac69f2835`
    (was `ac97a643851fa9988880d85af3201ef39158aa7e054f8c74062f0c4e68970b33`)

### Build incantation (per worktree)

```powershell
Set-Location C:\atx-wt\<wt>
git submodule update --init atx-core/third-party/databento-cpp
& C:\atx-wt\<wt>\scripts\atx-build.ps1 --preset dev `
    '-DFETCHCONTENT_BASE_DIR=C:/atx-wt/<wt>/deps/dev' -DATX_BUILD_EXAMPLES=ON
# build:  raw --build <dir> -j 6      (the `build` verb hardcodes --target)
# rel-avx2 perf/counters: add -DATX_VOL_COUNTERS=ON -DATX_VOL_PROFILE=ON
```

`-j 6` — the default `-j` OOMs on rel-avx2. Host has 16 cores / 15.69 GB.

### Stale worktrees to clean up

`C:\atx-wt\wt-zc-baseline` (detached at `fdfc418`, created by the gate) —
`git worktree remove` when done.
