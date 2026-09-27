# Lane W0-D0: Data-layer PIT leak fixes

Host tag: **RAM:light** · Batch: **W0a** · Pool: **`C:\atx-wt\pool-4`** ·
Branch: **`feat/w0-d0`** (run id `aes-w0-d0`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-D0 · Data-layer PIT leak fixes
- **Closes:** D-01, D-02 (engine side), D-03, D-04, D-05, D-06 (rebase part), D-08, D-09.
- **Owns:** `data/history_panel.{hpp,cpp}`, `alpha/augment.hpp` (the `dollar_volume` definition only), `data/finra_short.*`, `data/adjust.*`, `data/align.*`, `data/corporate_actions.*`, `data/context.*`, `data/universe.*`, `data/real_panel.*`.
- **Build:**
  - Build `dollar_volume`, `adv{d}` and `vwap` from `raw_close × volume`.
  - Tag every field with its level basis (`raw` / `adjusted_level` / `ratio`) in panel metadata, for the A3 lint.
  - FINRA publication lag counted in NYSE sessions, +1 for after-close publication.
  - The TRI gap uses the ratio `prev_tri·S_t/S_last`.
  - Event columns join on the exact date only, with a staleness cap, and fail when price dates exceed master coverage.
  - Shares are rebased by the cum_adj ratio. Same-filed-date ties use only rows dated ≤ d.
  - The DataContext as_of mismatch returns Err (it was a release-build no-op assert).
  - The NaN-floor semantics match the documentation.
  - real_panel O/H/L are scaled by TRI/raw.
  - The `si_publication_lag` default change is delivered through W0-I0b (integration note).
- **Suites:** `DataLevelBasis_*`, `DataFinraLag_*`, `DataAdjustGap_*`, `DataAlignEvent_*`, `DataCorpActRebase_*`, `DataContextAsOf_*`.
- **Accept:**
  - **Future-perturbation invariance:** mutating every row (factors included) dated after t leaves every history_panel output row ≤ t bit-identical, for all fields.
  - FINRA Thu/Fri/holiday fixtures land on the correct session.
  - A 3% payer shows no phantom drop across a gap.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| D-01 | H | `data/history_panel.cpp:216,247-261`; `alpha/augment.hpp:169`; used at `stage_equity_mine.cpp:1375` | `dollar_volume`/`adv{d}`/`vwap` are built from **adjusted close** × raw volume. The adjustment is a snapshot backward factor that already contains future splits and dividends, so the Alpha101 family has look-ahead. | W0-D0 |
| D-02 | H | `data/finra_short.cpp:242`, `finra_short.hpp:61`; `atx-impl/src/config.hpp:399` (`si_publication_lag=2`) | Short-interest publication is placed at settlement + 10 calendar days, which leaks 1–2 sessions on about 40% of observations. | W0-D0 (+W0-I0b default) |
| D-03 | H (latent) | `data/real_panel.cpp:388-398` | close = TRI while O/H/L/vwap stay raw: mixed price bases in one panel. | W0-D0 |
| D-04 | M | `data/adjust.cpp:88-92` (test locks it in: `data_adjust_test.cpp:393`) | The TRI re-anchors after a gap and drops accumulated dividends. A 3% payer 5 years in shows a phantom −14%. | W0-D0 |
| D-05 | M | `data/align.cpp:97-113` | The as-of join forward-fills event columns (dividend, cum_adj) with no staleness cap, double-counting dividends and freezing splits. | W0-D0 |
| D-06 | M | `data/corporate_actions.cpp:262-291`; `universe.cpp:102` | Shares are not rebased to the split basis. A same-filed-date tie resolves to a row dated after d. | W0-D0 (rebase) / W2-D3 |
| D-08 | M | `data/context.cpp:163` | `ATX_ASSERT` is a no-op in release, so a reused DataContext with an earlier as_of returns future-realized candidates. | W0-D0 |
| D-09 | L | `universe.hpp:93-95` vs `universe.cpp:207-208` | A NaN floor excludes names even when "floor 0 disables". | W0-D0 |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-engine/include/atx/engine/data/history_panel.hpp`, `atx-engine/src/data/history_panel.cpp`
  - `atx-engine/include/atx/engine/alpha/augment.hpp` — ONLY the `dollar_volume`/`adv{d}`/`vwap` definition
  - `atx-engine/include/atx/engine/data/finra_short.hpp`, `atx-engine/src/data/finra_short.cpp`
  - `atx-engine/include/atx/engine/data/adjust.hpp`, `atx-engine/src/data/adjust.cpp`
  - `atx-engine/include/atx/engine/data/align.hpp`, `atx-engine/src/data/align.cpp`
  - `atx-engine/include/atx/engine/data/corporate_actions.hpp`, `atx-engine/src/data/corporate_actions.cpp`
  - `atx-engine/include/atx/engine/data/context.hpp`, `atx-engine/src/data/context.cpp`
  - `atx-engine/include/atx/engine/data/universe.hpp`, `atx-engine/src/data/universe.cpp`
  - `atx-engine/include/atx/engine/data/real_panel.hpp`, `atx-engine/src/data/real_panel.cpp`
  - existing tests that pin a cited defect (e.g. `data_adjust_test.cpp:393` pins D-04)
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `data;alpha` (reconfigure only if the tree differs).
- Owning targets: `atx-engine-data-tests`, `atx-engine-alpha-tests (augment only)`.
- Suites: `DataLevelBasis_*`, `DataFinraLag_*`, `DataAdjustGap_*`, `DataAlignEvent_*`, `DataCorpActRebase_*`, `DataContextAsOf_*`, plus a `DataHistoryPanelFuturePerturb_*` suite for the future-perturbation invariance item; must stay green: whole data target + alpha augment tests.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-d0-report.md`.

## Lane notes (orchestrator)

- D-02 config default (`atx-impl/src/config.hpp` `si_publication_lag`) is delivered by I0b: write the exact new default + semantics in 'Integration notes'.
- D-06 is split: you own the share rebase + same-filed-date tie; the PIT shares source is W2-D3.
- D-01 consumer at `stage_equity_mine.cpp:1375` needs no edit if the engine definition is fixed; confirm in the report.
- Pool-4 was a factory pool; reconfigure `-Groups "data;alpha"` once.

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
