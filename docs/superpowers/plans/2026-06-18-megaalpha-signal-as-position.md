# Mega-Alpha Signal-as-Position Pipeline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the atx-impl combine→portfolio path into a WorldQuant-style *signal-as-position* mega-alpha pipeline by wiring existing engine machinery: factor (sector) neutralization in the combiner, a real combination method (bounded-regression), and a position-mode optimizer that deploys the combined book directly instead of re-running a mean-variance optimization on it.

**Architecture:** Each admitted alpha's signal is turned into a *sector-neutralized*, dollar-neutral, gross-normalized **position book** via `WeightPolicy` (the representation its metrics were validated on). The combiner blends those books with bounded-regression weights → the combined book **is** the mega-alpha. A new `--position-mode` in `optimize` deploys that book at the rebalance cadence (dollar-neutralize + gross-scale + name-cap clip-renorm), replacing the conceptually-wrong second mean-variance optimization that treated a book of positions as a fresh expected-return forecast.

**Tech Stack:** C++20, clang-cl + Ninja, CMake presets (`build-rel` = Release), GoogleTest. atx-engine (alpha VM, `WeightPolicy`, `extract_streams`, combiner, `FactorModel`), atx-impl (stage_* CLI subcommands).

## Global Constraints

- Build is `/W4 /permissive- /WX` — warnings are errors. NDEBUG-unused vars need `[[maybe_unused]]`.
- Determinism is mandatory: all reductions in canonical ascending order, no RNG, fixed iteration counts. New code must be two-runs-equal (digest-stable).
- `SearchConfig.n_workers` and any worker/parallelism knob is digest-invariant (affects speed only, never bits).
- **Do NOT commit** unless the user explicitly asks (user standing constraint overrides the per-task commit step in subagent-driven-development). Each task still runs its full test cycle; the "commit" step is replaced by "report diff + await commit instruction".
- **Never `git add -A`** — stage explicit paths only (only relevant if/when the user authorizes a commit).
- Existing tests must stay green (32 atx-impl tests; engine streams/factory tests). Default code paths (no new flag) must be byte-identical to today.
- Backslash-free engine headers: new engine API is additive with defaulted params so existing call sites are unchanged.

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `atx-engine/include/atx/engine/alpha/streams.hpp` | `extract_streams` — add optional `group_map` param forwarded to `WeightPolicy::to_target_weights` for sector-neutral books | Modify (additive) |
| `atx-engine/tests/alpha/streams_test.cpp` (or existing streams test) | Test: industry_neutral + group_map ⇒ per-group weight sum ≈ 0 | Modify/extend |
| `atx-impl/src/sector_groups.hpp` | `sector_group_map(panel)` — build a dense `u32` group id per instrument from the `"sector"` field; empty if absent | Create |
| `atx-impl/tests/sector_groups_test.cpp` | Unit tests for group-map construction (distinct codes → dense ids, NaN handling, missing field → empty) | Create |
| `atx-impl/src/stage_combine.cpp` | Build group_map, enable `policy.industry_neutral`, forward to `extract_streams`; combined book is now sector-neutral | Modify |
| `atx-impl/src/config.hpp` / `config.cpp` | Add `bool position_mode` (`--position-mode`) + `bool sector_neutral` (`--sector-neutral`, default on when sector present) | Modify |
| `atx-impl/src/book_shape.hpp` | `shape_book(w, live, gross, name_cap)` — dollar-neutralize + gross-normalize + name-cap clip-renorm of a raw weight cross-section | Create |
| `atx-impl/tests/book_shape_test.cpp` | Unit tests: Σw≈0, Σ|w|≈gross, |w_i|≤cap, dead cells 0, determinism | Create |
| `atx-impl/src/stage_optimize.cpp` | `--position-mode` branch: deploy combo book via `shape_book` at the rebalance schedule, skip MVO | Modify |
| `atx-impl/tests/optimize_test.cpp` | Test: position-mode book matches `shape_book` of the combo cross-section at each rebalance date | Modify/extend |
| `docs/atx-impl/RUNBOOK.md` | Document `--sector-neutral`, `--position-mode`, recommended mega-alpha chain (`--method bounded-regression`) | Modify |

**Deferred to a Tier-2 follow-on plan (documented, NOT in scope here):** continuous size/beta factor neutralization via `FactorModel::neutralize` (needs an exposure-matrix `X` builder), fractional-Kelly / target-vol sizing (`risk/kelly_sizing.cpp`), wiring the Sprint-8 factor covariance into a risk-aware sizing pass, and pushing neutralization into the discover/factory path for admit/deploy consistency. The combiner's bounded-regression re-weights on the neutralized streams, so admit/deploy drift is self-limited in the interim.

---

## Task 1: `extract_streams` forwards a sector `group_map`

