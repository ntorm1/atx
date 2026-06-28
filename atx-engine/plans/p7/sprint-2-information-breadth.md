# Sprint 2 — Information Breadth

**Goal:** break the price/return monoculture that p6 research identified as the
dominant root cause of edge poverty. Add three signal families — FINRA
short-interest, IV-surface, and liquidity — as first-class augmented panel
fields, then extend the seed catalog so the GA and discover stage have real
information structure to draw on across all four families (price, IV, liquidity,
short-interest).

**Owns (exclusive):**
- `atx-engine/include/atx/engine/data/finra_short.hpp` (from track-b landing)
- `atx-engine/src/data/finra_short.cpp` (from track-b landing)
- `atx-engine/tests/data/finra_short_test.cpp` (from track-b landing)
- `atx-impl/src/stage_augment.cpp` / `stage_augment.hpp` (from track-b landing)
- `atx-impl/tests/augment_test.cpp` (from track-b landing)
- `atx-engine/include/atx/engine/alpha/datafields.hpp` — new derived families
- `atx-engine/include/atx/engine/alpha/augment.hpp` — new `with_iv_fields` /
  `with_liquidity_fields` entry points
- `atx-impl/tests/fixtures/iv_earnings_templates.txt` (from track-b landing)
- `atx-impl/tests/fixtures/neutralized_templates.txt` (from track-b landing)
- NEW `atx-impl/tests/fixtures/short_interest_seeds.txt`
- NEW `atx-impl/tests/fixtures/liquidity_seeds.txt`
- NEW `atx-engine/tests/alpha/multi_family_seeds_test.cpp`
- NEW `atx-engine/tests/alpha/iv_fields_test.cpp`
- NEW `atx-engine/tests/alpha/liquidity_fields_test.cpp`
- `atx-impl/tests/seed_parse_test.cpp` (extend for new families)

**Must NOT touch:**
- `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp` (S7 CLI hub)
- `atx-impl/src/stage_discover.cpp`, `atx-impl/src/stage_run.cpp` (S7)
- `atx-engine/tests/factory/oracle.hpp` (frozen by all sprints)
- `combine/`, `eval/`, `factory/`, `loop/` paths — outside S2 scope
- `atx-impl/tests/fixtures/alpha101.txt` (frozen oracle; new catalog goes in
  separate files)

**Determinism contract: ADDITIVE / opt-in (identical to p6 S5 contract).**
The no-flag panel build path (`with_alpha101_fields`, `with_datafields`) is
byte-identical before and after this sprint. The three new families
(`with_iv_fields`, `with_liquidity_fields`, `augment_panel_with_finra`) appear
**only when explicitly requested** by the caller; they append at the END of the
field list and never renumber existing FieldIds. The serialization digest of a
panel built without the new families is unchanged. Pinned goldens unchanged.

---

## Implementation quality standard (paste into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear
module-level intent, grouped constants/types/APIs, explicit ownership and
lifecycle rules, named error contracts, and concise comments that explain
invariants, non-obvious control flow, or domain semantics. Do not follow
weaker patterns that expose constants/structs/prototypes without enough API
contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not
done until the public API, implementation, tests, docs/ledger row, and
build/test gate are complete. Do not leave TODO placeholders, fake success
paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership,
ordering, crash/recovery semantics, and tricky domain rules. Do not comment
obvious assignments or wrap every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and
  lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new
  abstractions.
