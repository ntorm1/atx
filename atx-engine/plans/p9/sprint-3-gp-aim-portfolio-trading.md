# Sprint 3 — Gârleanu-Pedersen Aim-Portfolio Trading

**Goal:** behind `--gp-trading` (inert default = today's linear blend), replace the position-mode
deploy's crude linear partial-trade (`w := prev + trade_rate·(target − prev)`,
`atx-impl/src/stage_optimize.cpp:191-195`) with the already-built, already-tested GP aim-portfolio
trade — `risk::gp_aim_and_value` + `risk::gp_turnover_native_step`
(`atx-engine/include/atx/engine/risk/garleanu_pedersen.hpp`) — so the live book trades toward the
GP AIM (which folds in risk-model curvature via `V`) instead of blindly chasing the freshly-shaped,
un-risk-weighted target every period. **Zero new estimator math**: `gp_aim_and_value` and
`gp_turnover_native_step` are FROZEN, fully-tested closed-form bodies (S8.7/S4-5a) with zero call
sites in `atx-impl` today — S3 wires them in, it does not extend or re-derive them. All opt-in
behind an inert `RunConfig` triple (`gp_trading=false`, `gp_risk_aversion=0.0`,
`gp_trade_cost_scale=0.0`); the no-flag path stays byte-identical.

**Owns (exclusive):**
`atx-impl/src/stage_optimize.cpp` — **the position-mode partial-trade site ONLY**
(the `if (cfg.position_mode)` branch, roughly lines 158-220 in the pre-S3 file; concretely the
`prev` declaration at :170 and the `trade_rate_val < 1.0` blend at :191-195),
`atx-impl/src/config.{hpp,cpp}` (the `gp_trading`/`gp_risk_aversion`/`gp_trade_cost_scale` fields +
their CLI parse arms — additive only), NEW `atx-impl/tests/config_gp_trading_test.cpp`,
NEW `atx-impl/tests/stage_optimize_gp_trading_test.cpp`.

**Must NOT touch:** `atx-engine/include/atx/engine/risk/garleanu_pedersen.{hpp,cpp}` (FROZEN —
`gp_aim_and_value`/`gp_turnover_native_step` are called, never edited, never re-derived; no matrix
Riccati is added — the header's own docs record that as an explicit future lift, not this
sprint's job); `risk/{capacity,optimizer}.hpp`, `cost/*`, `loop/*`, `exec/*` (other sprints/out of
scope); `fund/*` (p8-S2); `learn/*`, `combine/regime_combiner.hpp`, `atx-impl/src/stage_combine.cpp`
(p9-S2); **the MVO branch of `stage_optimize.cpp`** — specifically the `build_risk_model` call
sites at (pre-S3, post-p9-S1) roughly lines 252/260/267 (the `Diagonal`/`Factor` covariance-source
selection for the 5b MVO path) — those are **p9-S1's** region; S3 reads nothing from `risk_cfg`
there and edits none of it. `factory/*`, `stage_discover.cpp`, `stage_metabook.cpp` (other sprints).

**Cross-sprint seam (S1↔S3 — ROADMAP-mandated, land S1 first):** both S1 and S3 edit
`stage_optimize.cpp`. S1 (p9) threads a real `dead_lib`/`dead_ids` into the two
`build_risk_model(..., {}, nullptr, {}, ...)` calls inside the **MVO branch** (5b, roughly
:252-273). S3 touches only the **position-mode branch** (5a, :158-220), which today builds **no**
risk model at all and does not read `risk_cfg` (the 2-arg `run_optimize(cfg, risk_cfg)` overload
returns at :219 before `risk_cfg` is ever consulted, when `cfg.position_mode` is true). S3 adds a
**new, disjoint** `build_risk_model` call inside the position-mode branch (a fixed `Diagonal`
config, independent of `--risk-model`/`risk_cfg` — see the Architecture note) — this is a new call
site, not an edit to either of S1's two call sites, so the files are textually disjoint regions of
the same TU. **Land S1's commits first; S3 rebases on top so its diff context lines match.** If S1
has not landed when S3 starts, S3 can still be implemented and tested against the current `main`
tip (`c7c7b44`) since S3 never reads anything S1 adds; the rebase is purely to avoid a manual merge
of adjacent-but-non-overlapping hunks.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use the surrounding engine headers as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, and tricky
domain rules. Do not comment obvious assignments.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby engine code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The GP-trading gap (verified file:line)

| Gap | File:line | Evidence |
|---|---|---|
| Live turnover control is a linear blend, not GP | `stage_optimize.cpp:191-195` | `if (trade_rate_val < 1.0) { w[i] = prev[i] + trade_rate_val * (w[i] - prev[i]); }` — blends toward whatever target `shape_book` just shaped THIS period, with no risk-model curvature and no memory of the aim's own decay path |
| `gp_turnover_native_step` has zero call sites | grep `atx-impl/src` | only hit is its own declaration comment in `garleanu_pedersen.hpp:191` documenting the seam: *"wiring this into the live book is an S1/S5 seam at `atx-impl/src/stage_optimize.cpp:159-172` (S4 must not edit that file)"* — S4 (p8) explicitly deferred this wire; p9-S3 is that deferred wire |
| `gp_aim_and_value` has zero call sites in `atx-impl` | grep `atx-impl/src` | only real caller today is `atx-engine/src/risk/multi_horizon.cpp:160` (inside `MultiHorizonOptimizer`, a SEPARATE, heavier receding-horizon driver `stage_optimize.cpp` does not use and S3 does not either — see Architecture note) |
| Position-mode branch builds no risk model | `stage_optimize.cpp:158-220` | the entire `if (cfg.position_mode)` branch never calls `build_risk_model`/`diagonal_risk_model` — `w` comes straight from the raw combo cross-section, shaped by `shape_book` (gross/name-cap/dollar-neutral), with no covariance input anywhere |
| `RunConfig` has no GP-trading surface | grep `atx-impl/src/config.hpp` for `gp_trading\|gp_risk_aversion\|gp_trade_cost_scale` | zero hits (pre-S3) — confirms S3-0 is additive, not a rename/relocation |

---

## Architecture note — what "wire GP trading" actually means (and does NOT mean)

`gp_turnover_native_step(prev, aim_pos, trade_rate)` (`garleanu_pedersen.hpp:201-203`) is a **pure**
elementwise blend — same functional shape as today's blend, just fed an `aim_pos` instead of a raw
`target`:
```cpp
[[nodiscard]] std::vector<atx::f64> gp_turnover_native_step(std::span<const atx::f64> prev,
                                                            std::span<const atx::f64> aim_pos,
                                                            atx::f64 trade_rate);
```
It does not know where `aim_pos` came from. `aim_pos` is produced by
`gp_aim_and_value(alpha_bar, V, lambda)` (`garleanu_pedersen.hpp:144-145` /
`garleanu_pedersen.cpp:17-54`):
```cpp
[[nodiscard]] atx::core::Result<GpAimValue>
gp_aim_and_value(std::span<const atx::f64> alpha_bar, const FactorModel &V, atx::f64 lambda);
// struct GpAimValue { std::vector<atx::f64> alpha_bar; std::vector<atx::f64> aim_pos; };
```
`aim_pos = (2λV)⁻¹ · alpha_bar` via `V.apply_inverse` (the cached-Cholesky Woodbury factor apply —
`FactorModel::apply_inverse`, `factor_model.hpp:202`); `lambda == 0` is a documented, valid
convention (`aim_pos == alpha_bar`, no curvature fold — used when `--gp-risk-aversion` is left at
its inert default). **Three real inputs S3 must supply that do not exist in the position-mode
branch today:**