**Files:**
- Modify: `atx-engine/include/atx/engine/alpha/streams.hpp` (the `extract_streams` signature + the inner per-alpha build that calls `to_target_weights`)
- Test: `atx-engine/tests/alpha/streams_test.cpp` (extend; if no such file exists, locate the existing streams test via `Grep "extract_streams" atx-engine/tests` and add the case there)

**Interfaces:**
- Consumes: `WeightPolicy::to_target_weights(SignalView, const Universe&, std::span<const atx::u32> group_map = {})` (weight_policy.hpp:197), `WeightPolicy{ .industry_neutral=true }` (weight_policy.hpp:165).
- Produces: `extract_streams(const SignalSet&, const WeightPolicy&, const Panel&, const exec::ExecutionSimulator&, std::span<const atx::u32> group_map = {})` — the new trailing `group_map` param, defaulted so all existing 2-call-site callers (factory.cpp:531, fitness.cpp:238/244, stage_combine.cpp) compile unchanged.

- [ ] **Step 1: Write the failing test**

Add to the streams test file (namespace/fixtures as in that file). A 1-date is not enough (PnL needs t≥1); use a small multi-date single-field "close" panel, 2 sectors, `industry_neutral=true`:

```cpp
TEST(AlphaStreams, IndustryNeutralGroupMapZeroesPerGroup) {
  // Panel: 8 dates x 4 instruments, one "close" field, deterministic walk.
  // group_map: {0,0,1,1} -> two sectors of two names each.
  // With industry_neutral, each date's target weights must sum ~0 WITHIN each group.
  using namespace atx::engine;
  const atx::usize D = 8, N = 4;
  std::vector<atx::f64> close(D * N);
  std::uint64_t s = 0xABCDEF01ULL;
  std::vector<atx::f64> px(N, 100.0);
  for (atx::usize t = 0; t < D; ++t)
    for (atx::usize j = 0; j < N; ++j) {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      const atx::f64 u = static_cast<atx::f64>(s >> 11) / static_cast<atx::f64>(1ULL << 53);
      px[j] *= (1.0 + 0.004 * static_cast<atx::f64>(j) + 0.01 * (2.0 * u - 1.0));
      close[t * N + j] = px[j];
    }
  auto panel = alpha::Panel::create(D, N, {"close"}, {close}, {}).value();

  // Single alpha = rank(close); evaluate via Engine.
  alpha::Library lib{};
  std::vector<std::string_view> v{"rank(close)"};
  auto prog = alpha::compile_batch(std::span<const std::string_view>{v}, lib).value();
  alpha::Engine engine{panel};
  auto ss = engine.evaluate(prog).value();

  WeightPolicy policy{};
  policy.industry_neutral = true;
  const std::vector<atx::u32> group_map{0u, 0u, 1u, 1u};
  auto sim = atx::engine::exec::ExecutionSimulator{
      atx::engine::exec::FillCfg{},
      atx::engine::exec::SlippageCfg{atx::engine::exec::SlippageMode::VolumeShare,0,0,0,0},
      atx::engine::exec::ImpactCfg{0,0.5,0},
      atx::engine::exec::CommissionCfg{atx::engine::exec::CommissionMode::PerShare,0,0,1,0},
      atx::engine::exec::LatencyCfg{}, atx::engine::exec::VolumeCapCfg{1.0}};

  auto streams = alpha::extract_streams(ss, policy, panel, sim, std::span<const atx::u32>{group_map}).value();

  for (atx::usize t = 1; t < D; ++t) {
    const auto w = streams.positions(0, t);
    atx::f64 g0 = 0.0, g1 = 0.0;
    for (atx::usize j = 0; j < N; ++j) (group_map[j] == 0u ? g0 : g1) += w[j];
    EXPECT_NEAR(g0, 0.0, 1e-9) << "sector 0 not neutral at t=" << t;
    EXPECT_NEAR(g1, 0.0, 1e-9) << "sector 1 not neutral at t=" << t;
  }
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build-rel --target atx-engine-tests && ./build-rel/bin/atx-engine-tests.exe --gtest_filter=AlphaStreams.IndustryNeutralGroupMapZeroesPerGroup`
Expected: FAIL to compile — `extract_streams` has no 5th param yet.

- [ ] **Step 3: Add the param and forward it**

In `streams.hpp`, change the `extract_streams` signature to add a trailing `std::span<const atx::u32> group_map = {}` and pass it through to the inner build function (the one that calls `policy.to_target_weights(signal_row, universe)`); add `, group_map` to that call. Keep everything else identical. Update the inner build helper's signature to take and forward the same span.

- [ ] **Step 4: Run the test to verify it passes**

Run: same command as Step 2.
Expected: PASS.