```

---

## Background: what the track-b branch actually contains

Branch `worktree-track-b-information-structure` (HEAD `8df5010`) adds **1 410
lines** across 12 files — a complete, CLI-wired FINRA short-interest pipeline:

| File | What it contains |
|---|---|
| `atx-engine/include/atx/engine/data/finra_short.hpp` | `FinraFeatures` struct + `load_finra_features` (axis-parametric, causal loader) + constants `kFinraFieldDtc/Util/Chg`, `kFinraDefaultPublicationLagDays=10` |
| `atx-engine/src/data/finra_short.cpp` | 323-line parquet reader; causality placement; forward-fill; `si_util` from `market_cap/raw_close` shares with ADV fallback |
| `atx-engine/tests/data/finra_short_test.cpp` | 4 tests (`FinraShort.*`): `CausalityNoLookAhead`, `DerivedFieldMath`, `UtilAdvFallback`, `MissingCoverageStaysNaN` |
| `atx-impl/src/stage_augment.{hpp,cpp}` | `augment_panel_with_finra` (pure core) + `run_augment` (CLI stage); axis reconstructed from ORATS seg partition + `_symbology.parquet` |
| `atx-impl/tests/augment_test.cpp` | 4 tests (`Augment.*`): `AppendsThreeFieldsAtEnd`, `CausalityInPanel`, `RoundTripDigestMatches`, `UtilFromPanelShares` |
| `atx-impl/tests/fixtures/iv_earnings_templates.txt` | 12 IV/earnings DSL seeds (B2): `ivts1/2`, `ivrv1/2`, `ec1/2`, `eag1/2`, `comb1/2` + 2 more |
| `atx-impl/tests/fixtures/neutralized_templates.txt` | 12 sector/size-neutral DSL seeds (B3): `mom_ss1/2`, `lv_ss1/2`, `rev_ss1/2`, `ivts_ss1`, `ivrv_ss1` + siblings |
| `atx-impl/tests/seed_parse_test.cpp` | 3 tests (`SeedParse.*`): `IvEarningsTemplatesParsesAndTypechecks`, `NeutralizedTemplatesParsesAndTypechecks`, `CsResidualizeNeutralizesSectorAndSize` |
| `atx-impl/src/{config.hpp,config.cpp,dispatch.cpp,stages.hpp}` | Additive CLI wiring for `augment` subcommand (`--short-interest`, `--augment-out`, `--si-publication-lag`) |
| `atx-engine/CMakeLists.txt`, `atx-impl/CMakeLists.txt` | `finra_short.cpp` + `stage_augment.cpp` added to build |

**8 tests green on track-b, unmerged into main.** The branch also contains
`stage_regime_oos.cpp` and `regime_oos_test.cpp` (B4 — OOS analyzer); these are
**out of scope for S2** and should not be cherry-picked in the landing.

---

## The field-availability map (post-p6 + post-S2)

### Fields available today on main (post-p6, verified)

| Source | Field | Notes |
|---|---|---|
| ORATS raw | `open`, `high`, `low`, `close`, `raw_close`, `volume`, `market_cap`, `sector` | Base OHLCV+meta |
| ORATS raw | `earnFlag`, `atmCenI_21d`, `atmCenI_126d`, `nEarnCnt_5d` | Options/earnings (dormant pre-S2) |
| `with_datafields` | `dollar_volume`, `vwap`, `adv{d}` | `augment.hpp:168` delegates here |
| `with_alpha101_fields` | `returns`, `cap`, `IndClass.sector/.industry/.subindustry` | Production since p6-S5; `augment.hpp:64` |

### New fields S2 adds (opt-in only)

| Family | Field | Derivation | Entry point |
|---|---|---|---|
| Short-interest (track-b) | `si_dtc` | `days_to_cover_quantity` from FINRA parquet | `augment_panel_with_finra` |
| Short-interest (track-b) | `si_util` | `short_position / shares` (or `/ADV` fallback) | `augment_panel_with_finra` |
| Short-interest (track-b) | `si_chg` | `change_percent` from FINRA parquet | `augment_panel_with_finra` |
| IV-surface | `iv_term` | `zscore(atmCenI_21d / atmCenI_126d)` (term slope) | `with_iv_fields` |
| IV-surface | `iv_vrp` | `atmCenI_21d - ts_std(returns, 21)` (IV minus realized) | `with_iv_fields` |
| IV-surface | `iv_lo` | `atmCenI_21d / (nEarnCnt_5d + 1)` (IV conditioned on no earnings) | `with_iv_fields` |
| Liquidity | `illiq` | `group_neutralize(zscore(-1 * adv20), sector)` (Amihud-style) | `with_liquidity_fields` |

**Panel after `with_alpha101_fields` + `with_iv_fields` + `with_liquidity_fields`:**
returns, cap, IndClass.*, dollar_volume, vwap, adv{windows}, iv_term, iv_vrp,
iv_lo, illiq.

**Panel after additional `augment_panel_with_finra`:**
the above + si_dtc, si_util, si_chg (appended at the END, existing FieldIds
bitwise-unchanged).

### Guard semantics

All three new entry points are **idempotent via `detail::has_field` guards**
(same pattern as `with_alpha101_fields`). A panel that already carries `iv_term`
will not get a duplicate column; a panel that already carries `illiq` will not be
re-derived. The default `with_alpha101_fields(base, windows)` call remains
unchanged — it does not call `with_iv_fields` or `with_liquidity_fields`.

---

## Tasks

### S2-0 — Open sprint ledger + landing checklist *(marker commit; do first)*

**Goal:** create the sprint ledger (`phase-s2-progress.md` in the working
worktree), record the base SHA, enumerate the landing checklist from S2-1 below,
and commit the marker. No code changes in this unit.

**Wiring (file:line):** `atx-engine/plans/p7/` — ledger only; no source files.

**Determinism (inert default):** documentation only; zero impact on any build
output.

**Accept:**
- Ledger file exists with `Base: main @ <SHA>`, scope, and unit rows pre-filled.
- `git log --oneline -1` shows the marker commit.
- No source files modified.

---

### S2-1 — Land track-b FINRA short-interest (digest-stable merge)

**Goal:** merge the FINRA short-interest work from
`worktree-track-b-information-structure` (commits `02e21e7`–`8df5010`) into the
S2 working branch, excluding the `B4` regime-OOS commit
(`37a517a feat(regime-oos): B4...`). Verify the 8 shipped tests remain green and
that the default panel build is **byte-identical** (serialization digest
unchanged).

The landing requires one non-trivial correctness check: the track-b branch
predates the p6 S5/S6/S7 landing (the branch diverged before those merges). The
cherry-pick / merge may encounter:
1. **`config.hpp` / `config.cpp` conflicts** — track-b added `--short-interest`,
   `--augment-out`, `--si-publication-lag` to `RunConfig`; p6-S7 added
   `--augment-panel`, `--adv-windows`. Both sets of fields must survive; resolve
   conflicts additively (keep both).
2. **`CMakeLists.txt` conflicts** — track-b added `finra_short.cpp` and
   `stage_augment.cpp`; p6 added other sources. Resolve by keeping all entries.
3. **`stages.hpp` / `dispatch.cpp`** — track-b adds `"augment"` to the dispatch
   table; p6 may have restructured. Keep both subcommands.

**Digest-stable requirement:** the existing `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
/ `FactoryOos.MineIntoOffPathDigestUnchanged` / OOS goldens must all remain green
after the landing. The `augment_panel_with_finra` function only fires when
`run_augment` is explicitly called — it is not in the default `stage_panel` path.

