# Lane W0-I0B: Equity stages and config hygiene

Host tag: **RAM:light** · Batch: **W0b** · Pool: **`C:\atx-wt\pool-11`** ·
Branch: **`feat/w0-i0b`** (run id `aes-w0-i0b`) · Base: the W0 base commit (head of
`feat/w0-integration` at lease time; see `progress.md`).

Read first: `.superpowers/sdd/w0/RULES.md` (binding), then this brief, then the plan and findings
docs under `docs/plans/` (all inside your pool).

## Goal (plan §7, verbatim)

#### W0-I0b · Equity stages and config hygiene
- **Closes:** D-12, I-10, I-11, I-12, I-15 (interface), I-16, I-17, I-23, E-18, B-02 (impl), D-02 (config default).
- **Owns:** `atx-impl/src/{config.{hpp,cpp}, dispatch.cpp, stage_equity_ic.{hpp,cpp}, equity_baseline_views.{hpp,cpp}, stage_equity_baseline.cpp, stage_equity_book.cpp, stage_equity_mine.{hpp,cpp}}` (mask, delay, recording and pending-order edits only).
- **Build:**
  - Boolean flag values are parsed (true/false). `--config` works in every stage or is rejected.
  - `isfinite` checks on every double flag.
  - Report costs are mandatory; no silent frictionless headline.
  - **As-of membership mask** in IC and baseline views. Cross-sectional ops are masked by as-of membership.
  - `--membership` is required in equity-mine.
  - delay < 1 is rejected without `--allow-same-close`.
  - `min_names_per_date` is a parameter, default 50.
  - Terminal-return table interface: data arrives in W2-D2. The 2013 audit is no longer mandatory.
  - Manifest written before `.pending` is released.
  - `nw_lags`, `ic_horizons` and `ppy` recorded in `gate_report`.
  - `si_publication_lag` default aligned with D0.
- **Suites:** `ImplConfigBool_*`, `ImplConfigFinite_*`, `ImplIcAsOfMembership_*`, `ImplMineRequiresMembership_*`, `ImplPendingOrder_*`, `ImplDelayGuard_*`.
- **Accept:**
  - A mid-year joiner is invisible before its effective session.
  - With constant membership, output is identical to the year-union path.
  - Output labels change to `as-of`.
- **Deps:** none. **Load:** light.

## Cited findings rows (verbatim; every ID must end CLOSED or explicitly DEFERRED in your report)

| ID | Sev | Location | Problem | Lane |
|---|---|---|---|---|
| D-12 | B | `stage_panel.cpp:318-346,294` → `equity_baseline_views.cpp:302,205-221` → `stage_equity_ic.cpp:546`; mine fallback `:1370-1373` | IC and baseline use the year-union membership mask (a within-year selection look-ahead). Cross-sectional ops see a future-selected cross-section. | W0-I0b |
| I-10 | M | `config.cpp:35-68,686`; `dispatch.cpp:131-139` | Boolean flags ignore their value (`metabook=false` turns it ON). `--config` is silently ignored in most stages. | W0-I0b |
| I-11 | M | `stage_run.cpp:161`; `config.hpp:451-452`; `replay_report.cpp:386-391` | The report defaults to 0 trade bps and 0 borrow, so headlines are frictionless. | W0-I0b |
| I-12 | L | `config.cpp:286-298,397-403` | "nan" is accepted for doubles (`--holdout-frac nan` silently disables the holdout). | W0-I0b |
| I-15 | H | `stage_equity_ic.cpp:1173,150-153,91,480-496` | Terminal pricing covers 3 hardcoded 2013 deals, and the stage refuses to run without the 2013 audit. | W0-I0b (interface) / W2-D2 (data) |
| I-16 | M | `stage_equity_mine.cpp:1370-1373` | Without `--membership`, the mask silently falls back to the year-union. | W0-I0b |
| I-17 | L | `stage_equity_baseline.cpp:476-479,591-595`; `stage_equity_book.cpp:573-576` | `.pending` is removed before the manifest is written. | W0-I0b |
| I-23 | L | `stage_equity_mine.hpp:136-138`; `stage_equity_mine.cpp:1799-1817` | nw_lags, ic_horizons and ppy are hardcoded and not recorded. | W0-I0b |
| E-18 | L | `stage_equity_ic.cpp:77` | `kMinNamesPerDate=2`. | W0-I0b |
| B-02 | H | `book/replay.cpp:184`; `stage_equity_mine.cpp:1179,1244-1247`; `config.cpp:455-464` | Execution delay 0 is accepted everywhere (same-close fills). | W0-B0 + W0-I0b |
| D-02 | H | `data/finra_short.cpp:242`, `finra_short.hpp:61`; `atx-impl/src/config.hpp:399` (`si_publication_lag=2`) | Short-interest publication is placed at settlement + 10 calendar days, which leaks 1–2 sessions on about 40% of observations. | W0-D0 (+W0-I0b default) |