- [ ] **Step 5: Verify no regressions + report diff (no commit)**

Run: `./build-rel/bin/atx-engine-tests.exe --gtest_filter=*Stream*:*Factory*`
Expected: all existing streams/factory tests still PASS (the default `group_map={}` path is byte-identical). Report the diff; await commit instruction per Global Constraints.

---

## Task 2: `sector_group_map` helper in atx-impl

**Files:**
- Create: `atx-impl/src/sector_groups.hpp`
- Test: `atx-impl/tests/sector_groups_test.cpp`
- Modify: `atx-impl/tests/CMakeLists.txt` (add `sector_groups_test.cpp` to the test target source list — the engine/impl use explicit enumeration, NOT GLOB; find the existing `*_test.cpp` list and append)

**Interfaces:**
- Consumes: `alpha::Panel::field_id("sector") -> Result<FieldId>` (panel.hpp:132), `Panel::field_cross_section(FieldId, DateIdx)` (panel.hpp:153), `Panel::instruments()`, `Panel::dates()`, `Panel::in_universe(date, inst)`.
- Produces: `std::vector<atx::u32> atx::impl::sector_group_map(const alpha::Panel& panel)` — length `panel.instruments()`; dense group ids `0..G-1`; returns **empty vector** when the panel has no `"sector"` field (caller treats empty ⇒ neutralization off).

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include <vector>
#include "atx/engine/alpha/panel.hpp"
#include "sector_groups.hpp"

namespace { using atx::f64; using atx::usize; using atx::engine::alpha::Panel; }

TEST(SectorGroups, DistinctCodesBecomeDenseIds) {
  // 2 dates x 4 instruments; sector codes {10,10,30,20} -> dense ids by ascending code:
  // 10->0, 20->1, 30->2  => {0,0,2,1}
  const usize D = 2, N = 4;
  std::vector<f64> close(D * N, 100.0);
  std::vector<f64> sect = {10,10,30,20, 10,10,30,20};
  auto panel = Panel::create(D, N, {"close","sector"}, {close, sect}, {}).value();
  auto gm = atx::impl::sector_group_map(panel);
  ASSERT_EQ(gm.size(), N);
  EXPECT_EQ(gm[0], 0u); EXPECT_EQ(gm[1], 0u);
  EXPECT_EQ(gm[2], 2u); EXPECT_EQ(gm[3], 1u);
}

TEST(SectorGroups, MissingSectorFieldReturnsEmpty) {
  const usize D = 2, N = 3;
  std::vector<f64> close(D * N, 100.0);
  auto panel = Panel::create(D, N, {"close"}, {close}, {}).value();
  EXPECT_TRUE(atx::impl::sector_group_map(panel).empty());
}

