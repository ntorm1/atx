# p7 Sprint 2 — Information Breadth — Full Report

**Status:** DONE
**Branch:** `feat/p7-s2`  base `2eaf3da` → head `bccfe5b`
**Worktree:** `C:\atx-wt\p7-s2`

Adds three signal families (FINRA short-interest, IV-surface, liquidity) as
opt-in augmented panel fields plus a multi-family seed catalog, breaking the
price/return monoculture identified in p6 research. No CLI (deferred to S7 per
decision D1). The default panel-build path is byte-identical to pre-sprint.

---

## Commits (one per unit)

| Unit | SHA | Title |
|---|---|---|
| S2-0 | `ae73fde` | docs(p7-s2): open sprint ledger + landing checklist |
| S2-1 | `b0f9c1f` | feat(p7-s2): land track-b FINRA short-interest (CLI-free) |
| S2-2 | `16be122` | feat(p7-s2): IV-surface derived fields with_iv_fields |
| S2-3 | `59d2f56` | feat(p7-s2): liquidity Amihud field with_liquidity_fields |
| S2-4 | `6314536` | test(p7-s2): multi-family seed catalog (short-interest + liquidity) |
| S2-5 | `cc9270c` | test(p7-s2): multi-family augmentation smoke |
| ledger | `bccfe5b` | docs(p7-s2): finalize sprint ledger |

Branch diff (`2eaf3da..HEAD`): 18 files, all inside the S2 Owns set. No
`config.{hpp,cpp}`, `dispatch.cpp`, `src/stages.hpp`, `oracle.hpp`,
`alpha101.txt`, `ts_ops.hpp`, or `alpha/vm.hpp` touched (verified by grep over
the branch name-only diff).

---

## Per-unit detail

### S2-0 — ledger + marker
Created `phase-2-progress.md` (base SHA, D1 CLI-deferral scope, unit checklist,
byte-identity gate). No source changes.

### S2-1 — land track-b FINRA short-interest (CLI-free)
Landed from `worktree-track-b-information-structure @ 8df5010`, adapted per D1 to
ship engine + pure augment core + tests + fixtures only:
- `data/finra_short.{hpp,cpp}` — axis-parametric causal FINRA loader
  (si_dtc/si_util/si_chg; publication-lag causality + forward-fill; shares|ADV
  util denominator). Landed verbatim (self-contained in `atx::engine::data`).
- `atx-impl/src/stage_augment.{hpp,cpp}` — **pure core only**:
  `augment_panel_with_finra(...)` + its `derive_shares` helper. The `run_augment`
  CLI stage (RunConfig / seg-axis reconstruction / `_symbology.parquet`) was
  **stripped** — it reads RunConfig fields that do not exist until the S7 CLI hub.
  The TU has no `stages.hpp` dependency.
- Tests: `FinraShort.*` (4) in `data/finra_short_test.cpp`; `Augment.*` (4)
  appended to the existing `augment_test.cpp`; `SeedParse.*` (3) from track-b.
- Fixtures: `iv_earnings_templates.txt`, `neutralized_templates.txt`.
- CMake: `finra_short.cpp` → `atx-engine/CMakeLists.txt`; `stage_augment.cpp` →
  `atx-impl/CMakeLists.txt`. No other CMake edits.

**Drift / deviations encountered (and resolved):**
1. **`augment_test.cpp` already exists on main and is a DIFFERENT file.** On
   current main `augment_test.cpp` is the p6-S5 suite for `with_alpha101_fields`
   (`WithAlpha101Fields.*` ×5 + `DelegationIdentity.*` ×1) — the plan/brief
   assumed it was the FINRA file (true only on the older track-b branch). Rather
   than clobber the 6 pre-existing tests, the 4 track-b `Augment.*` tests were
   **appended** to the existing file in their own `atxtest_augment` namespace
   (existing tests are in an anonymous namespace; no symbol collision). Both
   suites now coexist and are green. This honors the brief's "single
   augment_test.cpp" target while preserving zero regressions.
2. **`seed_parse_test.cpp` `#include "config.hpp"`.** Track-b's file included
   `config.hpp` for `ATX_IMPL_TESTS_DIR`. On current main that resolves to the
   **S7-owned `atx-impl/src/config.hpp`** (on the `atx-impl-core` include path),
   which is forbidden and does not define the macro. Replaced the include with
   the standard `#ifndef ATX_IMPL_TESTS_DIR` fallback used by
   `alpha101_orats_test.cpp`; the macro is supplied by the tests CMake
   compile-def. No dependency on `src/config.hpp`.
