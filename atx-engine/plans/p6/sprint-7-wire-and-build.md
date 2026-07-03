# Sprint 7 — Wire Everything + Build Tradeable Alphas

**Goal:** thread every feature-sprint engine knob through the CLI (`config.hpp`, `config.cpp`,
`stage_discover.cpp`, `stage_run.cpp`), then run the capstone `build-tradeable-alphas.ps1`
harness on the real panel and produce ≥1 WQ-fit, IS-robust, net-of-10bps OOS Sharpe > 0.8
admitted alpha with a sign-correct non-empty deployed book — or a documented frontier naming
the binding constraint.

**Owns (exclusive):** `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
`atx-impl/src/stage_discover.cpp`, `atx-impl/src/stage_run.cpp`, NEW
`scripts/build-tradeable-alphas.ps1`, NEW
`atx-impl/research/2026-06-27-tradeable-alpha-results.md`.

**Runs LAST** — depends on Sprints 1–6 being merged. If a feature sprint is absent, wire what
exists and note the gap in the research doc.

**Determinism contract (A):** every new CLI flag DEFAULTS to the inert value so the existing
`AtxImplDiscover` determinism slice + factory golden+digest slice pass byte-identical when no
new flag is passed (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.
MineIntoOffPathDigestUnchanged`, OOS goldens — all green, `oracle.hpp` untouched). The
tradeable-alpha BUILD profile turns the opt-ins ON as an explicit, documented non-default
profile; it is NEVER a golden re-baseline.

**Depends-on:** S1–S6 (disjoint-file owners per `ROADMAP.md`). S7 is the only sprint that
touches the four reserved hub files.

---

## Dependency map — flag → feature sprint → engine field

| CLI flag (S7 adds) | Feature sprint | Engine config field | Inert default |
|---|---|---|---|
| `--turnover-penalty-slope` | S3 T1 | `FitnessCfg.turnover_penalty_slope` | `0.0` |
| `--max-turnover-target` | S3 T1 | `FitnessCfg.max_turnover_target` | `+inf` |
| `--protect-seed-elites` | S3 T3 | `SearchConfig.protect_seed_elites` | `false` |
| `--mutate-seed-copies` | S3 T3 | `SearchConfig.mutate_seed_copies` | `false` |
| `--deflate-selection` | S3 T4 / already parsed (config.cpp:40) | `SearchConfig.deflate_selection` → `FactoryConfig.search.deflate_selection` (stage_discover.cpp:435) | `false` |
| `--min-viable-raw` | S3 T4 | `SearchConfig.min_viable_raw` | `0.0` |
| `--enable-wrap-in-op` | S3 T3 / already parsed (config.cpp:37) | `SearchConfig.enable_wrap_in_op` (stage_discover.cpp:949) | `false` |
| grammar-weight preset | S3 | `SearchConfig` grammar weights | inert (no flag) |
| `--cost-bps-admit` | S4 T1 | `GateConfig.rt_cost_bps` | `0.0` |
| `--min-holding-days` | S4 T2 | `GateConfig.min_holding_days` | `0.0` |
| `--cost-max-turnover` | S4 T3 | `GateConfig.max_turnover` (via `cost_aware_knobs`) | `0.70` |
| `--adv-windows` | S5 T3 | `RunConfig.adv_windows` (`std::vector<u16>`); consumed by `stage_panel` at `SPRINT7-WIRES:` marker | `{}` (falls back to `{adv_window}`) |
| `--augment-panel` | S5 T3 | `RunConfig.augment_panel` bool; consumed by `stage_panel` at `SPRINT7-WIRES:` marker | `false` |
| S6 sign-correct deploy default | S6 T1 | `run_all` sets `c_opt.position_mode = true` (or `c_opt.risk_aversion = 0.0`) when no explicit flag | inert (matches S6 corrected path) |
| `report_aum` / `risk_aversion` / `position_mode` defaults in `run_all` | S6 | `stage_run.cpp:run_all` sub-config defaults | sane defaults, not golden re-baseline |

> **Existing wires (already landed, verify not regressed):** `--deflate-selection`
> (config.cpp:40, stage_discover.cpp:435), `--enable-wrap-in-op` (config.cpp:37,
> stage_discover.cpp:949), `--admit-seeds-presearch` (config.cpp:41,
> stage_discover.cpp:434). S7 adds the NEW S3/S4/S5 knobs and the S6 `run_all` defaults
> only.

---

## Tasks

### S7-0 — Verify green baseline before touching anything

**Goal:** confirm all existing determinism goldens and the discover digest slice pass on the
current tree before S7 opens any file.