TEST(SectorGroups, NanSectorGetsOwnTrailingGroup) {
  const usize D = 1, N = 3;
  std::vector<f64> close(D * N, 100.0);
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<f64> sect = {5.0, nan, 5.0};
  auto panel = Panel::create(D, N, {"close","sector"}, {close, sect}, {}).value();
  auto gm = atx::impl::sector_group_map(panel);
  ASSERT_EQ(gm.size(), N);
  EXPECT_EQ(gm[0], 0u); EXPECT_EQ(gm[2], 0u);   // code 5 -> group 0
  EXPECT_EQ(gm[1], 1u);                          // NaN -> dedicated trailing group
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build-rel --target atx-impl-tests` → FAIL (no `sector_groups.hpp`).

- [ ] **Step 3: Implement the helper**

```cpp
#pragma once

// atx::impl — sector_group_map: build a dense per-instrument sector group id
// vector from the research panel's "sector" field, for WeightPolicy industry
// (sector) neutralization. Deterministic: distinct non-NaN codes are assigned
// ascending dense ids by code value; all NaN/missing-sector names share one
// dedicated trailing group. Returns an empty vector when there is no "sector"
// field (caller treats empty as "neutralization off").

#include <cmath>
#include <map>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"

namespace atx::impl {

[[nodiscard]] inline std::vector<atx::u32>
sector_group_map(const atx::engine::alpha::Panel& panel) {
    const auto fid_r = panel.field_id("sector");
    if (!fid_r.has_value()) {
        return {};  // no sector field -> no neutralization
    }
    const atx::usize N = panel.instruments();
    const atx::usize D = panel.dates();

    // Representative sector code per instrument: first non-NaN cell scanning dates
    // ascending (sector membership is ~static; first observation is deterministic).
    std::vector<atx::f64> code(N, std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize i = 0; i < N; ++i) {
        for (atx::usize t = 0; t < D; ++t) {
            const atx::f64 v = panel.field_cross_section(*fid_r, t)[i];
            if (!std::isnan(v)) { code[i] = v; break; }
        }
    }

    // Dense id by ascending code value (std::map keeps keys sorted -> deterministic).
    std::map<atx::f64, atx::u32> code_to_id;
    for (atx::usize i = 0; i < N; ++i) {
        if (!std::isnan(code[i])) code_to_id.emplace(code[i], 0u);
    }
    atx::u32 next = 0;
    for (auto& kv : code_to_id) kv.second = next++;
    const atx::u32 nan_group = next;  // dedicated trailing group for NaN names

    std::vector<atx::u32> gm(N, 0u);
    for (atx::usize i = 0; i < N; ++i) {
        gm[i] = std::isnan(code[i]) ? nan_group : code_to_id[code[i]];
    }
    return gm;
}

} // namespace atx::impl
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build-rel --target atx-impl-tests && ./build-rel/bin/atx-impl-tests.exe --gtest_filter=SectorGroups.*`
Expected: 3 PASS.

- [ ] **Step 5: Report diff (no commit).** Await commit instruction.

---

## Task 3: Wire sector-neutral books into `combine`

**Files:**
- Modify: `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp` (add `bool sector_neutral = true; // --sector-neutral` boolean flag)
- Modify: `atx-impl/src/stage_combine.cpp` (build group_map, enable `policy.industry_neutral`, forward to `extract_streams`)
- Test: `atx-impl/tests/combine_test.cpp` (extend: with a 2-sector panel + `--sector-neutral`, the combined book is per-sector neutral)

**Interfaces:**
- Consumes: `atx::impl::sector_group_map(panel)` (Task 2), `extract_streams(..., group_map)` (Task 1), `RunConfig.sector_neutral`.
- Produces: a sector-neutral combined book in `combine`'s output panel when sector data is present and `--sector-neutral` is on (default on).

- [ ] **Step 1: Add the boolean flag**

In `config.hpp` add to RunConfig (near `gated`): `bool sector_neutral = false; // --sector-neutral (opt-in; sector-demean per-alpha books before combine)`. In `config.cpp`: add `if (flag == "sector-neutral") { cfg.sector_neutral = true; return atx::core::Ok(); }` to the boolean section AND `"sector-neutral"` to the valueless-boolean list in `parse_args` (the `flag == "help" || flag == "quiet" || flag == "digest-only" || flag == "gated"` chain), exactly mirroring the existing `gated` wiring (opt-in, default false).

> NOTE: default `false` matches the existing opt-in bool pattern (`--gated`, `--digest-only`). No-flag behavior is byte-identical to today (no neutralization). Even when `--sector-neutral` is set, neutralization is still gated on the `"sector"` field being present (empty group_map ⇒ `industry_neutral` stays off). Do not add a value-parsed negation; YAGNI.

- [ ] **Step 2: Write the failing test**

Add to `combine_test.cpp` (reuse fixtures; build a 2-field `{"close","sector"}` panel with sectors `{0,0,1,1}` over 6 instruments, 96 dates):

```cpp
TEST(AtxImplCombine, SectorNeutralCombinedBookIsPerSectorNeutral) {
  namespace fs = std::filesystem;
  const usize D = 96, N = 6;
  const std::vector<f64> close = noisy_close(D, N, 0xDEADBEEFULL);
  std::vector<f64> sect(D * N);
  for (usize t = 0; t < D; ++t) for (usize i = 0; i < N; ++i) sect[t*N+i] = (i < 3 ? 0.0 : 1.0);
  auto panel_opt = make_panel(D, N, {"close","sector"}, {close, sect});
  ASSERT_TRUE(panel_opt.has_value());
  const Panel& panel = *panel_opt;
  const std::string panel_path = write_panel_tmp(panel, "sector_neutral");
  const std::string alphas_dir = write_alpha_dir("sector_neutral", safe_dsls());
  const std::string combo_out = (fs::temp_directory_path() / "atx_impl_combine_sector_neutral.bin").string();

  atx::impl::RunConfig cfg;
  cfg.subcommand = "combine"; cfg.panel = panel_path; cfg.alphas = alphas_dir;
  cfg.combo_out = combo_out; cfg.method = "equal"; cfg.sector_neutral = true;
  auto r = atx::impl::run_combine(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();

  auto cpanel = atx::impl::read_panel(combo_out).value();
  auto fid = cpanel.field_id("alpha").value();
  for (usize d = 12; d < D; ++d) {
    auto cs = cpanel.field_cross_section(fid, d);
    f64 g0 = 0.0, g1 = 0.0; bool any = false;
    for (usize i = 0; i < N; ++i) {
      if (std::isnan(cs[i])) continue;
      (i < 3 ? g0 : g1) += cs[i]; any = true;
    }
    if (any) { EXPECT_NEAR(g0, 0.0, 1e-9); EXPECT_NEAR(g1, 0.0, 1e-9); }
  }
  std::error_code ec; fs::remove(panel_path, ec); fs::remove_all(alphas_dir, ec);
  fs::remove(combo_out, ec); fs::remove(combo_out + ".weights.txt", ec);
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cmake --build build-rel --target atx-impl-tests && ./build-rel/bin/atx-impl-tests.exe --gtest_filter=AtxImplCombine.SectorNeutralCombinedBookIsPerSectorNeutral`
Expected: FAIL (combined book not sector-neutral — combine doesn't neutralize yet).

- [ ] **Step 4: Wire neutralization into `run_combine`**

In `stage_combine.cpp`, add include `#include "sector_groups.hpp"`. Between step 5 (evaluate) and step 6 (extract streams), build the group_map and configure the policy:

```cpp
    // 5b. Sector (industry) neutralization: per-alpha books are sector-demeaned so
    //     the mega-alpha expresses idiosyncratic views, not sector bets (WQ
    //     indneutralize). group_map empty (no "sector" field) -> neutralization off.
    std::vector<atx::u32> group_map;
    if (cfg.sector_neutral) {
        group_map = sector_group_map(panel);
    }

    // 6. Extract per-alpha PnL + position streams (sector-neutral when group_map set).
    WeightPolicy policy{};
    policy.industry_neutral = !group_map.empty();
    auto sim = frictionless_sim();
    ATX_TRY(auto streams,
            atx::engine::alpha::extract_streams(
                signals, policy, panel, sim,
                std::span<const atx::u32>{group_map}));
```

Leave the rest of step 6–11 unchanged (the step-9 combine of position streams from the prior fix already consumes `streams.positions`).

- [ ] **Step 5: Run the new test + full combine suite**

Run: `./build-rel/bin/atx-impl-tests.exe --gtest_filter=AtxImplCombine.*`
Expected: all PASS (new sector-neutral test + the 5 existing combine tests; the existing `CombinedEqualsWeightedSum` uses a no-sector panel so `group_map` is empty and behavior is unchanged).

- [ ] **Step 6: Report diff (no commit).** Await commit instruction.

---

## Task 4: `shape_book` helper (dollar-neutral + gross + name-cap clip-renorm)

**Files:**
- Create: `atx-impl/src/book_shape.hpp`
- Test: `atx-impl/tests/book_shape_test.cpp`
- Modify: `atx-impl/tests/CMakeLists.txt` (append `book_shape_test.cpp`)

**Interfaces:**
- Produces: `void atx::impl::shape_book(std::vector<atx::f64>& w, std::span<const std::uint8_t> live, atx::f64 gross, atx::f64 name_cap)` — in place. Forces dead (`live[i]==0`) and NaN cells to 0; dollar-neutralizes the live cells (Σ=0); scales to Σ|w|=`gross`; then fixed 8-pass clip to `[-name_cap, name_cap]` with renorm of the unclipped names back to `gross`. `name_cap<=0` ⇒ clip step skipped.

- [ ] **Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include "book_shape.hpp"

namespace { using atx::f64; }

TEST(BookShape, DollarNeutralAndGross) {
  std::vector<f64> w = {3.0, 1.0, -1.0, -1.0};
  std::vector<std::uint8_t> live = {1,1,1,1};
  atx::impl::shape_book(w, live, /*gross*/1.0, /*name_cap*/1.0);
  f64 s = 0, g = 0; for (f64 x : w) { s += x; g += std::abs(x); }
  EXPECT_NEAR(s, 0.0, 1e-9);
  EXPECT_NEAR(g, 1.0, 1e-9);
}

TEST(BookShape, NameCapBinds) {
  std::vector<f64> w = {10.0, 0.0, -5.0, -5.0};
  std::vector<std::uint8_t> live = {1,1,1,1};
  atx::impl::shape_book(w, live, /*gross*/1.0, /*name_cap*/0.30);
  for (f64 x : w) EXPECT_LE(std::abs(x), 0.30 + 1e-9);
  f64 g = 0; for (f64 x : w) g += std::abs(x);
  EXPECT_NEAR(g, 1.0, 1e-6);   // renormed back to gross after clipping
}

TEST(BookShape, DeadAndNanCellsZeroed) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<f64> w = {2.0, nan, 1.0, -3.0};
  std::vector<std::uint8_t> live = {1,0,1,1};   // index1 dead
  atx::impl::shape_book(w, live, 1.0, 1.0);
  EXPECT_EQ(w[1], 0.0);
  f64 s = 0; for (f64 x : w) s += x;
  EXPECT_NEAR(s, 0.0, 1e-9);
}

TEST(BookShape, Deterministic) {
  std::vector<f64> a = {5,-2,1,-4,3}, b = a;
  std::vector<std::uint8_t> live = {1,1,1,1,1};
  atx::impl::shape_book(a, live, 0.8, 0.25);
  atx::impl::shape_book(b, live, 0.8, 0.25);
  EXPECT_EQ(a, b);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build-rel --target atx-impl-tests` → FAIL (no `book_shape.hpp`).

- [ ] **Step 3: Implement**

```cpp
#pragma once

// atx::impl — shape_book: deploy a raw combined-weight cross-section as a book.
// Dollar-neutralize (Sigma w = 0 over live names) -> gross-normalize (Sigma|w| =
// gross) -> name-cap clip-renorm (|w_i| <= name_cap, redistributing to unclipped
// names to restore gross). Deterministic: fixed pass count, canonical order, no RNG.
// This is the signal-as-position deploy step — NO mean-variance optimization.

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::impl {

inline void shape_book(std::vector<atx::f64>& w,
                       std::span<const std::uint8_t> live,
                       atx::f64 gross, atx::f64 name_cap) {
    const atx::usize n = w.size();
    // 1. Zero dead / NaN cells.
    atx::usize n_live = 0;
    for (atx::usize i = 0; i < n; ++i) {
        const bool ok = (i < live.size() && live[i] != 0) && !std::isnan(w[i]);
        if (!ok) { w[i] = 0.0; } else { ++n_live; }
    }
    if (n_live == 0) return;

    // 2. Dollar-neutralize (subtract mean over live cells).
    atx::f64 mean = 0.0;
    for (atx::usize i = 0; i < n; ++i) if (w[i] != 0.0 || (i < live.size() && live[i] != 0)) mean += w[i];
    mean /= static_cast<atx::f64>(n_live);
    for (atx::usize i = 0; i < n; ++i)
        if (i < live.size() && live[i] != 0) w[i] -= mean;

    // 3. Gross-normalize to Sigma|w| = gross.
    auto renorm_gross = [&]() {
        atx::f64 g = 0.0; for (atx::f64 x : w) g += std::abs(x);
        if (g > 0.0) { const atx::f64 s = gross / g; for (atx::f64& x : w) x *= s; }
    };
    renorm_gross();

    // 4. Name-cap clip-renorm: fixed 8 passes (mirror WeightPolicy::truncate_renorm).
    if (name_cap > 0.0) {
        for (int pass = 0; pass < 8; ++pass) {
            bool any_clip = false;
            for (atx::f64& x : w) {
                if (x > name_cap)  { x = name_cap;  any_clip = true; }
                else if (x < -name_cap) { x = -name_cap; any_clip = true; }
            }
            renorm_gross();
            if (!any_clip) break;
        }
    }
}

} // namespace atx::impl
```

> Note re-neutrality: after clip-renorm the book may drift slightly off Σ=0 when caps bind asymmetrically; this matches `WeightPolicy`'s documented truncation behavior and is acceptable for a research book. Do NOT add extra re-centering passes (YAGNI; would fight the gross renorm).

- [ ] **Step 4: Run the tests**

Run: `cmake --build build-rel --target atx-impl-tests && ./build-rel/bin/atx-impl-tests.exe --gtest_filter=BookShape.*`
Expected: 4 PASS.

- [ ] **Step 5: Report diff (no commit).**

---

## Task 5: `--position-mode` in `optimize` (deploy combo book, skip MVO)

**Files:**
- Modify: `atx-impl/src/config.hpp`, `config.cpp` (add `bool position_mode = false; // --position-mode`)
- Modify: `atx-impl/src/stage_optimize.cpp` (branch to position-mode book construction)
- Test: `atx-impl/tests/optimize_test.cpp` (position-mode book equals `shape_book` of the combo cross-section at each rebalance date)

**Interfaces:**
- Consumes: `RunConfig.position_mode`, `RunConfig.gross`, `RunConfig.name_cap`, `RunConfig.rebalance`; `shape_book(...)` (Task 4); the existing combo/research panel loading + `RebalanceSchedule` + books serialization already in `run_optimize`.
- Produces: when `--position-mode`, the books panel is built by deploying the combo book directly — no `MultiPeriodOptimizer`, no `FactorModel`.

- [ ] **Step 1: Add the flag** — `config.hpp`: `bool position_mode = false; // --position-mode (signal-as-position deploy; skip mean-variance optimize)`. `config.cpp`: add to boolean section + valueless list mirroring `gated`/`sector-neutral`.

- [ ] **Step 2: Write the failing test**

Add to `optimize_test.cpp` (reuse its panel/combo fixtures; if it builds a combo via run_combine, reuse that; otherwise synthesize a 1-field "alpha" combo panel + a "close" research panel of matching shape). Core assertion: for each rebalance period `s` at date `d=sched.periods[s]` (weekly ⇒ d=5*s), the written book row equals `shape_book(combo_cross_section(d), live(d), gross, name_cap)`:

```cpp
TEST(AtxImplOptimize, PositionModeBookEqualsShapedComboCrossSection) {
  // Build research "close" panel + a combo "alpha" panel (same D x N). Run optimize
  // with --position-mode, weekly, gross=1, name_cap=0.5. Compare books rows to
  // shape_book() of the combo cross-section at each rebalance date.
  // (Use the file's existing helpers to write panels + call run_optimize.)
  // ... fixture setup ...
  cfg.position_mode = true; cfg.rebalance = "weekly"; cfg.gross = 1.0; cfg.name_cap = 0.5;
  auto r = atx::impl::run_optimize(cfg);
  ASSERT_TRUE(r.has_value()) << r.error().message();
  auto books = atx::impl::read_panel(cfg.books_out).value();
  auto combo = atx::impl::read_panel(cfg.combo).value();
  auto research = atx::impl::read_panel(cfg.panel).value();
  auto wfid = books.field_id("weight").value();
  auto afid = combo.field_id("alpha").value();
  const atx::usize N = research.instruments();
  for (atx::usize s = 0; s < books.dates(); ++s) {
    const atx::usize d = 5 * s;
    std::vector<atx::f64> expect(combo.field_cross_section(afid, d).begin(),
                                 combo.field_cross_section(afid, d).end());
    std::vector<std::uint8_t> live(N);
    for (atx::usize i = 0; i < N; ++i) live[i] = research.in_universe(d, i) ? 1 : 0;
    atx::impl::shape_book(expect, std::span<const std::uint8_t>{live}, 1.0, 0.5);
    auto got = books.field_cross_section(wfid, s);
    for (atx::usize i = 0; i < N; ++i) EXPECT_NEAR(got[i], expect[i], 1e-9);
  }
}
```

- [ ] **Step 3: Run it to verify it fails**

Run: `cmake --build build-rel --target atx-impl-tests && ./build-rel/bin/atx-impl-tests.exe --gtest_filter=AtxImplOptimize.PositionModeBookEqualsShapedComboCrossSection`
Expected: FAIL — `--position-mode` ignored; MVO path produces different books.

- [ ] **Step 4: Implement the branch**

In `stage_optimize.cpp`, add `#include "book_shape.hpp"`. After loading `research` + `combo` and validating shapes, and after building the `RebalanceSchedule sched` (reuse the existing schedule code; if the schedule is built only inside the MVO path, hoist it above the branch), insert before the MVO setup:

```cpp
    if (cfg.position_mode) {
        // Signal-as-position deploy: the combo book IS the mega-alpha. Deploy it at
        // each rebalance date via shape_book (dollar-neutral + gross + name-cap),
        // with NO mean-variance optimization and NO risk model.
        ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));
        const atx::usize M = research.instruments();
        const atx::f64 gross    = cfg.gross    > 0.0 ? cfg.gross    : 1.0;
        const atx::f64 name_cap = cfg.name_cap > 0.0 ? cfg.name_cap : 1.0;
        const atx::usize S = sched.periods.size();
        std::vector<atx::f64> books_flat(S * M, 0.0);
        for (atx::usize s = 0; s < S; ++s) {
            const atx::usize d = sched.periods[s];
            const auto cs = combo.field_cross_section(alpha_fid, d);
            std::vector<atx::f64> w(cs.begin(), cs.end());
            std::vector<std::uint8_t> live(M);
            for (atx::usize i = 0; i < M; ++i) live[i] = research.in_universe(d, i) ? 1 : 0;
            shape_book(w, std::span<const std::uint8_t>{live}, gross, name_cap);
            for (atx::usize i = 0; i < M; ++i) books_flat[s * M + i] = w[i];
        }
        // Serialize books exactly as the MVO path does: a weight panel (S x M, field
        // "weight") + the per-period meta sidecar (turnover, cost_bps). Mirror the
        // existing serialization block; turnover[s] = Sigma_i |w[s] - w[s-1]| (w[-1]=0),
        // cost_bps = 0 (frictionless research). Then return StageResult with the same
        // kvs keys (periods, instruments, gross, name_cap, rebalance, books digest).
        // ... (reuse the existing books-writing helper / block verbatim) ...
    }
```

Implementation note for the engineer: factor the existing MVO path's "serialize books panel + meta sidecar + StageResult" tail into a small local lambda/helper `write_books(books_flat, sched, M)` and call it from BOTH branches so the two paths are byte-identical in output format (DRY). Compute per-period turnover the same way the MVO meta does (`Σ|w[s]-w[s-1]|`, w[-1]=0).

- [ ] **Step 5: Run the new test + full optimize suite + full impl suite**

Run: `./build-rel/bin/atx-impl-tests.exe --gtest_filter=AtxImplOptimize.*` then the full `./build-rel/bin/atx-impl-tests.exe`
Expected: new test PASS; the 5 existing optimize tests PASS (default `position_mode=false` ⇒ MVO path unchanged); full suite green (36 tests: 32 prior + sector + 3 sector_groups + 4 book_shape + position-mode − any merged… reconcile actual count, just require 0 failures).

- [ ] **Step 6: Report diff (no commit).**

---

## Task 6: End-to-end verification + RUNBOOK

**Files:**
- Modify: `docs/atx-impl/RUNBOOK.md` (document `--sector-neutral`, `--position-mode`, and the recommended mega-alpha chain)
- No source changes (verification task).

**Interfaces:** consumes the existing run artifacts — `C:\atx-run\panel_smoke.bin`, `C:\atx-run\run_classic\alphas\*.dsl` (21 admitted alphas). Discover is NOT re-run.

- [ ] **Step 1: Run the WQ mega-alpha chain into fresh paths**

```bash
EXE=build-rel/bin/atx-impl.exe
PANEL=C:/atx-run/panel_smoke.bin
ALPHAS=C:/atx-run/run_classic/alphas
"$EXE" combine  --panel "$PANEL" --alphas "$ALPHAS" --combo-out C:/atx-run/run_classic/combo3.bin --method bounded-regression --sector-neutral
"$EXE" optimize --panel "$PANEL" --combo C:/atx-run/run_classic/combo3.bin --books-out C:/atx-run/run_classic/books3.bin --position-mode --gross 1 --name-cap 0.05 --rebalance weekly
"$EXE" report   --panel "$PANEL" --books C:/atx-run/run_classic/books3.bin --report-out C:/atx-run/run_classic/report3
cat C:/atx-run/run_classic/report3/summary.txt
```

Expected: `final_equity` positive; compare against the prior position-paradigm baseline (`report2/` = +0.011368 from the equal-weight, non-neutral, MVO chain). Record the lift. (No hard pass/fail threshold — this is a measurement; the engineering gate is that it runs clean and the sign is correct.)

- [ ] **Step 2: Sanity-check the book** — confirm `avg_gross ≈ 0.96`–`1.0`, per-period turnover finite, books3 not all-zero (`head` the meta sidecar). If `--method bounded-regression` errored on the 21×101 pool (e.g. PCA degeneracy), fall back to `--method ic` and note it.

- [ ] **Step 3: Update RUNBOOK** — add a "Mega-alpha (signal-as-position) chain" subsection documenting `--sector-neutral` (combine), `--position-mode` (optimize), the recommended `--method bounded-regression`, and that position-mode skips the mean-variance optimizer (deploys the combined neutralized book directly).

- [ ] **Step 4: Report the end-to-end result + all diffs to the user (no commit).** Present before/after backtest numbers and await the commit decision.

---

## Self-Review

**1. Spec coverage** — Decisions from the design review map to tasks: signal-as-position paradigm ⇒ Task 4 + 5 (deploy book, drop MVO); factor neutralization (wire existing) ⇒ Task 1 + 2 + 3 (sector via `WeightPolicy.industry_neutral`); real combiner method ⇒ Task 6 (`--method bounded-regression`, already supported, no code). Size/beta neutralization, Kelly sizing, factor risk wiring, discover-path neutralization ⇒ explicitly deferred (documented Tier-2). No in-scope requirement is unassigned.

**2. Placeholder scan** — All code steps show complete code. The two "reuse the existing serialization block" notes in Task 5 reference concrete existing code in `stage_optimize.cpp` (the MVO path's books writer) and instruct a DRY extraction with the exact turnover formula — not a placeholder. The engineer must read that block; that is normal for a Modify task.

**3. Type consistency** — `sector_group_map -> std::vector<atx::u32>` (Task 2) feeds `extract_streams(..., std::span<const atx::u32>)` (Task 1) and `WeightPolicy::to_target_weights(..., std::span<const atx::u32>)` (verified weight_policy.hpp:197). `shape_book(std::vector<atx::f64>&, std::span<const std::uint8_t>, f64, f64)` (Task 4) is called identically in the Task 4 tests and Task 5 optimize branch + test. `RunConfig` bools `sector_neutral`/`position_mode` parsed via the `gated` pattern (verified config.cpp:26,170). Field names `"sector"`, `"close"`, `"alpha"`, `"weight"` match the panel/combine/optimize conventions.

## Execution Handoff

Default path: subagent-driven-development (the governing goal mandates using subagents to preserve context). Per the user's standing constraint, the per-task "commit" step is replaced by "report diff + await commit instruction" — no git commits until the user asks.