3. **`/WX` unused-const.** Dropped track-b's unused `kNaN` in
   `finra_short_test.cpp` (clang-cl `-Wunused-const-variable` is fatal under
   first-party `/WX`).
4. B4 (`stage_regime_oos.cpp` / `regime_oos_test.cpp`) intentionally NOT landed.
   `git show --stat` of S2-1 lists exactly the 11 expected files.

### S2-2 — `with_iv_fields` (IV-surface)
Appended to `augment.hpp`:
- `iv_term = zscore(atmCenI_21d / atmCenI_126d)` — cross-sectional sample z
  (ddof=1) per date over in-universe non-NaN cells, NaN where valid count < 2
  (mirrors `cs_zscore_row`).
- `iv_vrp  = atmCenI_21d - ts_std(returns, 21)` — causal trailing sample std,
  full-window / any-NaN → NaN (same policy as `datafields::rolling_mean`).
- `iv_lo   = atmCenI_21d / (nEarnCnt_5d + 1.0)` — `nEarnCnt_5d` optional;
  denominator defaults to 1.0 (documented fallback).
Requires `atmCenI_21d` and `returns` (Err(NotFound) otherwise). Idempotent via
`detail::has_field`; appends at END. Two detail helpers added
(`cs_zscore_row_aug`, `rolling_sample_std`). `IvFields.*` (5) green.

### S2-3 — `with_liquidity_fields` (Amihud illiquidity)
Appended to `augment.hpp`:
- `illiq = group_neutralize(zscore(-1 * adv20), sector)` — cross-sectional
  sample z of negated adv20, demeaned within sector. Requires `adv20`
  (Err(NotFound) otherwise); `sector` optional (absent → global demean).
Idempotent; appends at END. Detail helper `group_demean_row` (CsNeutG semantics,
ascending-instrument reduction). `LiquidityFields.*` (5) green.

**Unit-split note:** S2-2 and S2-3 both live in `augment.hpp`. Committed as two
clean, individually-compiling commits by removing the liquidity code + test
around the S2-2 commit, then restoring them for S2-3. The S2-3 augment.hpp diff
is exactly +138 lines (group_demean_row + with_liquidity_fields), confirming the
S2-2 commit was self-consistent.

### S2-4 — multi-family seed catalog
- `short_interest_seeds.txt` (10: si1..si10) — DTC / utilization / short-change,
  sector-neutral; each references ≥1 of si_dtc/si_util/si_chg.
- `liquidity_seeds.txt` (8: liq1..liq8) — illiq / adv20 / dollar_volume,
  sector-neutral; each references ≥1 of those.
- `seed_parse_test.cpp` +2 tests (`ShortInterestSeedsParsesAndTypechecks`,
  `LiquiditySeedsParsesAndTypechecks`) parsing + typechecking every line via the
  existing `parse_fixture_file` against `make_multi_family_panel` (carries all
  four-family fields). B2/B3 tests unchanged; `alpha101.txt` untouched.
`SeedParse.*` (5 = 3 track-b + 2 new) green.

### S2-5 — multi-family smoke
`multi_family_smoke_test.cpp` (`MultiFamilySmoke.*` ×5):
- `PriceReturnsFamily` / `IvSurfaceFamily` / `LiquidityFamily` — one seed per
  family compiles + evaluates through `parse_expr`/`analyze`/`compile`/
  `Engine::evaluate` on the fully augmented panel, ≥1 finite cell.
- `AugmentedPanelFieldCount` — exact count = base + 8 (alpha101) + 3 (iv) + 1
  (illiq); each derived name present exactly once.
- `OffPathDigestUnchanged` — `with_alpha101_fields(base,{20})` digests
  identically across two calls (local FNV-1a NaN-safe panel digest) and carries
  NO opt-in iv/liquidity column — additive opt-in contract proven at test level.