**Wiring (file:line):**
- `atx-engine/include/atx/engine/data/finra_short.hpp` (new; `kFinraFieldDtc/Util/Chg` at top; `load_finra_features` signature ~L77)
- `atx-engine/src/data/finra_short.cpp` (new; 323 lines)
- `atx-engine/tests/data/finra_short_test.cpp` (new; 4 tests)
- `atx-impl/src/stage_augment.{hpp,cpp}` (new; `augment_panel_with_finra` declared in `.hpp`)
- `atx-impl/tests/augment_test.cpp` (new; 4 tests)
- `atx-impl/src/config.hpp` — add `short_interest`, `augment_out`,
  `si_publication_lag` alongside existing p6 fields
- `atx-impl/src/config.cpp` — parse `--short-interest`, `--augment-out`,
  `--si-publication-lag` args
- `atx-impl/src/dispatch.cpp` — add `"augment"` case
- `atx-impl/src/stages.hpp` — declare `run_augment`
- `atx-engine/CMakeLists.txt` — `finra_short.cpp` source entry
- `atx-impl/CMakeLists.txt` — `stage_augment.cpp` source entry

**Determinism (inert default):** `augment_panel_with_finra` is called only by
`run_augment`. The `stage_panel` → `with_alpha101_fields` path is untouched.
Off-path byte-identity confirmed by running the golden-digest test suite before
and after the landing commit.

**Accept:**
- `FinraShort.*` (4 tests) green.
- `Augment.*` (4 tests) green.
- All pre-existing alpha101 / capacity / metric-alignment / pseed-illiq /
  golden-digest tests green (zero regressions).
- A panel built via `atx-impl panel` on the dev fixture produces the same
  serialization digest as before the landing (confirmed by running the
  `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` test or its equivalent).
- `git show --stat HEAD` lists the 12 expected files, not `stage_regime_oos.cpp`
  or `regime_oos_test.cpp`.

---

### S2-2 — IV-surface derived fields (`with_iv_fields`)

