# Production Vola Parity Push — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive production surface quality to parity targets across the full
616-name universe — cells that fit, reprice their listed chains in-band, and
never serve fabricated tenors — by combining the month-1 autopsy findings with
the capabilities that just merged from `feat/vol-sbd-integration` (f3a5bf2).

**Architecture:** Measure-first sprint. Task 0 re-baselines month-1 on the
merged code (the integration lanes already changed the fit path: T3c board
carry on the eSSVI lane, T6 one-sided quote admission, D4 dof honesty, T8
deep-ITM refusal). Then two production defect fixes (panel tenor gate; pooled
board carry), one admission reform (spread-normalized quality), and a final
re-measure against pre-registered numeric gates. Every carry change runs the
`opra_parity_bench CARRY_MODE` A/B harness kept by `547a466` — the sprint
inherits that revert's discipline: coverage gains must show quality
discrimination or they revert.

**Tech Stack:** C++20 (atx-vol), Andersen–Lake American pricing, eSSVI
production selector, `surface-db` V2 archives, Python 3.12 audit scripts.

**Spec:** `atx-vol/research/2026-08-13-surface-library-deep-dive-sota-roadmap.md`
(W1–W8) + the carried-forward list in
`docs/superpowers/plans/2026-08-09-atx-vol-surface-breadth-depth.md` §1.11.

## Global Constraints

- Tier-A governance: any new/changed public header in `atx-vol/include/atx/vol/`
  bumps the `vol_umbrella_test.cpp` literal AND the README Versioning table in
  the same commit (currently 45).
- Carry-change discipline (`547a466`): any change that admits previously
  refused carry MUST run `opra_parity_bench CARRY_MODE=default|risk` on both
  corpora (lqbench 240 / sp100 104) with the acceptance test pre-registered in
  this plan BEFORE the run. Fail → revert, record, stop.
- Production fit path only (`--fit-path production`); populate preset;
  `--snap-et 15:55`; rates from `atx-vol/data/rates/us_3m_monthly.csv`.
- No gate-threshold loosening on the rate-unit carry confidence gate
  (`max_carry_dispersion`, `max_carry_leave_one_out`,
  `min_confident_borrow_pairs` stay at defaults). New coverage must come from
  new estimators/information sets, not wider gates.
- Month-1 window = 2025-08-11..2025-08-29, 611 fit-eligible symbols, hive
  `C:/atx-data/opra-hive`. Baseline DB `C:/atx-data/surface-db/xsec-2025`
  (pre-merge binaries) is FROZEN as the comparison object — post-merge refits
  go to `C:/atx-data/surface-db/xsec-pp-2025`.

## Pre-registered gates (record pass/fail against each at sprint close)

| Gate | Metric | Baseline (pre-merge binaries) | Target |
|---|---|---|---|
| G0 | month-1 stored cells, merged code | 93.6% (8,576/9,165) | measured; every delta vs baseline attributed to a lane |
| G1 | panel rows priced on extrapolated tenor | 26.7% of cells eligible, unflagged | **0** (refused or flagged; test-enforced) |
| G2 | SP100 pilot IC on gated labels | ridge +0.139 (NW t 5.09) on contaminated labels | re-measured; contamination delta reported (measurement gate, no floor) |
| G3 | pooled-carry A/B | — | admitted-cohort de-Am round-trip median ≤ 1.25× confident-cohort median on BOTH corpora, zero new oracle violations; else revert |
| G4 | month-1 failed cells | 6.4% (589) | ≤ 2.5%, attributed per family |
| G5 | band-audit, cheap tier (25-name sample, same symbols) | 0.819 quote-wtd in-band; 10.4% slices < 0.35 | ≥ 0.86; ≤ 5% — with liquid tier not degrading below 0.89 |
| G6 | per-cell quality queryable | band-audit repricing pass required | `surface-db info/verify` reads stored per-cell quality without repricing |

---

### Task 0: Re-baseline month-1 on merged main

The integration sprint measured its gains on lqbench/sp100 corpora. Nobody has
yet measured what T3c + T6 + T5c + D4 + T8 do to the PRODUCTION xsec month.
This number decides how much of Tasks 3–4 is still needed.

