# atx-vol Universe Auto-Fit Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise full-universe (Russell-3000 proxy, real OPRA snapshot) auto-fit survival from 62% to ≥85% of data-bearing boards, cut fit CPU ≥40%, and make every failure and quality flag truthful and machine-readable.

**Architecture:** All changes land inside the existing unified fit-policy pipeline (`select_fit_policy` → CurveSelector/direct route → `VolaSession::build` → fallback ladder). No new subsystem: we widen the fallback ladder, parametrize slice admission, plumb a spot override, add structured fit outcomes + phase timings, and expose the knobs the universe entry point needs. Ground truth for every acceptance gate is the real snapshot at `data/opra_universe` (2026-07-01T14:00Z, 2734 data boards) and the measured baseline in `atx-vol/docs/reviews/2026-07-12-universe-autofit-deep-dive.md`.

**Tech Stack:** C++20 (clang-cl, Ninja), GoogleTest (`atx-vol/tests/*_test.cpp`), CMake presets (`build-rel` = Release, examples ON), Python 3 analyzers in `atx-vol/tools/`.

## Global Constraints

- Baseline numbers (do not regress): auto-robust full run ok=1698, fit CPU=1929.5 s, value CPU=494 s; pinned-essvi ok=2267; fast-preset ok=1698, fit CPU=455.9 s.
- Every acceptance rerun uses: `./build-rel/bin/universe_autofit.exe --opra-root data/opra_universe --date 2026-07-01 --symbols-file data/universe/r3000_proxy_symbols.txt --fit-workers 16`.
- Release build for all timing gates: `cmake --build build-rel --target universe_autofit atx_vol_tests`.
- Unit tests must pass: `ctest --test-dir build-rel -R atx_vol --output-on-failure` (baseline: 1055/1058 pass; the 3 known-skips are unrelated).
- Public structs get new fields APPENDED with defaulted values (ABI-stable in-tree, no reordering).
- `-Werror` is on: every new enum consumer switch must be exhaustive (VolCurveKind now includes SplineVol).
- Commit after every green task; message style `feat(atx-vol): ...` / `fix(atx-vol): ...` / `test(atx-vol): ...`.

---

### Task 1: Add eSSVI rung to the SVI fallback ladder

The measured 519-board recovery: SparseGuard routes thin boards to SVI; when SVI produces no usable slice the ladder tries only LinearVariance (which has the *strictest* admission and also fails), while a pinned eSSVI fit succeeds on 519 of those 874 boards. eSSVI must be a rung after SVI.

**Files:**
- Modify: `atx-vol/src/pricer_fitter.cpp:30` (the `kFromSvi` ladder)
- Test: `atx-vol/tests/pricer_fitter_test.cpp` (existing file; add one test)

**Interfaces:**
- Consumes: `fallback_curve_rungs(VolCurveKind) -> std::span<const VolCurveKind>` (pricer_fitter.cpp:27).
- Produces: `fallback_curve_rungs(VolCurveKind::Svi)` returns `{Essvi, LinearVariance}` — Task 13's rerun depends on this ordering.

- [ ] **Step 1: Write the failing test**

Find the existing test file's include block and add (or extend the existing fallback-ladder test group):

```cpp
TEST(FallbackLadder, SviLadderTriesEssviBeforeLinearVariance) {
  const auto rungs = fallback_curve_rungs(VolCurveKind::Svi);
  ASSERT_EQ(rungs.size(), 2u);
  EXPECT_EQ(rungs[0], VolCurveKind::Essvi);
  EXPECT_EQ(rungs[1], VolCurveKind::LinearVariance);
}
```

`fallback_curve_rungs` is declared in `atx/vol/pricer_fitter.hpp`; if it is file-local today, hoist the declaration into the header (it already has external linkage semantics — check the header first; only add the declaration if absent).

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build-rel --target atx_vol_tests && ctest --test-dir build-rel -R FallbackLadder --output-on-failure`
Expected: FAIL — `rungs.size()` is 1.

- [ ] **Step 3: Minimal implementation**

```cpp
// pricer_fitter.cpp:30 — was: {VolCurveKind::LinearVariance}
static constexpr VolCurveKind kFromSvi[]{VolCurveKind::Essvi, VolCurveKind::LinearVariance};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ctest --test-dir build-rel -R FallbackLadder --output-on-failure`
Expected: PASS. Also run the full suite (`-R atx_vol`) — no regressions.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/src/pricer_fitter.cpp atx-vol/tests/pricer_fitter_test.cpp atx-vol/include/atx/vol/pricer_fitter.hpp
git commit -m "fix(atx-vol): add eSSVI rung to SVI fallback ladder (sparse-board recovery)"
```

---

### Task 2: Route selector NotFound through the fallback ladder

Measured: 50 mid-liquidity boards (600–2400 rows) die with `select_curve: no candidate produced a scorable fit`; all 50 fit fine when pinned eSSVI. Today `ATX_TRY` at pricer_fitter.cpp:159 propagates NotFound and the fallback ladder (lines 183–196) never runs because `built` was never attempted.

**Files:**
- Modify: `atx-vol/src/pricer_fitter.cpp:144-170` (the selector branch of `PricerFitter::fit`)
- Test: `atx-vol/tests/pricer_fitter_test.cpp`

**Interfaces:**
- Consumes: `select_curve(under, sp, cfg_.selector) -> Result<SelectorResult>`; `decision_->curve` (the profile-direct route the policy computed BEFORE deciding to cross-validate — it is always populated by `configure_direct_route`).
- Produces: on selector NotFound, `fit()` proceeds with `decision_->curve` as primary and sets `decision_->used_fallback = true`; `selection_` stays empty. Task 9's `FitStage::Selector` error only appears when the direct route AND ladder also fail.