1. **A risk model `V`.** The position-mode branch (5a) builds no covariance at all — S1's
   `build_risk_model`/`risk::RiskModelConfig` machinery lives entirely in the MVO branch (5b). S3
   does **not** extend `--risk-model`/`risk_cfg` selection into position mode (that would blur the
   S1/S3 ownership line and requires per-step PIT refitting machinery S3 has no mandate to build).
   Instead, S3 builds **one whole-panel `Diagonal` `FactorModel`** — `build_risk_model(research,
   risk::RiskModelConfig{})` (the exact inert-default kind, reusing the already-included
   `stage_riskmodel.hpp`/`adapt_factor.hpp`) — **once**, outside the per-period loop, gated behind
   `cfg.gp_trading`. This is a deliberate, documented scope cut: GP-trading in position mode always
   rides a Diagonal (per-name variance) lens, never Factor, regardless of `--risk-model`. Extending
   it to Factor covariance is future work (see Out of scope).
2. **`alpha_bar` (the return-space aim input).** The header's own docs describe this as "the
   per-name α cross-section with its SignalHorizon decay ALREADY baked into forecast_trajectory's
   rows" — normally produced by `MultiHorizonOptimizer::gp_aim` averaging multiple horizon-forecast
   rows (`multi_horizon.hpp:160-165`). `MultiHorizonOptimizer` is a **separate, heavyweight
   receding-horizon driver** (`multi_horizon.hpp:146-199`) with its own constraint-set dispatch, QP
   solver, and schedule walk — wiring it into position mode would replace the entire branch's logic,
   far beyond "S3 owns only the partial-trade step." S3 does **not** call it. Instead, `alpha_bar`
   is the plain **current-period raw combo alpha cross-section** (`cs`, already read at
   `stage_optimize.cpp:174`, the exact same value `shape_book` turns into today's `target`) — no
   temporal smoothing is invented. This is a documented simplification, flagged below as an
   ambiguity for the reconciler.
3. **`lambda` and the trade-rate scalar.** `lambda = cfg.gp_risk_aversion` directly (no
   `set_flags`-gated substitution — the registry pins `0.0` itself as "inert," unlike the nearby
   `risk_aversion` field, which substitutes `1.0` when unset; S3 must NOT copy that pattern here).
   The registry's third field, `gp_trade_cost_scale`, has **no matching parameter** in either frozen
   function — the shipped scalar-Λ GP reduction (`Λ = λΣ`) has no matrix trade-cost knob to bind it
   to (the header's own docs: *"the matrix Riccati rate is the recorded refinement"* — not shipped).
   S3's design: a **caller-side scalar** applied at the `stage_optimize.cpp` call site (not inside
   the frozen body): `kappa = trade_rate_val / (1.0 + cfg.gp_trade_cost_scale)`. At the inert
   default (`0.0`) this is `kappa == trade_rate_val` exactly. This is an invented-but-documented
   mapping, flagged as an ambiguity below.

**Why the GP path can genuinely lower turnover here (not just relabel the blend):** `aim_pos`
divides `alpha_bar` through `V` — under a `Diagonal` `V`, name `i`'s aim is scaled by `1/D_i` (its
own return variance). A name whose per-period alpha swings sign but whose underlying return series
is noisy/high-variance gets a **damped, more stable** aim contribution every period, while
`shape_book`'s un-risk-weighted target treats that name identically to a low-variance, genuinely
persistent-edge name (it only cares about raw alpha magnitude + gross/name-cap normalization). A
mean-reverting/noisy name therefore contributes less period-to-period churn under the GP aim than
under the raw target — a real, testable mechanism, not a tuned coincidence (see S3-2).

**Ambiguities flagged for the reconciler:**
- (i) `alpha_bar` == the raw current-period `cs` (no cross-period decay averaging) — a scope cut
  from the header's own "decay already baked in" framing; wiring true multi-horizon decay would
  require `MultiHorizonOptimizer`/`SignalHorizon` plumbing, out of S3's mandate.
- (ii) `gp_trade_cost_scale`'s `kappa = trade_rate/(1+scale)` mapping is S3-invented (documented,
  monotone, inert at 0.0) since neither frozen function accepts a cost-scale parameter directly.
- (iii) GP always uses a whole-panel `Diagonal` `V` in position mode, never `--risk-model factor`,
  even if the operator sets both flags together — this is a scope cut, not a bug; a future sprint
  could extend GP to ride the same per-step Factor cadence S1 built for the MVO branch.

---

## Determinism contract (Sprint 3)

Inherits the p9 ROADMAP contract verbatim. Every new capability lives behind the inert
`RunConfig` triple: `gp_trading = false`, `gp_risk_aversion = 0.0`, `gp_trade_cost_scale = 0.0`.

At the inert default, `stage_optimize`'s position-mode branch is **untouched code** — the new
`if (cfg.gp_trading) { ... } else if (trade_rate_val < 1.0) { ... }` structure takes the `else`
arm exactly as before, and the new whole-panel `Diagonal` `V` build is skipped entirely (never
allocated, never read), so no new work happens and no new failure mode can be introduced on the
off-path. `gp_aim_and_value`/`gp_turnover_native_step` are themselves order-fixed pure functions
(no RNG/clock/map — `garleanu_pedersen.hpp:87-91`), so the on-path result is reproducible run-to-run.