**Goal:** add three IV-surface panel columns — `iv_term`, `iv_vrp`, `iv_lo` —
derived from the existing options/earnings panel fields that are already loaded by
ORATS but never used as signal inputs. These are pure arithmetic derivations over
existing panel fields; no new data source is needed.

**Derivations (hand-verifiable on a tiny fixture):**

| Field | Formula | Required inputs |
|---|---|---|
| `iv_term` | `zscore(atmCenI_21d / atmCenI_126d)` | `atmCenI_21d`, `atmCenI_126d` |
| `iv_vrp` | `atmCenI_21d - ts_std(returns, 21)` | `atmCenI_21d`, `returns` |
| `iv_lo` | `atmCenI_21d / (nEarnCnt_5d + 1.0)` | `atmCenI_21d`, `nEarnCnt_5d` |

`zscore` here is the cross-sectional z-score (mean-0, std-1 across instruments on
each date, NaN where in-universe count < 2). `ts_std` is the causal trailing
standard deviation over 21 dates (NaN on any date where fewer than 2 in-universe
observations precede it — same NaN-propagation policy as `with_datafields`).

All three cells are NaN whenever any required input is NaN or the instrument is
out-of-universe.

**Implementation:**

Add a new function to `augment.hpp`:

```cpp
[[nodiscard]] inline atx::core::Result<Panel>
with_iv_fields(const Panel& base);
```

The function appends `iv_term`, `iv_vrp`, `iv_lo` in that order, guarded by
`detail::has_field` idempotency checks. It requires `atmCenI_21d`; if absent,
returns `Err(NotFound, "with_iv_fields: panel has no 'atmCenI_21d' field")`. It
requires `returns` (for `iv_vrp`); if absent, returns `Err(NotFound, ...)`.
`iv_lo` requires only `atmCenI_21d` and `nEarnCnt_5d`; if `nEarnCnt_5d` is
absent the denominator is `1.0` (documented fallback, not a silent error).

The function does NOT call `with_alpha101_fields` — it expects the caller to
ensure `returns` is already present (it will be after `with_alpha101_fields`).

**Wiring (file:line):**
- `atx-engine/include/atx/engine/alpha/augment.hpp` — append `with_iv_fields`
  after `with_alpha101_fields` (currently ends at line 172); new function at
  line ~175+
- NEW `atx-engine/tests/alpha/iv_fields_test.cpp` — 5 tests (see Accept)

**Determinism (inert default):** `with_iv_fields` is a new opt-in function.
`with_alpha101_fields(base, windows)` does not call it. No existing code path
invokes `with_iv_fields`. Off-path digest unchanged.

**Accept:**

Test file `atx-engine/tests/alpha/iv_fields_test.cpp`, suite `IvFields`:

| Test | What it proves |
|---|---|
| `AddsThreeColumns` | synthetic 5-instrument × 10-date panel → output has `iv_term`, `iv_vrp`, `iv_lo` (exactly); no duplicates |
| `IdempotentOnRe-call` | calling `with_iv_fields` twice on same panel → identical output to calling once |
| `IvTermCorrectValues` | hand-computed: for a 5-instrument panel with known `atmCenI_21d/126d` values, `iv_term` at a specific cell equals the expected cross-sectional z-score (within 1e-12) |
| `IvVrpCorrectValues` | `iv_vrp[d,n] == atmCenI_21d[d,n] - ts_std(returns[0..d], 21)[d,n]` on a synthetic fixture where `ts_std` is known by construction |
| `IvLoFallbackNEarnMissing` | panel without `nEarnCnt_5d` → `iv_lo == atmCenI_21d` (denominator defaults to `1.0`); no error |

All tests use deterministic synthetic panels (constant seeds; no real ORATS
data). All values asserted to within 1e-12 tolerance.

---

### S2-3 — Liquidity / Amihud-style derived field (`with_liquidity_fields`)

**Goal:** add one liquidity panel column — `illiq` — derived from the `adv20`
column that `with_datafields` already materializes. This is the simplest new
family: one column, one formula, one new entry point.

**Derivation:**

```
illiq = group_neutralize(zscore(-1 * adv20), sector)
```

where `group_neutralize(x, g)` subtracts the within-sector cross-sectional mean
of `x` from each cell in that sector (so `illiq` is a sector-relative illiquidity
rank, long low-liquidity instruments). `zscore` is cross-sectional (per-date,
across instruments). `-1 * adv20` makes the signal: **low ADV → high illiquidity
→ positive signal**.