**Drift:** panel uses **25 dates** (not the plan's nominal 15). `adv20 =
ts_mean(dollar_volume, 20)` requires a full 20-date trailing window, so a
15-date panel leaves `adv20` — and therefore `illiq`, derived from it — all-NaN,
and `LiquidityFamily` (≥1 finite cell) cannot pass. 25 dates keeps the panel
tiny while making the liquidity family evaluable. Documented in the test + the
commit message.

---

## Test results (final)

| Suite | Result |
|---|---|
| `FinraShort.*` (S2-1) | 4/4 green |
| `Augment.*` (S2-1) | 4/4 green |
| `SeedParse.*` (S2-1+S2-4) | 5/5 green (3 track-b + 2 new) |
| `IvFields.*` (S2-2) | 5/5 green |
| `LiquidityFields.*` (S2-3) | 5/5 green |
| `MultiFamilySmoke.*` (S2-5) | 5/5 green |
| `WithAlpha101Fields.*` + `DelegationIdentity.*` (pre-existing) | 6/6 green (preserved) |
| **Byte-identity gate** `factory *Oracle*:*Golden*:*Digest*` | **18/18 green** |
| Full alpha suite (`atx-engine-alpha-tests`) | 585/585 green |
| Full data suite (`atx-engine-data-tests`) | 118 pass / 3 env-skip |
| Full impl suite (`atx-impl-tests`) | 169 pass / 4 env-skip |

New tests added by S2: 28 (4 FinraShort + 4 Augment + 2 SeedParse + 5 IvFields +
5 LiquidityFields + 5 MultiFamilySmoke + the 3 track-b SeedParse landed in S2-1).

The byte-identity gate was confirmed green BEFORE S2-1 (pre-change), AFTER S2-1,
AFTER the S2-2/S2-3 augment.hpp changes, and AFTER S2-5 — proving the landing and
the three new opt-in families are non-destructive to the default path.

Env-gated skips (expected, not regressions): `DataUniverse.SurvivorshipCaveat…`,
`OratsE2ESmoke.*` (×2), `Alpha101Orats.RankBy…` (×2), `AtxImplDiscover.W6_…`,
`SingleAlphaCapacity.SweepAndVerify` — all require real ORATS data / optimizer
fixtures not present in CI.

---

## Self-review (agent.md §10)

- No UB; no narrowing; no uninitialized vars; no raw owning pointers. New helpers
  take `std::span` (non-owning) and write into caller-provided spans.
- All inputs validated at the boundary: `with_iv_fields` / `with_liquidity_fields`
  return Err(NotFound) on missing required fields; loaders validated in track-b.
- `const` / `[[nodiscard]]` applied; leaf helpers `noexcept` where they do not
  allocate (`cs_zscore_row_aug`, `group_demean_row` are span-only); allocating
  helpers (`rolling_sample_std`) are not noexcept by design.
- Loops bounded by dates/instruments/window; functions single-purpose.
- Idempotency / inert-default contract holds (proven by OffPathDigestUnchanged +
  the byte-identity gate).
- No out-of-scope edits; no TODO/stub/fake-success; no CLI references to RunConfig
  augment fields (D1).
- `/W4 /permissive- /WX` clean (one track-b `/WX` issue fixed). clang-format not
  re-run by hand but new code follows the surrounding 2-space style; clang-tidy
  not run (disabled in repo per agent.md §8).

## Build / environment notes for the reviewer

- Configure once: `cmake --preset dev -DATX_UNITY_BUILD=OFF
  -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -Wno-unknown-argument"
  -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS -Wno-unknown-argument"` inside vcvars64.
- The inline `cmd /c '…'` form from the protocol did not pass the command through
  in this Git-Bash environment (it dropped into an interactive cmd and exited 0
  with no work done). Worked around by writing the configure/build incantations
  to `.bat` files in the scratchpad and invoking `cmd //c <bat>`. Reviewers on a
  VS Developer shell can use the documented form directly.
- New `*_test.cpp` files auto-glob (CONFIGURE_DEPENDS) into
  `atx-engine-alpha-tests`; a reconfigure is needed after ADDING a test file.

## Not done / out of scope (by design)

- CLI threading for `augment` / `--iv-fields` / `--liquidity-fields` (S7).
- B4 regime-OOS analyzer (out of scope).
- True GICS industry/subindustry ingestion (I5-HOOK marker in augment.hpp).
- Numerical OOS back-testing of the new signals (S7 discover loop).