## Scope

- Files in scope (the ONLY files you may modify; new test files per RULES §2 are always allowed):
  - `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`
  - `atx-impl/src/dispatch.hpp`, `atx-impl/src/dispatch.cpp`
  - `atx-impl/src/stage_equity_ic.hpp`, `atx-impl/src/stage_equity_ic.cpp`
  - `atx-impl/src/equity_baseline_views.hpp`, `atx-impl/src/equity_baseline_views.cpp`
  - `atx-impl/src/stage_equity_baseline.cpp`, `atx-impl/src/stage_equity_book.cpp`
  - `atx-impl/src/stage_equity_mine.hpp`, `atx-impl/src/stage_equity_mine.cpp` — mask, delay, recording and pending-order edits only
  - `atx-impl/src/replay_report.cpp` — ONLY the I-11 default-cost site (no other W0 owner)
- Files forbidden: everything else — in particular files owned by other W0 lanes (see
  `progress.md` ownership table). Needs elsewhere → report "Integration notes".

## Gate closure

- Test groups for `build-equity`: `(n/a — atx-impl)` (reconfigure only if the tree differs).
- Owning targets: `atx-impl-tests (+ atx-shm-worker)`.
- Suites: `ImplConfigBool_*`, `ImplConfigFinite_*`, `ImplIcAsOfMembership_*`, `ImplMineRequiresMembership_*`, `ImplPendingOrder_*`, `ImplDelayGuard_*`; must stay green: whole atx-impl-tests.
- Anchored runs: `-Ctest -Preset equity-dev -R '^<Suite>'` per suite; whole owning executable once
  before review (`--gtest_brief=1`).

## Done criteria

Every plan **Accept** item above MET with a named test and pasted evidence; every cited ID CLOSED
or DEFERRED with reason; owning targets green; tree clean; report committed at
`.superpowers/sdd/w0/lane-i0b-report.md`.

## Lane notes (orchestrator)

- Merge `feat/w0-integration` first: it carries E0a (`execution_delay`), E0b (registry windows / OOS flag), D0 (FINRA lag semantics) and O1. Wire them: E-09 delay in `stage_equity_ic` (default 1); E-16 registry fields in equity-mine recording; `si_publication_lag` default aligned with D0's integration note.
- B0 runs concurrently and adds the engine field `allow_same_close`; you own the CLI `--allow-same-close` and reject delay < 1 at config/stage level without it. If B0 has merged when you pre-merge, pass the flag through to the replay config.
- stage_run.cpp belongs to I0a — enforce 'report costs mandatory' (I-11) in config validation and `replay_report.cpp`, not in stage_run.cpp.
- Year-union → as-of membership (D-12): with constant membership the output must be identical to the year-union path (prove with a test).

## Out of scope

Anything the plan assigns to W1+ lanes; any real-data run; any CMake/preset edit (except O1);
refactors not required by a cited defect.