This is the Amihud (2002) intuition materialized as a pure derivation over the
existing `adv20` field — no new data source. `adv20` must already be present
(i.e., the caller must have already passed `adv_windows` containing `20` through
`with_alpha101_fields` or `with_datafields`).

**Implementation:**

Add to `augment.hpp`:

```cpp
[[nodiscard]] inline atx::core::Result<Panel>
with_liquidity_fields(const Panel& base);
```

Appends `illiq` guarded by `detail::has_field` idempotency. Requires `adv20` and
`sector`; if `adv20` absent, returns `Err(NotFound, "with_liquidity_fields:
panel has no 'adv20' field (call with_alpha101_fields with window 20 first)")`.
If `sector` absent, `group_neutralize` degenerates to a global z-score (the whole
universe is one group) — document this explicitly in the header comment, do not
return an error.

**Wiring (file:line):**
- `atx-engine/include/atx/engine/alpha/augment.hpp` — append `with_liquidity_fields`
  after `with_iv_fields`; new function at line ~215+ (after S2-2)
- NEW `atx-engine/tests/alpha/liquidity_fields_test.cpp` — 5 tests (see Accept)

**Determinism (inert default):** new opt-in function; no existing call site
invokes it. Off-path digest unchanged.

**Accept:**

Test file `atx-engine/tests/alpha/liquidity_fields_test.cpp`, suite `LiquidityFields`:

| Test | What it proves |
|---|---|
| `AddsIlliqColumn` | synthetic 4-instrument × 8-date panel with known `adv20` and `sector` → output has exactly `illiq`; no duplicates |
| `IdempotentOnRe-call` | calling `with_liquidity_fields` twice → identical output to calling once |
| `IlliqCorrectValues` | hand-computed: for a 4-instrument, 2-sector panel with known `adv20` values, `illiq` at a specific (date, instrument) cell equals the expected sector-relative z-score of `-adv20` (within 1e-12) |
| `MissingAdv20ReturnsError` | panel without `adv20` → `Err(NotFound)`; no crash |
| `NoSectorFallsBackToGlobal` | panel without `sector` → `illiq` equals global z-score of `-adv20`; no error |

All tests use deterministic synthetic panels. Numerical assertions within 1e-12.

---

### S2-4 — Multi-family seed catalog

**Goal:** extend the seed catalog with DSL expressions that span all four signal
families so the GA + discover stage have real information structure across
price/returns, IV-surface, liquidity, and short-interest. Two new fixture files;
extend the existing `seed_parse_test.cpp`.

**The seed catalog after S2:**

| File | Seeds | Families covered |
|---|---|---|
| `atx-impl/tests/fixtures/alpha101.txt` | 120 lines (101 seeds) | price/returns — FROZEN, no edits |
| `atx-impl/tests/fixtures/iv_earnings_templates.txt` (track-b landing) | 12 seeds | IV-surface + earnings |
| `atx-impl/tests/fixtures/neutralized_templates.txt` (track-b landing) | 12 seeds | price/returns sector+size-neutral |
| NEW `atx-impl/tests/fixtures/short_interest_seeds.txt` | 10–14 seeds | short-interest (`si_dtc`, `si_util`, `si_chg`) |
| NEW `atx-impl/tests/fixtures/liquidity_seeds.txt` | 8–12 seeds | liquidity (`illiq`, `adv20`, `dollar_volume`) |

**Seed format** (from `atx-impl/src/config.cpp`): `<id>: <dsl-expression>`,
`#` comment lines, split on first `:`.

**Required seeds for `short_interest_seeds.txt`** (write at least these 10):

```
# Short-interest seed templates (S2 — atx p7).
# Parse + typecheck against panel carrying si_dtc, si_util, si_chg,
# returns, sector (after augment + with_alpha101_fields).

# Days-to-cover: long high short-squeeze potential
si1: group_neutralize(rank(si_dtc), sector)

# Utilization: long high-borrow-demand names
si2: group_neutralize(rank(si_util), sector)

# Short-change: long increasing short interest (short-squeeze anticipation)
si3: group_neutralize(rank(si_chg), sector)

# DTC z-score sector-neutral
si4: group_neutralize(zscore(si_dtc), sector)

# Negative DTC: contrarian — short high-DTC (momentum side)
si5: group_neutralize(-1 * rank(si_dtc), sector)

# Utilization scaled by 21d returns: crowded-short reversal
si6: group_neutralize(rank(si_util * (-1 * ts_mean(returns, 21))), sector)

# Short-change x DTC interaction
si7: group_neutralize(rank(si_dtc * si_chg), sector)

# DTC minus adv20-ranked illiquidity (borrow premium proxy)
si8: group_neutralize(rank(si_dtc - rank(adv20)), sector)

# Util x return reversal: high-util names with recent negative returns
si9: group_neutralize(rank(si_util * (-1 * ts_mean(returns, 5))), sector)

# Short-change z-score sector-neutral
si10: group_neutralize(zscore(si_chg), sector)
```