**Files:**
- No source changes. Build: `build-rel/` reconfigured at `f3a5bf2`.
- Output: `C:/atx-data/surface-db/xsec-pp-2025`, logs `C:/atx-data/logs/xsec-pp-fit/`.

**Interfaces:**
- Produces: `xsec-pp-2025` DB root + `build_*.csv` reports — Tasks 5–7 audit
  this root, not the frozen baseline.

- [ ] **Step 1: Rebuild Release at the merge**

```bash
cmake --build build-rel --config Release --target atx-vol-surface-db-build atx-vol-surface-db atx-vol-vega-panel -j 14
```

Expected: clean build. If `vol_umbrella_test` literal drifted in the merge,
stop and reconcile before anything else.

- [ ] **Step 2: Run the unit suites once (sanity, not a full qual pass)**

```bash
ctest --test-dir build-rel -R "vol" --output-on-failure -j 8
```

Expected: green except the documented pre-existing `SurfaceDbPopulate` red
(sbd plan §1.10). Any OTHER red stops the sprint here.

- [ ] **Step 3: Refit month-1 into the new prefix**

```bash
python atx-vol/tools/run_surface_db_backfill.py \
  --universe atx-vol/data/universe/xsec_2026-08.csv \
  --hive C:/atx-data/opra-hive \
  --db-prefix C:/atx-data/surface-db/xsec-pp \
  --from 2025-08-11 --to 2025-08-31 \
  --phase build --snap-et 15:55 \
  --rates atx-vol/data/rates/us_3m_monthly.csv \
  --build-exe build-rel/bin/atx-vol-surface-db-build.exe \
  --admin-exe build-rel/bin/atx-vol-surface-db.exe \
  --chunk-sessions 6 --fit-workers 0 --index SPY \
  --log-dir C:/atx-data/logs/xsec-pp-fit
```

Expected: ~20 min. Record `cells_ok`, `cells_failed`, per-family failure
counts from the report CSVs (same parse as the autopsy:
`len(r)==4 and r[0][:4]=='2025'` rows; families = no-usable-slice /
inversion-failed / quality-floor / symbol-missing).

- [ ] **Step 4: Audit battery on the new root**

```bash
./build-rel/bin/atx-vol-surface-db.exe tenor-audit --db C:/atx-data/surface-db/xsec-pp-2025 > C:/atx-data/logs/xsec-pp-fit/tenor_audit.tsv 2>&1
for s in $(cat /c/atx-data/universe/band_sample25.txt); do
  ./build-rel/bin/atx-vol-surface-db.exe band-audit --db C:/atx-data/surface-db/xsec-pp-2025 \
    --hive C:/atx-data/opra-hive --from 2025-08-11 --to 2025-08-29 --r 0.043 --symbol $s 2>&1 \
    | tail -n +2 >> C:/atx-data/logs/xsec-pp-fit/band_sample25.tsv
done
```

Expected: same 25-name stratified sample as the autopsy so tiers compare
1:1. Compute the G0/G4/G5 columns and the delta table vs baseline
(fill %, per-family fails, tier in-band, truncation classes).

- [ ] **Step 5: Record G0 in the plan's execution log and commit**

```bash
git add docs/superpowers/plans/2026-08-14-vola-production-parity-push.md
git commit -m "docs(vol): parity push task 0 — month-1 re-baseline on merged main"
```

---

### Task 1: Vega-panel tenor-domain gate (W1)

The panel prices 1y strangles on flat-extrapolated smiles for every truncated
cell — the library's own comment calls those values fabricated
(`analytics_aggregate.cpp:85`). The backtester refuses this
(`backtest.cpp:1351`); the panel must too, on BOTH sides: entry resolution and
every daily mark (legs are pinned to absolute expiry, so a mark surface with a
shorter domain than the remaining leg tenor is also fabrication).

**Files:**
- Modify: `atx-vol/include/atx/vol/vega_panel.hpp` (row schema + doc)
- Modify: `atx-vol/src/vega_panel.cpp` (`resolve_atmf_strangle` ~line 206;
  the mark loop ~line 396; `push` entry creation ~line 480)