**Four test classes (mandatory):**
(a) **off-path byte-identity** — `--gp-trading` absent (or explicitly `false` with the other two
fields at their inert values) reproduces the exact pre-S3 linear-blend digest;
(b) **on-path RED→GREEN** — a mean-reverting/noisy synthetic fixture where GP-wired trading
realizes strictly lower cumulative turnover than the linear blend at matched-or-better realized
gross Sharpe;
(c) **twice-run** — same panel/config on the GP path ⇒ identical digest and identical bytes;
(d) **seq==parallel** — **N/A, justified**: the position-mode branch is an inherently sequential
per-period state machine (`w[s]` depends on `prev = w[s-1]`, threaded forward one period at a time,
`stage_optimize.cpp:170-211`); no `parallel_for`/executor touches this branch before or after S3,
so there is no seq/parallel axis to test (the ROADMAP's own class (d) is conditional: "if the
optimize path is parallelized"). Documented, not silently skipped.

---

## Dependency / wiring map

```
config.hpp   ← S3-0 adds RunConfig::{gp_trading, gp_risk_aversion, gp_trade_cost_scale} (append at
                struct end, p8/p9 convention -- aggregate-init order is load-bearing)
config.cpp   ← S3-0 adds the 3 CLI parse arms (1 valueless bool + 2 validated doubles)
stage_riskmodel.hpp:build_risk_model  ← S3-1 calls it ONCE (Diagonal, whole-panel) inside the
                                         position-mode branch, gated by cfg.gp_trading (does NOT
                                         touch the MVO branch's own S1 call sites)
data/adapt_factor.hpp:artifact_to_factor_model ← S3-1 lowers the artifact to a risk::FactorModel
                                                  (same helper the MVO branch already uses)
risk/garleanu_pedersen.hpp:gp_aim_and_value    ← S3-1 calls it per period (alpha_bar=cs, V, lambda)
risk/garleanu_pedersen.hpp:gp_turnover_native_step ← S3-1 replaces the linear blend with this call
stage_optimize.cpp:158-220 (position-mode branch) ← S3-1 the ONLY edited region
tests/config_gp_trading_test.cpp              ← S3-0 (auto-globbed, no CMake edit)
tests/stage_optimize_gp_trading_test.cpp      ← S3-1/S3-2 (auto-globbed, no CMake edit)
```

No `CMakeLists.txt` edits anywhere in this sprint: `stage_optimize.cpp`/`config.cpp` are already
explicitly registered in `atx-impl/CMakeLists.txt:20-22` (source list), and
`atx-impl/tests/CMakeLists.txt:1-2` globs `*_test.cpp` (`file(GLOB ATX_IMPL_TEST_SOURCES
CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")`) — new test files are picked up on the
next configure automatically. `ATX_UNITY_BUILD` batching (dev preset default ON) is wired only for
`atx-engine-tests` (`atx-engine/tests/CMakeLists.txt:94-118`); `atx-impl/tests/CMakeLists.txt` has
no Unity grouping at all, so the new `atx-impl` test files carry no Unity-collision risk.

---

## Build/test wrapper (self-contained; subagent shell state does not persist)

Canonical invocation for every unit below (mirrors the project convention,
`atx-engine/plans/p2/sprint-5-progress.md:33-40`):

```powershell
Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
Set-Location "C:\atx-wt\p9"
cmake --preset dev
cmake --build --preset dev --target atx-impl-tests
ctest --preset dev -R <Suite>
```
`cmake --preset dev` only needs to run once per fresh configure (re-run if `config.hpp`/CMake files
changed and the cache is stale). DEFAULT `dev` preset — Unity ON (`ATX_UNITY_BUILD=ON`,
`CMakePresets.json:32-45`), unaffected here per the wiring-map note above.

---

## Tasks

### S3-0 — Ledger + `RunConfig` plumbing (do first; all units depend on this)

**Goal:** open the sprint ledger; add the inert-default CLI/config surface
(`gp_trading`/`gp_risk_aversion`/`gp_trade_cost_scale`) with zero behavior change — the fields
exist and parse, nothing reads them non-inertly yet (mirrors p8-S1-0's "S1-0" convention).

**Files:** `atx-engine/plans/p9/sprint-3-progress.md` (NEW, ledger); `atx-impl/src/config.hpp`;
`atx-impl/src/config.cpp`; NEW `atx-impl/tests/config_gp_trading_test.cpp`.

**Steps:**

1. Create `atx-engine/plans/p9/sprint-3-progress.md` with a header (Goal / Worktree
   `C:\atx-wt\p9` / Branch `feat/p9` / Base `main @ c7c7b44` / Build gate = the wrapper above) and
   an opening ledger row `S3-0 kickoff — ledger opened`.

2. **`config.hpp`** — append at struct end (after the existing `incremental_panel` field, before
   the `set_flags` member — the p8/p9 "appended at struct END" convention,
   `config.hpp:384-391`):
   ```cpp
   // --gp-trading / --gp-risk-aversion / --gp-trade-cost-scale (p9 S3): opt-in
   // Gârleanu-Pedersen aim-portfolio trade for the position-mode partial-step
   // (stage_optimize.cpp's "5a" branch). false (default) leaves the existing
   // linear trade_rate blend (w := prev + trade_rate*(target-prev)) untouched --
   // the no-flag path is byte-identical. When true, the partial-trade step
   // instead trades toward the GP AIM (risk::gp_turnover_native_step), computed
   // from risk::gp_aim_and_value(alpha_bar, V, gp_risk_aversion) over a single
   // whole-panel Diagonal risk model V built once for the run (see
   // stage_optimize.cpp's gp_trading block; independent of --risk-model, which
   // remains an MVO-branch-only concern owned by S1/S2). gp_risk_aversion=0.0
   // (inert default) is gp_aim_and_value's own documented lambda==0 convention
   // (aim_pos == alpha_bar, no risk-curvature fold -- a valid, if aggressive, GP
   // setting, NOT an error; unlike the nearby risk_aversion field, this one is
   // read RAW, with no set_flags-gated substitution). gp_trade_cost_scale=0.0
   // (inert default) leaves the effective GP trade rate kappa == cfg.trade_rate
   // unchanged; kappa = trade_rate / (1 + gp_trade_cost_scale) for
   // gp_trade_cost_scale > 0 -- a scoped, caller-side scalar approximation (the
   // shipped GP reduction has no matrix trade-cost knob to bind this to; see
   // garleanu_pedersen.hpp's own "matrix Riccati is the recorded lift, not
   // shipped" note).
   bool   gp_trading          = false; // --gp-trading
   double gp_risk_aversion    = 0.0;   // --gp-risk-aversion (0 = lambda=0 GP convention)
   double gp_trade_cost_scale = 0.0;   // --gp-trade-cost-scale (0 = kappa == trade_rate)
   ```

3. **`config.cpp`** — add the valueless bool arm (mirrors `robustness-battery`,
   `config.cpp:52`):
   ```cpp
   if (flag == "gp-trading") { cfg.gp_trading = true; return atx::core::Ok(); } // p9 S3
   ```
   and to the valueless-flag detection OR-chain (`config.cpp:388`, append `|| flag ==
   "gp-trading"` with a `// p9 S3` note in the trailing comment). Add the two validated doubles
   (mirrors the `cost-bps` block, `config.cpp:308-315`), placed after the `cost-bps` arm:
   ```cpp
   if (flag == "gp-risk-aversion") {
       ATX_TRY_VOID(parse_double(cfg.gp_risk_aversion));
       if (cfg.gp_risk_aversion < 0.0) {
           return atx::core::Err(EC::InvalidArgument,
               "--gp-risk-aversion must be >= 0: got " + std::string(value));
       }
       return atx::core::Ok();
   } // p9 S3
   if (flag == "gp-trade-cost-scale") {
       ATX_TRY_VOID(parse_double(cfg.gp_trade_cost_scale));
       if (cfg.gp_trade_cost_scale < 0.0) {
           return atx::core::Err(EC::InvalidArgument,
               "--gp-trade-cost-scale must be >= 0: got " + std::string(value));
       }
       return atx::core::Ok();
   } // p9 S3
   ```

4. **RED** — write `atx-impl/tests/config_gp_trading_test.cpp` (mirrors
   `config_megabook_flags_test.cpp`'s `ConfigParse`/`ConfigFile` suites exactly) BEFORE step 2/3
   land; it fails to compile (`cfg.gp_trading` etc. do not exist):
   ```cpp
   // config_gp_trading_test.cpp — p9 S3-0: CLI/config-file surface for GP-trading.
   #include <fstream>
   #include <string>
   #include <vector>

   #include <gtest/gtest.h>

   #include "config.hpp"

   using atx::impl::parse_args;
   using atx::impl::parse_config_file;
   using atx::impl::merge_config_file;
   using atx::impl::RunConfig;

   namespace {
   atx::core::Result<RunConfig> parse(std::vector<std::string> args) {
       std::vector<char*> argv;
       argv.reserve(args.size());
       for (auto& a : args) argv.push_back(a.data());
       return parse_args(static_cast<int>(argv.size()), argv.data());
   }
   } // namespace

   TEST(ConfigParse, GpTradingFlags_RoundTrip) {
       auto r = parse({"atx-impl", "optimize",
                       "--gp-trading",
                       "--gp-risk-aversion", "2.5",
                       "--gp-trade-cost-scale", "0.1"});
       ASSERT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message());
       const RunConfig& cfg = *r;
       EXPECT_TRUE(cfg.gp_trading);
       EXPECT_DOUBLE_EQ(cfg.gp_risk_aversion, 2.5);
       EXPECT_DOUBLE_EQ(cfg.gp_trade_cost_scale, 0.1);
   }

   TEST(ConfigParse, GpTradingFlags_OmittedAreInert) {
       auto r = parse({"atx-impl", "optimize"});
       ASSERT_TRUE(r.has_value());
       const RunConfig& cfg = *r;
       EXPECT_FALSE(cfg.gp_trading);
       EXPECT_DOUBLE_EQ(cfg.gp_risk_aversion, 0.0);
       EXPECT_DOUBLE_EQ(cfg.gp_trade_cost_scale, 0.0);
   }

   TEST(ConfigParse, GpRiskAversionRejectsNegative) {
       auto r = parse({"atx-impl", "optimize", "--gp-risk-aversion", "-1.0"});
       EXPECT_FALSE(r.has_value());
   }

   TEST(ConfigParse, GpTradeCostScaleRejectsNegative) {
       auto r = parse({"atx-impl", "optimize", "--gp-trade-cost-scale", "-0.5"});
       EXPECT_FALSE(r.has_value());
   }

   TEST(ConfigFile, GpTradingFlags_RoundTrip) {
       const std::string path = "atx_s3_0_gp_trading_flags_test.cfg";
       {
           std::ofstream f(path);
           f << "gp-trading=\n"
                "gp-risk-aversion=1.5\n"
                "gp-trade-cost-scale=0.2\n";
       }
       auto file_r = parse_config_file(path, "optimize");
       ASSERT_TRUE(file_r.has_value()) << file_r.error().message();
       EXPECT_TRUE(file_r->gp_trading);
       EXPECT_DOUBLE_EQ(file_r->gp_risk_aversion, 1.5);
       EXPECT_DOUBLE_EQ(file_r->gp_trade_cost_scale, 0.2);
       std::remove(path.c_str());
   }
   ```

5. **GREEN** — apply steps 2-3; build + run:
   ```powershell
   cmake --build --preset dev --target atx-impl-tests
   ctest --preset dev -R "ConfigParse.GpTrading|ConfigFile.GpTrading"
   ```
   Expect: pre-step-2/3, build FAILS (`cfg.gp_trading`/`cfg.gp_risk_aversion`/
   `cfg.gp_trade_cost_scale` undeclared). Post: build exit 0, 5/5 new tests PASS.

6. Full regression to confirm nothing else moved:
   ```powershell
   ctest --preset dev -R AtxImpl
   ```
   Expect: same pass count as the pre-S3 baseline (no `atx-impl` test regressed by the additive
   field/parse-arm changes).

**Commit:**
```
git add atx-engine/plans/p9/sprint-3-progress.md atx-impl/src/config.hpp atx-impl/src/config.cpp atx-impl/tests/config_gp_trading_test.cpp
git commit -m "feat(p9-s3): S3-0 RunConfig + CLI plumbing for --gp-trading (inert, no wiring yet)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### S3-1 — Wire the GP aim-portfolio trade into the position-mode branch

**Goal:** replace `stage_optimize.cpp`'s linear partial-trade step with the GP aim-portfolio trade
when `cfg.gp_trading` is set; off ⇒ the exact pre-S3 code path (byte-identical by construction —
the new branch is an `if`/`else if` sibling of the untouched original code, not a rewrite of it).

**Files:** `atx-impl/src/stage_optimize.cpp` only.

**Root cause (recap):** `stage_optimize.cpp:191-195` unconditionally blends toward `shape_book`'s
un-risk-weighted target; `gp_turnover_native_step`/`gp_aim_and_value` are built, tested, and
unreachable from `atx-impl`.

**Steps:**

1. **RED** — extend `atx-impl/tests/stage_optimize_gp_trading_test.cpp` (NEW file) with a single
   smoke test proving the wire exists and changes behavior when set (fails to compile pre-wire:
   `cfg.gp_trading` exists from S3-0, but the branch does not read it yet, so this test's assertion
   — that GP output DIFFERS from legacy output on a fixture where GP must move the book — fails):
   ```cpp
   // stage_optimize_gp_trading_test.cpp — p9 S3-1/S3-2: GP aim-portfolio trade wiring.
   //
   // Suite: AtxImplOptimizeGpTrading
   #include <cmath>
   #include <cstdint>
   #include <filesystem>
   #include <fstream>
   #include <vector>

   #include <gtest/gtest.h>

   #include "atx/engine/alpha/panel.hpp"
   #include "atx/core/types.hpp"

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stages.hpp"

   namespace atxtest_stage_optimize_gp_trading {

   namespace fs = std::filesystem;
   namespace alpha = atx::engine::alpha;
   using atx::f64;
   using atx::usize;

   class AtxImplOptimizeGpTrading : public ::testing::Test {
   protected:
     fs::path tmp_dir_;
     void SetUp() override {
       tmp_dir_ = fs::temp_directory_path() / "atx_impl_gp_trading_test";
       fs::create_directories(tmp_dir_);
     }
     void TearDown() override {
       std::error_code ec;
       fs::remove_all(tmp_dir_, ec);
     }
   };

   // Minimal 2-name, gently-trending research + a constant combo alpha, position-mode.
   TEST_F(AtxImplOptimizeGpTrading, GpTradingChangesBookWhenSet) {
     constexpr usize M = 2, D = 20;
     std::vector<f64> close(D * M);
     for (usize t = 0; t < D; ++t) {
       close[t * M + 0] = 100.0 * std::exp(0.001 * static_cast<f64>(t));
       close[t * M + 1] = 100.0 * std::exp(-0.0005 * static_cast<f64>(t));
     }
     std::vector<std::uint8_t> uni(D * M, 1u);
     auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
     ASSERT_TRUE(rp.has_value());
     const std::string research_path = (tmp_dir_ / "research.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());

     std::vector<f64> combo(D * M);
     for (usize t = 0; t < D; ++t) { combo[t * M + 0] = 1.0; combo[t * M + 1] = -1.0; }
     auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
     ASSERT_TRUE(cp.has_value());
     const std::string combo_path = (tmp_dir_ / "combo.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

     atx::impl::RunConfig cfg;
     cfg.panel = research_path;
     cfg.combo = combo_path;
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "daily";
     cfg.position_mode = true;
     cfg.trade_rate = 0.4;
     cfg.set_flags.emplace("trade-rate");

     cfg.books_out = (tmp_dir_ / "books_legacy.bin").string();
     auto legacy = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(legacy.has_value()) << legacy.error().message();

     cfg.gp_trading = true;
     cfg.gp_risk_aversion = 1.0;
     cfg.books_out = (tmp_dir_ / "books_gp.bin").string();
     auto gp = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(gp.has_value()) << gp.error().message();

     EXPECT_NE(legacy->digest, gp->digest)
         << "gp_trading=true must route through a different partial-trade step than the "
            "legacy linear blend on a fixture where the risk model is non-trivial";
   }

   } // namespace atxtest_stage_optimize_gp_trading
   ```
   Run: `ctest --preset dev -R AtxImplOptimizeGpTrading` — expect FAIL (`legacy->digest ==
   gp->digest`, since `cfg.gp_trading` is not yet read by `stage_optimize.cpp`).

2. **GREEN** — the actual wire. Add the include (top of file, near the other `risk/` includes,
   `stage_optimize.cpp:17`):
   ```cpp
   #include "atx/engine/risk/garleanu_pedersen.hpp"
   ```
   Insert the one-time, gated risk-model build immediately after the existing `prev` declaration
   (`stage_optimize.cpp:170`, inside the `if (cfg.position_mode)` branch, BEFORE the `for (atx::usize
   s = 0; s < S; ++s)` loop):
   ```cpp
   std::vector<atx::f64> prev(M, 0.0);  // w[-1] = 0 (flat)

   // S3: Gârleanu-Pedersen aim-portfolio trading (opt-in via cfg.gp_trading). Built
   // ONCE for the whole run: a single whole-panel Diagonal FactorModel (the SAME
   // inert-default kind risk::RiskModelConfig{} builds on the MVO branch above),
   // INDEPENDENT of --risk-model/risk_cfg -- position-mode never threads risk_cfg
   // today, and extending risk-model SELECTION into position mode is S1/S2 turf,
   // not S3's. Kept outside the per-period loop: it is the fixed risk lens
   // gp_aim_and_value inverts every period, not a per-period PIT refit (Diagonal's
   // own variance estimate already reads the whole research panel once, exactly as
   // the MVO Diagonal branch above does -- no look-ahead concern distinct from that
   // existing path).
   //
   // Fail-open (never silent, per the ROADMAP guardrail): if the build Errs (e.g.
   // `research` lacks "close"), gp_trading is disabled FOR THIS RUN -- every period
   // falls back to the pre-S3 linear blend, and the fallback is recorded in kvs
   // (see write_books call below) so it is never silent.
   std::optional<risk::FactorModel> gp_v;
   bool gp_fallback = false;
   if (cfg.gp_trading) {
     auto gp_artifact = build_risk_model(research, risk::RiskModelConfig{});
     if (gp_artifact.has_value()) {
       auto gp_model = data::artifact_to_factor_model(*gp_artifact);
       if (gp_model.has_value()) {
         gp_v.emplace(std::move(*gp_model));
       } else {
         gp_fallback = true;
       }
     } else {
       gp_fallback = true;
     }
   }
   ```
   Replace the existing blend (`stage_optimize.cpp:182-195`) with:
   ```cpp
   // Partial-step: either the Gârleanu-Pedersen aim-portfolio trade (opt-in,
   // cfg.gp_trading) or the pre-S3 linear blend toward the freshly-shaped target.
   // See garleanu_pedersen.hpp for the closed-form math; this call site is the
   // ONLY thing S3 changes. Guard preserves byte-identical legacy output when
   // gp_trading is false (the untouched `else if` arm below).
   if (cfg.gp_trading && gp_v.has_value()) {
     // alpha_bar: the per-name RETURN-space signal this period -- the SAME raw
     // cross-section shape_book above just turned into the legacy target `w`.
     // NaN names are preserved (gp_aim_and_value maps them to 0 in the V^-1 apply).
     std::vector<atx::f64> alpha_bar(cs.begin(), cs.end());
     auto gp = risk::gp_aim_and_value(std::span<const atx::f64>{alpha_bar}, *gp_v,
                                      cfg.gp_risk_aversion);
     if (gp.has_value()) {
       // Shape the GP aim through the SAME gross/name-cap/dollar-neutral contract
       // as the legacy target `w`, so the GP path never breaks the book-shape
       // invariants the rest of this function (and shape_book's own header)
       // document.
       std::vector<atx::f64> aim = gp->aim_pos;
       shape_book(aim, std::span<const std::uint8_t>{live}, gross_val, name_cap_val);
       // kappa: cfg.trade_rate discounted by the trade-cost-scale knob
       // (gp_trade_cost_scale == 0 => kappa == trade_rate_val, inert).
       const atx::f64 kappa = trade_rate_val / (1.0 + cfg.gp_trade_cost_scale);
       w = risk::gp_turnover_native_step(std::span<const atx::f64>{prev},
                                        std::span<const atx::f64>{aim}, kappa);
     } else {
       // Degenerate per-period fallback (defensive -- lambda>=0 is CLI-guarded and
       // the length always matches M by construction, so this should not fire in
       // practice). Never silently drop the period: trade the legacy way instead.
       if (trade_rate_val < 1.0) {
         for (atx::usize i = 0; i < M; ++i) {
           w[i] = prev[i] + trade_rate_val * (w[i] - prev[i]);
         }
       }
     }
   } else if (trade_rate_val < 1.0) {
     for (atx::usize i = 0; i < M; ++i) {
       w[i] = prev[i] + trade_rate_val * (w[i] - prev[i]);
     }
   }
   ```
   Finally, record the fallback (never-silent) in the position-mode `kvs`, next to the existing
   `trade_rate` kvs emit (`stage_optimize.cpp:216-218`):
   ```cpp
   ATX_TRY(auto sr, write_books(books_flat, turnover, cost_bps));
   if (cfg.set_flags.count("trade-rate"))
       sr.kvs.emplace_back("trade_rate", std::to_string(trade_rate_val));
   if (cfg.gp_trading)
       sr.kvs.emplace_back("gp_trading", gp_fallback ? "fallback" : "on");
   return atx::core::Ok(std::move(sr));
   ```

3. Build + run:
   ```powershell
   cmake --build --preset dev --target atx-impl-tests
   ctest --preset dev -R AtxImplOptimizeGpTrading
   ```
   Expect: build exit 0; `GpTradingChangesBookWhenSet` now PASSES (digests differ).

4. Off-path sanity (byte-identity is formally proven in S3-2, but confirm here too as part of
   GREEN): re-run the full `atx-impl` suite to confirm no other test's digest moved:
   ```powershell
   ctest --preset dev -R AtxImpl
   ```
   Expect: identical pass count to the S3-0 baseline (the new `if`/`else if` structure's `else if`
   arm is textually the pre-S3 code, unreachable-change for `gp_trading=false`).

**Accept:**
- `GpTradingChangesBookWhenSet` RED→GREEN (compiles + asserts digest divergence once wired).
- Full `atx-impl` regression pass count unchanged from pre-S3-1.
- No `CMakeLists.txt` edits (confirmed: only existing, already-registered files touched).

**Commit:**
```
git add atx-impl/src/stage_optimize.cpp atx-impl/tests/stage_optimize_gp_trading_test.cpp
git commit -m "feat(p9-s3): S3-1 wire gp_aim_and_value/gp_turnover_native_step into stage_optimize position-mode trade step

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### S3-2 — Determinism battery + mean-reverting turnover proof + sprint close

**Goal:** the mandatory four test classes, formally proven; sprint close ledger.

**Files:** `atx-impl/tests/stage_optimize_gp_trading_test.cpp` (extend); ledger.

**Steps:**

1. **(a) Off-path byte-identity** — mirrors `stage_optimize_riskmodel_test.cpp`'s
   `DiagonalByteIdentical` pattern: an implicit-default run must equal a run with every new field
   set EXPLICITLY to its inert value (a stronger proof than a hardcoded pin — it proves the fields
   are inert even when present, not merely when absent).
   ```cpp
   TEST_F(AtxImplOptimizeGpTrading, OffPathByteIdentical) {
     constexpr usize M = 4, D = 60;
     std::vector<f64> close(D * M);
     for (usize t = 0; t < D; ++t)
       for (usize i = 0; i < M; ++i)
         close[t * M + i] = 100.0 * std::exp(0.0003 * (1.0 + 0.2 * i) * static_cast<f64>(t));
     std::vector<std::uint8_t> uni(D * M, 1u);
     auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
     ASSERT_TRUE(rp.has_value());
     const std::string research_path = (tmp_dir_ / "research_a.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());

     std::vector<f64> combo(D * M);
     for (usize t = 0; t < D; ++t)
       for (usize i = 0; i < M; ++i)
         combo[t * M + i] = (i % 2 == 0) ? 1.0 : -1.0;
     auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
     ASSERT_TRUE(cp.has_value());
     const std::string combo_path = (tmp_dir_ / "combo_a.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

     atx::impl::RunConfig cfg;
     cfg.panel = research_path;
     cfg.combo = combo_path;
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "daily";
     cfg.position_mode = true;
     cfg.trade_rate = 0.5;
     cfg.set_flags.emplace("trade-rate");

     cfg.books_out = (tmp_dir_ / "books_implicit.bin").string();
     auto implicit_r = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(implicit_r.has_value()) << implicit_r.error().message();

     cfg.gp_trading = false;
     cfg.gp_risk_aversion = 0.0;
     cfg.gp_trade_cost_scale = 0.0;
     cfg.books_out = (tmp_dir_ / "books_explicit_inert.bin").string();
     auto explicit_r = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(explicit_r.has_value()) << explicit_r.error().message();

     EXPECT_EQ(implicit_r->digest, explicit_r->digest)
         << "explicit inert gp_trading/gp_risk_aversion/gp_trade_cost_scale must reproduce the "
            "SAME digest as the implicit (never-touched) defaults -- routing leak";

     std::ifstream fa((tmp_dir_ / "books_implicit.bin").string(), std::ios::binary);
     std::ifstream fb((tmp_dir_ / "books_explicit_inert.bin").string(), std::ios::binary);
     const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
     const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
     EXPECT_EQ(da, db) << "books.bin not byte-identical between implicit and explicit-inert configs";
   }
   ```

2. **(b) RED→GREEN — mean-reverting fixture, GP lowers turnover at matched-or-better Sharpe.**
   Construction (offline-tunable constants, same idiom as `sign_deploy_test.cpp`'s
   "offline-solved" fixture — tune amplitudes/vol during TDD if the inequality doesn't hold on
   first try): `M = 4`. Names 0-1 are **stable/persistent-edge**: alpha is a constant `+1.0`/`-1.0`
   sign, research `close` has a real, low-noise drift matching that sign (a genuine, low-volatility
   edge). Names 2-3 are **noisy/mean-reverting, zero true edge**: alpha **flips sign every
   period** (`+1.0` on even `t`, `-1.0` on odd `t`), and research `close` is HIGH-volatility
   zero-drift noise (so their Diagonal-model variance `D_i` is large — the GP aim divides by
   `D_i` and damps them; the raw `shape_book` target does not, so it chases their flip every
   period).
   ```cpp
   TEST_F(AtxImplOptimizeGpTrading, GpLowersTurnoverAtMatchedOrBetterSharpe) {
     constexpr usize M = 4, D = 120;
     struct Lcg {
       std::uint64_t s;
       f64 next() noexcept {
         s = s * 6364136223846793005ULL + 1442695040888963407ULL;
         return 2.0 * (static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U)) - 1.0;
       }
     };
     Lcg rng{0xC0FFEEULL};
     std::vector<f64> close(D * M, 100.0);
     std::vector<f64> combo(D * M, 0.0);
     std::vector<f64> px(M, 100.0);
     for (usize t = 0; t < D; ++t) {
       // Names 0-1: stable, low-noise, genuinely profitable, constant-sign alpha.
       for (usize i = 0; i < 2; ++i) {
         const f64 sign = (i == 0) ? 1.0 : -1.0;
         const f64 ret = sign * 0.0015 + 0.0005 * rng.next();
         px[i] *= (1.0 + ret);
         close[t * M + i] = px[i];
         combo[t * M + i] = sign;
       }
       // Names 2-3: noisy, zero-drift, mean-reverting (flipping) alpha, zero true edge.
       for (usize i = 2; i < M; ++i) {
         const f64 ret = 0.02 * rng.next(); // high vol, zero mean
         px[i] *= (1.0 + ret);
         close[t * M + i] = px[i];
         combo[t * M + i] = (t % 2 == 0) ? 1.0 : -1.0;
       }
     }
     std::vector<std::uint8_t> uni(D * M, 1u);
     auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
     ASSERT_TRUE(rp.has_value());
     const std::string research_path = (tmp_dir_ / "research_mr.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());
     auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
     ASSERT_TRUE(cp.has_value());
     const std::string combo_path = (tmp_dir_ / "combo_mr.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

     auto total_turnover = [](const std::string& meta_path) {
       std::ifstream f(meta_path);
       std::string line;
       f64 total = 0.0;
       while (std::getline(f, line)) {
         const auto pos = line.find("turnover=");
         if (pos == std::string::npos) continue;
         const auto end = line.find(' ', pos);
         total += std::stod(line.substr(pos + 9, end - (pos + 9)));
       }
       return total;
     };

     atx::impl::RunConfig cfg;
     cfg.panel = research_path;
     cfg.combo = combo_path;
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "daily";
     cfg.position_mode = true;
     cfg.trade_rate = 1.0; // full step both ways -- isolates the aim vs. target choice itself

     cfg.books_out = (tmp_dir_ / "books_legacy_mr.bin").string();
     auto legacy = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(legacy.has_value()) << legacy.error().message();
     const f64 legacy_turnover = total_turnover(cfg.books_out + ".meta.txt");

     cfg.gp_trading = true;
     cfg.gp_risk_aversion = 0.5;
     cfg.books_out = (tmp_dir_ / "books_gp_mr.bin").string();
     auto gp = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(gp.has_value()) << gp.error().message();
     const f64 gp_turnover = total_turnover(cfg.books_out + ".meta.txt");

     // RED (pre-S3-1): both paths identical (gp_trading unread) -> this would be EXPECT_EQ and
     // pass trivially; GREEN (post-wire): GP must realize strictly lower cumulative turnover --
     // the noisy/flipping names (2-3) are damped by their own high Diagonal variance under the
     // GP aim, while the legacy target chases their flip every period at full rate.
     EXPECT_LT(gp_turnover, legacy_turnover)
         << "GP-wired trading must realize strictly lower cumulative turnover than the legacy "
            "linear blend on the mean-reverting/noisy fixture (gp=" << gp_turnover
         << " legacy=" << legacy_turnover << ")";

     // "Matched-or-better Sharpe": realized book·true-drift alignment on the persistent-edge
     // names (0-1) must not be worse under GP -- the noisy names carry no true edge, so damping
     // them costs nothing. Proxy: sum of period-avg |w| on names 0-1 (capital kept on the real
     // edge) must be >= under GP than under legacy.
     auto avg_edge_weight = [&](const std::string& books_path) {
       auto br = atx::impl::read_panel(books_path);
       if (!br.has_value()) return 0.0;
       auto wfid = br->field_id("weight");
       if (!wfid.has_value()) return 0.0;
       f64 total = 0.0;
       for (usize s = 0; s < br->dates(); ++s) {
         const auto row = br->field_cross_section(*wfid, s);
         total += std::fabs(row[0]) + std::fabs(row[1]);
       }
       return total / static_cast<f64>(br->dates());
     };
     const f64 legacy_edge = avg_edge_weight((tmp_dir_ / "books_legacy_mr.bin").string());
     const f64 gp_edge     = avg_edge_weight((tmp_dir_ / "books_gp_mr.bin").string());
     EXPECT_GE(gp_edge, legacy_edge - 1e-6)
         << "GP must not reduce capital on the genuinely profitable names (0-1) vs legacy "
            "(gp_edge=" << gp_edge << " legacy_edge=" << legacy_edge << ")";
   }
   ```
   Note for the implementer: the `EXPECT_LT`/`EXPECT_GE` thresholds may need constant re-tuning
   (noise amplitude, `gp_risk_aversion`) during the actual RED phase, exactly as
   `sign_deploy_test.cpp`'s `kAlpha`/`kMu`/`kVol` triple was offline-solved — the fixture's
   CONSTRUCTION (variance-differentiated noisy vs. stable names) is the load-bearing part, not
   the literal numeric constants above.

3. **(c) Twice-run determinism:**
   ```cpp
   TEST_F(AtxImplOptimizeGpTrading, TwiceRunByteIdentical) {
     // Reuses the S3-1 GpTradingChangesBookWhenSet fixture construction inline (kept
     // self-contained per test-file convention -- no cross-test fixture coupling).
     constexpr usize M = 2, D = 20;
     std::vector<f64> close(D * M);
     for (usize t = 0; t < D; ++t) {
       close[t * M + 0] = 100.0 * std::exp(0.001 * static_cast<f64>(t));
       close[t * M + 1] = 100.0 * std::exp(-0.0005 * static_cast<f64>(t));
     }
     std::vector<std::uint8_t> uni(D * M, 1u);
     auto rp = alpha::Panel::create(D, M, {"close"}, {close}, uni);
     ASSERT_TRUE(rp.has_value());
     const std::string research_path = (tmp_dir_ / "research_tw.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*rp, research_path).has_value());
     std::vector<f64> combo(D * M);
     for (usize t = 0; t < D; ++t) { combo[t * M + 0] = 1.0; combo[t * M + 1] = -1.0; }
     auto cp = alpha::Panel::create(D, M, {"alpha"}, {combo}, uni);
     ASSERT_TRUE(cp.has_value());
     const std::string combo_path = (tmp_dir_ / "combo_tw.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*cp, combo_path).has_value());

     atx::impl::RunConfig cfg;
     cfg.panel = research_path;
     cfg.combo = combo_path;
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "daily";
     cfg.position_mode = true;
     cfg.trade_rate = 0.4;
     cfg.set_flags.emplace("trade-rate");
     cfg.gp_trading = true;
     cfg.gp_risk_aversion = 1.0;
     cfg.gp_trade_cost_scale = 0.25;

     cfg.books_out = (tmp_dir_ / "books_tw_a.bin").string();
     auto r1 = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r1.has_value()) << r1.error().message();
     cfg.books_out = (tmp_dir_ / "books_tw_b.bin").string();
     auto r2 = atx::impl::run_optimize(cfg);
     ASSERT_TRUE(r2.has_value()) << r2.error().message();

     EXPECT_EQ(r1->digest, r2->digest);
     std::ifstream fa((tmp_dir_ / "books_tw_a.bin").string(), std::ios::binary);
     std::ifstream fb((tmp_dir_ / "books_tw_b.bin").string(), std::ios::binary);
     const std::vector<char> da((std::istreambuf_iterator<char>(fa)), std::istreambuf_iterator<char>());
     const std::vector<char> db((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
     EXPECT_EQ(da, db);
   }
   ```

4. **(d) seq==parallel:** documented N/A (see Determinism contract above) — add a one-line comment
   at the top of the test file recording the justification so the consolidated-battery convention
   (mirroring p8-S3-5) is visibly satisfied, not silently dropped:
   ```cpp
   // (d) seq==parallel: N/A for this sprint -- the position-mode trade loop is an inherently
   // sequential per-period state machine (w[s] depends on prev=w[s-1]); no parallel_for/executor
   // touches stage_optimize.cpp's position-mode branch before or after S3. See sprint-3's
   // Determinism contract section for the full justification.
   ```

5. Build + run the full new suite, then the whole-repo regression:
   ```powershell
   cmake --build --preset dev --target atx-impl-tests
   ctest --preset dev -R AtxImplOptimizeGpTrading
   ctest --preset dev -R AtxImpl
   ctest --preset dev -R "GpTurnover|GarleanuPedersen"
   ```
   Expect: all new tests PASS; full `AtxImpl*` pass count unchanged except the new suite's tests
   added; the untouched engine-level GP tests (`gp_turnover_test.cpp`,
   `risk_garleanu_pedersen_test.cpp`) still PASS unmodified (confirms the frozen body was never
   edited).

6. Sprint close: append the bench/acceptance summary + commit list to
   `atx-engine/plans/p9/sprint-3-progress.md` (mirrors p8-S3's
   `sprint-3-progress.md` "close" convention).

**Accept:**
- (a)/(b)/(c) all green; (d) documented N/A with justification.
- No regression in `atx-impl-tests` or the untouched `atx-engine-tests` GP suites.
- Ledger closed with the full unit table + commit SHAs.

**Commit:**
```
git add atx-impl/tests/stage_optimize_gp_trading_test.cpp atx-engine/plans/p9/sprint-3-progress.md
git commit -m "test(p9-s3): S3-2 consolidated determinism battery + mean-reverting turnover proof; sprint close

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Sequencing

1. **S3-0 first** (ledger + config plumbing) — every unit reads the 3 new `RunConfig` fields.
2. **S3-1** (the wire) — depends on S3-0's fields existing; lands AFTER p9-S1 (rebase if S1 has
   already landed on this branch; otherwise proceed against `main @ c7c7b44` since S3 reads none of
   S1's additions).
3. **S3-2** (battery + close) — depends on S3-1's wire being in place; the RED phase for (b) is
   about tuning the fixture's constants against the REAL wired behavior, not proving the wire
   compiles (that was S3-1's own RED/GREEN).

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| `alpha_bar` == raw per-period `cs` (no cross-period decay averaging) understates the "GP anticipates the reversal" story from `garleanu_pedersen.hpp`'s own docs | The turnover win comes from variance-damping (via `V`), not decay-anticipation, on names with a real true edge vs. pure noise | Documented explicitly (Architecture note, ambiguity (i)); the S3-2 fixture is built to exercise the VARIANCE-damping mechanism honestly, not to fake a decay story S3 doesn't wire |
| `gp_trade_cost_scale`'s `kappa = trade_rate/(1+scale)` mapping is S3-invented, not derivable from the frozen header | A reviewer could reasonably ask for a different mapping | Flagged as ambiguity (ii) for the reconciler; the mapping is monotone, inert at 0.0, and never mutates the frozen function's own inputs beyond the `trade_rate` scalar it already accepts |
| GP always builds a `Diagonal` `V` in position mode, ignoring `--risk-model factor` | An operator who sets both `--gp-trading` and `--risk-model factor` might expect Factor covariance in the GP aim too | Documented scope cut (ambiguity (iii)); `--risk-model` remains an MVO-branch-only flag per S1/S2 ownership — extending GP to Factor is explicit future work, not a silent gap (no flag combination silently no-ops: `--gp-trading` always changes behavior when set, `--risk-model` always changes the MVO branch when set — they are simply independent knobs this sprint, stated plainly) |
| `gp_aim_and_value`/`gp_turnover_native_step` signature drift vs. what this plan assumes | S3-1 won't compile | Read `garleanu_pedersen.hpp:144-145,201-203` at kickoff (already re-confirmed verbatim in this plan); assemble the call to the ACTUAL signature |
| The one-time `build_risk_model` call fails on a real-world research panel missing "close" | `--gp-trading` would silently do nothing if not handled | Fail-open + loud: `gp_fallback` bool + a `gp_trading=fallback` kvs row (S3-1 step 2) — never silent per the ROADMAP's "fail-loud, never silent no-op" guardrail |
| S3-2's `EXPECT_LT`/`EXPECT_GE` numeric fixture constants may not hold on first attempt | RED phase churn | Documented as expected/normal (mirrors `sign_deploy_test.cpp`'s own "offline-solved" caveat); the fixture's STRUCTURE (variance-differentiated noisy vs. stable names) is load-bearing, constants are tunable |
| Merge collision with p9-S1 in `stage_optimize.cpp` | Manual merge of adjacent hunks | S1 owns lines in the MVO branch (~252-273 pre-S3); S3 owns lines in the position-mode branch (~170, ~182-220 pre-S3) — textually disjoint; land S1 first and rebase S3 to minimize diff-context noise, per the ROADMAP's explicit S1↔S3 seam note |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** `--gp-trading` absent (or explicitly false with inert
  `gp_risk_aversion`/`gp_trade_cost_scale`) reproduces the exact pre-S3 position-mode digest; the
  pinned p9 ROADMAP goldens (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, `AtxImplDiscover` slice,
  `LibraryVerdict.AdmitKindEnumFrozenPrefix`) are untouched by this sprint's files and need no
  re-verification beyond the standard full regression.
- **Per-task RED→GREEN:** S3-0's config-parse RED (undeclared fields) → GREEN; S3-1's
  digest-divergence RED → GREEN; S3-2's turnover-inequality RED (pre-wire, trivially equal) →
  GREEN (post-wire, strictly lower).
- **GP-trading win, measured:** on the S3-2 mean-reverting fixture, record cumulative book
  turnover and the persistent-edge-name capital proxy for {legacy linear blend, GP-wired} — GP
  must show strictly lower turnover and non-worse capital on the true-edge names (the concrete,
  quantified S3 claim).
- **Twice-run** on the GP path; **seq==parallel documented N/A** with justification.
- **Full regression:** `ctest --preset dev -R AtxImpl` pass count unchanged except the new suite's
  additions; `ctest --preset dev -R "GpTurnover|GarleanuPedersen"` (the untouched frozen-body
  engine tests) still green, confirming `garleanu_pedersen.{hpp,cpp}` was never edited.

---

## Out of scope

- Extending GP-trading to ride `--risk-model factor` / per-step PIT covariance in position mode
  (ambiguity (iii)) — future work; S3 always uses a whole-panel Diagonal `V`.
- Wiring `MultiHorizonOptimizer`/`SignalHorizon` decay-averaging into position mode to give
  `alpha_bar` genuine cross-period decay smoothing (ambiguity (i)) — a materially larger lift
  (a full alternate optimizer/dispatch stack), not "the partial-trade step."
- The matrix-Riccati `A_xx` refinement (`garleanu_pedersen.hpp`'s own recorded, explicitly
  NOT-shipped lift) — frozen, out of scope for every sprint until a future one explicitly re-opens
  it.
- Wiring GP trading into the MVO branch (5b) — that branch already has its own turnover control
  (`mc.single.turnover_penalty`, a QP-native cost, not a blend); this sprint's GP wire targets only
  the position-mode branch's linear blend, per the ROADMAP's own cited root cause and file:line.
- Any change to `risk::RiskModelConfig`, `build_risk_model`'s dead-alpha/group-neutralize
  parameters, or the MVO branch's per-step Factor cadence — all p9-S1 territory.
