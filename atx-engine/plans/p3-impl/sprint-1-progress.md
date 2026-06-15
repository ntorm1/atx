# p3 S1 — Real-Data Hardening — Implementation Progress

**Worktree:** `atx-wt/p3-s1` (`C:\Users\natha\OneDrive\Desktop\atx-wt\p3-s1`)
**Branch:** `feat/p3-s1-real-data`
**Base:** `main @ b796a3b` (the `feat/p2-s6-data-layer` merge commit — S5 + S6 already merged)
**Started:** 2026-06-14
**Source plan:** [`sprint-1-real-data-hardening-implementation-plan.md`](sprint-1-real-data-hardening-implementation-plan.md)
**Module ROADMAP:** [`ROADMAP.md`](ROADMAP.md)
**Prior progress:** none (first p3 sprint).

---

## S5/S6 merge-base record (the module's #1 precondition — RESOLVED)

The plan was written when `p2` S5/S6 lived on feature branches and the kickoff directive said "assume done". **They are now genuinely merged to `main`.** No synthetic integration was needed:

- `feat/p2-s5-dl-alphas` (DL sequence alphas) and `feat/p2-s6-data-layer` (BYO-data layer) are both in `main` history.
- The S6 merge commit is `b796a3b` (`Merge branch 'feat/p2-s6-data-layer'`), which is the p3 worktree base.
- The worktree `atx-wt/p3-s1` branches off `main @ b796a3b` via `scripts\new-worktree.ps1 -Name p3-s1 -Branch feat/p3-s1-real-data -Base main`.

The Pattern-B "depends-on-not-yet-merged" risk on the ROADMAP is therefore closed.

## As-built recon (the §0 seams, verified against the base)

Verified present on the worktree base before any unit opened:

- **S6 BYO-data layer** — `atx-engine/include/atx/engine/data/` carries `dataset.hpp`, `dataset_schema.hpp`, `catalog.hpp`, `catalog_report.hpp`, `align.hpp`, `adapt_panel.hpp`, `adapt_feature.hpp`, `adapt_factor.hpp`, `adapt_signal.hpp`, `context.hpp`, `data_handler.hpp`, `factor_model_artifact.hpp` (+ `src/data/*.cpp` for each). The §0.3 contract (`Dataset`/`DatasetSchema`/`DatasetCatalog`/`align`/`adapt_panel`/`adapt_feature`) is satisfied. Exact signatures to be reconciled per-unit by each implementer (trust the code over the plan, per §0).
- **Segment→Panel bridge** — `alpha/segment_panel.hpp`, `alpha/datafields.hpp`, `alpha/panel.hpp` present (§0.2).
- **On-disk real data** — `data/databento/equs_ohlcv_1d_by_date/date=YYYY-MM-DD/part-*.parquet` (from 2024-07-01); `data/us_split_adjustment_factors/security_master/security_master.parquet` + by-symbol partitions; `data/us_security_master_smoke/` smoke copy (§0.5). Present.

## Plan adjustments vs. the source plan

Two adjustments at kickoff; scope otherwise unchanged from the frozen plan.

1. **Merge-base is real, not synthetic** (above). S1-0's "resolve the merge-base" step collapses to "record that `main` already contains S5/S6" — no merge performed in the worktree.
2. **Build preset override: `-DATX_UNITY_BUILD=OFF`.** The `dev` preset sets `ATX_UNITY_BUILD=ON`, which concatenates the test `.cpp` files into Unity TUs. On the current test surface that Unity build does **not** compile — ~20+ ODR collisions (`redefinition of 'frictionless_sim'/'Lcg'/'kSlotSize'/'make_panel'`, "target of using declaration conflicts", "call to 'make_input' is ambiguous") across `parallel_*_test.cpp` / `factory_cost_aware_fitness_test.cpp` etc. This is a **pre-existing** test-hygiene gap (a `chore/unity-test-build` worktree already exists for it), **out of p3-s1 scope**. The per-file build (Unity OFF) is the configuration that is green on `main` (matches the prior `[164/165] Linking` per-file baseline). The canonical build recipe for this sprint is therefore:

   ```powershell
   & 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
   Set-Location C:\Users\natha\OneDrive\Desktop\atx-wt\p3-s1
   cmake --preset dev -DATX_UNITY_BUILD=OFF        # one-time per worktree (done at S1-0)
   cmake --build --preset dev --target atx-shm-worker atx-engine-tests
   ctest --preset dev -R <Suite> --output-on-failure
   ```

   (`vswhere.exe is not recognized` prints during configure/build — harmless noise, exit 0. `atx-shm-worker` must be built for the `Parallel*Process` runtime tests to pass.)

