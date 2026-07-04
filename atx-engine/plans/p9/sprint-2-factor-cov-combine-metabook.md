# Sprint S2 — Factor Covariance Reaches Combine + Metabook

**Goal:** `--risk-model factor` governs the whole book, not just `stage_optimize`. Thread
`risk::RiskModelConfig` into `run_combine` (kill the Diagonal hardcode at
`stage_combine.cpp:765,771`) and add the deferred `model_at` Factor overload to
`stage_metabook` (the p8-S2 documented seam). Diagonal (default, `cfg.risk_model=="diagonal"`)
stays byte-identical at both call sites. **Zero new estimator math** — S2 only routes an
already-built covariance selector to two more places; the Factor-covariance math itself
(`FactorModelBuilder::build_components`, `cleaned_alpha_cov`, `build_risk_model`) is frozen,
already shipped by p8-S1/S3, and unmodified here.

**Branch:** `feat/p9` (worktree `C:\atx-wt\p9`). **Predecessor state (confirmed by reading the
worktree, not assumed):** p8's S1 (`stage_riskmodel.{cpp,hpp}`, `diag_risk.hpp`,
`risk::RiskModelConfig`, `cfg.risk_model`/`cfg.dead_alpha_factors`/`cfg.group_neutralize`,
`stage_optimize.cpp`'s per-step Factor loop) and p8's S3-4 (`run_combine`'s 3-arg
`RiskModelConfig`-aware overload, `fit_shrinkage_mv_cleaned_cov`, `data::cleaned_alpha_cov`)
are **already landed and unmodified in this worktree** — verified by reading
`atx-impl/src/stage_optimize.cpp`, `atx-impl/src/stage_combine.cpp`, `atx-impl/src/config.hpp`,
and `atx-impl/src/stage_riskmodel.hpp` directly. This changes S2's actual shape versus the
ROADMAP's summary: the Factor covariance math for **both** stages already exists and is
already tested; S2's entire job is two thin **routing** fixes plus one genuinely new
**per-step producer loop** in metabook (mirrored from `stage_optimize.cpp`'s own loop, not
invented).