**Required seeds for `liquidity_seeds.txt`** (write at least these 8):

```
# Liquidity seed templates (S2 — atx p7).
# Parse + typecheck against panel carrying illiq, adv20, dollar_volume,
# returns, sector, cap (after with_alpha101_fields + with_liquidity_fields).

# Sector-neutral illiquidity (the Amihud signal itself)
liq1: group_neutralize(rank(illiq), sector)

# Illiquidity z-score
liq2: group_neutralize(zscore(illiq), sector)

# Low-ADV (illiquid) names with positive recent return: contrarian
liq3: group_neutralize(rank(illiq * ts_mean(returns, 5)), sector)

# ADV momentum: names whose liquidity is growing
liq4: group_neutralize(rank(ts_mean(adv20, 5) - adv20), sector)

# Size-neutralized illiquidity (adv relative to market cap)
liq5: group_neutralize(rank(adv20 / cap), sector)

# Negative ADV: short liquid names (crowded)
liq6: group_neutralize(-1 * rank(adv20), sector)

# Dollar volume z-score
liq7: group_neutralize(zscore(dollar_volume), sector)

# Low dollar-volume with reversal: short recent winners among illiquid
liq8: group_neutralize(rank(-1 * dollar_volume * ts_mean(returns, 21)), sector)
```

**Implementation:** write the two fixture files and extend
`atx-impl/tests/seed_parse_test.cpp` with two new tests (see Accept). The
existing `IvEarningsTemplatesParsesAndTypechecks` and
`NeutralizedTemplatesParsesAndTypechecks` tests from track-b are NOT modified.

**Wiring (file:line):**
- NEW `atx-impl/tests/fixtures/short_interest_seeds.txt`
- NEW `atx-impl/tests/fixtures/liquidity_seeds.txt`
- `atx-impl/tests/seed_parse_test.cpp` — append two new `TEST(SeedParse, ...)`
  cases after the existing B2/B3 tests

**Determinism (inert default):** fixture files are test data only; zero impact
on any build output or panel bytes. No production source files modified.

**Accept:**

Two new tests added to `seed_parse_test.cpp`, suite `SeedParse`:

| Test | What it proves |
|---|---|
| `ShortInterestSeedsParsesAndTypechecks` | Every non-empty, non-comment line in `short_interest_seeds.txt` parses (no `parse_expr` error) and typechecks (no `analyze` error) against a panel that carries `si_dtc`, `si_util`, `si_chg`, `adv20`, `returns`, `sector`, `cap`, `dollar_volume`, `illiq`. Every seed references at least one of `si_dtc`, `si_util`, `si_chg`. |
| `LiquiditySeedsParsesAndTypechecks` | Every non-empty, non-comment line in `liquidity_seeds.txt` parses + typechecks against the same panel. Every seed references at least one of `illiq`, `adv20`, `dollar_volume`. |

The test panel must carry ALL of `si_dtc`, `si_util`, `si_chg`, `illiq`,
`adv20`, `dollar_volume`, `returns`, `sector`, `cap` as synthetic columns (can
all be constant NaN for typecheck purposes — shape coherence is what matters, not
numerical values). Use the same `parse_fixture_file` helper pattern established
by the B2/B3 tests already in `seed_parse_test.cpp`.

All 5 `SeedParse.*` tests green (3 from track-b + 2 new).

---

### S2-5 — Off-path smoke: augmented-panel evaluates ≥1 seed per family

**Goal:** prove that the full augmentation chain
(`with_alpha101_fields` → `with_iv_fields` + `with_liquidity_fields`) produces a
panel on which the VM can actually evaluate at least one seed from each family,
end-to-end, on a tiny synthetic fixture. This is the integration gate — it
catches wiring mismatches (wrong field name, wrong type, wrong dimension) that
unit tests on individual entry points cannot detect.