- [ ] **Step 1: Write the failing test**

Build a minimal synthetic underlying that the selector cannot score but a direct parametric fit can handle: one expiry, exactly 6 two-sided quotes, T = 10/365 (selector's holdout split leaves < 8 eu-obs per side, tripping its `eu->obs < 8` gate). Reuse the existing synthetic-chain helper in `pricer_fitter_test.cpp` (there is one used by the auto-route tests; follow its construction pattern verbatim, shrinking to one expiry / 6 strikes). Then:

```cpp
TEST(PricerFitterAuto, SelectorNotFoundFallsBackToDirectRoute) {
  OptionChain chain = make_sparse_single_expiry_chain(/*n_strikes=*/6);
  PricerConfig cfg;                 // curve unset => auto route
  cfg.policy.mode = FitSelectionMode::CrossValidated; // force the selector path
  PricerFitter fitter{cfg};
  const Status st = fitter.fit(chain);
  ASSERT_TRUE(st) << st.error().to_string();
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_TRUE(fitter.decision()->used_fallback);
  EXPECT_FALSE(fitter.selection().has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build-rel -R SelectorNotFoundFallsBack --output-on-failure`
Expected: FAIL — `st` holds `NotFound: select_curve: no candidate produced a scorable fit`.

- [ ] **Step 3: Implementation**

Replace the `ATX_TRY` selector call with explicit NotFound handling:

```cpp
  } else {
    SurfaceParityInputs sp;
    // ... (existing sp population stays unchanged) ...
    Result<SelectorResult> chosen = select_curve(chain.underlying(), sp, cfg_.selector);
    if (chosen.has_value()) {
      chosen->chosen.parametric = in.calib;
      in.curve = chosen->chosen;
      if (decision_.has_value()) {
        decision_->curve = chosen->chosen;
        decision_->preset = effective_preset;
      }
      selection_ = std::move(*chosen);
    } else if (chosen.error().code() == ErrorCode::NotFound && decision_.has_value()) {
      // Zero scorable candidates is a verdict about the SELECTOR's gates, not
      // the board: fall back to the profile-direct route and let the rung
      // ladder below handle any residual failure.
      decision_->curve.parametric = in.calib;
      in.curve = decision_->curve;
      decision_->used_fallback = true;
    } else {
      return Err(std::move(chosen).error());
    }
  }
```

`decision_->curve` at this point still holds the `configure_direct_route` result (the selector branch has not overwritten it on the failure path).

- [ ] **Step 4: Run tests**

Run: `ctest --test-dir build-rel -R atx_vol --output-on-failure`
Expected: new test PASS, zero regressions.

- [ ] **Step 5: Real-data spot check**

Run: `./build-rel/bin/universe_autofit.exe --opra-root data/opra_universe --date 2026-07-01 --symbols-file data/universe/smoke100.txt --out data/opra_universe/task2_check.csv --fit-workers 12`
Expected: CBRS, COHR, CRDO flip from `fit_error` to `ok` (they were the 100-name set's selector-NotFound casualties); ok count ≥ 99 (BRK.B still fails until Task 5).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/src/pricer_fitter.cpp atx-vol/tests/pricer_fitter_test.cpp
git commit -m "fix(atx-vol): fall back to direct route when selector finds no scorable candidate"
```

---

### Task 3: Parametrize slice admission (`min_usable_obs`) and align the two fit paths

Measured: `kMinUsableObs = 5` is duplicated in three files (curve_fit.cpp:33, surface_parity.cpp:58, vola_parity.cpp:60) and the sparse tail (874 boards, p50 56 rows) can rarely field 5 de-Am'd two-sided OTM legs per expiry. A 3-obs floor is enough to pin down SVI's per-slice parameters under the sequential calendar floor. Make the floor a config field, default 5 (unchanged), and have the SparseGuard route set 3.

**Files:**
- Modify: `atx-vol/include/atx/vol/calib.hpp` (CalibConfig — find the struct; it is the `in.calib` type populated in `PricerFitter::fit`) — append field.
- Modify: `atx-vol/src/curve_fit.cpp:33,110,200`, `atx-vol/src/surface_parity.cpp:58,381`, `atx-vol/src/vola_parity.cpp:60,237` — replace constant with config read.
- Modify: `atx-vol/src/fit_policy.cpp:129-133` (SparseGuard branch sets the sparse floor).
- Test: `atx-vol/tests/curve_fit_test.cpp` (or the file holding existing `fit_curve_surface` admission tests — search `usable slice` in tests first and extend that file).

**Interfaces:**
- Consumes: `CalibConfig` flows `PricerConfig -> SessionInputs.calib -> fit_curve_surface / run_surface_parity`.
- Produces: `CalibConfig::min_usable_obs` (`std::uint32_t`, default 5, floor-clamped to 3 at use sites). SparseGuard sets `out.curve.parametric.min_usable_obs = 3`. Tasks 9/13 read the recovered boards.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CurveFitAdmission, MinUsableObsIsConfigurable) {
  // Chain with exactly 4 valid two-sided OTM legs on its single expiry.
  Underlying under = make_underlying_with_n_valid_legs(4);
  SurfaceFitInputs in = default_fit_inputs();     // follow existing test helpers
  in.calib.min_usable_obs = 5;
  EXPECT_FALSE(fit_curve_surface(under, in).has_value());  // 4 < 5: rejected
  in.calib.min_usable_obs = 3;
  EXPECT_TRUE(fit_curve_surface(under, in).has_value());   // 4 >= 3: fits
}
```

(Adapt helper names to the actual ones in the existing admission tests — the test file already constructs `Underlying`s with controlled quote validity for the `leg_quote_valid` tests.)

- [ ] **Step 2: Run test to verify it fails**

Expected: compile error — `min_usable_obs` not a member. That is the red state.

- [ ] **Step 3: Implementation**

`calib.hpp` (append to CalibConfig):

```cpp
  // Minimum de-Americanized OTM legs for an expiry to yield a fitted slice.
  // 5 = historical default (dense-board safe); the sparse-guard route lowers
  // it to 3, the identifiability floor for a 3-parameter slice under the
  // sequential calendar constraint. Values below 3 are clamped to 3.
  std::uint32_t min_usable_obs{5};
```

Each use site replaces the file-local constant, e.g. curve_fit.cpp:

```cpp
const std::size_t min_obs = std::max<std::uint32_t>(3u, in.calib.min_usable_obs);
...
if (!obs || obs->obs.size() < min_obs) {
```

surface_parity.cpp and vola_parity.cpp identically (their input structs also carry `calib`; verify the member path — `in.calib.min_usable_obs` — and if vola_parity's inputs lack calib, leave vola_parity at the constant and note it: it is the legacy parity harness, not the universe path).

fit_policy.cpp SparseGuard branch, after `configure_direct_route(out, context, config);`:

```cpp
    out.curve.parametric.min_usable_obs = 3;
```

- [ ] **Step 4: Run tests**

Run: `ctest --test-dir build-rel -R atx_vol --output-on-failure`
Expected: all pass (default 5 keeps every existing expectation intact).

- [ ] **Step 5: Real-data acceptance**

Rebuild release, rerun the FULL universe (auto, robust):
`./build-rel/bin/universe_autofit.exe ... --out data/opra_universe/autofit_task3_robust.csv --fit-workers 16`
Expected: ok ≥ 2100 (baseline 1698; Tasks 1+2+3 together should recover most of the 519+50), `no expiry produced a usable slice` count ≤ 360. Record exact numbers in the commit message.

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/calib.hpp atx-vol/src/curve_fit.cpp atx-vol/src/surface_parity.cpp atx-vol/src/vola_parity.cpp atx-vol/src/fit_policy.cpp atx-vol/tests/curve_fit_test.cpp
git commit -m "feat(atx-vol): configurable slice-admission floor; sparse route admits 3-obs slices"
```

---

### Task 4: Spot override plumbing (kill the 106 PCP failures)

Measured: 106 boards (p50 27 rows) die at panel build — `no well-conditioned co-terminal expiry to imply spot; pass spot_override`. The knob exists downstream but no universe entry point feeds it.

**Files:**
- Modify: `atx-vol/include/atx/vol/opra_batch.hpp` (OpraBatchSpec — append field), `atx-vol/src/opra_batch.cpp` (thread it to the panel builder), `atx-vol/src/opra_panel.cpp` (accept the override before PCP inference; find the error-site function around line 517).
- Modify: `atx-vol/examples/universe_autofit.cpp` (new `--spot-file FILE` flag, format `SYMBOL,SPOT` CSV).
- Create: `atx-vol/tools/build_universe_spot_file.py` (spots from the local EQUS hive).
- Test: `atx-vol/tests/opra_batch_test.cpp` (existing loader tests live here — search `load_opra_daterange` in tests).

**Interfaces:**
- Consumes: `OpraBatchSpec` (symbols/date/root/snapshot_suffix/r), the panel builder that today errors with `Unavailable`.
- Produces: `OpraBatchSpec::spot_overrides` (`std::map<std::string, double>`, default empty). Panel builder tries PCP first; on PCP failure uses the override if present (override is fallback, not master — PCP-implied spot stays authoritative when available). `universe_autofit --spot-file` populates the map.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(OpraBatch, SpotOverrideRescuesPcpFailure) {
  // Panel fixture with a single far-dated expiry and one-sided quotes:
  // PCP inference must fail. (Reuse the existing panel fixture builder.)
  OpraBatchSpec spec = tiny_one_sided_fixture_spec();
  Result<OpraBatchResult> r1 = load_opra_daterange(spec);
  ASSERT_TRUE(r1.has_value());
  EXPECT_FALSE(r1->entries.at(0).panel.has_value());  // baseline: PCP fails

  spec.spot_overrides["TINY"] = 42.50;
  Result<OpraBatchResult> r2 = load_opra_daterange(spec);
  ASSERT_TRUE(r2.has_value());
  ASSERT_TRUE(r2->entries.at(0).panel.has_value());
  EXPECT_NEAR(r2->entries.at(0).panel->spot, 42.50, 1e-9);
}
```

- [ ] **Step 2: Run to verify it fails** — compile error on `spot_overrides`; red.

- [ ] **Step 3: Implementation**

opra_batch.hpp (append to OpraBatchSpec):

```cpp
  // Per-symbol spot fallback used only when put-call-parity inference fails
  // (thin boards can lack a well-conditioned co-terminal expiry). Keyed by the
  // spec's symbol string. PCP-implied spot remains authoritative when it works.
  std::map<std::string, double> spot_overrides{};
```

In opra_batch.cpp, where the per-symbol panel build reports the PCP `Unavailable` error, retry with the override:

```cpp
Result<OpraPanel> panel = build_opra_panel(frame, opts);
if (!panel.has_value() && panel.error().code() == ErrorCode::Unavailable) {
  if (const auto it = spec.spot_overrides.find(symbol); it != spec.spot_overrides.end()) {
    OpraPanelOptions with_spot = opts;
    with_spot.spot_override = it->second;   // field already exists on the panel options
    panel = build_opra_panel(frame, with_spot);
  }
}
```

(Verify the actual names: the error text says "pass spot_override", so grep `spot_override` in opra_panel.cpp for the existing option; wire whatever exists rather than inventing a parallel path.)

universe_autofit.cpp: parse `--spot-file`, read `SYMBOL,SPOT` lines into `spec.spot_overrides`.

build_universe_spot_file.py: read the last available close ≤ snapshot date per symbol from `data/databento/equs_ohlcv_1d_by_date` (same access pattern as `atx-vol/tools/build_r3000_proxy_universe.py` — copy its hive-reading code), write `data/universe/spots_2026-07-01.csv`.

- [ ] **Step 4: Run tests** — `ctest -R OpraBatch`; expected PASS, no regressions.

- [ ] **Step 5: Real-data acceptance**

```bash
python atx-vol/tools/build_universe_spot_file.py --date 2026-07-01 --symbols-file data/universe/r3000_proxy_symbols.txt --out data/universe/spots_2026-07-01.csv
./build-rel/bin/universe_autofit.exe ... --spot-file data/universe/spots_2026-07-01.csv --out data/opra_universe/autofit_task4_robust.csv --fit-workers 16
```
Expected: `no well-conditioned co-terminal expiry` count drops from 106 to ≤ 15 (symbols absent from EQUS keep failing — acceptable).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/include/atx/vol/opra_batch.hpp atx-vol/src/opra_batch.cpp atx-vol/src/opra_panel.cpp atx-vol/examples/universe_autofit.cpp atx-vol/tools/build_universe_spot_file.py atx-vol/tests/opra_batch_test.cpp
git commit -m "feat(atx-vol): spot_override fallback for PCP-unresolvable thin boards"
```

---

### Task 5: Dotted class-share symbol normalization (BRK.B et al.)

Measured: 6 boards fail `underlying carries no chains`. The hive stores `underlying = "BRK.B"` (universe symbol) while the chain builder keys chains by OSI root `"BRKB"`; the two never meet.

**Files:**
- Modify: `atx-vol/src/opra_batch.cpp` — in `corpus_board_from_opra` (or the chain-grouping step inside the panel/board build), normalize the join key: `key = symbol with "." removed` on BOTH sides.
- Test: `atx-vol/tests/opra_batch_test.cpp`

**Interfaces:**
- Consumes: QuoteFrame rows with `underlying` = dotted spec symbol, `symbol` = 21-char OSI (root `BRKB`).
- Produces: boards for dotted symbols carry chains; no API change.

- [ ] **Step 1: Failing test**

```cpp
TEST(OpraBatch, DottedClassShareSymbolFindsItsChains) {
  // Fixture frame: underlying "BRK.B", OSI symbols rooted "BRKB".
  QuoteFrame frame = make_frame_with_underlying_and_osi_root("BRK.B", "BRKB");
  auto board = corpus_board_from_opra("2026-07-01", "BRK.B", make_panel(frame));
  EXPECT_FALSE(board.frame.rows.empty());
  // The previous behavior produced a board whose chain set was empty ->
  // fit_curve_surface: "underlying carries no chains".
}
```

(Shape the fixture with the same helpers Task 4's test used; assert on whatever the "has chains" observable is — `board.env`/chain count — matching existing board tests.)

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implementation** — at the chain-group join, replace direct string equality with a normalizer:

```cpp
inline std::string osi_join_key(std::string_view sym) {
  std::string k;
  k.reserve(sym.size());
  for (const char c : sym) if (c != '.' && c != ' ') k.push_back(c);
  return k;
}
```

Apply to both the hive `underlying` value and the requested symbol before comparing.

- [ ] **Step 4: Run tests; expect PASS.**

- [ ] **Step 5: Real-data check** — rerun `smoke100`; BRK.B flips to ok (100/100 with Tasks 1–4 in).

- [ ] **Step 6: Commit** — `fix(atx-vol): normalize dotted class-share symbols to OSI join key`.

---

### Task 6: Turn on the de-Am fit cache for flat-rate boards (biggest single CPU lever)

Measured: the de-Americanization observation build is ~60–65% of direct-route fit CPU and `use_deam_cache_for_fit` is `false` in every preset; correction caches are already built and used at valuation time, so cache correctness machinery exists. Flat-r boards (the whole universe harness: single scalar `r`) are exactly the case the cache supports (pricer_fitter.cpp:120-124 disables it only for term-rate boards).

**Files:**
- Modify: `atx-vol/src/session.cpp` (or wherever `FitPreset -> SessionInputs` defaults materialize — grep `use_deam_cache_for_fit` for the preset table) — flip default to `true` for Fast and Robust when the board is scalar-rate.
- Test: `atx-vol/tests/session_test.cpp` + a timing/parity assertion in `atx-vol/tests/fit_metrics_test.cpp` style.

**Interfaces:**
- Consumes: existing `PricerConfig::use_deam_cache_for_fit` (`std::optional<bool>`, explicit value still wins).
- Produces: preset default becomes true on scalar-rate boards; term-rate boards keep `false` (the existing guard already enforces this — do not touch it).

- [ ] **Step 1: Failing test (parity gate, not timing)**

```cpp
TEST(DeAmFitCache, CachedAndColdFitsAgreeOnDenseBoard) {
  Underlying under = load_spy_fixture();     // existing SPY fixture used by session tests
  SessionInputs cold = default_inputs();  cold.use_deam_cache_for_fit = false;
  SessionInputs hot  = default_inputs();  hot.use_deam_cache_for_fit  = true;
  auto a = VolaSession::build(under, cold);
  auto b = VolaSession::build(under, hot);
  ASSERT_TRUE(a.has_value() && b.has_value());
  const auto &da = a->diagnostics(); const auto &db = b->diagnostics();
  EXPECT_NEAR(da.mean_rmse_vol, db.mean_rmse_vol, 5e-4);
  EXPECT_NEAR(da.mean_frac_within_bidask, db.mean_frac_within_bidask, 5e-3);
}
```

Then the default-flip assertion:

```cpp
TEST(DeAmFitCache, RobustPresetDefaultsCacheOnForScalarRate) {
  SessionInputs in = materialize_preset(FitPreset::Robust, /*scalar rate fixture*/);
  EXPECT_TRUE(in.use_deam_cache_for_fit);
}
```

- [ ] **Step 2: Run — parity test may already PASS (cache correctness is not new); the preset-default test FAILS. That failing test is the red state.**

- [ ] **Step 3: Implementation** — in the preset materialization, set `use_deam_cache_for_fit = true` for Fast/Robust (Hft already pins dense LV; leave as-is), keeping the existing term-rate override at pricer_fitter.cpp:120-124 untouched.

- [ ] **Step 4: Run full suite — expect PASS, and specifically the SPY calendar/parity gates unchanged.**

- [ ] **Step 5: Real-data acceptance (CPU gate)**

Rerun `smoke100` auto-robust. Expected: serial fit CPU ≤ 300 s (baseline 433 s, i.e. ≥30% cut), family choices unchanged on ≥95 of the ok boards, median in-band within ±0.005 of baseline. Record before/after in commit message.

- [ ] **Step 6: Commit** — `perf(atx-vol): default de-Am fit cache on for scalar-rate boards`.

---

### Task 7: value_chain fast path for unset quote sides + NaN provenance

Measured: value CPU 494 s for 981k options (0.50 ms/option); 41% of bid legs are unset (INT64_MIN → no bid) yet each still walks into an American IV inversion attempt before producing NaN, and the output cannot distinguish "no bid quoted" from "inversion failed".

**Files:**
- Modify: `atx-vol/src/pricer_fitter.cpp` (`PricerFitter::value_chain`, from line ~210): early-skip inversion when the side's quote is non-positive/non-finite; count skips.
- Modify: `atx-vol/include/atx/vol/pricer_fitter.hpp` (`ChainValuation` — append counters).
- Test: `atx-vol/tests/pricer_fitter_test.cpp`

**Interfaces:**
- Produces: `ChainValuation::n_bid_unset, n_ask_unset, n_bid_iv_fail, n_ask_iv_fail` (`std::size_t`, default 0). Semantics: `unset` = quote absent/invalid so inversion was never attempted; `iv_fail` = inversion attempted and failed. `bid_iv`/`ask_iv` stay NaN in both cases (no output change).

- [ ] **Step 1: Failing test**

```cpp
TEST(ValueChain, UnsetBidSkipsInversionAndCountsProvenance) {
  OptionChain chain = make_chain_with_missing_bids(/*n=*/10, /*missing=*/4);
  PricerFitter fitter{PricerConfig{}};
  ASSERT_TRUE(fitter.fit(chain));
  auto val = fitter.value_chain(chain, OutputField::Prices | OutputField::Bands, 1);
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val->n_bid_unset, 4u);
  EXPECT_EQ(val->n_bid_iv_fail + val->n_bid_unset,
            std::count_if(val->bid_iv.begin(), val->bid_iv.end(),
                          [](double x) { return x != x; }));
}
```

- [ ] **Step 2: Run — compile error on the new counters; red.**

- [ ] **Step 3: Implementation** — in the per-option loop, before the bid-side inversion:

```cpp
if (!(bid > 0.0) || !std::isfinite(bid)) { ++val.n_bid_unset; /* bid_iv stays NaN */ }
else if (Result<double> iv = invert_american_iv(bid, ...); iv.has_value()) { val.bid_iv[k] = *iv; }
else { ++val.n_bid_iv_fail; }
```

(Mirror for ask/mid; mid is derived — skip mid inversion when either side is unset and mid is non-positive. Counter increments must be thread-safe: the loop is parallel over options when `n_threads > 1` — accumulate per-worker locals and sum, matching how the existing NaN counting in `universe_autofit.cpp` stays outside the parallel region.)

- [ ] **Step 4: Run tests; expect PASS.**

- [ ] **Step 5: Real-data acceptance** — full-universe rerun: value CPU ≤ 350 s (baseline 494 s), `n_bid_unset` universe sum ≈ 400k (41% of 981k), model prices bit-identical for options with two-sided quotes (spot-check 3 symbols' CSV rows against baseline).

- [ ] **Step 6: Commit** — `perf(atx-vol): skip IV inversion on unset quote sides; NaN provenance counters`.

---

### Task 8: Selector budget knobs + harness exposure

Measured: CV routing = 318 boards = 82% of fit CPU (mean 5.0 s); robust preset spends 4.2× fast's CPU for zero measured quality gain; `SelectorConfig`/`FitPolicyConfig` are unreachable from the universe entry point. Two knobs, no new algorithm: (a) `oos_max_expiries` already exists — expose it; (b) selector-level candidate time budget.

**Files:**
- Modify: `atx-vol/include/atx/vol/curve_selector.hpp` (SelectorConfig — append `time_budget_ms`), `atx-vol/src/curve_selector.cpp` (budget check between candidates).
- Modify: `atx-vol/examples/universe_autofit.cpp` — flags `--oos-max-expiries N`, `--selector-budget-ms N`, `--sparse-floor N`, `--min-direct-confidence X` mapped onto `PricerConfig`.
- Test: `atx-vol/tests/curve_selector_test.cpp`

**Interfaces:**
- Produces: `SelectorConfig::time_budget_ms` (`double`, default 0 = unlimited). Contract: candidates are scored in order; once elapsed wall time exceeds the budget AND ≥1 candidate is scorable, remaining candidates are skipped (never returns NotFound *because* of the budget — an exhausted budget with zero scorable candidates keeps scoring the next candidate until one scores or the list ends).

- [ ] **Step 1: Failing test**

```cpp
TEST(SelectorBudget, BudgetSkipsRemainingCandidatesButNeverStarves) {
  SelectorConfig cfg;
  cfg.time_budget_ms = 0.001;            // expires immediately
  auto r = select_curve(dense_fixture_underlying(), fixture_inputs(), cfg);
  ASSERT_TRUE(r.has_value());            // still returns SOME scorable winner
  EXPECT_LT(r->scores_evaluated, default_selector_candidates().size());
}
```

Add `scores_evaluated` (`std::size_t`) to `SelectorResult` while here (append, default 0) so the test — and Task 13's analysis — can observe skipping.

- [ ] **Step 2: Run — compile error (new fields); red.**

- [ ] **Step 3: Implementation** — in `select_curve`'s candidate loop:

```cpp
const auto t0 = std::chrono::steady_clock::now();
bool any_scorable = false;
for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
  if (cfg.time_budget_ms > 0.0 && any_scorable) {
    const double el = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    if (el > cfg.time_budget_ms) break;
  }
  // ... existing per-candidate scoring ...
  if (/* candidate produced n_holdout > 0 && !disqualified */) any_scorable = true;
  ++result.scores_evaluated;
}
```

Harness flags map straight onto `cfg.selector.oos_max_expiries`, `cfg.selector.time_budget_ms`, `cfg.policy.sparse_validation_floor`, `cfg.policy.min_direct_confidence`.

- [ ] **Step 4: Run tests; expect PASS (budget=0 default keeps all existing selector tests identical).**

- [ ] **Step 5: Real-data acceptance** — `smoke100` with `--selector-budget-ms 2000 --oos-max-expiries 4`: CV-board mean fit ≤ 2.5 s (baseline 5.0 s), chosen family unchanged on ≥90% of CV boards vs the unbudgeted run, median in-band within ±0.01.

- [ ] **Step 6: Commit** — `feat(atx-vol): selector time budget + universe harness knob exposure`.

---

### Task 9: Structured fit outcome (stage + code), replacing string-only errors

Measured: the harness had to regex four error families out of strings; empty-bid vs failed-inversion, panel-stage vs fit-stage were indistinguishable without Task 7's counters and manual taxonomy.

**Files:**
- Modify: `atx-vol/include/atx/vol/pricer_fitter.hpp` — add enum + accessor; `atx-vol/src/pricer_fitter.cpp` — populate at each failure site.
- Modify: `atx-vol/examples/universe_autofit.cpp` — CSV gains a `stage` column from the accessor.
- Test: `atx-vol/tests/pricer_fitter_test.cpp`

**Interfaces:**
- Produces:

```cpp
enum class FitStage : std::uint8_t {
  None = 0,       // fit() succeeded
  Forward,        // resolve_chain_forward / carry inputs
  Observations,   // de-Am observation build produced nothing fittable
  Selector,       // select_curve failed AND direct-route fallback also failed
  SliceFit,       // VolaSession::build / fit_curve_surface (incl. exhausted ladder)
  Repair,         // calendar repair / floor enforcement
};
// PricerFitter:
[[nodiscard]] FitStage failure_stage() const noexcept;  // None unless last fit() failed
```

Population rule: set `stage_` immediately before each `return Err(...)` in `fit()`; the ladder-exhausted path is `SliceFit` with the primary error preserved (existing behavior). `load_*` failures stay the harness's job (they never reach the fitter).

- [ ] **Step 1: Failing test**

```cpp
TEST(FitOutcome, ExhaustedLadderReportsSliceFitStage) {
  OptionChain chain = make_unfittable_chain();     // 2 one-sided quotes
  PricerFitter fitter{PricerConfig{}};
  EXPECT_FALSE(fitter.fit(chain));
  EXPECT_EQ(fitter.failure_stage(), FitStage::SliceFit);
}
TEST(FitOutcome, SuccessLeavesStageNone) {
  PricerFitter fitter{PricerConfig{}};
  ASSERT_TRUE(fitter.fit(make_dense_fixture_chain()));
  EXPECT_EQ(fitter.failure_stage(), FitStage::None);
}
```

- [ ] **Step 2: Run — compile error; red.**
- [ ] **Step 3: Implement (enum, member init `None`, set at each `return Err` site, reset to `None` at fit() entry and on success).**
- [ ] **Step 4: Run tests; PASS.**
- [ ] **Step 5: Harness: write `stage` column (`none/forward/observations/selector/slice_fit/repair` strings); update `atx-vol/tools/analyze_universe_autofit.py` to group by it.**
- [ ] **Step 6: Commit** — `feat(atx-vol): structured FitStage failure taxonomy on PricerFitter`.

---### Task 10: Truthful calendar attestation (repair domain vs report domain)

Measured: `calendar_arb_free` false on 62.7% of the universe and 0% of essvi boards — the flag checks the FULL |k|≤3 grid after a repair that only enforces |k|≤0.7, so the "robust" preset can never attest the surface it just repaired. Split the attestation.

**Files:**
- Modify: `atx-vol/include/atx/vol/session.hpp` (SessionDiagnostics — append fields), `atx-vol/src/session.cpp` (populate both domains where the existing full-grid check runs).
- Test: `atx-vol/tests/session_test.cpp` (extend the existing `SpyRealCalendarReporting` group).

**Interfaces:**
- Produces (append to SessionDiagnostics):

```cpp
  // Post-repair calendar check on the REPAIR domain (|k| <= repair.k_max, the
  // region MonotoneFit actually enforces). This is the flag the repair can
  // honestly promise. The existing calendar_arb_free (full |k|<=3 grid) keeps
  // its strict meaning.
  bool calendar_arb_free_repaired_domain{false};
  double calendar_checked_k_max{3.0};   // domain of the strict flag
```

- [ ] **Step 1: Failing test**

```cpp
TEST(CalendarAttestation, RepairedDomainFlagTrueWhenNearMoneyClean) {
  auto sess = build_spy_fixture_session_with_repair();   // existing helper path
  const auto &dg = sess.diagnostics();
  EXPECT_TRUE(dg.calendar_arb_free_repaired_domain);     // repair did its job
  // strict flag unchanged semantics:
  EXPECT_DOUBLE_EQ(dg.calendar_checked_k_max, 3.0);
}
```

- [ ] **Step 2: Run — compile error; red.**
- [ ] **Step 3: Implement — run the existing grid check twice (full grid as today; repair-domain grid `|k| <= repair.k_max`), populate both fields. The repair's `k_max` is already in the repair config the session holds (grep `0.7` / `k_max` in the MonotoneFit config).**
- [ ] **Step 4: Full suite — the SPY calendar invariant tests must stay green.**
- [ ] **Step 5: Harness/analyzer: emit both flags; full-run expectation: `calendar_arb_free_repaired_domain` true on ≥90% of essvi/svi ok boards (vs 0%/30% for the strict flag).**
- [ ] **Step 6: Commit** — `feat(atx-vol): calendar attestation split into repair-domain and full-grid flags`.

---

### Task 11: Fit-phase timings + progress callback in the library

Measured: the harness measured stages from outside; `ATX_VOL_PROFILE` regions exist only for the backtest loop; the first full-universe run was invisible for 4 minutes (buffered stdout, no progress hook anywhere).

**Files:**
- Modify: `atx-vol/include/atx/vol/pricer_fitter.hpp` — `FitTimings` struct + accessor; `atx-vol/src/pricer_fitter.cpp` + `atx-vol/src/curve_fit.cpp` — populate (curve_fit already computes `ms_forward_borrow`, `ms_obs_eu`, `ms_fit_slice` under `ATX_VOL_PROFILE`; lift those into returned timings unconditionally — they are cheap steady_clock reads).
- Modify: `atx-vol/include/atx/vol/opra_batch.hpp` — `OpraBatchSpec::progress` callback; `atx-vol/src/opra_batch.cpp` — invoke per symbol.
- Test: `atx-vol/tests/pricer_fitter_test.cpp`

**Interfaces:**
- Produces:

```cpp
struct FitTimings {
  double forward_ms{0}, observations_ms{0}, slice_fit_ms{0},
         selector_ms{0}, repair_ms{0}, total_ms{0};
};
// PricerFitter:
[[nodiscard]] const FitTimings &timings() const noexcept;

// OpraBatchSpec (append):
//   called after each symbol's load completes (any status); may be invoked
//   from worker threads — implementations must be thread-safe.
std::function<void(std::size_t done, std::size_t total, std::string_view symbol)> progress{};
```

- [ ] **Step 1: Failing test**

```cpp
TEST(FitTimings, StagesSumApproxTotal) {
  PricerFitter fitter{PricerConfig{}};
  ASSERT_TRUE(fitter.fit(make_dense_fixture_chain()));
  const FitTimings &t = fitter.timings();
  EXPECT_GT(t.total_ms, 0.0);
  EXPECT_GT(t.observations_ms, 0.0);
  EXPECT_LE(t.forward_ms + t.observations_ms + t.slice_fit_ms + t.selector_ms + t.repair_ms,
            t.total_ms * 1.10);
}
```

- [ ] **Step 2: Run — compile error; red.**
- [ ] **Step 3: Implement. universe_autofit drops its external stopwatch for fit sub-stages and emits `obs_ms,slice_ms,selector_ms` CSV columns; keep the wall columns for continuity.**
- [ ] **Step 4: Tests PASS; full suite green.**
- [ ] **Step 5: Full-run acceptance: analyzer prints stage shares from library timings; expect obs-build share ≈ 60% on direct-route boards pre-Task-6, ≈ 35% post (numbers go in the doc).**
- [ ] **Step 6: Commit** — `feat(atx-vol): per-stage fit timings + batch progress callback`.

---

### Task 12: Profile classifier — ETF hint + extensible prior table

Measured: 42/96 dense single names classified `IndexEtfUltraLiquid` while real sector ETFs (XLV/XLE) landed `OrdinarySingleName`; the hardcoded 21-ticker table is the only defense. Root cause: the voter reads "penny-dense huge board" as index-ETF. Two scoped fixes — an explicit is-ETF hint on FitContext, and a file-loadable prior table. NO new heuristic tuning beyond the hint gate (that is a research task, not sprint scope).

**Files:**
- Modify: `atx-vol/include/atx/vol/profile.hpp` (FitContext already carries hints — append `std::optional<bool> is_etf{}`), `atx-vol/src/profile.cpp` (voting: `IndexEtfUltraLiquid` verdict requires `is_etf != false`; when `is_etf == true` and board is liquid, prefer `IndexEtfUltraLiquid`), plus `load_ticker_priors(std::istream&)` parsing `TICKER,PROFILE` CSV into the existing seed-table lookup.
- Modify: `atx-vol/examples/universe_autofit.cpp` — `--etf-file FILE` (one symbol per line) + `--priors-file FILE` flags; ETF list membership sets `board.fit_context.is_etf`.
- Create: `data/universe/etf_symbols.txt` (the proxy-universe build already excludes most ETFs; this file lists the sector/index ETFs intentionally kept: SPY QQQ IWM DIA XLB XLC XLE XLF XLI XLK XLP XLRE XLU XLV XLY SMH XBI KRE GDX GDXJ ARKK TLT HYG LQD EEM EFA FXI EWZ USO UNG SLV GLD — verify against symbols actually present in `data/universe/r3000_proxy_symbols.txt` and keep only those).
- Test: `atx-vol/tests/profile_test.cpp`

**Interfaces:**
- Produces: `FitContext::is_etf` (`std::optional<bool>`, default nullopt = today's behavior); `load_ticker_priors(std::istream&) -> Result<std::size_t>` (count loaded; entries extend the seed table consulted by `ticker_seed_profile`).

- [ ] **Step 1: Failing tests**

```cpp
TEST(ProfileClassifier, DenseSingleNameWithEtfFalseNeverVotesIndexEtf) {
  ClassifierInputs f = dense_penny_board_features();   // the MU-shaped fixture
  FitContext ctx; ctx.is_etf = false;
  const ProfileVerdict v = classify_profile_with_context(f, ctx);
  EXPECT_NE(v.kind, ProfileKind::IndexEtfUltraLiquid);
}
TEST(ProfileClassifier, PriorTableLoadsAndSeeds) {
  std::istringstream in("CRWD,LiquidSingleName\nXLV,IndexEtfUltraLiquid\n");
  ASSERT_TRUE(load_ticker_priors(in).has_value());
  ProfileKind k{};
  EXPECT_TRUE(ticker_seed_profile("XLV", k));
  EXPECT_EQ(k, ProfileKind::IndexEtfUltraLiquid);
}
```

(If `classify_profile` today takes only features, add the `_with_context` overload delegating to the existing function when `is_etf` is nullopt.)

- [ ] **Step 2: Run — red.**
- [ ] **Step 3: Implement (hint gate in the voter; CSV loader appends to the seed map; loader rejects unknown profile names with `InvalidArgument`).**
- [ ] **Step 4: Tests PASS; full suite green (nullopt hint keeps all existing classifications byte-identical — assert by running the profile test suite).**
- [ ] **Step 5: Real-data acceptance — `smoke100` with `--etf-file`: XLV/XLE classified `IndexEtfUltraLiquid`; CRWD/IBM/LLY/WDC/LITE no longer `IndexEtfUltraLiquid` when listed in a priors file (ship `data/universe/priors_top100.csv` covering the 42 misrouted names from the deep-dive CSV — generate the list with `python -c` from `autofit_smoke100.csv` where profile==IndexEtfUltraLiquid, excluding true ETFs).**
- [ ] **Step 6: Commit** — `feat(atx-vol): ETF context hint + file-loadable ticker prior table`.

---

### Task 13: Full-universe acceptance rerun + deep-dive doc update

**Files:**
- Modify: `atx-vol/docs/reviews/2026-07-12-universe-autofit-deep-dive.md` (append "After the hardening sprint" section)
- Output: `data/opra_universe/autofit_after_robust.csv`, `autofit_after_fast.csv`

**Interfaces:** consumes everything above; produces the sprint's evidence.

- [ ] **Step 1: Rebuild release; full test suite green.**

- [ ] **Step 2: Run the after-measurement (both presets):**

```bash
./build-rel/bin/universe_autofit.exe --opra-root data/opra_universe --date 2026-07-01 \
  --symbols-file data/universe/r3000_proxy_symbols.txt --spot-file data/universe/spots_2026-07-01.csv \
  --etf-file data/universe/etf_symbols.txt --selector-budget-ms 2000 --oos-max-expiries 4 \
  --out data/opra_universe/autofit_after_robust.csv --fit-workers 16
./build-rel/bin/universe_autofit.exe ... --preset fast --out data/opra_universe/autofit_after_fast.csv --fit-workers 16
python atx-vol/tools/analyze_universe_autofit.py data/opra_universe/autofit_after_robust.csv
```

- [ ] **Step 3: Acceptance gates (all measured against the 2734 data boards):**

| gate | baseline | target |
|---|---|---|
| ok boards (auto robust) | 1698 (62%) | ≥ 2325 (85%) |
| `no usable slice` errors | 874 | ≤ 250 |
| selector NotFound errors | 50 | 0 |
| spot-implication failures | 106 | ≤ 15 |
| fit CPU (robust, with budget knobs) | 1929 s | ≤ 1150 s (−40%) |
| value CPU | 494 s | ≤ 350 s |
| essvi boards attesting repaired-domain calendar | 0% | ≥ 90% |
| CSV `stage` column populated on every failure | n/a | 100% |

Any missed gate: diagnose from the CSV, fix forward within the relevant task's files, rerun. Do not relax a gate silently — a genuinely unreachable gate gets documented with the measured number and the reason.

- [ ] **Step 4: Append the before/after table + remaining-gaps list (one-sided-quote fitting, event/inverted term structures, global board scheduler, load-phase parallelism) to the deep-dive doc.**

- [ ] **Step 5: Commit** — `docs(atx-vol): universe hardening before/after evidence`.

---

## Self-Review Notes

- Spec coverage: availability (T1-T5), CPU (T6-T8), error handling/logging (T7, T9, T11), curve/calendar semantics (T10), auto-fitter config exposure (T4, T8, T12 flags), validation (T13). The deep-dive's "curve family gaps" item (event/inverted term structures, one-sided sparse fitting) is intentionally OUT of sprint scope — modeling research, recorded as remaining-gaps in T13 Step 4.
- Fixture helper names in test snippets (`make_sparse_single_expiry_chain`, `dense_penny_board_features`, …) must be adapted to the concrete helpers already present in each test file; the behavior asserted is the contract.
- Type consistency: `FitStage` (T9) and `FitTimings` (T11) are consumed by universe_autofit CSV columns referenced in T13's gates; `SelectorResult::scores_evaluated` added in T8 Step 1 is used only by tests/analyzer.
