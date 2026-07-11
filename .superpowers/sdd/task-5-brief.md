### Task 5: Fitting-pipeline binding — `apply_symbol_config` + end-to-end integration test

The manifest config must load into the fitting pipeline: one function maps `SymbolFitConfig` onto `SessionInputs` (the core builder input, session.hpp:69) with pinned semantics, plus a preset-capture helper, plus an end-to-end test through a real db on disk.

**Files:**
- Modify: `atx-vol/include/atx/vol/surface_db.hpp`
- Modify: `atx-vol/src/surface_db.cpp`
- Modify: `atx-vol/tests/surface_db_test.cpp`

**Interfaces:**
- Consumes: `apply_fit_preset(SessionInputs&, FitPreset)` (session.hpp:141), Task 3 `SurfaceDb`.
- Produces:

```cpp
// Map `cfg` onto the fit-policy fields of `in`, leaving the market snapshot
// (S, r, expiry rates, cash_divs, now_ts_ns) untouched. Order: apply_fit_preset
// (cfg.preset) first — it sets the DeAm/cache/inversion policy — then every
// explicit SymbolFitConfig field overwrites the preset's choice:
//   in.curve = cfg.curve (when pin_curve; otherwise the preset's curve stands),
//   in.calib = cfg.curve.parametric (when pin_curve),
//   in.deam.al_opts = cfg.al (when al_override),
//   in.band_k / in.calendar_repair / in.use_correction_cache / in.score_parity
//   / in.enforce_calendar_floor / in.use_deam_cache_for_fit = cfg.<same>.
void apply_symbol_config(const SymbolFitConfig& cfg, SessionInputs& in);

// Capture `preset`'s effective policy into a SymbolFitConfig whose explicit
// fields equal what apply_fit_preset(in, preset) would produce — the identity
// starting point for per-symbol tuning (adjust one knob, store, done).
[[nodiscard]] SymbolFitConfig symbol_config_from_preset(FitPreset preset);
```

**Steps:**

- [ ] **Step 1: Write failing tests.**