This is NOT a discover run; it is NOT a dev-panel run. It is a test that
constructs a synthetic 5-instrument × 15-date panel, runs the full augmentation
chain, then evaluates one representative seed DSL from each family via the
existing `Engine` + `compile` + `execute` path and asserts the output is finite
(non-NaN) on at least one cell.

**Wiring (file:line):**
- NEW `atx-engine/tests/alpha/multi_family_smoke_test.cpp`
- Uses `atx::engine::alpha::with_alpha101_fields` (`augment.hpp:64`)
- Uses `atx::engine::alpha::with_iv_fields` (`augment.hpp:~175`, after S2-2)
- Uses `atx::engine::alpha::with_liquidity_fields` (`augment.hpp:~215`, after S2-3)
- Uses `atx::engine::alpha::parse_expr` / `compile` / `Engine::execute`

**Determinism (inert default):** test file only; no production source modified.

**Accept:**

Test file `atx-engine/tests/alpha/multi_family_smoke_test.cpp`, suite
`MultiFamilySmoke`:

| Test | What it proves |
|---|---|
| `PriceReturnsFamily` | `group_neutralize(rank(ts_mean(returns, 5)), sector)` evaluates without error on the augmented panel; ≥1 cell is finite |
| `IvSurfaceFamily` | `group_neutralize(rank(iv_term), sector)` evaluates without error; ≥1 cell finite |
| `LiquidityFamily` | `group_neutralize(rank(illiq), sector)` evaluates without error; ≥1 cell finite |
| `AugmentedPanelFieldCount` | the fully augmented panel (iv + liquidity, adv{20}) carries ≥ 15 named fields (base 12 + returns + cap + IndClass.* (3) + dollar_volume + vwap + adv20 + iv_term + iv_vrp + iv_lo + illiq); exact count asserted |
| `OffPathDigestUnchanged` | a panel built by ONLY `with_alpha101_fields(base, {20})` (no IV, no liquidity) serializes to the same digest on two consecutive calls — confirms the opt-in contract holds at the test level |

The short-interest family (`si_dtc`, `si_util`, `si_chg`) does NOT need a
runtime-smoke here because `augment_panel_with_finra` requires real or synthetic
parquet on disk (covered by `Augment.*` tests in S2-1). The parse+typecheck
coverage from S2-4 is sufficient for the catalog integration gate.

All 5 `MultiFamilySmoke.*` tests green.

---

## Sequencing

```
S2-0  (marker + ledger)
  └─► S2-1  (land track-b; MUST be first code unit — S2-4 seed tests
             need the track-b fixture files in the tree)
        ├─► S2-2  (IV-surface fields; independent of S2-3)
        ├─► S2-3  (liquidity fields; independent of S2-2)
        └─► S2-4  (seed catalog; depends on track-b fixture files from S2-1,
                   and on S2-2/S2-3 having named their fields so the panel
                   built in the parse test can carry them)
              └─► S2-5  (multi-family smoke; depends on S2-2 + S2-3 entry
                         points existing, and on S2-4 for full field list
                         confirmed)
```

S2-2 and S2-3 can be dispatched in parallel once S2-1 is committed (they touch
disjoint regions of `augment.hpp` — S2-2 appends `with_iv_fields`, S2-3 appends
`with_liquidity_fields`; no symbol cross-dependency). S2-4 requires both to have
defined their field name constants before the parse-test panel fixture is
constructed. S2-5 requires both entry points to be callable.