**Realistic scope for this sprint** (unchanged from the plan):

1. **S1-1** — two canonical reference docs (alpha pipeline + data ingestion). Independent; parallel with the chain.
2. **S1-2** — security-master ingestion → typed PIT corporate-action `Dataset`.
3. **S1-3** — split + dividend total-return adjustment, validated vs known AAPL events *(load-bearing)*.
4. **S1-4** — universe / market-cap / ADV / sector PIT membership.
5. **S1-5** — end-to-end real-data Panel (databento ⋈ corp-actions ⋈ universe), digest-pinned *(integration + pin)*.
6. **S1-6** — coverage expansion crawl (independent, non-blocking, `⚠️ partial`-tolerant).

Defer to P4 (already on the ROADMAP backlog): dividend cash-flow simulation, delisted/survivorship recovery, broad GICS, intraday/options/multi-asset, PIT fundamental restatement.

## Baseline (green before S1 code)

Full atx-engine suite on the worktree base, Unity OFF: **`ctest --preset dev` → 1657/1657 atx-engine tests passed** (1659 total minus the 2 `atx-core-tests_NOT_BUILT` / `atx-tsdb-tests_NOT_BUILT` ctest sentinels, which are unbuilt sibling test binaries — not engine failures). This is the green line every per-unit commit must hold.

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| S1-0  | ✅ done | `a47959c` | Marker. Worktree `atx-wt/p3-s1` off `main @ b796a3b`; databento-cpp submodule checked out; `dev` preset configured with `ATX_UNITY_BUILD=OFF`; baseline 1657/0/0 atx-engine tests green; S5/S6 merge-base recorded (real merge, not synthetic); §0 S6 seams + on-disk data verified present. No engine code. |
| S1-1  | ⏳ pending | — | alpha-pipeline-reference.md + data-ingestion-reference.md |
| S1-2  | ✅ done | `60e91a0` | `data/corporate_actions.{hpp,cpp}` + `data_corporate_actions_test.cpp`. Loads `security_master.parquet` into a Reference-role PIT `Dataset` keyed (date×instrument) with the canonical 6 columns (`cum_adj_factor`, `cash_dividend`, `shares_outstanding`, `shares_filed_date`, `gics_sector_code`, `sic_code`). **PIT leak guard** on `shares_filed_date`: `shares_outstanding` is resolved by as-of on filing knowledge-events (greatest `filed_date ≤ d`), so a not-yet-filed share count never appears before its filing date — verified at two boundaries (AAPL 895.8M filed 2009-07-22, and the 2026-05-01 14.687B filing vs the prior-knowable 14.681B). Corporate-action facts (`cum_adj_factor`/`cash_dividend`) joined on event date; missing dividend→0.0, missing sector→`-1` sentinel, missing shares/factor→NaN; non-USD `dividend_currency`→`Err`; symbol interning is first-seen deterministic. **Required an atx-core lift** (`ParquetTable::date32_days` + `null_mask`): the as-built parquet bridge could not read date32 columns or distinguish null numerics — both blockers for reading `date`/`shares_filed_date` and NaN-ing null shares. 6 new tests (the 5 named + a partitioned-loader order test); suite **1663/0/0** atx-engine (1665 total incl. the 2 `*_NOT_BUILT` sentinels). `/W4 /permissive- /WX` + `/fp:precise` clean. |
| S1-3  | ✅ done | `21eae78` | `data/adjust.{hpp,cpp}` + `data_adjust_test.cpp`. Folds split (`cum_adj_factor`) + reinvested `cash_dividend` into a **total-return index** for one symbol: `S_t = raw_close_t · cum_adj_factor_t` (split-adjusted, continuous across splits); `r_t = (S_t + D_t·cum_adj_factor_t)/S_{t-1} − 1`, `r_0 = 0` (additive dividend reinvestment in the numerator — no `1−D/C` negative-factor pathology); `TRI_0 = S_0`, `TRI_t = TRI_{t-1}·(1+r_t)`. **NaN policy:** a NaN raw close OR a NaN `cum_adj_factor` is a **gap** (NaN S/r/TRI, no zero-fill), and the series re-anchors (r=0, TRI=S) at the next valid close — an unknown split factor is **never** defaulted to 1.0 (that would silently un-adjust across an unknown split and fabricate a return); a non-positive/non-finite prior price is also a gap (no divide-by-zero). A NaN dividend is treated as 0.0 (additive, absence = none). **Validation:** event test reads the **real on-disk smoke AAPL** `cum_adj_factor` and asserts no fabricated jump in `S_t` across the 4:1 (2020-08-31) and 7:1 (2014-06-09) split ex-dates (factor steps by exactly the split ratio, cancelling a synthesized inverse-ratio price drop), tolerance **ε_event = 1e-6** (absorbs only the 8-digit on-disk factor rounding; a real split break is a factor-of-N, orders of magnitude larger). **Oracle:** used the documented **hand-fixture fallback** (NOT the `--mode adjclose` web crawl) — 5 real published AAPL ex-date `(prev_close, ex_close, dividend)` triples (databento close + smoke `cash_dividend`, all factor=1.0 post-2020) asserting the engine TRI ratio across each ex-date == the published total-return ratio `(ex+div)/prev` at tolerance **ε_oracle = 1e-9** (exact algebraic match — no float budget needed). **Return-invariance non-leak (§0.7 #2):** rescaling raw close + the price-denominated dividend by an arbitrary constant leaves `total_return` **byte-identical** (asserted via `bit_cast<u64>` equality). 6 new tests (the 6 named); suite **1669/0/0** atx-engine (1671 total incl. the 2 `*_NOT_BUILT` sentinels). `/W4 /permissive- /WX` + `/fp:precise` clean. |
| S1-4  | ✅ done | `76f4fc1` | `data/universe.{hpp,cpp}` + `data_universe_test.cpp`. Derives the four date×instrument universe fields from an **axis-matched** price `Panel` (raw close + volume + datafields) and corp-action `Dataset` (S1-2): **market_cap = shares_outstanding (PIT as-of) × RAW close** (split-invariant — uses raw, never adjusted, close; NaN if either input NaN); **adv_usd** = the Panel's pre-computed causal `adv{adv_window}` datafield when present, else a causal trailing mean of `dollar_volume` (else close×volume) — **no look-ahead** (the rolling window reads only dates ≤ t, byte-identically per the engine's full-window/any-NaN→NaN ts_mean policy); **sector_code** = GICS code if present, else SIC fallback, else sentinel **-1** (never coerced to 0); **in_universe** = `present(t,i) ∧ market_cap ≥ min_mktcap_usd ∧ adv_usd ≥ min_adv_usd ∧ (top-N-by-ADV cap)`, with NaN market_cap/adv failing their floor (so NaN price/shares cells are excluded — the no-survivorship/no-look-ahead guard) and the top-N tie-break = **ascending canonical instrument id** (deterministic). **Alignment contract:** `corp_actions` must already be aligned onto the price-defined canonical axis (same date+instrument count+order; corp index/row ↔ Panel column/date) — S1-5 owns the `align_onto` projection, so `build_universe` is a pure transform; a shape mismatch fails closed with `Err(InvalidArgument)`. **Survivorship bias:** the screen sees only the **listed-only** master's symbols, so a delisted name simply never appears — a survivorship bias that flatters backtests. The unit **documents** it (universe.hpp header doc-comment + this ledger row); the canonical statement lands in `data-ingestion-reference.md` §universe (S1-1) — the `SurvivorshipCaveatDocumentedOrDeferred` test asserts the reference IF the doc exists, else **skips as deferred-to-S1-1** (never a false pass). 6 named tests + the doc-caveat test (skips until S1-1); suite **1675/0/0** atx-engine passing (+1 skipped survivorship test; 1677 total incl. the 2 `*_NOT_BUILT` sentinels). `/W4 /permissive- /WX` + `/fp:precise` clean. |
| S1-5  | ✅ done | `d38f35d` | `data/real_panel.{hpp,cpp}` + `data_real_panel_e2e_test.cpp`. The S1 integration capstone: `build_real_panel(RealDataConfig)` assembles the real-data `alpha::Panel` deterministically from the on-disk databento daily-OHLCV hive ⋈ the S1-2 corporate-action master ⋈ the S1-4 universe, and pins it with a **golden `signal_set_digest`**. **Assembly (fixed order):** (1) `load_security_master` → corp `Dataset` (its first-seen master-file intern order defines the canonical symbol→InstKey map); (2) read the databento parquet(s) over the window, restricted to the corp symbol set, into a Role::Price `Dataset` on the SAME InstKeys (**price defines the canonical axis** §0.7 #3); (3) `price_to_panel` → augmented Panel (`dollar_volume`/`vwap`/`adv{adv_window}`); (4) `align_onto(price, corp)` → corp columns on the price axis, then per symbol `adjust_total_return(raw_close, cum_adj_factor, cash_dividend)` (S1-3) → `total_return_index` (the canonical **`close`**) with `raw_close` retained; (5) `build_universe` (S1-4) over an axis-matched corp Dataset rebuilt from the AlignedView → `market_cap`/`adv`/`sector`/`in_universe`, the mask applied to the Panel; (6) register `price`/`corp_actions`/`universe` in a `DatasetCatalog` + record derivation lineage; `signal_set_digest` over the Panel fields in canonical order → the pin. **Golden digest: `0x2a22a873483d9157`** over the 3-symbol smoke window **[2024-07-01, 2024-08-01)** (22 trading dates); two builds byte-identical. **Spot-check tolerance ε_spot = 1e-6** (AAPL 2024-07-01: `cum_adj_factor`=1.0 + no dividend in-window ⇒ TRI re-anchors at the first date to exactly the raw close 216.75, so `close`==`raw_close`==216.75 — exact f64, tol for safety). **As-built divergences from the frozen plan (followed as-built per the brief §2):** (a) the planned `build_dated_segments`+`attach_segment_panel` seam cannot consume the real databento hive — `build_dated_segments` hard-codes `date=.../data.parquet` but the real hive holds `part-00000.parquet`, AND `load_parquet_scaled` reads each field as **i64 fixed-point × scale** while the real OHLCV columns are **f64 dollars** (the i64 read would fail). So the price Dataset is read **directly via `atx::core::io::read_parquet`** (the same reader `load_parquet` uses) and landed through the SAME `adapt_panel`/`align`/`universe`/`catalog` layer the plan specifies; `seg_cache_dir` is retained in the API for stability but not written (no `.seg` round-trip). (b) `build_augmented_panel` is not an as-built symbol — the augmentation is `price_to_panel`→`with_datafields` (dollar_volume/vwap/adv appended). (c) `build_dated_segments` returns `Status`, not `Result<vector<path>>`, and corp/price InstKey agreement is guaranteed by reusing the master's first-seen intern map (the smoke master interns **IBM=0, AAPL=1, MSFT=2**, NOT alphabetical), since `align_onto` joins by matching InstKey value. 5 named tests (deterministic digest + 2nd-build stability; AAPL adjusted-close spot-check; non-empty + causal universe mask under window truncation byte-identical at date 0; missing-symbol/date sector-sentinel + no-fabricated-close fallback; lineage names price+corp_actions+universe). Suite **1680/0/0** atx-engine passing (+1 pre-existing S1-4 survivorship SKIP; 1682 total incl. the 2 `*_NOT_BUILT` sentinels). `/W4 /permissive- /WX` + `/fp:precise` clean. |
| S1-6  | ⏳ pending | — | coverage crawl + data-ingestion-reference.md §coverage |
| S1-close | ⏳ pending | — | residuals→backlog, status table, `sprint1.md`, `--no-ff` merge |

## p3 S1 sprint commits

| Commit  | Unit | Test counts |
|---------|------|-------------|
| `a47959c` | S1-0 | — (baseline 1657/0/0) |
| `60e91a0` | S1-2 | +6 (suite 1663/0/0; +`date32_days`/`null_mask` atx-core lift) |
| `21eae78` | S1-3 | +6 (suite 1669/0/0; ε_event=1e-6, ε_oracle=1e-9, hand-fixture vs adjclose-oracle) |
| `76f4fc1` | S1-4 | +6 named +1 doc-caveat skip (suite 1675/0/0 passing; survivorship caveat documented; tie-break = canonical id) |
| `d38f35d` | S1-5 | +5 named (suite 1680/0/0 passing; golden digest `0x2a22a873483d9157`; window [2024-07-01,2024-08-01); ε_spot=1e-6; read databento parquet directly — as-built divergence from build_dated_segments) |

## What S1 proves / Next sprint priorities

*(written at S1-close — the baton to S2.)*