**Wiring (file:line):** `atx-engine/tests/factory/factory_oos_test.cpp` (the
`AtxImplDiscover` slice and `FactoryOos` digest tests); `atx-engine/tests/factory/`
golden tests; `oracle.hpp` untouched.

**Determinism:** this task ONLY runs tests — it makes no edits.

**Accept:**
- `AtxImplDiscover` determinism slice: green.
- Factory golden+digest slice: green.
- `seq==parallel` slice: green.
- Record the passing SHA as the S7 baseline in the research doc.

---

### S7-1 — Thread S3 search net-cost knobs (RunConfig → SearchConfig / FitnessCfg)

**Goal:** add `RunConfig` fields and `config.cpp` parse arms for the five S3 knobs, then
wire them into the `SearchConfig`/`FitnessCfg` construction in `stage_discover.cpp`.

**Wiring (file:line):**

- `config.hpp` — add after the existing `enable_wrap_in_op` field (config.hpp:129):
  `double turnover_penalty_slope = 0.0`, `double max_turnover_target` (default
  `+inf` via `std::numeric_limits<double>::infinity()`),
  `bool protect_seed_elites = false`, `bool mutate_seed_copies = false`,
  `double min_viable_raw = 0.0`.
- `config.cpp` — add `bool` parse arms alongside the S3-adjacent existing booleans
  (config.cpp:37–41); add `double` parse arms in the numeric block (config.cpp:157–269).
  Add `protect-seed-elites` and `mutate-seed-copies` to the valueless-bool fast path
  (config.cpp:330).
- `stage_discover.cpp` — in the `sc` / `fcfg` construction block (stage_discover.cpp:
  939–960 for `sc`; stage_discover.cpp:423–449 for `fcfg`), wire:
  - `sc.turnover_penalty_slope = cfg.turnover_penalty_slope`
  - `sc.max_turnover_target = cfg.max_turnover_target`
  - `sc.protect_seed_elites = cfg.protect_seed_elites`
  - `sc.mutate_seed_copies = cfg.mutate_seed_copies`
  - `sc.min_viable_raw = cfg.min_viable_raw`
  (all delegate to `SearchConfig`/`FitnessCfg` fields added by S3)

**Determinism (inert default):** all defaults match S3's inert values (0.0/+inf/false) so
the `AtxImplDiscover` slice and factory goldens are byte-identical with no new flags.