```cpp
TEST(SurfaceDbApply, PinnedConfig_OverridesPreset) {
  auto cfg = make_full_config();          // pin_curve=true, al_override=true, Hft
  SessionInputs in;
  in.S = 100.0; in.r = 0.04; in.now_ts_ns = 42;   // market snapshot
  apply_symbol_config(cfg, in);
  // market snapshot untouched:
  EXPECT_DOUBLE_EQ(in.S, 100.0);
  EXPECT_DOUBLE_EQ(in.r, 0.04);
  EXPECT_EQ(in.now_ts_ns, 42);
  // explicit fields won over the Hft preset:
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 56);
  EXPECT_EQ(in.calib.optimization_level, OptimizationLevel::Risk);
  EXPECT_DOUBLE_EQ(in.band_k, 1.25);
  EXPECT_EQ(in.calendar_repair, CalendarRepair::Project);
  EXPECT_FALSE(in.use_correction_cache);
  EXPECT_FALSE(in.score_parity);
  EXPECT_FALSE(in.enforce_calendar_floor);
  EXPECT_TRUE(in.use_deam_cache_for_fit);
  ASSERT_TRUE(in.deam.al_opts.has_value());
  EXPECT_EQ(in.deam.al_opts->n_collocation, 9);
  EXPECT_DOUBLE_EQ(in.deam.al_opts->tol, 1e-9);
}

TEST(SurfaceDbApply, UnpinnedConfig_PresetCurveStands) {
  SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Robust);
  cfg.pin_curve = false;
  SessionInputs via_apply;
  apply_symbol_config(cfg, via_apply);
  SessionInputs via_preset;
  apply_fit_preset(via_preset, FitPreset::Robust);
  // Identity: a config captured from a preset and applied unpinned reproduces
  // apply_fit_preset exactly on the fields SymbolFitConfig carries.
  EXPECT_EQ(via_apply.curve.kind, via_preset.curve.kind);
  EXPECT_DOUBLE_EQ(via_apply.band_k, via_preset.band_k);
  EXPECT_EQ(via_apply.calendar_repair, via_preset.calendar_repair);
  EXPECT_EQ(via_apply.use_correction_cache, via_preset.use_correction_cache);
  EXPECT_EQ(via_apply.score_parity, via_preset.score_parity);
  EXPECT_EQ(via_apply.enforce_calendar_floor, via_preset.enforce_calendar_floor);
  EXPECT_EQ(via_apply.use_deam_cache_for_fit, via_preset.use_deam_cache_for_fit);
  EXPECT_EQ(via_apply.deam.al_opts.has_value(), via_preset.deam.al_opts.has_value());
  if (via_preset.deam.al_opts.has_value()) {
    EXPECT_EQ(via_apply.deam.al_opts->n_collocation, via_preset.deam.al_opts->n_collocation);
    EXPECT_DOUBLE_EQ(via_apply.deam.al_opts->tol, via_preset.deam.al_opts->tol);
  }
}

TEST(SurfaceDbEndToEnd, ConfigureStoreReloadServe) {
  const auto root = test_root("e2e");
  // Session 1: operator configures the universe + pipeline stores fits.
  {
    auto db = SurfaceDb::create(root.string());
    ASSERT_TRUE(db.has_value());
    auto spy = symbol_config_from_preset(FitPreset::Robust);
    spy.pin_curve = true;
    spy.curve.kind = VolCurveKind::ConvexDense;
    spy.curve.convex.node_cap = 48;
    ASSERT_TRUE(db->upsert_symbol("SPY", spy).has_value());
    auto aapl = symbol_config_from_preset(FitPreset::Fast);
    aapl.enabled = false;
    ASSERT_TRUE(db->upsert_symbol("AAPL", aapl).has_value());

    // SPY stored as ConvexDense (matches its pinned config; exercises the
    // variable-length-node kind end-to-end), AAPL as Essvi.
    const auto s1 = make_convex(1, 3, 40);
    const auto s2 = make_essvi(2, 3);
    const std::vector<SurfaceArchiveItem> items{{"SPY", &s1}, {"AAPL", &s2}};
    ASSERT_TRUE(db->write_partition("2026-07-11", items).has_value());
  }
  // Session 2 (fresh open — the fitting pipeline at startup):
  auto db = SurfaceDb::open(root.string());
  ASSERT_TRUE(db.has_value());
  auto spy_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(spy_cfg.has_value());
  EXPECT_TRUE(spy_cfg->enabled);
  SessionInputs in;
  in.S = 500.0; in.r = 0.05;
  apply_symbol_config(*spy_cfg, in);
  EXPECT_EQ(in.curve.kind, VolCurveKind::ConvexDense);
  EXPECT_EQ(in.curve.convex.node_cap, 48);
  auto aapl_cfg = db->symbol_config("AAPL");
  ASSERT_TRUE(aapl_cfg.has_value());
  EXPECT_FALSE(aapl_cfg->enabled);      // pipeline skips disabled names
  // Real-time adjustment: another handle flips node_cap; pipeline refreshes.
  {
    auto ops = SurfaceDb::open(root.string());
    ASSERT_TRUE(ops.has_value());
    auto c = *ops->symbol_config("SPY");
    c.curve.convex.node_cap = 64;
    ASSERT_TRUE(ops->upsert_symbol("SPY", c).has_value());
  }
  ASSERT_TRUE(db->refresh().has_value());
  EXPECT_EQ(db->symbol_config("SPY")->curve.convex.node_cap, 64);
  // Stored surfaces still serve:
  ASSERT_TRUE(db->load_surface("2026-07-11", "SPY").has_value());
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 2: Build; verify failure** (missing `apply_symbol_config` / `symbol_config_from_preset`).

- [ ] **Step 3: Implement.**
  - `apply_symbol_config`: exactly the doc-comment order: `apply_fit_preset(in, cfg.preset);` then `if (cfg.pin_curve) { in.curve = cfg.curve; in.calib = cfg.curve.parametric; }` then `if (cfg.al_override) in.deam.al_opts = cfg.al;` then the six scalars/flags unconditionally.
  - `symbol_config_from_preset`: `SessionInputs tmp; apply_fit_preset(tmp, preset);` capture into a `SymbolFitConfig`: `preset` = preset, `pin_curve = false`, `curve = tmp.curve`, `al_override = tmp.deam.al_opts.has_value()`, `al = tmp.deam.al_opts.value_or(AlOpts{})`, `band_k = tmp.band_k`, `calendar_repair = tmp.calendar_repair`, four bool flags from `tmp`. (Read session.cpp's `apply_fit_preset` first to confirm which fields it touches; the capture must mirror it faithfully.)

- [ ] **Step 4: Build + full module gate.** `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDb|SurfaceArchive"` — ALL PASS. Then the whole-module sanity run: `& .\scripts\atx-build.ps1 -Ctest -L atx_vol` — expect no regressions (same pass count as the pre-task baseline).

- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): apply_symbol_config pipeline binding + surface_db end-to-end test"
```

---

## Final Verification (controller, after all tasks)

- [ ] Full atx-vol suite: `& .\scripts\atx-build.ps1 -Ctest -L atx_vol` — zero failures.
- [ ] Dispatch the final whole-branch code review (superpowers:requesting-code-review) with the merge-base diff package.
- [ ] superpowers:finishing-a-development-branch.