**Regression gate before proceeding from S2-1:** all pre-existing alpha101 /
capacity / metric-alignment / pseed-illiq / golden-digest suites must be green.
Do not proceed to S2-2/S2-3 until this gate is confirmed.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| track-b cherry-pick conflicts with p6-S7 `config.hpp` additions | Both sets of fields are additive; resolve by keeping all CLI args; both the `augment` and `--augment-panel` subcommands must survive. Test: compile + all existing tests green. |
| `stage_regime_oos.cpp` / `regime_oos_test.cpp` accidentally landed | The accept for S2-1 explicitly checks `git show --stat HEAD` does not include these files. If accidentally landed, revert those two files in a follow-up commit. |
| Serialization digest drifts if `with_iv_fields` / `with_liquidity_fields` are accidentally called on the default path | Each entry point is a named function; `with_alpha101_fields` does not call them. Confirmed by `OffPathDigestUnchanged` test in S2-5 and the golden-digest CI tests. |
| `adv20` absent when `with_liquidity_fields` is called | `with_liquidity_fields` returns `Err(NotFound)` — explicitly tested in S2-3 `MissingAdv20ReturnsError`. The smoke test S2-5 passes `adv_windows={20}` to `with_alpha101_fields` before calling `with_liquidity_fields`. |
| IV fields require `returns` which requires `with_alpha101_fields` first | `with_iv_fields` returns `Err(NotFound)` if `returns` absent — tested in S2-2 by the field-presence assert in `AddsThreeColumns`. The smoke test S2-5 calls `with_alpha101_fields` before `with_iv_fields`. Documented in `with_iv_fields` header comment. |
| `nEarnCnt_5d` absent on some panels (ORATS not always populated) | `iv_lo` denominator defaults to `1.0` (documented fallback); tested in S2-2 `IvLoFallbackNEarnMissing`. No silent error. |
| Short-interest seed DSL references `adv20` which requires `adv_windows={20}` | The seed parse test constructs a panel with `adv20` pre-present as a synthetic field. Callers in production must ensure `adv_windows` contains 20. Document in `short_interest_seeds.txt` header comment. |
| Track-b `si_util` shares denominator uses `raw_close` (not `close`) | Already verified in `Augment.UtilFromPanelShares` test (track-b). Landing does not alter `finra_short.cpp`. Digest-stable because `si_util` only appears in the `augment` output, not in the default panel. |
| LSP false positives on new headers | Do not chase LSP mid-sprint. Verify against `cmake --build`; ignore IDE noise without `compile_commands.json`. |

---

## Bench / acceptance

**S2 is a data-structure / derivation sprint; no performance claim is being made.**
The following constitutes the full gate:

### Primary gate — unit tests on tiny deterministic fixtures

| Suite | Tests | Green required |
|---|---|---|
| `FinraShort.*` (from track-b, S2-1) | 4 | all |
| `Augment.*` (from track-b, S2-1) | 4 | all |
| `SeedParse.*` (track-b 3 + S2-4 2 new) | 5 | all |
| `IvFields.*` (S2-2) | 5 | all |
| `LiquidityFields.*` (S2-3) | 5 | all |
| `MultiFamilySmoke.*` (S2-5) | 5 | all |
| **Pre-existing suites (regression gate)** | all | zero regressions |

**Total new tests S2 adds: 24** (4 + 4 + 2 + 5 + 5 + 5 = 25 new; 3 from track-b
seed parse tests already counted in the 4+4 above → net 25 across 4 new files).

### Off-path byte-identity (mandatory)

Run `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` (or the equivalent) before and
after S2-1. The digest must be identical. This is the single strongest indicator
that the landing is non-destructive.

### Seed-catalog parse coverage

Every seed in the four non-frozen fixture files must parse + typecheck with no
error (gated by `SeedParse.*` tests). No unknown-field errors permitted.

### Twice-run determinism

Run the `MultiFamilySmoke.OffPathDigestUnchanged` test twice in sequence; confirm
no timestamp or hash drift.

### What S2 does NOT gate on

- No dev-panel discover run required for S2 (that is S7's job).
- No hour-long production run (p7 mandate; V1 is the only prod run, post-S5).
- No benchmark delta required (no hot path touched; performance is S3's scope).

---

## Out of scope (future sprints)

- **B4 regime-OOS analyzer** (`stage_regime_oos.cpp`) — built on track-b but not
  in scope for S2; carry forward to a future sprint or S7 if it is needed for the
  dev-panel validate.
- **True GICS industry/subindustry ingestion** — deferred; `// I5-HOOK:` marker in
  `augment.hpp` calls out the location.
- **`--iv-fields` / `--liquidity-fields` CLI flags** — S7 owns CLI threading;
  S2 ships the engine entry points only. The `with_iv_fields` /
  `with_liquidity_fields` functions are callable from code (and tested), but no
  CLI flag exposes them yet.
- **`adv20`-window CLI flag propagation** — already owned by S7 (`--adv-windows`).
  S2 tests pass `adv_windows={20}` directly.
- **Numerical OOS back-testing of IV / liquidity / SI signals** — not in S2; the
  discover loop (S7) exercises this once CLI threading is complete.