**Accept:**
- `config_parse_test` passes: new flags round-trip correctly through CLI and config-file.
- `AtxImplDiscover` slice stays byte-identical (no new flags → inert defaults).
- On-path test (S3's off-path/on-path tests stay green; wire test confirms `sc.
  turnover_penalty_slope == 0.5` when `--turnover-penalty-slope 0.5` is passed).

---

### S7-2 — Thread S4 cost-aware gate knobs (RunConfig → GateConfig)

**Goal:** add `RunConfig` fields for `--cost-bps-admit`, `--min-holding-days`,
`--cost-max-turnover` and wire them into the `GateConfig` (`gc`) construction in
`run_discover_gated` in `stage_discover.cpp`.

**Wiring (file:line):**

- `config.hpp` — add after `max_pool_corr` field (config.hpp:66–67):
  `double cost_bps_admit = 0.0`, `double min_holding_days = 0.0`,
  `double cost_max_turnover = 0.0` (0.0 = off, uses default `max_turnover`).
- `config.cpp` — add `double` parse arms: `cost-bps-admit`, `min-holding-days`,
  `cost-max-turnover` in the numeric block near `max-turnover` (config.cpp:215).
- `stage_discover.cpp` — in `run_discover_gated`, after the `gc` construction block
  (stage_discover.cpp:414–419), wire:
  - `gc.rt_cost_bps = cfg.cost_bps_admit`
  - `gc.min_holding_days = cfg.min_holding_days`
  - when `cfg.cost_max_turnover > 0`: `gc.max_turnover = cfg.cost_max_turnover`
    (S4's `cost_aware_knobs` helper, if available, or direct assignment)

**Determinism (inert default):** `rt_cost_bps=0.0`, `min_holding_days=0.0` are S4's
inert defaults — `AlphaGate::admit` / `Library::verdict_for` behave byte-identically.
`cost_max_turnover=0.0` leaves `gc.max_turnover` at its existing default (0.70).

**Accept:**
- `AtxImplDiscover` slice stays byte-identical.
- S4's off-path/on-path tests stay green.
- Wire test: `--cost-bps-admit 10` produces `gc.rt_cost_bps == 10.0`.

---

### S7-3 — Thread S5 panel-augment knobs (RunConfig → stage_panel SPRINT7-WIRES markers)

**Goal:** add `RunConfig.adv_windows` (`std::vector<u16>`) and `RunConfig.augment_panel`
(bool), parse `--adv-windows` as a comma-separated list, and resolve every
`// SPRINT7-WIRES:` marker left in `stage_panel.cpp` by S5.

**Wiring (file:line):**

- `config.hpp` — add after the `adv_window` field (config.hpp:186):
  `std::vector<uint16_t> adv_windows`, `bool augment_panel = false`.
- `config.cpp` — add parse arm for `--adv-windows` (comma-split string → `u16` list;
  Err on empty or out-of-range values); add `--augment-panel` to the valueless-bool
  fast path (config.cpp:330).
- `stage_panel.cpp` — at each `// SPRINT7-WIRES:` marker left by S5 T3 (the augmentation
  call between `build_history_panel` at stage_panel.cpp:53 and `write_panel` at
  stage_panel.cpp:56):
  - when `cfg.augment_panel` is true (or `!cfg.adv_windows.empty()`), call
    `with_alpha101_fields(hp.panel, cfg.adv_windows.empty() ? std::span<const u16>{} : cfg.adv_windows)`
  - when false / empty, pass through unchanged (byte-identical to today).
- Also wire `--augment-panel` and `--adv-windows` into `run_all` in `stage_run.cpp`
  as part of the S7-5 `run_all` defaults pass (a fully-augmented panel is required for the
  build profile; the default `run_all` does NOT augment unless the flags are set).

**Determinism (inert default):** `augment_panel=false` + `adv_windows={}` → `stage_panel`
falls through to `write_panel` unchanged; every existing panel-related golden is
byte-identical.

**Accept:**
- `AtxImplDiscover` slice and panel digest tests stay byte-identical.
- All `// SPRINT7-WIRES:` comments in `stage_panel.cpp` are replaced with live wiring.
- S5's `augment_test.cpp` tests stay green.
- Wire test: `--augment-panel --adv-windows 5,20,60` produces a panel with `adv5`,
  `adv20`, `adv60` fields.

---

### S7-4 — Wire S6 sign-correct deploy defaults into `run_all` (stage_run.cpp)

**Goal:** make `run_all` in `stage_run.cpp` use the S6-corrected sign-correct deploy path
by default (position-mode or λ=0) and set sane `report_aum` / `risk_aversion` /
`position_mode` sub-config defaults so the default end-to-end pipeline deploys a
sign-correct non-empty book.

**Wiring (file:line):**

- `stage_run.cpp:run_all` — in the optimize sub-config block (stage_run.cpp:88–90),
  when the user has NOT explicitly set `position-mode` or `risk-aversion`:
  - set `c_opt.position_mode = true` (or `c_opt.risk_aversion = 0.0`) to engage the
    S6 sign-correct deploy path; guard with `cfg.set_flags.count("position-mode") == 0`
    (mirrors the `holdout-frac` guard at stage_run.cpp:82).
  - set `c_rep.report_aum` to a sane default (e.g. `1e9` already in config.hpp:258);
    verify `report_aum` default is not causing participation overflow post-S6 fix.
- If S6 added a `book_shape.hpp` function for the sign-correct path, ensure `run_all`
  flows through it; do NOT edit `optimizer.hpp`.

**Determinism (inert default):** the sign-correct path itself changes the output of
`run_all` vs the broken pre-S6 baseline — but that is a BUG FIX, not a golden regression.
The `AtxImplDiscover` slice (discover only) is unaffected. New S7 `run_all` test confirms
the sign-correct end-to-end (Sharpe sign matches raw blend IR).

**Accept:**
- `AtxImplDiscover` slice stays byte-identical.
- S6 end-to-end regression test stays green.
- `run_all` end-to-end on a synthetic admitted alpha: deployed book Sharpe sign ==
  admitted alpha OOS Sharpe sign (not inverted).
- `report_aum` / participation are sane (not 8,464,812%).

---

### S7-5 — `scripts/build-tradeable-alphas.ps1` capstone harness

**Goal:** a single PowerShell script that, on `work/accept/panel.bin`, runs the full
augmented-panel → cost-aware gated discover → sign-correct combine/optimize/report
pipeline and emits a ranked net-of-cost alpha table.

**Script structure:**

```
scripts/build-tradeable-alphas.ps1
  [-DryRun]          # compose argv, print, do NOT invoke the CLI
  [-WorkDir <path>]  # default work/tradeable-build
  [-PanelBin <path>] # default work/accept/panel.bin
  [-SeedFile <path>] # default work/seeds/alpha101-broad.txt
  [-Workers <n>]     # default auto
```

**Steps the script executes:**

1. **Augment panel** — `atx panel --augment-panel --adv-windows 5,10,20,60 --segs ... --panel-out <work>/aug-panel.bin` (build once; reuse downstream via `--panel`).

2. **Gated discover** (cost-aware, broad seed catalog):
   ```
   atx discover
     --panel <aug-panel.bin>
     --seed-file <broad-alpha101-catalog.txt>
     --admit-seeds-presearch
     --gated
     --library-dir <work>/_library
     --turnover-penalty-slope 0.1 --max-turnover-target 0.25
     --protect-seed-elites --mutate-seed-copies
     --deflate-selection
     --min-viable-raw 0.05
     --enable-wrap-in-op
     --cost-bps-admit 10
     --min-holding-days 5
     --min-dsr 0.5 --min-sharpe 0.25 --min-fitness 1.0 --max-turnover 0.50
     --reject-price-scale 0.5
     --dsr-subwindows 3
     --typed-fields
     --robust-holdout-frac 0.30
     --oos-fraction 0.25
     --panel-out <work>/discover-panel.bin
     --out <work>/alphas
     --population 300 --generations 15
   ```
   (This is the explicit **BUILD PROFILE** — all knobs ON, non-default, never a golden re-baseline.)

3. **Combine** — `atx combine --panel <discover-panel.bin> --library-dir <work>/_library --combo-out <work>/combo.bin --holdout-frac 0.25`

4. **Optimize** — `atx optimize --panel <discover-panel.bin> --combo <work>/combo.bin --books-out <work>/books.bin --position-mode --cost-bps 10`

5. **Report** — `atx report --panel <discover-panel.bin> --books <work>/books.bin --combo <work>/combo.bin --report-out <work>/report`

6. **Emit ranked table** — read `<work>/report/summary.txt` + the library manifest,
   print: admitted alphas ranked by net-of-10bps OOS Sharpe, with columns:
   rank, ID, gross OOS Sharpe, net OOS Sharpe, IS Sharpe, fitness, turnover,
   capacity AUM, deployed-book Sharpe (sign-correct).

**Staged execution (avoid 10-min timeout):** steps 1–2 (discover, the long-pole) are run
first and exit. Steps 3–5 (combine/optimize/report, seconds) run in a separate invocation.
This mirrors the close-discovery-loop session precedent. The script accepts a
`-Stage <augment|discover|pipeline|all>` parameter defaulting to `all`.

**`-DryRun`:** compose all argv arrays and `Write-Host` them; do NOT call the CLI.

**Pester test** `scripts/tests/build-tradeable-alphas.Tests.ps1`:
- `DryRun` mode: script outputs the expected argv fragments (seed-file, cost-bps-admit,
  position-mode) without invoking anything.
- Validates that every expected flag appears in the composed argv.
- Does NOT require the binary or the real panel (pure argv construction test).

**Determinism:** the script builds no goldens. The BUILD PROFILE is documented as an
explicit non-default config. Re-running the script on the same panel + seed should
produce the same admit set (seed-stable, F1 property inherited from the engine).

**Accept:**
- `-DryRun` prints the full argv without error.
- Pester test passes in CI.
- `build-tradeable-alphas.ps1` runs end-to-end on `work/accept/panel.bin` (real data)
  and emits the ranked net-of-cost table with a sign-correct non-empty deployed book.

---

### S7-6 — Run harness, capture results, write research doc

**Goal:** execute `build-tradeable-alphas.ps1` on `work/accept/panel.bin` (staged to avoid
the 10-min timeout) and record the findings.

**Execution:**

```powershell
# Step 1: augment + discover (background / long-pole)
./scripts/build-tradeable-alphas.ps1 -Stage augment,discover -WorkDir work/tradeable-build

# Step 2: combine/optimize/report (seconds, after discover completes)
./scripts/build-tradeable-alphas.ps1 -Stage pipeline -WorkDir work/tradeable-build
```

**Research doc** `atx-impl/research/2026-06-27-tradeable-alpha-results.md`:
- Environment: panel path, date-range, field count (post-augment), worker count.
- Build profile (the explicit knob settings used — the non-default config).
- Admitted set: count, IDs, DSL expressions.
- Ranked net-of-cost table: gross OOS Sharpe, net OOS Sharpe (−10bps), IS Sharpe,
  fitness, turnover, capacity AUM, deployed-book Sharpe.
- Best tradeable alpha (target: net OOS Sharpe > 0.8, IS > 0, fitness ≥ 1.0,
  sign-correct non-empty book).
- Speedup vs ~21-min pre-uplift baseline (S1+S2 perf contribution).
- If no alpha clears the net bar: document the frontier + binding constraint (which gate
  killed the candidates — reject_histogram dominant bucket). The honest null is the valid
  output, as the close-discovery-loop session demonstrated.
- Final determinism note: default (no-new-flags) run of `atx discover` on the same panel
  is byte-identical to pre-S7; the build profile is the explicit opt-in.

**Accept:**
- Research doc committed with the full result table (not a placeholder).
- If north-star met: ≥1 alpha with net OOS Sharpe > 0.8 + sign-correct book.
- If north-star not met: reject_histogram analysis + the binding constraint named.

---

### S7-7 — Determinism + final verification

**Goal:** confirm that S7's wiring (all the new inert defaults) has not shifted any
existing golden, and that the build-profile `seq==parallel` invariant holds.

**Checks:**

1. **Default-path byte-identity** — re-run the `AtxImplDiscover` determinism slice and
   the factory golden+digest slice with NO new flags; assert all green vs. S7-0 baseline.
   Passing test names: `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
   `FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens.

2. **`seq==parallel` on the build profile** — run the build profile twice (once
   sequential workers=1, once parallel workers=N); assert the factory digest and the
   `AtxImplDiscover` slice digest are identical across both runs
   (F1: digest-invariant under worker count).

3. **Pester test green** — `build-tradeable-alphas.Tests.ps1` passes.

4. **Record** in the research doc: "Default path byte-identical: YES (goldens green).
   seq==parallel on build profile: YES (digest `<hex>`). Build profile is the explicit
   opt-in, not a new baseline."

**Accept:**
- Zero regressions in any existing test suite.
- `seq==parallel` confirmed on the build profile.
- Research doc records the determinism note.

---

## North-star acceptance (ROADMAP S7 row)

On `work/accept/panel.bin`, end-to-end produce ≥1 admitted alpha that is simultaneously:
- WQ-fit: fitness ≥ 1.0
- In-sample robust: IS Sharpe > 0
- Net-of-10bps OOS Sharpe > 0.8 (vs a42's 0.37 pre-uplift baseline)
- Deployed with a SIGN-CORRECT non-empty book and a sane participation footprint

OR a documented frontier naming the binding constraint (honest null is a valid outcome).

---

## Risks / guardrails

| Risk | Guardrail |
|---|---|
| S3/S4/S5/S6 engine fields not yet landed when S7 starts | Wire what exists; mark missing fields with `// S7-TODO: depends on SN` and note gap in research doc. Do not block on absent sprints. |
| New flags accidentally shift the golden on the default path | Every new `RunConfig` field MUST default to the inert value. S7-7 catches any drift. |
| The build profile accidentally becomes the golden re-baseline | The BUILD PROFILE is the explicit non-default; goldens are always the no-new-flags path. `oracle.hpp` is untouched. |
| 10-min discover timeout | Staged execution in `build-tradeable-alphas.ps1` (`-Stage augment,discover` then `-Stage pipeline`) — exact precedent from the close-discovery-loop session. |
| `augment_panel` + `adv_windows` not yet in config (S5 left `SPRINT7-WIRES:`)  | S7-3 resolves all markers; if `stage_panel.cpp` does not compile without them, S7-3 is a hard prerequisite for S7-5. |
| Sign-correct fix (S6) not yet merged | S7-4 wires the default into `run_all`. If S6 is absent, document the sign-flip in the research doc and proceed; the build profile emits an inverted book (degraded, not broken). |
| Net OOS Sharpe < 0.8 — north-star not met | Record frontier + binding gate. That is the valid honest output. Do NOT loosen gates to force an admit. |

---

## Bench / acceptance

- **Determinism:** `AtxImplDiscover` slice + factory golden+digest slice — all green,
  zero regressions (S7-0, S7-7).
- **Wire correctness:** `config_parse_test` — all new flags round-trip (S7-1, S7-2, S7-3).
- **DryRun / Pester:** `build-tradeable-alphas.Tests.ps1` — green in CI (S7-5, S7-7).
- **End-to-end:** `build-tradeable-alphas.ps1` runs to completion on the real panel and
  emits the ranked table (S7-6).
- **North-star:** ≥1 alpha with net OOS Sharpe > 0.8 + sign-correct book, OR documented
  frontier. Speedup vs ~21-min baseline reported in the research doc (S7-6).

Sprint discipline: [../docs/sprint.md](../docs/sprint.md). Implementation quality:
[../docs/implementation-quality.md](../docs/implementation-quality.md).