- Modify: `atx-vol/tests/vega_panel_test.cpp`
- Modify: `atx-vol/tests/vol_umbrella_test.cpp` (45 → 46) + `atx-vol/README.md`
  Versioning table, same commit.

**Interfaces:**
- Consumes: `PricedSurface::extrapolates_tenor(double T)` and
  `tenor_domain().max_T` (`priced_surface.hpp:451-455`), already public.
- Produces: `VegaPanelRow` gains `double entry_max_T{nan}` (numeric column
  `entry_max_T` appended to the NumCol table at `vega_panel.cpp:~200`);
  entry refusal reason string `"resolve_atmf_strangle: target_T outside fitted
  tenor domain"`; marks beyond domain mark the pending entry failed (soft,
  `label_valid == 0`), never fabricated. Task 2's re-run consumes the column.

- [ ] **Step 1: Write the failing tests**

In `vega_panel_test.cpp`, reuse the existing synthetic-surface fixture the
current tests build (`PricedSurface` via the same helper the resolve tests
use) but cap its longest slice at T = 0.5:

```cpp
TEST(VegaPanel, EntryRefusesExtrapolatedTenor) {
  auto surf = make_fixture_surface(/*max_T=*/0.5);   // existing helper, shorter board
  auto rs = resolve_atmf_strangle(surf, /*target_T=*/1.0, 0.30, 1000.0, +1);
  ASSERT_FALSE(rs.has_value());
  EXPECT_EQ(rs.error().code, ErrorCode::InvalidArgument);
  EXPECT_NE(rs.error().message.find("tenor domain"), std::string::npos);
}

TEST(VegaPanel, MarkBeyondDomainFailsSoftNotFabricated) {
  VegaPanelBuilder b({.tenor_T = 0.4, .horizon_sessions = 2}, "TT");
  // day 1: surface covers 0.5y — entry resolves (0.4 < 0.5)
  ASSERT_TRUE(b.push("2025-01-02", make_fixture_surface(0.5)).has_value());
  // day 2: surface truncated to 0.2y — remaining leg tenor ~0.396 is OUTSIDE
  ASSERT_TRUE(b.push("2025-01-03", make_fixture_surface(0.2)).has_value());
  auto done = b.push("2025-01-06", make_fixture_surface(0.5));
  ASSERT_TRUE(done.has_value());
  ASSERT_TRUE(done->has_value());
  EXPECT_FALSE((*done)->label_valid);      // failed soft — no fabricated mark
  EXPECT_EQ(b.skipped(), 1u);
}

TEST(VegaPanel, RowCarriesEntryMaxT) {
  VegaPanelBuilder b({.tenor_T = 0.3, .horizon_sessions = 1}, "TT");
  (void)b.push("2025-01-02", make_fixture_surface(0.5));
  auto rows = b.finish();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NEAR(rows[0].entry_max_T, 0.5, 1e-12);
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `ctest --test-dir build-rel -R vega_panel --output-on-failure`
Expected: FAIL — no `entry_max_T` member, no refusal message.

- [ ] **Step 3: Implement**

In `resolve_atmf_strangle` after the argument checks (~line 221):

```cpp
if (entry.extrapolates_tenor(target_T)) {
  return Err(ErrorCode::InvalidArgument,
             "resolve_atmf_strangle: target_T outside fitted tenor domain (max_T=" +
                 std::to_string(entry.tenor_domain().max_T) + ")");
}
```

In the mark loop (~line 396), before valuing a pending entry against `surf`,
compute the remaining leg tenor from the pinned expiry and refuse fabricated
marks the same soft way an inversion failure is handled today:

```cpp
const double T_rem = static_cast<double>(p.strangle->legs.front().expiry_ts - ts) / kNsPerYear;
if (surf.extrapolates_tenor(T_rem)) {
  p.failed = true;
  ++skipped_;
  continue;
}
```

Add `double entry_max_T{std::numeric_limits<double>::quiet_NaN()};` to
`VegaPanelRow`, set it at entry creation
(`row.entry_max_T = surf.tenor_domain().max_T;`), append
`{"entry_max_T", &VegaPanelRow::entry_max_T}` to the NumCol table.

- [ ] **Step 4: Run tests — all vega_panel green, then full vol suite**

Run: `ctest --test-dir build-rel -R "vega_panel|vol_umbrella" --output-on-failure`
Expected: vega_panel green; umbrella RED on the version literal — bump 45 → 46
and update the README table, rerun, green.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/vega_panel.hpp atx-vol/src/vega_panel.cpp \
        atx-vol/tests/vega_panel_test.cpp atx-vol/tests/vol_umbrella_test.cpp atx-vol/README.md
git commit -m "fix(vol): vega panel refuses extrapolated tenors on entry and mark (W1)"
```