**Owns (exclusive):** `atx-impl/src/stage_combine.cpp` (the 0-/1-arg `run_combine` bodies
only — the 3-arg overload and everything inside it is S3-4's frozen, unmodified code),
`atx-impl/src/stage_metabook.{cpp,hpp}`, new test files under `atx-impl/tests/`.
`atx-impl/src/config.{hpp,cpp}` is listed as S2-touchable by the ROADMAP's ownership matrix
**only via the registry** — see "Config surface" below: the registry requires **no new
field**, so S2 makes **zero edits** to either file.

**Must NOT touch:** `atx-impl/src/stage_optimize.cpp` (S1/S3-owned; S2 only *reads* it as the
pattern to mirror), `atx-impl/src/stage_riskmodel.{cpp,hpp}` and `atx-impl/src/diag_risk.hpp`
(S1-owned, frozen — S2 calls `build_risk_model`/`diagonal_risk_model`, does not re-derive),
`atx-engine/include/atx/engine/risk/factor_model.hpp` and
`atx-engine/include/atx/engine/data/{factor_model_artifact,adapt_factor}.hpp` (frozen estimator
+ artifact surface — S2 calls `build_components`-derived helpers, never edits them),
`atx-engine/include/atx/engine/combine/combiner.hpp` (frozen per p8-S3-4's own ownership
rationale: editing it in place would double-shrink or violate the ownership boundary),
`atx-impl/src/stage_run.cpp` / `atx-impl/src/dispatch.cpp` (hub wiring — S2's fix must flow
through these files **without editing them**, by construction of the routing fix), `factory/*`,
`alpha/oracle.hpp` (universally untouchable, every sprint).

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

## The orphan gap (verified file:line, current worktree state)

| Gap | File:line | Evidence |
|---|---|---|
| `run_combine`'s 0-arg entry hardcodes Diagonal | `stage_combine.cpp:765` | `return run_combine(cfg, combiner_cfg0, risk::RiskModelConfig{});` — ignores `cfg.risk_model` entirely |
| `run_combine`'s 1-arg entry hardcodes Diagonal | `stage_combine.cpp:771` | `return run_combine(cfg, combiner_cfg, risk::RiskModelConfig{});` — same hardcode, second call site |
| The 3-arg overload already consumes Factor correctly | `stage_combine.cpp:774-776`, `:1029` (`fit_shrinkage_mv_cleaned_cov` dispatch), breadth instrumentation (step 12) | p8-S3-4, unmodified: `risk_cfg.kind==Factor` swaps `mle_covariance` for `data::cleaned_alpha_cov` in the ShrinkageMv fit — but **only reachable if a caller builds `risk_cfg` explicitly**; `dispatch.cpp:102` (`run_combine(cfg)`) and `stage_run.cpp:107` (`run_combine(c_comb)`) both call the hardcoded 0-arg path |
| `build_metabook_result` always builds the whole-panel diagonal model | `stage_metabook.cpp:503` | `ATX_TRY(auto model, diagonal_risk_model(research));` — unconditional, no `RiskModelConfig` parameter exists on this function at all |
| `model_at` ignores its own argument | `stage_metabook.cpp:550` | `const auto model_at = [&](atx::usize) -> const risk::FactorModel & { return model; };` — the period argument is discarded; one whole-panel diagonal model for every rebalance step, forever |
| No `RiskModelConfig`-parameterized overload exists on `stage_metabook`'s API | `stage_metabook.hpp:155-159` | `build_metabook_result(const RunConfig&, const MetaBookStageConfig&)` / `run_metabook(const RunConfig&, const MetaBookStageConfig&)` — two-arg only |
| The seam is explicitly documented as deferred | p8 `sprint-2-progress.md:269-294` | "A Factor-model `model_at` variant is a straightforward follow-on... was not attempted by S2... Recorded here as the S5/S1/final-wave integration seam for whoever picks it up next." — S2 (p9) is that pickup |
| The precedent this sprint mirrors already exists, twice | `stage_optimize.cpp:40-48` (1-arg→2-arg forward, `run_optimize`) and `stage_combine.cpp:768-772` vs `:774-776` (the existing 1-arg→3-arg-via-hardcode) | The exact "N-arg thin wrapper builds `risk_cfg` from `cfg.risk_model` and forwards to an (N+1)-arg overload" shape S2 must replicate for `run_combine`'s wrapper and invent (mirrored, not new) for `stage_metabook` |

---

## Architecture note — what S2 actually does (two routing fixes, one mirrored producer loop)

**`run_combine` (S2-1):** The covariance-swap math already exists and is already tested
(`fit_shrinkage_mv_cleaned_cov`, `stage_combine_cleaned_cov_test.cpp`). The ONLY defect is that
the two convenience overloads callers actually reach through the CLI/`run_all`
(`dispatch.cpp:102`, `stage_run.cpp:107`) default-construct `risk::RiskModelConfig{}` instead of
reading `cfg.risk_model` (`config.hpp:307`, already a `std::string` field parsed and validated by
`config.cpp:129-136` — `{"diagonal","factor"}`, S1/S5-0 shipped, unmodified by S2). S2-1 replaces
the two hardcodes with a 3-line `risk_cfg` construction, textually identical to the pattern
`stage_optimize.cpp:40-48` already uses for `run_optimize` — duplicated locally (not extracted
into a shared helper: `stage_optimize.cpp` is not S2-owned, and inventing a new shared-header
abstraction for a 3-line block violates the "follow existing local patterns" guardrail).

**`stage_metabook` (S2-2):** No hardcode to kill — there is no `RiskModelConfig` parameter at
all yet. S2-2 adds one, as an **additive overload** (mirroring `stage_optimize.cpp`'s own
1-arg/2-arg split, and `stage_combine.cpp`'s 2-arg/3-arg split — the codebase's established
convention is always a separate N-arg/  (N+1)-arg pair, never a default function argument). Inside
the new 3-arg body, `risk_cfg.kind == Diagonal` keeps today's exact single whole-panel
`diagonal_risk_model(research)` call (byte-identical). `risk_cfg.kind == Factor` builds **one
`FactorModel` per rebalance step**, PIT-style, by calling the SAME public `build_risk_model`
(`stage_riskmodel.hpp:145-152`, S1-shipped, frozen) at `fit_end = sched.periods[s] + 1` for each
step `s` — textually the same loop `stage_optimize.cpp:248-273` already runs, including its
diagonal warm-up fallback for a step too early to support a genuine Factor fit
(`fit_end < 2` or an under-determined cross-section). This is **not new estimator logic**; it is
the same per-step PIT-fit call, driving a different consumer (`fund::MetaBook::run`'s `model_at`
callback instead of `risk::MultiPeriodOptimizer`'s).

**Why `model_at`'s argument convention matters:** `stage_metabook.cpp:477-479`'s own comment
confirms `model_at`/`returns_at` are called with the **raw date** (`sched.periods[p]`), not a
step index — exactly the same convention `stage_optimize.cpp:280-285`'s `model_at` uses
(`step_models[period / step]`). This is why the metabook per-step lookup can reuse
`stage_optimize`'s exact `period / step` indexing — the two stages already share the callback
contract.

---

## Determinism contract (Sprint S2)

Reused from the p9 ROADMAP verbatim; restated with S2's concrete opt-in field:

- **The opt-in field is `cfg.risk_model` (`std::string`, default `"diagonal"`), not a new
  field** — S2 adds no field to the SHARED CONFIG-FIELD REGISTRY. `risk_model=="diagonal"`
  (the `RunConfig{}` default) constructs `risk::RiskModelConfig{}` (`kind==Diagonal`) at BOTH
  new call sites, byte-identical by construction to the pre-S2 hardcode.
- **`risk_model=="factor"`** is opt-in: `run_combine` reaches the already-shipped
  `fit_shrinkage_mv_cleaned_cov`/breadth-instrumentation path; `run_metabook` reaches the new
  per-step `build_risk_model` producer loop.

**Four test classes per call site (mandatory):**
(a) off-path byte-identity — `risk_model=="diagonal"` (default) unchanged;
(b) on-path RED→GREEN — combine: digest changes + reaches the S3-4 diversification win
(cf. p8-S3-4's own measured `max|w|_diagonal=0.164248 → max|w|_factor=0.142554`, unmodified,
now reachable via `cfg.risk_model`); metabook: `model_at`'s new per-step model is PIT-correct
(a step's fit window is strictly `[0, step's own date+1)` — no later step's data enters it);
(c) twice-run — same config → same digest, both call sites;
(d) seq==parallel where touched — combine: N/A, documented (pure routing fix, no new loop);
metabook: the new per-step loop's own primitive (`build_risk_model`) is proven
order-independent directly (forward vs. reverse construction order → identical artifacts).

**Byte-identity:** element-wise `std::bit_cast<std::uint64_t>` (matches signed zeros) where a
books panel is compared cell-by-cell; `StageResult::digest` equality otherwise (matches this
codebase's existing `MetabookStageBoundary`/`StageCombineCleanedCov` test idiom).

---

## Config surface — resolved, no new field

The ROADMAP registry's S2 rows read: *"thread `risk::RiskModelConfig` from `cfg.risk_model`
(no new field — remove the hardcoded `RiskModelConfig{}`)"* and *"new overload
`build_metabook_result(..., const risk::RiskModelConfig&)` / `run_metabook(cfg, risk_cfg)`;
default overload keeps Diagonal"*. Reading the actual `config.hpp:302-309` state (S1/S5-0
already shipped `cfg.risk_model`, `cfg.dead_alpha_factors`, `cfg.group_neutralize`), **no
`config.{hpp,cpp}` edit is required or made by this sprint.** `atx-impl/src/config.hpp` and
`atx-impl/src/config.cpp` are not touched by any S2 unit below.

**Resolved ambiguity (see the reconciler note at the end of this doc):** the existing 2-arg
`build_metabook_result(cfg, scfg)` / `run_metabook(cfg, scfg)` are changed from doing nothing
risk-model-aware to building `risk_cfg` **from `cfg.risk_model`** and forwarding to the new
3-arg overload — exactly mirroring `stage_optimize.cpp:40-48`'s 1-arg wrapper. This is a
deliberate choice, not the only literal reading of the registry text (see below); it is required
to make the sprint's own stated goal true for metabook via the real CLI/`run_all` path.

---

## Dependency / wiring map

```
config.hpp:307            cfg.risk_model (S1/S5-0, UNCHANGED, no edit)  ─────────┐
                                                                                  │
stage_optimize.cpp:40-48  run_optimize(cfg) 1-arg->2-arg pattern  ── MIRRORED ──►│
                                                                                  │
S2-1: stage_combine.cpp:765,771  run_combine 0-/1-arg  ◄─────────────────────────┤ (builds risk_cfg
        └─ forwards to stage_combine.cpp:774  run_combine(cfg,ccfg,risk_cfg)     │  from cfg.risk_model,
             (S3-4, UNCHANGED) ─┬─ step 8a: fit_shrinkage_mv_cleaned_cov         │  same 3 lines both
                                 └─ step 12: breadth via data::cleaned_alpha_cov │  call sites)
                                                                                  │
S2-2: stage_metabook.hpp   NEW build_metabook_result/run_metabook 3-arg  ◄───────┘
        stage_metabook.cpp:405  2-arg becomes a thin forwarder (builds risk_cfg
                                 from cfg.risk_model, calls the 3-arg body)
        stage_metabook.cpp:503,550  Diagonal branch: UNCHANGED diagonal_risk_model(research)
                                 Factor branch (NEW): per-step build_risk_model(research,
                                 risk_cfg, {}, nullptr, {}, 0, sched.periods[s]+1) — mirrors
                                 stage_optimize.cpp:248-273 exactly, including the diagonal
                                 warm-up fallback for an under-determined early step
        stage_riskmodel.hpp:145-152  build_risk_model (S1, frozen, called not re-derived)
        data/adapt_factor.hpp:48-49  artifact_to_factor_model (S1, frozen, called)
        data/factor_model_artifact.hpp:210-213  digest_artifact (used by S2-2's (d) test)

atx-impl/src/dispatch.cpp:102,108, atx-impl/src/stage_run.cpp:37,107  UNCHANGED — these already
    call the 0-/2-arg entry points S2 fixes internally; --risk-model reaches both stages with
    ZERO edits to either hub file.
```

---

## Tasks

### S2-0 — Open ledger + confirm the config/wiring surface (do first; audit only, no code)

**Goal:** record the sprint's starting state precisely (this doc already did the file:line
audit above) so S2-1/S2-2 have a pinned, agreed-upon baseline, and open the ledger file every
later unit appends to. **No production or test code changes in this unit.**

**Steps:**
1. Create `atx-engine/plans/p9/sprint-2-progress.md` with an opening entry recording: (a) the
   confirmed pre-existing state (`cfg.risk_model` already shipped by S1/S5-0; `run_combine`'s
   3-arg Factor path already shipped by S3-4; neither needs a new field); (b) the two exact
   defect sites (`stage_combine.cpp:765,771`; `stage_metabook.cpp` has no `RiskModelConfig`
   parameter at all); (c) the resolved config-surface ambiguity (metabook's 2-arg overload
   becomes `cfg.risk_model`-aware, not frozen-Diagonal — see "Config surface" above) with its
   one-line rationale.
2. No source edit. Confirm the existing full suite is green before starting (baseline):
   ```powershell
   $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
   Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
   Set-Location C:\atx-wt\p9
   cmake --preset dev
   cmake --build --preset dev
   ctest --preset dev --output-on-failure
   ```
   Expected: `100% tests passed` (baseline pass count recorded in the ledger entry). This is
   the pre-S2 GREEN baseline S2-1/S2-2's RED tests will be measured against.

**Accept:** `atx-engine/plans/p9/sprint-2-progress.md` exists with the opening entry; zero
source diff; baseline `ctest` pass count recorded.

**Commit:**
```
git add atx-engine/plans/p9/sprint-2-progress.md
git commit -m "$(cat <<'EOF'
PF-P9 S2-0 open ledger, confirm risk-model wiring baseline

No source change: audits the two defect sites (stage_combine.cpp:765,771 hardcode;
stage_metabook.{cpp,hpp} missing RiskModelConfig param) and records the config-surface
ambiguity resolution (metabook's 2-arg overload becomes cfg.risk_model-aware) before
S2-1/S2-2 land the fixes.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### S2-1 — `run_combine`: kill the Diagonal hardcode

**Goal:** the 0-arg and 1-arg `run_combine` entry points (the ones `dispatch.cpp:102` and
`stage_run.cpp:107` actually call) build `risk::RiskModelConfig` from `cfg.risk_model` instead
of default-constructing it, so `--risk-model factor` reaches the already-shipped S3-4
cleaned-covariance path through the real CLI/`run_all` surface.

**Files:** `atx-impl/src/stage_combine.cpp` (body edit only — no header/signature change;
`stage_combine.hpp`'s three declarations at lines 47-53 are untouched, since no new overload is
added here, only the existing 0-/1-arg BODIES change).

**Interfaces (unchanged signatures, changed bodies):**
```cpp
// stages.hpp:23 (untouched):        atx::core::Result<StageResult> run_combine(const RunConfig&);
// stage_combine.hpp:48 (untouched): atx::core::Result<StageResult> run_combine(const RunConfig&, const combine::CombinerConfig&);
// stage_combine.hpp:52 (untouched, ALREADY risk-aware, S3-4):
//   atx::core::Result<StageResult> run_combine(const RunConfig&, const combine::CombinerConfig&, const risk::RiskModelConfig&);
```

**Current code (`stage_combine.cpp:760-772`):**
```cpp
atx::core::Result<StageResult> run_combine(const RunConfig& cfg)
{
    ATX_TRY(auto cm0, method_from_string(cfg.method));
    combine::CombinerConfig combiner_cfg0{};
    combiner_cfg0.method = cm0;
    return run_combine(cfg, combiner_cfg0, risk::RiskModelConfig{});
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                           const combine::CombinerConfig& combiner_cfg)
{
    return run_combine(cfg, combiner_cfg, risk::RiskModelConfig{});
}
```

**Edited code:**
```cpp
// S2 (p9): builds RiskModelConfig from cfg.risk_model, mirroring stage_optimize.cpp's own
// run_optimize(const RunConfig&) seam (S1/S5-0, stage_optimize.cpp:40-48) field-for-field.
// Duplicated locally rather than shared: stage_optimize.cpp is not an S2-owned file, and this
// is a 3-line block, not an abstraction worth a new shared header. At the RunConfig{} default
// (risk_model=="diagonal") this constructs RiskModelConfig{} (kind==Diagonal) -- identical to
// the pre-S2 hardcode below -- so the no-flag path is byte-identical BY CONSTRUCTION.
static risk::RiskModelConfig risk_cfg_from_run_config(const RunConfig& cfg) {
    risk::RiskModelConfig risk_cfg{};
    risk_cfg.kind = (cfg.risk_model == "factor") ? risk::RiskModelKind::Factor
                                                  : risk::RiskModelKind::Diagonal;
    // dead_alpha_factors / group_neutralize are deliberately NOT copied from cfg here: the
    // S3-4 Factor branch this feeds (fit_shrinkage_mv_cleaned_cov) reads ONLY risk_cfg.kind --
    // it never calls build_risk_model/extract_dead_factors, so those two fields have no
    // observable effect on this path (they exist only for stage_optimize's build_risk_model
    // call). Leaving them at their RiskModelConfig{} default (false) keeps this function
    // honest about what it actually threads.
    return risk_cfg;
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg)
{
    ATX_TRY(auto cm0, method_from_string(cfg.method));
    combine::CombinerConfig combiner_cfg0{};
    combiner_cfg0.method = cm0;
    return run_combine(cfg, combiner_cfg0, risk_cfg_from_run_config(cfg));
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                           const combine::CombinerConfig& combiner_cfg)
{
    return run_combine(cfg, combiner_cfg, risk_cfg_from_run_config(cfg));
}
```

**TDD steps:**

1. **RED.** Add `atx-impl/tests/stage_combine_riskmodel_wire_test.cpp` (suite
   `StageCombineRiskModelWire`) with the three tests below, against the **pre-edit** code.
   `RiskModelFactorReachesS3_4CleanedCovPath` must FAIL (today's hardcode makes
   `r_wire->digest == r_diag->digest`, not `!=`); the other two pass trivially (they don't yet
   depend on the fix). Full file:

   ```cpp
   // stage_combine_riskmodel_wire_test.cpp -- p9 S2-1: run_combine's 0-/1-arg entry points
   // build risk::RiskModelConfig FROM cfg.risk_model instead of hardcoding kind==Diagonal
   // (stage_combine.cpp:765,771, pre-S2). Proves the CLI-shaped RunConfig surface actually
   // reaches the S3-4 Factor covariance path (fit_shrinkage_mv_cleaned_cov), not merely the
   // already-tested explicit 3-arg call (stage_combine_cleaned_cov_test.cpp).
   //
   // Suite: StageCombineRiskModelWire

   #include <cstdint>
   #include <filesystem>
   #include <fstream>
   #include <optional>
   #include <string>
   #include <vector>

   #include <gtest/gtest.h>

   #include "atx/core/types.hpp"
   #include "atx/engine/alpha/panel.hpp"
   #include "atx/engine/combine/combiner.hpp"
   #include "atx/engine/risk/factor_model.hpp"

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stage_combine.hpp"
   #include "stages.hpp"

   namespace atxtest_stage_combine_riskmodel_wire {

   using atx::f64;
   using atx::usize;
   using atx::engine::alpha::Panel;
   namespace combine = atx::engine::combine;
   namespace risk = atx::engine::risk;

   struct Lcg {
       std::uint64_t s;
       [[nodiscard]] f64 next() noexcept {
           s = s * 6364136223846793005ULL + 1442695040888963407ULL;
           const std::uint64_t hi = s >> 11U;
           return 2.0 * (static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U)) - 1.0;
       }
   };

   // Mirrors stage_combine_cleaned_cov_test.cpp's own noisy_close/make_panel exactly (same
   // seed, same shape) -- the p8-S3-4-proven panel where ShrinkageMv's Factor-kind weight fit
   // is known to diverge from the Diagonal-kind fit.
   static std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
       std::vector<f64> drift(insts);
       for (usize j = 0; j < insts; ++j) {
           drift[j] = 0.006 - 0.0024 * static_cast<f64>(j % 4U);
       }
       std::vector<f64> close(dates * insts);
       std::vector<f64> px(insts, 100.0);
       Lcg rng{seed};
       for (usize t = 0; t < dates; ++t) {
           for (usize j = 0; j < insts; ++j) {
               px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
               close[t * insts + j] = px[j];
           }
       }
       return close;
   }

   static std::optional<Panel> make_panel(usize dates, usize insts) {
       const std::vector<f64> close = noisy_close(dates, insts, 0x0FEEDBABEULL);
       auto r = Panel::create(dates, insts, {"close"}, {close}, {});
       if (!r.has_value()) {
           ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
           return std::nullopt;
       }
       return std::move(r.value());
   }

   static std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
       namespace fs = std::filesystem;
       const std::string path = (fs::temp_directory_path() / ("atx_impl_scrmw_" + stem + ".bin")).string();
       auto r = atx::impl::write_panel(panel, path);
       EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
       return path;
   }

   static std::string write_alpha_dir(const std::string& stem, const std::vector<std::string>& dsls) {
       namespace fs = std::filesystem;
       const std::string dir = (fs::temp_directory_path() / ("atx_impl_scrmw_alphas_" + stem)).string();
       fs::create_directories(dir);
       for (usize i = 0; i < dsls.size(); ++i) {
           std::ofstream f{(fs::path{dir} / ("alpha_" + std::to_string(i) + ".dsl")).string()};
           EXPECT_TRUE(f.is_open());
           f << dsls[i] << '\n';
       }
       return dir;
   }

   static std::vector<std::string> safe_dsls() {
       return {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
   }

   struct Fixture { std::string panel_path; std::string alphas_dir; };

   static Fixture make_fixture(const std::string& tag) {
       Fixture fx;
       auto panel_opt = make_panel(80, 6);
       EXPECT_TRUE(panel_opt.has_value());
       fx.panel_path = write_panel_tmp(*panel_opt, tag);
       fx.alphas_dir = write_alpha_dir(tag, safe_dsls());
       return fx;
   }

   // ===========================================================================
   //  (a) off-path byte-identity: cfg.risk_model defaulted ("diagonal") through the
   //  NOW-FIXED 0-arg run_combine(cfg) must still match the pre-p9 legacy digest --
   //  proven against the EXPLICIT kind==Diagonal 3-arg call.
   // ===========================================================================
   TEST(StageCombineRiskModelWire, DefaultRiskModelByteIdenticalToExplicitDiagonal) {
       namespace fs = std::filesystem;
       const Fixture fx = make_fixture("offpath");

       atx::impl::RunConfig cfg;
       cfg.subcommand = "combine";
       cfg.panel      = fx.panel_path;
       cfg.alphas     = fx.alphas_dir;
       cfg.method     = "shrinkage-mv";
       ASSERT_EQ(cfg.risk_model, "diagonal");

       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_offpath_0arg.bin").string();
       auto r0 = atx::impl::run_combine(cfg); // 0-arg, S2-fixed
       ASSERT_TRUE(r0.has_value()) << r0.error().message();

       combine::CombinerConfig ccfg{};
       ccfg.method = combine::CombineMethod::ShrinkageMv;
       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_offpath_explicit.bin").string();
       auto r_explicit = atx::impl::run_combine(cfg, ccfg, risk::RiskModelConfig{});
       ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

       EXPECT_EQ(r0->digest, r_explicit->digest)
           << "cfg.risk_model=='diagonal' (the default) must route the 0-arg wrapper to the "
           << "SAME Diagonal path an explicit RiskModelConfig{} reaches";
   }

   // ===========================================================================
   //  (b) on-path RED->GREEN: cfg.risk_model=="factor" through the 0-arg run_combine(cfg)
   //  reaches EXACTLY the S3-4 Factor covariance path (digest-equal to the explicit 3-arg
   //  call) and is LIVE (digest differs from the Diagonal path).
   // ===========================================================================
   TEST(StageCombineRiskModelWire, RiskModelFactorReachesS3_4CleanedCovPath) {
       namespace fs = std::filesystem;
       const Fixture fx = make_fixture("onpath");

       atx::impl::RunConfig cfg;
       cfg.subcommand = "combine";
       cfg.panel      = fx.panel_path;
       cfg.alphas     = fx.alphas_dir;
       cfg.method     = "shrinkage-mv";
       cfg.risk_model = "factor";

       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_0arg.bin").string();
       auto r_wire = atx::impl::run_combine(cfg); // 0-arg, S2-fixed
       ASSERT_TRUE(r_wire.has_value()) << r_wire.error().message();

       combine::CombinerConfig ccfg{};
       ccfg.method = combine::CombineMethod::ShrinkageMv;
       risk::RiskModelConfig factor_cfg{};
       factor_cfg.kind = risk::RiskModelKind::Factor;
       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_explicit.bin").string();
       auto r_explicit = atx::impl::run_combine(cfg, ccfg, factor_cfg);
       ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

       EXPECT_EQ(r_wire->digest, r_explicit->digest)
           << "cfg.risk_model=='factor' through the 0-arg wrapper must reach the EXACT SAME "
           << "S3-4 Factor path an explicit RiskModelConfig{kind=Factor} call reaches";

       risk::RiskModelConfig diag_cfg{}; // kind==Diagonal
       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_onpath_diag.bin").string();
       auto r_diag = atx::impl::run_combine(cfg, ccfg, diag_cfg);
       ASSERT_TRUE(r_diag.has_value()) << r_diag.error().message();

       EXPECT_NE(r_wire->digest, r_diag->digest)
           << "the wire must be LIVE: factor and diagonal digests must differ, or "
           << "cfg.risk_model is a silent no-op (the exact p9 anti-roadmap violation)";

       // The measured diversification itself (max|w| dropping under the cleaned covariance)
       // is S3-4's own proven claim (stage_combine_cleaned_cov_test.cpp,
       // FactorKindWiringIsLiveAndReportsMeasuredDiversification: max|w|_diagonal=0.164248 ->
       // max|w|_factor=0.142554 on its N~T=18/20 pool fixture), unmodified by S2. The
       // digest-equivalence assertion above is S2's own proof: that same win is now reachable
       // through cfg.risk_model, not just a direct 3-arg call.
   }

   // ===========================================================================
   //  (c) twice-run.
   // ===========================================================================
   TEST(StageCombineRiskModelWire, TwiceRunByteIdentical) {
       namespace fs = std::filesystem;
       const Fixture fx = make_fixture("twice");

       atx::impl::RunConfig cfg;
       cfg.subcommand = "combine";
       cfg.panel      = fx.panel_path;
       cfg.alphas     = fx.alphas_dir;
       cfg.method     = "shrinkage-mv";
       cfg.risk_model = "factor";

       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_twice_a.bin").string();
       auto r1 = atx::impl::run_combine(cfg);
       ASSERT_TRUE(r1.has_value()) << r1.error().message();
       cfg.combo_out = (fs::temp_directory_path() / "atx_impl_scrmw_twice_b.bin").string();
       auto r2 = atx::impl::run_combine(cfg);
       ASSERT_TRUE(r2.has_value()) << r2.error().message();

       EXPECT_EQ(r1->digest, r2->digest);
   }

   // (d) seq==parallel: documented N/A, not vacuously tested. S2-1 is a pure 3-line routing
   // fix in the 0-/1-arg wrapper bodies -- no new loop, no new threading, no new shared
   // mutable state. The Factor computation itself (fit_shrinkage_mv_cleaned_cov /
   // cleaned_alpha_cov) is S3-4's unmodified code, already covered by
   // CombineDeterminismBattery's parallel-safety story. Re-confirmed by the S2-3 regression
   // sweep, not re-implemented here.

   } // namespace atxtest_stage_combine_riskmodel_wire
   ```

2. **Build + confirm RED.**
   ```powershell
   $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
   Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
   Set-Location C:\atx-wt\p9
   cmake --preset dev
   cmake --build --preset dev --target atx-impl-tests
   ctest --preset dev -R StageCombineRiskModelWire --output-on-failure
   ```
   Expected: `RiskModelFactorReachesS3_4CleanedCovPath` FAILS on the `EXPECT_NE(r_wire->digest,
   r_diag->digest)` line (today both are the Diagonal digest); the other two tests PASS.

3. **GREEN.** Apply the edit above to `atx-impl/src/stage_combine.cpp:760-772` (insert
   `risk_cfg_from_run_config`, change both bodies). Re-run the same `ctest -R
   StageCombineRiskModelWire` — all three tests PASS.

4. **Regress.** Re-run the full existing `StageCombineCleanedCov` suite (untouched code path,
   must remain green) plus the rest of `atx-impl-tests`:
   ```powershell
   ctest --preset dev -R "StageCombineCleanedCov|StageCombine" --output-on-failure
   ```

**Accept:**
- `StageCombineRiskModelWire` — 3/3 green (RED→GREEN transition documented in the ledger with
  the before/after `ctest` output).
- `StageCombineCleanedCov` (S3-4's own suite, unmodified) — still 3/3 green.
- No signature change to any declaration in `stage_combine.hpp`.

**Commit:**
```
git add atx-impl/src/stage_combine.cpp atx-impl/tests/stage_combine_riskmodel_wire_test.cpp
git commit -m "$(cat <<'EOF'
PF-P9 S2-1 run_combine: build RiskModelConfig from cfg.risk_model

Kills the RiskModelConfig{} hardcode at stage_combine.cpp:765,771 (the 0-/1-arg entry
points dispatch.cpp/stage_run.cpp actually call). risk_model=="diagonal" (default)
constructs the same inert RiskModelConfig{} as before -- byte-identical. risk_model==
"factor" now reaches the already-shipped S3-4 cleaned-covariance path
(fit_shrinkage_mv_cleaned_cov) through the real CLI/run_all surface, not just an
explicit 3-arg call.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### S2-2 — `stage_metabook`: the deferred `RiskModelConfig`/`model_at` Factor overload

**Goal:** add an additive `RiskModelConfig`-parameterized overload of
`build_metabook_result`/`run_metabook` (the p8-S2 documented seam); the existing 2-arg
overloads become thin `cfg.risk_model`-aware forwarders (mirroring `stage_optimize.cpp`'s own
1-arg→2-arg split). `kind==Factor` builds one `FactorModel` per rebalance step via the SAME
`build_risk_model` primitive `stage_optimize.cpp`'s Factor branch already uses.

**Files:** `atx-impl/src/stage_metabook.hpp`, `atx-impl/src/stage_metabook.cpp`.

**Interfaces (exact signatures):**
```cpp
// stage_metabook.hpp -- NEW (additive; existing 2-arg declarations at :155-159 UNCHANGED):
[[nodiscard]] atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg,
                      const atx::engine::risk::RiskModelConfig &risk_cfg);

[[nodiscard]] atx::core::Result<StageResult>
run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg,
            const atx::engine::risk::RiskModelConfig &risk_cfg);
```

**`stage_metabook.hpp` edit:** add `#include "atx/engine/risk/factor_model.hpp"` (explicit —
the header now names `risk::RiskModelConfig` directly; do not rely on transitive inclusion via
`multi_horizon.hpp`, per the hygiene-preset per-TU-include discipline) and the two declarations
above, placed immediately after the existing `run_metabook` declaration (`:158-159`), with a doc
block citing the p8 seam note (`sprint-2-progress.md:269-294`) this closes.

**`stage_metabook.cpp` edit — includes:** add
```cpp
#include "atx/engine/data/adapt_factor.hpp" // data::artifact_to_factor_model (S1, frozen)
#include "stage_riskmodel.hpp"              // build_risk_model (S1, frozen)
```
and the namespace alias `namespace data = atx::engine::data;` next to the existing
`namespace risk = atx::engine::risk;` (`:38`).

**Current code (`stage_metabook.cpp:404-405, 500-503, 550, 558-559`):**
```cpp
atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg_in) {
  ...
  // model_at: diag_risk.hpp's diagonal_risk_model(research) -- the SAME model
  // stage_optimize's Diagonal path uses (stage_optimize.cpp:202/243-245). No Factor-model
  // variant is wired here (S1/S5 seam; recorded in the ledger).
  ATX_TRY(auto model, diagonal_risk_model(research));
  ...
  const auto model_at = [&](atx::usize) -> const risk::FactorModel & { return model; };
  ...
  return mb.run(sched, sources_at, model_at, returns_at, cost);
}

atx::core::Result<StageResult> run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg) {
  if (cfg.panel.empty() || cfg.combo.empty() || cfg.books_out.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --panel, --combo, and --out required");
  }
  ATX_TRY(auto result, build_metabook_result(cfg, scfg));
  ...
}
```

**Edited code:**
```cpp
// S2 (p9): the 2-arg overload is now a thin forwarder -- builds RiskModelConfig from
// cfg.risk_model (mirroring stage_optimize.cpp:40-48's run_optimize(const RunConfig&) seam
// exactly) and calls the 3-arg body below. risk_model=="diagonal" (RunConfig{} default)
// constructs RiskModelConfig{} (kind==Diagonal) -- byte-identical to every pre-S2 caller.
atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg_in) {
  risk::RiskModelConfig risk_cfg{};
  risk_cfg.kind = (cfg.risk_model == "factor") ? risk::RiskModelKind::Factor
                                                : risk::RiskModelKind::Diagonal;
  return build_metabook_result(cfg, scfg_in, risk_cfg);
}

atx::core::Result<fund::MetaBookResult>
build_metabook_result(const RunConfig &cfg, const MetaBookStageConfig &scfg_in,
                      const risk::RiskModelConfig &risk_cfg) {
  if (cfg.panel.empty() || cfg.combo.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --panel and --combo required");
  }
  ATX_TRY(auto research, read_panel(cfg.panel));
  ATX_TRY(auto combo, read_panel(cfg.combo));
  /* ... unchanged validation/schedule/sleeve-assignment/sources_at/returns_at blocks,
         lines 412-519 verbatim ... */

  // model_at (S2, p9): kind==Diagonal (default) -- the SAME single whole-panel
  // diagonal_risk_model(research) every pre-p9 caller got (byte-identical). kind==Factor --
  // one FactorModel PER REBALANCE STEP, PIT-fit at fit_end = period+1, mirroring
  // stage_optimize.cpp:248-273's Factor branch exactly (including its diagonal warm-up
  // fallback for a step too early for a genuine Factor fit). build_risk_model/
  // artifact_to_factor_model are S1's frozen producer -- not re-derived here.
  std::optional<risk::FactorModel> single_model;   // Diagonal path only
  std::vector<risk::FactorModel> step_models;      // Factor path only; one per sched.periods[s]

  if (risk_cfg.kind == risk::RiskModelKind::Diagonal) {
    ATX_TRY(auto model, diagonal_risk_model(research));
    single_model.emplace(std::move(model));
  } else {
    const risk::RiskModelConfig diag_fallback_cfg; // kind==Diagonal -- warm-up fallback only
    const atx::usize n_steps = sched.periods.size();
    step_models.reserve(n_steps);
    for (atx::usize s = 0; s < n_steps; ++s) {
      const atx::usize fit_end = sched.periods[s] + 1U; // PIT: through `period` inclusive
      auto factor_artifact = build_risk_model(research, risk_cfg, {}, nullptr, {}, 0, fit_end);
      if (factor_artifact.has_value()) {
        ATX_TRY(auto step_model, data::artifact_to_factor_model(*factor_artifact));
        step_models.push_back(std::move(step_model));
      } else {
        // Warm-up fallback: too little history for a genuine Factor fit at this step -- a
        // PIT diagonal over [0, fit_end) for THIS STEP ONLY (never the whole-panel diagonal,
        // which would reintroduce look-ahead for this step).
        ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, nullptr,
                                                     {}, 0, fit_end));
        ATX_TRY(auto diag_model, data::artifact_to_factor_model(diag_artifact));
        step_models.push_back(std::move(diag_model));
      }
    }
  }

  /* ... unchanged returns_at construction, cost inputs, R7 fractional_kelly override,
         MetaBook/sleeve assembly, sources_at lambda ... */

  const auto model_at = [&](atx::usize period) -> const risk::FactorModel & {
    if (risk_cfg.kind == risk::RiskModelKind::Diagonal) {
      return *single_model;
    }
    return step_models[period / step]; // `step` already in scope, :435
  };
  const auto returns_at = [&](atx::usize period) -> std::span<const atx::f64> {
    return std::span<const atx::f64>{returns[period]};
  };

  return mb.run(sched, sources_at, model_at, returns_at, cost);
}

atx::core::Result<StageResult> run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg) {
  risk::RiskModelConfig risk_cfg{};
  risk_cfg.kind = (cfg.risk_model == "factor") ? risk::RiskModelKind::Factor
                                                : risk::RiskModelKind::Diagonal;
  return run_metabook(cfg, scfg, risk_cfg);
}

atx::core::Result<StageResult> run_metabook(const RunConfig &cfg, const MetaBookStageConfig &scfg,
                                            const risk::RiskModelConfig &risk_cfg) {
  if (cfg.panel.empty() || cfg.combo.empty() || cfg.books_out.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "metabook: --panel, --combo, and --out required");
  }
  ATX_TRY(auto result, build_metabook_result(cfg, scfg, risk_cfg));
  /* ... unchanged: pack fund_books into a panel, write_panel, kvs/attribution telemetry ... */
}
```

**TDD steps:**

1. **RED.** Add `atx-impl/tests/stage_metabook_riskmodel_wire_test.cpp` (suite
   `MetabookRiskModelWire`) against the **pre-edit** code — it will not compile (the 3-arg
   overloads don't exist yet), which IS the RED state for an additive-overload unit (a
   compile failure is the correct RED signal here, not a runtime assertion failure, since the
   whole point of the unit is that the overload doesn't exist pre-fix).

   ```cpp
   // stage_metabook_riskmodel_wire_test.cpp -- p9 S2-2: the deferred RiskModelConfig-
   // parameterized build_metabook_result/run_metabook overload (p8 sprint-2-progress.md's
   // own documented S1/S5/final-wave seam). kind==Diagonal (default) is byte-identical;
   // kind==Factor drives model_at with a per-rebalance-step PIT FactorModel, mirroring
   // stage_optimize.cpp's own per-step loop.
   //
   // Suite: MetabookRiskModelWire

   #include <bit>
   #include <cstdint>
   #include <filesystem>
   #include <string>
   #include <vector>

   #include <gtest/gtest.h>

   #include "atx/core/types.hpp"
   #include "atx/engine/alpha/panel.hpp"
   #include "atx/engine/data/adapt_factor.hpp"
   #include "atx/engine/data/factor_model_artifact.hpp"
   #include "atx/engine/risk/factor_model.hpp"

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stage_metabook.hpp"
   #include "stage_riskmodel.hpp"

   namespace atxtest_stage_metabook_riskmodel_wire {

   using atx::impl::MetaBookStageConfig;
   namespace alpha = atx::engine::alpha;
   namespace data  = atx::engine::data;
   namespace risk  = atx::engine::risk;

   // D=17, weekly (step=5) -> sched.periods = [0,5,10,15]; date 16 is the ONE date past the
   // last step's fit window (fit_end = 15+1 = 16) -- unused by any model, the PIT witness.
   constexpr atx::usize kM = 6, kD = 17;

   [[nodiscard]] std::string make_research(const std::filesystem::path &out,
                                           atx::f64 perturb_date3, atx::f64 perturb_date16) {
     std::vector<atx::f64> close(kD * kM);
     for (atx::usize t = 0; t < kD; ++t) {
       for (atx::usize i = 0; i < kM; ++i) {
         const atx::f64 drift = 0.0003 * (1.0 + static_cast<atx::f64>(i) * 0.15);
         close[t * kM + i] = 100.0 * std::exp(drift * static_cast<atx::f64>(t));
       }
     }
     if (perturb_date3 != 0.0) {
       for (atx::usize i = 0; i < kM; ++i) close[3 * kM + i] += perturb_date3;
     }
     if (perturb_date16 != 0.0) {
       for (atx::usize i = 0; i < kM; ++i) close[16 * kM + i] += perturb_date16;
     }
     std::vector<std::uint8_t> uni(kD * kM, 1U);
     auto panel = alpha::Panel::create(kD, kM, {"close"}, {close}, uni);
     EXPECT_TRUE(panel.has_value());
     auto digest = atx::impl::write_panel(*panel, out.string());
     EXPECT_TRUE(digest.has_value());
     return out.string();
   }

   [[nodiscard]] std::string make_combo(const std::filesystem::path &out) {
     std::vector<atx::f64> a(kD * kM);
     for (atx::usize t = 0; t < kD; ++t) {
       for (atx::usize i = 0; i < kM; ++i) {
         a[t * kM + i] = static_cast<atx::f64>(i) - static_cast<atx::f64>(kM) / 2.0;
       }
     }
     std::vector<std::uint8_t> uni(kD * kM, 1U);
     auto panel = alpha::Panel::create(kD, kM, {"alpha"}, {a}, uni);
     EXPECT_TRUE(panel.has_value());
     auto digest = atx::impl::write_panel(*panel, out.string());
     EXPECT_TRUE(digest.has_value());
     return out.string();
   }

   [[nodiscard]] std::string tmp_dir(const std::string &tag) {
     const auto dir = std::filesystem::temp_directory_path() / "atx_s2_mb_rmw" / tag;
     std::error_code ec;
     std::filesystem::remove_all(dir, ec);
     std::filesystem::create_directories(dir, ec);
     return dir.string();
   }

   [[nodiscard]] atx::impl::RunConfig base_cfg(const std::string &dir, atx::f64 p3, atx::f64 p16) {
     atx::impl::RunConfig cfg;
     cfg.panel = make_research(std::filesystem::path(dir) / "research.bin", p3, p16);
     cfg.combo = make_combo(std::filesystem::path(dir) / "combo.bin");
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "weekly";
     cfg.risk_model = "factor";
     return cfg;
   }

   // ===========================================================================
   //  (a) off-path byte-identity: the 2-arg overload (now cfg.risk_model-aware) at the
   //  "diagonal" default must match the explicit 3-arg Diagonal call.
   // ===========================================================================
   TEST(MetabookRiskModelWire, DefaultTwoArgByteIdenticalToExplicitDiagonal) {
     const std::string dir = tmp_dir("offpath");
     atx::impl::RunConfig cfg = base_cfg(dir, 0.0, 0.0);
     cfg.risk_model = "diagonal";
     const MetaBookStageConfig scfg;

     auto r_default = atx::impl::build_metabook_result(cfg, scfg); // 2-arg
     ASSERT_TRUE(r_default.has_value()) << r_default.error().message();

     auto r_explicit = atx::impl::build_metabook_result(cfg, scfg, risk::RiskModelConfig{});
     ASSERT_TRUE(r_explicit.has_value()) << r_explicit.error().message();

     ASSERT_EQ(r_default->fund_books.size(), r_explicit->fund_books.size());
     for (atx::usize s = 0; s < r_default->fund_books.size(); ++s) {
       ASSERT_EQ(r_default->fund_books[s].size(), r_explicit->fund_books[s].size());
       for (atx::usize i = 0; i < r_default->fund_books[s].size(); ++i) {
         EXPECT_EQ(std::bit_cast<std::uint64_t>(r_default->fund_books[s][i]),
                   std::bit_cast<std::uint64_t>(r_explicit->fund_books[s][i]))
             << "period " << s << " name " << i;
       }
     }
   }

   // ===========================================================================
   //  (b) on-path RED->GREEN: PIT correctness of the new per-step model_at.
   //  PIT-A: perturbing date 16 (strictly AFTER every step's fit window; the last step's
   //  fit_end == 16) must not change ANY period's fund_books.
   //  PIT-B: perturbing date 3 (inside step1's fit window [0,6), outside step0's [0,1))
   //  must change step1's book but NOT step0's -- proving the model genuinely depends on
   //  its OWN trailing window, not a degenerate constant.
   // ===========================================================================
   TEST(MetabookRiskModelWire, FutureDateDoesNotAffectAnyBook_PitA) {
     const std::string dir_base = tmp_dir("pit_a_base");
     const std::string dir_pert = tmp_dir("pit_a_pert");
     atx::impl::RunConfig cfg_base = base_cfg(dir_base, 0.0, 0.0);
     atx::impl::RunConfig cfg_pert = base_cfg(dir_pert, 0.0, /*perturb_date16=*/5.0);
     const MetaBookStageConfig scfg;

     auto r_base = atx::impl::build_metabook_result(cfg_base, scfg);
     auto r_pert = atx::impl::build_metabook_result(cfg_pert, scfg);
     ASSERT_TRUE(r_base.has_value()) << r_base.error().message();
     ASSERT_TRUE(r_pert.has_value()) << r_pert.error().message();

     ASSERT_EQ(r_base->fund_books.size(), r_pert->fund_books.size());
     for (atx::usize s = 0; s < r_base->fund_books.size(); ++s) {
       for (atx::usize i = 0; i < r_base->fund_books[s].size(); ++i) {
         EXPECT_EQ(std::bit_cast<std::uint64_t>(r_base->fund_books[s][i]),
                   std::bit_cast<std::uint64_t>(r_pert->fund_books[s][i]))
             << "PIT violation: period " << s << " name " << i
             << " changed from perturbing a date past every step's fit window";
       }
     }
   }

   TEST(MetabookRiskModelWire, EarlierWindowPerturbationChangesOnlyLaterSteps_PitB) {
     const std::string dir_base = tmp_dir("pit_b_base");
     const std::string dir_pert = tmp_dir("pit_b_pert");
     atx::impl::RunConfig cfg_base = base_cfg(dir_base, 0.0, 0.0);
     atx::impl::RunConfig cfg_pert = base_cfg(dir_pert, /*perturb_date3=*/5.0, 0.0);
     const MetaBookStageConfig scfg;

     auto r_base = atx::impl::build_metabook_result(cfg_base, scfg);
     auto r_pert = atx::impl::build_metabook_result(cfg_pert, scfg);
     ASSERT_TRUE(r_base.has_value()) << r_base.error().message();
     ASSERT_TRUE(r_pert.has_value()) << r_pert.error().message();

     // step 0 covers date 0, fit_end==1 -- date 3 is NOT in [0,1); must be unchanged.
     for (atx::usize i = 0; i < r_base->fund_books[0].size(); ++i) {
       EXPECT_EQ(std::bit_cast<std::uint64_t>(r_base->fund_books[0][i]),
                 std::bit_cast<std::uint64_t>(r_pert->fund_books[0][i]))
           << "step 0 must be PIT-blind to a date-3 perturbation";
     }
     // step 1 covers date 5, fit_end==6 -- date 3 IS in [0,6); the book must differ
     // somewhere (a genuinely live, window-dependent model, not a degenerate constant).
     bool any_diff = false;
     for (atx::usize i = 0; i < r_base->fund_books[1].size(); ++i) {
       if (r_base->fund_books[1][i] != r_pert->fund_books[1][i]) any_diff = true;
     }
     EXPECT_TRUE(any_diff) << "step 1's model must depend on date 3 -- if unchanged, "
                           << "model_at is not actually reading its trailing window";
   }

   // ===========================================================================
   //  (c) twice-run.
   // ===========================================================================
   TEST(MetabookRiskModelWire, TwiceRunByteIdentical) {
     const std::string dir = tmp_dir("twice");
     atx::impl::RunConfig cfg = base_cfg(dir, 0.0, 0.0);
     const MetaBookStageConfig scfg;

     auto r1 = atx::impl::build_metabook_result(cfg, scfg);
     auto r2 = atx::impl::build_metabook_result(cfg, scfg);
     ASSERT_TRUE(r1.has_value()) << r1.error().message();
     ASSERT_TRUE(r2.has_value()) << r2.error().message();

     ASSERT_EQ(r1->fund_books.size(), r2->fund_books.size());
     for (atx::usize s = 0; s < r1->fund_books.size(); ++s) {
       for (atx::usize i = 0; i < r1->fund_books[s].size(); ++i) {
         EXPECT_EQ(std::bit_cast<std::uint64_t>(r1->fund_books[s][i]),
                   std::bit_cast<std::uint64_t>(r2->fund_books[s][i]));
       }
     }
   }

   // ===========================================================================
   //  (d) seq==parallel: the new per-step loop's own primitive, build_risk_model, is
   //  order-independent (S1's own documented contract: "fitting window s never reads window
   //  s' state") -- re-verified DIRECTLY here on this file's own fixture shape (not just
   //  cited), forward vs. reverse construction order.
   // ===========================================================================
   TEST(MetabookRiskModelWire, PerStepBuildRiskModelOrderIndependent) {
     const std::string dir = tmp_dir("order");
     const std::string research_path = make_research(std::filesystem::path(dir) / "research.bin", 0.0, 0.0);
     auto research = atx::impl::read_panel(research_path);
     ASSERT_TRUE(research.has_value());

     const std::vector<atx::usize> fit_ends = {6, 11, 16}; // steps 1,2,3 (step 0's fit_end=1 is warm-up)
     risk::RiskModelConfig factor_cfg{};
     factor_cfg.kind = risk::RiskModelKind::Factor;

     std::vector<atx::u64> forward, reverse(fit_ends.size());
     for (atx::usize k = 0; k < fit_ends.size(); ++k) {
       auto a = atx::impl::build_risk_model(*research, factor_cfg, {}, nullptr, {}, 0, fit_ends[k]);
       ASSERT_TRUE(a.has_value()) << a.error().message();
       forward.push_back(data::digest_artifact(*a));
     }
     for (atx::usize k = fit_ends.size(); k-- > 0;) {
       auto a = atx::impl::build_risk_model(*research, factor_cfg, {}, nullptr, {}, 0, fit_ends[k]);
       ASSERT_TRUE(a.has_value()) << a.error().message();
       reverse[k] = data::digest_artifact(*a);
     }
     EXPECT_EQ(forward, reverse)
         << "each fit-window's artifact must be independent of construction order";
   }

   } // namespace atxtest_stage_metabook_riskmodel_wire
   ```

2. **Build + confirm RED (compile failure).**
   ```powershell
   $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
   Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
   Set-Location C:\atx-wt\p9
   cmake --preset dev
   cmake --build --preset dev --target atx-impl-tests
   ```
   Expected: compile error — no matching overload for `build_metabook_result(cfg, scfg,
   risk::RiskModelConfig{})` (3-arg does not exist yet). This IS the RED signal for this unit.

3. **GREEN.** Apply the `stage_metabook.hpp` + `stage_metabook.cpp` edits above. Rebuild + run:
   ```powershell
   cmake --preset dev
   cmake --build --preset dev --target atx-impl-tests
   ctest --preset dev -R MetabookRiskModelWire --output-on-failure
   ```
   Expected: 5/5 green.

4. **Regress.** Re-run the full pre-existing metabook suite (must remain green — the R7
   stage-boundary pin is the load-bearing off-path check this unit must not disturb):
   ```powershell
   ctest --preset dev -R "Metabook|FundMetabookWire" --output-on-failure
   ```

**Accept:**
- `MetabookRiskModelWire` — 5/5 green, with the compile-failure→green transition documented.
- `MetabookStageBoundary.SingleSleeveByteIdenticalToStageOptimizeBook` (p8's own R7 pin,
  `metabook_test.cpp:343`) still green, unmodified — S2 did not touch its call shape (`cfg`
  defaults `risk_model=="diagonal"` in that test, so it now flows through the new 3-arg body's
  Diagonal branch, byte-identical by construction).
- `stage_metabook.hpp`'s two pre-existing 2-arg declarations are textually unchanged (only new
  declarations added, plus one new include).

**Commit:**
```
git add atx-impl/src/stage_metabook.hpp atx-impl/src/stage_metabook.cpp \
        atx-impl/tests/stage_metabook_riskmodel_wire_test.cpp
git commit -m "$(cat <<'EOF'
PF-P9 S2-2 stage_metabook: RiskModelConfig-parameterized model_at overload

Closes the p8-S2 documented seam (sprint-2-progress.md's "Item 4, deferred"): adds an
additive build_metabook_result/run_metabook(cfg, scfg, risk_cfg) overload. kind==Diagonal
(default) keeps the exact single whole-panel diagonal_risk_model(research) every caller
got before. kind==Factor drives model_at with one FactorModel per rebalance step, PIT-fit
via build_risk_model at fit_end=period+1 -- the same per-step loop stage_optimize.cpp's
Factor branch already runs, called here (not re-derived) against a new consumer. The
existing 2-arg overloads become cfg.risk_model-aware thin forwarders, mirroring
stage_optimize.cpp's own 1-arg/2-arg split, so --risk-model factor now reaches metabook
through the real CLI/run_all path with zero edits to stage_run.cpp/dispatch.cpp.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

### S2-3 — Determinism battery + cross-cutting proof + ledger close

**Goal:** consolidate the sprint's determinism story into one place, and add the ONE test S2-1/
S2-2 individually cannot: proof that a **single** `RunConfig` with `risk_model=="factor"`
drives **both** stages' Factor path at once — the literal reading of the sprint's own Goal
statement ("`--risk-model factor` governs the whole book, not just `stage_optimize`"). No new
production code in this unit — tests + ledger only, mirroring p8-S3-5's own role.

**Files:** new `atx-impl/tests/riskmodel_wire_crosscutting_test.cpp` (suite
`RiskModelWireCrossCutting`), `atx-engine/plans/p9/sprint-2-progress.md`.

**TDD steps:**

1. **Write the cross-cutting test** (this is a genuine new proof, not RED→GREEN against
   already-fixed code — S2-1/S2-2 are already GREEN by this point in the sequence, so this
   unit's tests should pass on the first run; if they don't, that is itself evidence S2-1/S2-2
   left a gap, and the unit's job is to catch it before ledger close):

   ```cpp
   // riskmodel_wire_crosscutting_test.cpp -- p9 S2-3: proves ONE RunConfig with
   // risk_model=="factor" drives BOTH run_combine and run_metabook's Factor path -- the
   // sprint's own Goal statement made concrete, not just each stage's own isolated test.
   //
   // Suite: RiskModelWireCrossCutting

   #include <filesystem>
   #include <fstream>
   #include <string>
   #include <vector>

   #include <gtest/gtest.h>

   #include "atx/core/types.hpp"
   #include "atx/engine/alpha/panel.hpp"

   #include "config.hpp"
   #include "serialize_panel.hpp"
   #include "stage_combine.hpp"
   #include "stage_metabook.hpp"
   #include "stages.hpp"

   namespace atxtest_riskmodel_wire_crosscutting {

   using atx::impl::MetaBookStageConfig;
   namespace alpha = atx::engine::alpha;

   TEST(RiskModelWireCrossCutting, SingleConfigDrivesBothCombineAndMetabookFactorPaths) {
     namespace fs = std::filesystem;
     const auto dir = fs::temp_directory_path() / "atx_s2_crosscutting";
     std::error_code ec;
     fs::remove_all(dir, ec);
     fs::create_directories(dir, ec);

     constexpr atx::usize M = 6, D = 40;
     std::vector<atx::f64> close(D * M);
     for (atx::usize t = 0; t < D; ++t) {
       for (atx::usize i = 0; i < M; ++i) {
         close[t * M + i] = 100.0 * std::exp(0.0004 * (1.0 + 0.1 * static_cast<atx::f64>(i)) *
                                             static_cast<atx::f64>(t));
       }
     }
     std::vector<std::uint8_t> uni(D * M, 1U);
     auto panel = alpha::Panel::create(D, M, {"close"}, {close}, uni);
     ASSERT_TRUE(panel.has_value());
     const std::string panel_path = (dir / "research.bin").string();
     ASSERT_TRUE(atx::impl::write_panel(*panel, panel_path).has_value());

     const std::string alphas_dir = (dir / "alphas").string();
     fs::create_directories(alphas_dir, ec);
     const std::vector<std::string> dsls = {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
     for (atx::usize i = 0; i < dsls.size(); ++i) {
       std::ofstream f{(fs::path(alphas_dir) / ("alpha_" + std::to_string(i) + ".dsl")).string()};
       f << dsls[i] << '\n';
     }

     atx::impl::RunConfig cfg;
     cfg.subcommand = "combine";
     cfg.panel  = panel_path;
     cfg.alphas = alphas_dir;
     cfg.method = "shrinkage-mv";
     cfg.gross = 1.0;
     cfg.name_cap = 1.0;
     cfg.rebalance = "weekly";

     // 1. combine: risk_model=="factor" vs "diagonal" through the SAME cfg (only the field flips).
     cfg.risk_model = "diagonal";
     cfg.combo_out = (dir / "combo_diag.bin").string();
     auto combo_diag = atx::impl::run_combine(cfg);
     ASSERT_TRUE(combo_diag.has_value()) << combo_diag.error().message();

     cfg.risk_model = "factor";
     cfg.combo_out = (dir / "combo_factor.bin").string();
     auto combo_factor = atx::impl::run_combine(cfg);
     ASSERT_TRUE(combo_factor.has_value()) << combo_factor.error().message();

     EXPECT_NE(combo_diag->digest, combo_factor->digest)
         << "run_combine must reach the Factor path when cfg.risk_model=='factor'";

     // 2. metabook: the SAME cfg (now with cfg.combo pointed at a real combo panel) reaches
     // ITS OWN Factor path too -- the "whole book" claim.
     cfg.combo = combo_factor->kvs.empty() ? cfg.combo_out : cfg.combo_out; // combo panel just written
     const MetaBookStageConfig scfg;

     cfg.risk_model = "diagonal";
     cfg.books_out = (dir / "books_diag.bin").string();
     auto mb_diag = atx::impl::run_metabook(cfg, scfg);
     ASSERT_TRUE(mb_diag.has_value()) << mb_diag.error().message();

     cfg.risk_model = "factor";
     cfg.books_out = (dir / "books_factor.bin").string();
     auto mb_factor = atx::impl::run_metabook(cfg, scfg);
     ASSERT_TRUE(mb_factor.has_value()) << mb_factor.error().message();

     EXPECT_NE(mb_diag->digest, mb_factor->digest)
         << "run_metabook must ALSO reach its own Factor path from the SAME cfg.risk_model "
         << "flag -- the sprint's literal goal: one flag governs the whole book";
   }

   } // namespace atxtest_riskmodel_wire_crosscutting
   ```

2. **Run + full regression sweep** (single-threaded ctest per the p9 testing directive):
   ```powershell
   $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
   Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
   Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
   Set-Location C:\atx-wt\p9
   cmake --preset dev
   cmake --build --preset dev
   ctest --preset dev --output-on-failure
   ```
   Expected: `RiskModelWireCrossCutting` 1/1 green; the WHOLE `atx-impl-tests` suite green
   (compare pass count against the S2-0 baseline — must be baseline + 9 new tests: 3
   `StageCombineRiskModelWire` + 5 `MetabookRiskModelWire` + 1
   `RiskModelWireCrossCutting`); every pinned golden named in the ROADMAP's determinism
   contract (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigest-
   Unchanged`, the `AtxImplDiscover` slice, `LibraryVerdict.AdmitKindEnumFrozenPrefix`) still
   green and untouched.

3. **Close the ledger.** Append to `atx-engine/plans/p9/sprint-2-progress.md`: per-unit
   summary (S2-0/S2-1/S2-2/S2-3), the final regression pass count, and the two resolved
   ambiguities (config-surface: metabook's 2-arg becomes risk_model-aware; dead_alpha_factors/
   group_neutralize deliberately not threaded into combine's risk_cfg).

**Accept:**
- `RiskModelWireCrossCutting` green.
- Full `atx-impl-tests` regression green at baseline+9.
- Ledger closed with the sprint summary.

**Commit:**
```
git add atx-impl/tests/riskmodel_wire_crosscutting_test.cpp atx-engine/plans/p9/sprint-2-progress.md
git commit -m "$(cat <<'EOF'
PF-P9 S2-3 cross-cutting proof + full regression sweep + ledger close

Adds RiskModelWireCrossCutting.SingleConfigDrivesBothCombineAndMetabookFactorPaths: one
RunConfig with risk_model=="factor" reaches BOTH run_combine's and run_metabook's Factor
path -- the sprint's Goal statement made concrete. Full atx-impl-tests regression green
at baseline+9; every p9 pinned golden unchanged. Closes the S2 ledger.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Sequencing

1. **S2-0 first** (ledger + baseline) — no code, but pins the starting state every later unit's
   RED description depends on.
2. **S2-1** (`run_combine`) and **S2-2** (`stage_metabook`) are file-disjoint
   (`stage_combine.cpp` vs. `stage_metabook.{cpp,hpp}`) and could be parallelized in planning,
   but land **serially** in this one-branch worktree, S2-1 then S2-2 (matches the ROADMAP's own
   S1→S2→...→S7 serial rule and this doc's own task numbering).
3. **S2-3 last** — depends on both S2-1 and S2-2 being GREEN (it exercises both in one test).

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| Metabook's 2-arg overload is read as "must stay frozen-Diagonal forever" (the OTHER literal reading of the p8 seam note / registry text) | `--risk-model factor` would never reach metabook via the real CLI/`run_all` path — repeating exactly the Potemkin-book defect p9 exists to fix | Resolved explicitly in "Config surface" above: the 2-arg overload becomes `cfg.risk_model`-aware, mirroring `stage_optimize.cpp`'s own precedent. Flagged for reconciler sign-off (see below) since it is a genuine two-way reading of the source text, not a typo. |
| `build_risk_model`'s Factor branch returns `Err` for an under-determined early rebalance step (small `M`, `fit_end<2`) | Metabook Factor path could hard-fail on small fixtures instead of degrading gracefully | Mirror `stage_optimize.cpp`'s own documented warm-up fallback exactly (diagonal over `[0, fit_end)` for that step only) — already in the S2-2 edit above, not a new risk to solve, a known pattern to copy. |
| A new `#include` in `stage_metabook.hpp` (`risk/factor_model.hpp`) creates an include cycle | Build failure under the `hygiene` preset (PCH off, strict per-TU) | `stage_optimize.cpp` and `stage_riskmodel.hpp` already include `risk/factor_model.hpp` alongside `alpha/panel.hpp`/`config.hpp` with no cycle; `stage_metabook.hpp` already transitively pulls `risk/multi_horizon.hpp` (which itself depends on `FactorModel`), so this is adding an existing, already-satisfied dependency explicitly, not a new edge. |
| The cross-cutting S2-3 test's combo-panel plumbing (writing `run_combine`'s output, then reading it back as `run_metabook`'s `cfg.combo`) is fiddly plumbing that could mask a real defect as a fixture bug | False confidence or a flaky test | Keep S2-1/S2-2's own isolated tests as the load-bearing proofs (they don't depend on this plumbing); treat S2-3 failures as "investigate the fixture first," per `systematic-debugging` discipline, before suspecting S2-1/S2-2's own code. |
| `dead_alpha_factors`/`group_neutralize` silently ignored by combine's `risk_cfg_from_run_config` | A future reader might expect `--dead-alpha-factors` to affect combine too | Documented explicitly in the S2-1 code comment and in "Config surface" above: those two fields have no reader on the combine path (`fit_shrinkage_mv_cleaned_cov` takes no `dead_lib`/`group_map` at all) — not an oversight, a scope boundary. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** `cfg.risk_model=="diagonal"` (the `RunConfig{}` default) —
  `run_combine`'s 0-/1-arg digests and `stage_metabook`'s 2-arg `fund_books` are byte-identical
  to the pre-S2 code, at both call sites, verified by direct comparison against an explicit
  `RiskModelConfig{}` call (not merely "no test caught a regression").
- **Factor-model win, measured:** `run_combine` with `cfg.risk_model=="factor"` reaches the
  exact digest an explicit 3-arg Factor call produces (S2-1), which is S3-4's own proven
  diversification win (`max|w|_diagonal=0.164248 → max|w|_factor=0.142554`, unmodified).
  `stage_metabook`'s new per-step `model_at` is PIT-correct: a step's book depends only on data
  strictly inside `[0, step's own fit_end)`, proven both by non-effect (future date) and
  by effect (in-window date) perturbation (S2-2).
- **Whole-book claim, measured:** one `RunConfig`, one flag flip, both stages' digests move
  (S2-3) — the sprint's Goal statement is not just individually true per stage, but jointly true
  from a single caller-visible switch.
- **Twice-run + seq==parallel:** both call sites twice-run byte-identical; the new metabook
  per-step loop's own primitive (`build_risk_model`) proven order-independent directly.
- **Full regression:** `atx-impl-tests` green at baseline+9; every ROADMAP-pinned golden
  unchanged; `dev` preset (Unity ON) links and passes throughout — no `hygiene`-only breakage
  introduced by the two new includes.

---

## Out of scope

- Threading `--dead-alpha-factors`/`--group-neutralize` into the combine or metabook paths —
  those fields have no reader on either path today (S1 wired them for `stage_optimize` only);
  not this sprint's registry row.
- Any change to `stage_optimize.cpp`, `stage_riskmodel.{cpp,hpp}`, `diag_risk.hpp`, or the
  frozen estimator (`risk::FactorModelBuilder::build_components`, `data::cleaned_alpha_cov`) —
  S2 calls these, never edits them.
- A book-level (cross-stage) turnover/capacity gate reading the Factor covariance — Sprint S5.
- GP aim-portfolio trading, capacity/turnover NSGA objectives — Sprints S3/S4 (file-disjoint,
  independent of S2).
- Updating `build-megaalpha-book.ps1` to assert `--risk-model factor` in the prod profile —
  Sprint S7 (the recipe correction), which depends on S2 (and S1/S3/S4/S5) being live first.

---

## Note for the reconciler (ambiguity flagged, not silently resolved)

The p9 ROADMAP's own registry text for the metabook row — *"new overload
`build_metabook_result(..., const risk::RiskModelConfig& risk_cfg)` / `run_metabook(cfg,
risk_cfg)`; **default overload keeps Diagonal**"* — and the p8 seam note it's drawn from
(`sprint-2-progress.md:286-291`, describing "a `const risk::RiskModelConfig &risk_cfg = {}`
default parameter... the default argument keeps every existing call site... byte-identical") are
both consistent with **two different implementations**:

1. **(Chosen here.)** The existing 2-arg overload becomes `cfg.risk_model`-aware (builds
   `risk_cfg` from the string field, exactly like `stage_optimize.cpp`'s own 1-arg
   `run_optimize`) and forwards to a new 3-arg overload. `risk_model=="diagonal"` (default) ⇒
   byte-identical ⇒ "default overload keeps Diagonal" reads as *"at the default value"*.
   `--risk-model factor` now reaches metabook via the real CLI/`run_all` path with **zero**
   edits to `stage_run.cpp`/`dispatch.cpp` (both hub files are S5-owned, untouched).
2. **(Not chosen.)** The existing 2-arg overload stays hardcoded to `RiskModelConfig{}` forever
   (never reads `cfg.risk_model`); only a caller who explicitly constructs and passes a 3rd
   `risk_cfg` argument can ever reach `kind==Factor` — "default overload keeps Diagonal" reads as
   *"unconditionally, permanently"*. Under this reading, `--risk-model factor` would **still
   never reach metabook** through `stage_run.cpp`/`dispatch.cpp` (both only ever call the 2-arg
   form) — leaving the sprint's own Goal statement ("governs the whole book") false for the
   metabook half, unless a later sprint threads a *second*, metabook-specific CLI flag.

**Decision made in this plan:** option 1, because it is the only reading under which S2 actually
satisfies its own stated Goal without inventing new CLI surface (which would violate S2's "no
new field" registry constraint and duplicate `--risk-model`). If the reconciler intended option
2 (e.g., because a later sprint is meant to own threading the CLI-reachable path deliberately,
the way p8-S5 owned wiring `--risk-model` onto `stage_optimize` after S1 shipped the engine-side
overload), S2-2's task above needs exactly one change: drop the "2-arg becomes a forwarder" edit
and leave `build_metabook_result(cfg, scfg)` calling `diagonal_risk_model(research)`
unconditionally, while still shipping the 3-arg overload for direct-call testing only. Everything
else in this plan (the per-step Factor loop, the PIT tests, the artifact plumbing) is unaffected
either way.

A second, smaller ambiguity: whether `run_combine`'s new `risk_cfg_from_run_config` helper should
also copy `cfg.dead_alpha_factors`/`cfg.group_neutralize` (harmless but currently inert on this
path, since `fit_shrinkage_mv_cleaned_cov` never reads them). This plan omits them for honesty
(documented in the S2-1 code comment); including them would not change any test outcome in this
sprint but would future-proof against a later sprint wiring a combine-side consumer for either
field.