---

### Task 2: Quantify pilot contamination, re-run gated SP100 panel

**Files:**
- No source changes. Rebuild `atx-vol-vega-panel` (done in Task 1's build).
- Output: `C:/atx-data/structure-panel/sp100_vega_panel_gated.tsv`,
  trainer outputs tagged `sp100_gated` in `C:/atx-data/backtests/xsec-vega/`.

**Interfaces:**
- Consumes: Task 1's gated binary.
- Produces: G1/G2 numbers for the execution log; the gated panel becomes the
  reference panel for any future trainer work.

- [ ] **Step 1: Re-run the SP100 panel with the gated tool**

```bash
./build-rel/bin/atx-vol-vega-panel.exe --out C:/atx-data/structure-panel/sp100_vega_panel_gated.tsv \
  --db-root C:/atx-data/surface-db/sp100-2025 --db-root C:/atx-data/surface-db/sp100-2026 \
  --universe C:/atx-data/universe/sp100_plain.txt \
  --tenor-years 1 --abs-delta 0.30 --vega-target 1000 --horizon 21
```

Expected: fewer rows than the 24,206 baseline (gated entries) and higher
`skipped()` per symbol. Record row delta = contamination extent.

- [ ] **Step 2: Re-train and compare**

```bash
python atx-vol/scripts/xsec_vega_train.py --panel C:/atx-data/structure-panel/sp100_vega_panel_gated.tsv \
  --out-dir C:/atx-data/backtests/xsec-vega --model ridge --tag sp100_gated --feature-ics
python atx-vol/scripts/xsec_vega_train.py --panel C:/atx-data/structure-panel/sp100_vega_panel_gated.tsv \
  --out-dir C:/atx-data/backtests/xsec-vega --model hgb --tag sp100_gated_hgb
```

Expected: IC/decile means comparable or better (contaminated labels were
noise, not signal). Record old-vs-new IC, top-decile mean, row counts. G2 is a
measurement gate — any outcome passes if measured and recorded.

- [ ] **Step 3: Update the xsec research doc status log + commit**

```bash
git add atx-vol/research/2026-08-12-xsec-vega-portfolio-ml.md
git commit -m "docs(vol): SP100 pilot re-measured on tenor-gated labels (W1 verification)"
```

---

### Task 3: Pooled board-level carry estimator (W2, estimator only)

Per-expiry carry solves on 1–3 wide pairs are ill-posed on low-priced names —
one tick on a $2 stock at 30d is ~6 rate points of borrow noise. The merged
T3c fallback interpolates OTHER expiries' confident solves; on boards where NO
expiry is confident (the FS-1 class, 140 cells month-1) it has nothing to
interpolate. This task builds the estimator that pools ALL pairs across ALL
expiries into one robust borrow curve. Wiring comes in Task 4 — this task is a
pure function with unit tests.

**Files:**
- Modify: `atx-vol/include/atx/vol/deamer.hpp` (new struct + function decl,
  next to `resolve_chain_forward` ~line 341)
- Modify: `atx-vol/src/deamer.cpp` (implementation, reusing
  `imply_term_borrow_from_base` ~line 289 and the pair-selection logic behind
  `carry_pair_strikes` ~line 379 of the header)
- Test: `atx-vol/tests/deamer_test.cpp`
- Same-commit: `vol_umbrella_test.cpp` 46 → 47 + README table (public header).

**Interfaces:**
- Consumes: `imply_term_borrow_from_base` (existing per-pair borrow solve),
  `carry_pair_strikes(chain, S, opts)` (existing eligible-pair selection),
  `robust_location` (existing Huber, `deamer.cpp:419`).
- Produces (Task 4 wires this):

```cpp
struct BoardCarry {
  double borrow_1y{0.0};        // pooled level at the 1y pillar
  double decay{0.0};            // borrow(T) = borrow_1y + decay * (1/max(T,T_floor) - 1)
  std::size_t n_pairs{0};       // pairs entering the pooled regression
  std::size_t n_expiries{0};    // expiries contributing >= 1 pair
  double weighted_dispersion{0.0}; // weighted MAD of pair residuals, rate units
  [[nodiscard]] double at(double T) const noexcept;
};

// Pools every eligible co-terminal pair on the board into ONE robust
// weighted borrow-curve fit. Weight per pair = 1 / max(rel_spread, tick/S)^2
// * S * T  (the noise model: a tick of C-P quantization is ~tick/(S*T) in
// rate units). Ridge toward borrow == 0 with lambda = lambda0 / n_pairs.
// Err(Unavailable) only when zero pairs are quotable board-wide.
[[nodiscard]] atx::core::Result<BoardCarry>
resolve_board_carry(std::span<const Chain> chains, double S, double r,
                    std::span<const DividendEvent> cash_divs,
                    std::int64_t now_ts_ns, const DeAmOptions &opts) noexcept;
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(BoardCarry, PoolsAcrossExpiriesOnSparseBoard) {
  // 4 synthetic chains, 2 quotable pairs each, all priced with borrow = 0.03
  // via the same American pricer the solver inverts (fixture builds chains
  // from AL prices, as the existing resolve_chain_forward tests do).
  auto board = make_synthetic_board(/*S=*/50.0, /*borrow=*/0.03, /*pairs_per_expiry=*/2,
                                    /*n_expiries=*/4);
  auto bc = resolve_board_carry(board.chains, 50.0, 0.04, {}, board.now_ts, DeAmOptions{});
  ASSERT_TRUE(bc.has_value());
  EXPECT_EQ(bc->n_pairs, 8u);
  EXPECT_NEAR(bc->at(1.0), 0.03, 5e-3);   // 8 pooled pairs recover the level
}

TEST(BoardCarry, TickNoiseOnCheapStockStaysBounded) {
  // $2 stock, quotes rounded to $0.01 — per-expiry solves fail the confidence
  // gate (this is FS-1); the pooled fit must still land within 2 rate points.
  auto board = make_synthetic_board(/*S=*/2.0, /*borrow=*/0.00, 2, 6,
                                    /*tick_round=*/0.01);
  auto bc = resolve_board_carry(board.chains, 2.0, 0.04, {}, board.now_ts, DeAmOptions{});
  ASSERT_TRUE(bc.has_value());
  EXPECT_LT(std::abs(bc->at(0.25)), 0.02);
  EXPECT_LT(std::abs(bc->at(1.0)), 0.02);
}

TEST(BoardCarry, ZeroQuotablePairsIsUnavailable) {
  auto board = make_synthetic_board(2.0, 0.0, /*pairs_per_expiry=*/0, 4);
  auto bc = resolve_board_carry(board.chains, 2.0, 0.04, {}, board.now_ts, DeAmOptions{});
  ASSERT_FALSE(bc.has_value());
  EXPECT_EQ(bc.error().code, ErrorCode::Unavailable);
}
```

- [ ] **Step 2: Run to verify failure** — `ctest --test-dir build-rel -R deamer`
Expected: FAIL, `resolve_board_carry` undeclared.

- [ ] **Step 3: Implement**

Per pair (expiry i, strike j): borrow_ij from `imply_term_borrow_from_base`
(existing nested fixed point, unchanged tolerances). Regression: weighted
Huber (reuse `robust_location`'s scale machinery) of borrow_ij on the basis
{1, (1/max(T_i, 0.08) − 1)} with weights `w_ij = S * T_i / max(rel_spread_ij,
0.01/S)^2`, ridge `lambda = 0.5 / n_pairs` toward (0, 0). Two Huber
reweighting sweeps, cutoff 5·scale — same constants as `robust_location`.
`weighted_dispersion` = weighted MAD of final residuals.

- [ ] **Step 4: Run tests — green.** Also rerun the umbrella (46 → 47 bump).

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/deamer.hpp atx-vol/src/deamer.cpp \
        atx-vol/tests/deamer_test.cpp atx-vol/tests/vol_umbrella_test.cpp atx-vol/README.md
git commit -m "feat(vol): pooled board-level robust borrow curve (W2, estimator)"
```

---

### Task 4: Wire pooled carry as the fallback anchor + pre-registered A/B

The merged T3c plumbing (`CarryAnchor`/`CarryFallback` in
`surface_parity.cpp`) currently interpolates confident per-expiry solves.
This task adds the pooled estimate as the anchor of LAST resort — used only
when T3c has no confident expiry to interpolate from (the FS-1 class), stamped
with its own provenance so nothing pooled masquerades as measured.

**Files:**
- Modify: `atx-vol/src/surface_parity.cpp` (the T3c fallback site — where
  `term_structure_fallback_borrow` resolves; on empty anchor set, call
  `resolve_board_carry` and use `bc.at(T_i)` per expiry via the existing
  `imply_borrow = false` / `borrow_fixed` injection path)
- Modify: `atx-vol/include/atx/vol/session.hpp` diagnostics: new counter
  `n_pooled_carry_expiries` beside `n_carry_fallback_expiries`, merged onto
  the same `CarryGap` degraded bit (never a clean publish).
- Test: `atx-vol/tests/surface_parity_test.cpp` — a board fixture where every
  expiry fails the confidence gate must now fit with
  `n_pooled_carry_expiries == n_chains` and `CarryGap` set; a board with one
  confident expiry must still prefer T3c interpolation (`n_pooled == 0`).

**Interfaces:**
- Consumes: Task 3's `resolve_board_carry`; existing T3c
  `CarryAnchor`/`CarryFallback` structs; `DeAmOptions.imply_borrow/borrow_fixed`.
- Produces: FS-1 boards fit with pooled carry, flagged `CarryGap`; the A/B
  verdict recorded in this plan.

- [ ] **Step 1: Write the two failing fixture tests** (as specified above,
  same fixture style as the existing T3c tests in `surface_parity_test.cpp`).

- [ ] **Step 2: Run — FAIL** (`n_pooled_carry_expiries` doesn't exist).

- [ ] **Step 3: Implement the wiring** (order of precedence in the fallback
  site: own confident solve → T3c interpolation from confident anchors →
  pooled board curve → drop expiry. Pooled path requires
  `bc.n_pairs >= opts.min_confident_borrow_pairs` board-wide — the same floor,
  applied to the pooled information set, NOT loosened).

- [ ] **Step 4: Tests green; full vol suite green.**

- [ ] **Step 5: PRE-REGISTERED A/B (the 547a466 harness).** Acceptance test,
  registered here before the run: on both corpora, the cohort of expiries
  admitted ONLY via pooled carry must have de-Am round-trip median ≤ 1.25× the
  confident-cohort median, AND zero new independent-oracle violations, AND
  byte-identical output on every board with zero pooled expiries. Run:

```bash
build-rel/bin/opra_parity_bench.exe CARRY_MODE=risk --corpus lqbench --out C:/atx-data/logs/parity-push/ab_lqbench.csv
build-rel/bin/opra_parity_bench.exe CARRY_MODE=risk --corpus sp100  --out C:/atx-data/logs/parity-push/ab_sp100.csv
```

Expected: pass → keep. Fail → `git revert`, record the numbers in the
execution log, stop Task 4 (Task 5+ proceed regardless).

- [ ] **Step 6: Commit**

```bash
git add atx-vol/src/surface_parity.cpp atx-vol/include/atx/vol/session.hpp atx-vol/tests/surface_parity_test.cpp
git commit -m "feat(vol): pooled board carry as last-resort anchor, CarryGap-flagged (W2)"
```

---

### Task 5: Spread-normalized admission floor for cheap names (W5)

`worst_frac_within_bidask >= 0.35` is an absolute yardstick: on $2–10 names
NBBO width in vol terms is enormous and the floor refuses fits as good as the
data allows (10.4% of cheap-tier slices vs 0.3% liquid). Klassen's production
answer: χ²/dof against per-quote error bars derived from spreads. The library
already computes `reduced_chi_square` per slice (D4 made its dof honest) — this
task adds it as an ALTERNATIVE admission route, not a replacement.

**Files:**
- Modify: `atx-vol/src/fit_policy.cpp` (`evaluate_surface_admission`,
  QualityBelowFloor site ~line 173)
- Modify: `atx-vol/include/atx/vol/fit_policy.hpp` (`FitAdmissionPolicy` gains
  `double max_admission_chi2_per_dof{0.0}` — 0 = route disabled, exact current
  behavior)
- Modify: `atx-vol/tools/include/atx/vol/tools/surface_db_populate.hpp`
  (`populate_admission_policy()` ~line 63: enable the route with 1.5 for the
  populate preset)
- Test: `atx-vol/tests/fit_policy_test.cpp`

**Interfaces:**
- Consumes: per-slice `mean_chi2` already in `SurfaceParityReport` (lane B).
- Produces: admission passes when `worst_frac_within_bidask >= floor` OR
  (`max_admission_chi2_per_dof > 0` AND every fitted slice's spread-normalized
  χ²/dof ≤ the cap). Both-route failures still reject.

- [ ] **Step 1: Failing test:** a board whose worst slice reprices 20%
  in-band on quotes 8 half-spreads wide but with χ²/dof 0.9 must ADMIT under
  `{min_worst_frac_within_bidask=0.35, max_admission_chi2_per_dof=1.5}` and
  REJECT under `{…, 0.0}`; a board with χ²/dof 40 (the 2025-04-10 shape) must
  reject under both.

- [ ] **Step 2: Run — FAIL.** Field doesn't exist.

- [ ] **Step 3: Implement** (the OR-route in `evaluate_surface_admission`;
  thread the new field through `populate_admission_policy`).

- [ ] **Step 4: Green; umbrella bump 47 → 48 (public header) + README.**

- [ ] **Step 5: Commit**

```bash
git add atx-vol/src/fit_policy.cpp atx-vol/include/atx/vol/fit_policy.hpp \
        atx-vol/tools/include/atx/vol/tools/surface_db_populate.hpp \
        atx-vol/tests/fit_policy_test.cpp atx-vol/tests/vol_umbrella_test.cpp atx-vol/README.md
git commit -m "feat(vol): spread-normalized chi2/dof admission route for wide-quote boards (W5)"
```

---### Task 6: Persist per-cell quality in the DB (G6)

Band-audit-grade accuracy currently requires a full repricing pass. Store the
three quality numbers computed at fit time so `verify`/`info` answer without
repricing.

**Files:**
- Modify: `atx-vol/tools/include/atx/vol/tools/surface_db_populate.hpp` +
  populate `.cpp` (write `in_band_frac`, `worst_slice_in_band`, `chi2_per_dof`
  into the cell's stored fit provenance — same mechanism as the stored fit
  config `config --symbol` already reads)
- Modify: `atx-vol/tools/surface_db_main.cpp` (`verify` prints the stored
  triple per cell; new `--quality-floor X` flag fails verify on stored
  `worst_slice_in_band < X`)
- Test: extend `atx-vol/tests/surface_db_populate_test.cpp` round-trip: fit a
  cell, reopen the DB, assert the stored triple equals the build report's.

**Interfaces:**
- Consumes: `ParityReport::frac_fv_within_bidask` (already computed per slice
  at `surface_parity.cpp:589`), Task 5's χ²/dof.
- Produces: `atx-vol-surface-db verify --quality-floor 0.35` works on
  `xsec-pp-2025` with zero repricing.

Steps: failing round-trip test → implement → green → commit
(`feat(vol): persist per-cell fit quality in the surface archive (G6)`).

---

### Task 7: Final re-measure — the parity scorecard

**Files:** no source changes. Outputs: refit `xsec-pp-2025` (delete and refit
— Tasks 4–6 changed fit behavior), audit battery, execution-log tables.

- [ ] **Step 1: Refit month-1** (same command as Task 0 Step 3; the orchestrator
  skips nothing since the root is fresh after `rm -rf C:/atx-data/surface-db/xsec-pp-2025`).
- [ ] **Step 2: Audit battery** (Task 0 Step 4 commands, verbatim).
- [ ] **Step 3: Score every gate G0–G6** in the table at the top; attribute
  every delta (fill by family, tier in-band, truncation classes — truncation
  should NOT move; it is data-side, and if it does move, investigate before
  celebrating).
- [ ] **Step 4: Re-run the gated SP100 pilot trainer IF Task 4/5 changed any
  sp100 surface** (they fit from the same code path; check `verify` digests —
  if unchanged, record that instead).
- [ ] **Step 5: Update roadmap doc + this plan's execution log; commit.**

```bash
git add atx-vol/research/2026-08-13-surface-library-deep-dive-sota-roadmap.md \
        docs/superpowers/plans/2026-08-14-vola-production-parity-push.md
git commit -m "docs(vol): parity push scorecard — gates G0-G6 measured"
```

---

### Task 8 (stretch, pre-registered before any code): Mingone global refit rung (W4)

Only if G4 misses its ≤2.5% target from Tasks 3–5 alone. Design is registered
now so the attempt is disciplined: new `essvi_global_refit(board)` in the
box-parametrized domain (ρᵢ, θ₁, aᵢ = θᵢ−θᵢ₋₁pᵢ, cᵢ ∈ (0,1)) per Mingone
(arXiv 2204.00312), warm-started from surviving slices, bounded trust-region
LSQ ≤ 500 evals, inserted as a rung between the fallback ladder and
strict-recovery in `pricer_fitter.cpp:1670`. Pre-registered gate: admitted-by-
refit cells must clear the SAME admission policy as first-pass fits (no
special-casing), zero oracle violations, and byte-identical output on boards
where the rung never fires. If the sprint reaches this task, split it into its
own plan first — it is a week on its own.

### Task 9 (parallel, investigation): InversionResidual root cause

Carried forward from sbd §1.11: 73 lqbench / 9 sp100 boards flag
`InversionResidual` and nobody knows why yet. Investigation task, no code:
sample 10 flagged boards, dump per-row audit residuals
(`max_iv_residual_half_spreads` exceedances vs `n_audited` accounting), decide
whether the residual budget, the shared-boundary batch accounting, or genuine
quote pathology dominates. Output: a finding memo in the execution log +
next-sprint recommendation. Can run any time after Task 0's rebuild.

---

## Explicitly out of scope (inherited, not lost)

- **W6 shrinkage ladder** (cross-expiry/temporal/cross-name priors): next
  sprint — it needs Task 6's stored quality metrics as its measurement base.
- **W7 production-book tenor policy** (min(1y, longest-listed) label): the
  research-run half is settled by Task 1's gate; the book definition waits for
  the full-corpus panel.
- **W8 speed work**: only the free fail-fast noted in the roadmap; nothing else.
- Carried forward from sbd §1.11, still owed: `universe_autofit` CSV export
  one-liner (gate 6), the 416-board control corpus re-run (gate 4's scope),
  the `corpus.cpp:322` sampling-distribution study, shape (c) threshold
  pre-registration, gate 3 tier-D number.

## Execution log

(append per task: date, measured numbers, gate verdicts, surprises)

## Sequencing

```
Task 0 ──► Task 1 ──► Task 2
   │
   ├─────► Task 3 ──► Task 4 (A/B gate)
   │                     │
   ├─────► Task 5 ──► Task 6
   │                     │
   └────────►────────► Task 7 (scorecard) ──► [Task 8 iff G4 missed]
Task 9 runs parallel any time after Task 0.
```

Tasks 1–2 and 3–4 and 5–6 are independent chains; parallelize across
worktrees if desired (the sbd sprint's 3-lane pattern applies).
