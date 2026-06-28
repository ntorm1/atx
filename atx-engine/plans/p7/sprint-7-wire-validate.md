# Sprint 7 — Wire Everything + Dev-Panel Validate

**Goal:** thread every p7 feature-sprint engine knob through the shared CLI hub
(`config.hpp`, `config.cpp`, `stage_discover.cpp`, `stage_run.cpp`), run the
**dev-panel smoke** (`-Profile smoke`, ≤ 5 min) to confirm end-to-end wiring, and
write a short research/validation note. Set up — but explicitly DO NOT run — the
operator **V1** full-panel prod milestone command.

**Owns (exclusive):** `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`,
`atx-impl/src/stage_discover.cpp`, `atx-impl/src/stage_run.cpp`,
`scripts/build-tradeable-alphas.ps1` (extend `-Profile smoke`),
`scripts/tests/build-tradeable-alphas.Tests.ps1` (Pester update),
NEW `atx-impl/research/<date>-p7-wire-validate.md` (research/validation note).

**Runs LAST** — depends on S1–S6 being merged. If a feature sprint is absent,
wire what exists and note the gap in the research doc. S7 is the ONLY sprint that
touches the four reserved hub files.

**Determinism contract (A):** every new CLI flag DEFAULTS to its inert value so the
existing `AtxImplDiscover` determinism slice + factory golden+digest slice pass
**byte-identical** when no new flag is passed
(`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens — all green,
`oracle.hpp` untouched). The smoke profile turns the opt-ins ON as an explicit,
documented non-default profile; it is NEVER a golden re-baseline.

**Depends-on:** S1–S6 (disjoint-file owners per `ROADMAP.md`). S7 is the
only sprint that touches the four reserved hub files. The V1 operator prod
run (full-panel, hours) is a MILESTONE handed off after this sprint, never a
gate inside it.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear
module-level intent, grouped constants/types/APIs, explicit ownership and lifecycle
rules, named error contracts, and concise comments that explain invariants,
non-obvious control flow, or domain semantics. Do not follow weaker patterns that
expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done
until the public API, implementation, tests, docs/ledger row, and build/test gate
are complete. Do not leave TODO placeholders, fake success paths, unused APIs, or
untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership,
ordering, crash/recovery semantics, and tricky domain rules. Do not comment obvious
assignments or wrap every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## Verified baseline — what p6-S7 already wired (do not re-add)

The following flags are already parsed in `config.cpp` and wired in
`stage_discover.cpp`. S7 MUST NOT re-add them; verify they are present and
undisturbed:

| CLI flag | `config.cpp` arm | `stage_discover.cpp` wire |
|---|---|---|
| `--deflate-selection` | line ~40 (valueless bool) | `fcfg.search.deflate_selection` (line ~437) |
| `--enable-wrap-in-op` | line ~37 (valueless bool) | `sc` construction (line ~949) |
| `--admit-seeds-presearch` | line ~41 (valueless bool) | `fcfg` construction (line ~434) |
| `--turnover-penalty-slope` | line ~235 (parse_double) | `sc.fitness.turnover_penalty_slope` (line ~873) |
| `--max-turnover-target` | line ~236 (parse_double) | `sc.fitness.max_turnover_target` (line ~874) |
| `--protect-seed-elites` | line ~41 (valueless bool) | `sc.protect_seed_elites` (line ~869) |
| `--mutate-seed-copies` | line ~42 (valueless bool) | `sc.mutate_seed_copies` (line ~870) |
| `--min-viable-raw` | line ~237 (parse_double) | `sc.min_viable_raw` (line ~871) |
| `--cost-bps-admit` | line ~232 (parse_double) | `gc.rt_cost_bps` (line ~420) |
| `--min-holding-days` | line ~233 (parse_double) | `gc.min_holding_days` (line ~421) |
| `--cost-max-turnover` | line ~234 (parse_double) | `gc.max_turnover` guard (line ~422) |
| `--adv-windows` | lines ~118–134 (comma-list parser) | `cfg.adv_windows` in `stage_panel` via SPRINT7-WIRES |
| `--augment-panel` | line ~43 (valueless bool) | `cfg.augment_panel` in `stage_panel` via SPRINT7-WIRES |
| `--conviction` | line ~30 (valueless bool) | `cfg.conviction` field (combine stage reads it) |
| S7-4 `run_all` position-mode default | — | `stage_run.cpp:96–98` guard already in place |

> **Observation (confirmed 2026-06-27):** `config.hpp` already carries `min_dsr`,
> `max_pbo`, `adv_windows`, `augment_panel`, `conviction`, `turnover_penalty_slope`,
> `max_turnover_target`, `protect_seed_elites`, `mutate_seed_copies`,
> `cost_bps_admit`, `min_holding_days`, `cost_max_turnover`. The S7-1/S7-2/S7-3/S7-4
> wiring is already present as labeled comments (`// S7-1`, `// S7-2`, `// S7-3`,
> `// S7-4`). S7's coding units below therefore focus on the **remaining new p7 knobs**
> that S4 and S5 expose but that are NOT yet in `config.hpp` / `stage_discover.cpp`.

---

## Dependency map — flag → feature sprint → engine field

| CLI flag (S7 adds or confirms) | Feature sprint | Engine config field | Inert default |
|---|---|---|---|
| `--min-dsr` | S1 T1 | `FactoryConfig.min_dsr` (`stage_discover.cpp:432`) | `0.5` (already wired from p6) |
| `--max-pbo` | S1 T2 | `FactoryConfig.max_pbo` (`stage_discover.cpp:434`) | `1.0` (already wired) |
| `--require-split-stable` | S1 T3 | NEW `GateConfig.require_split_stable` → `gc` | `false` |
| cumulative trial-count | S1 T4 | automatic (`factory.cpp` threading; no new flag) | automatic |
| `--tradeable-profile` | S4 T4 | `tradeable_fitness_cfg()` → `sc.fitness` | `false` (off = inert FitnessCfg{}) |
| `--ema-alpha` | S4 T1 | `EmaDecayPolicy.ema_alpha` (combine stage) | `1.0` (identity; byte-identical) |
| `--capacity-aum` | S4 T2/T3 | `compute_capacity_vector` target AUM; replaces 1.0 stub | `0.0` (off; stub preserved) |
| `--capacity-curve-report` | S4 T3 | `emit_capacity_scorecard` → report KV | `false` (off) |
| `--kelly-fraction` | S5 | NEW `RunConfig.kelly_fraction`; `stage_combine` / `stage_optimize` | `1.0` (full-Kelly = no change) |
| `--conviction-weight-floor` | S5 | NEW `RunConfig.conviction_weight_floor` | `0.0` (off; conviction block inert) |
| `--short-interest` / `--augment-out` / `--si-publication-lag` | S2 T1 (track-b landing) | already in `RunConfig` if track-b merged | (check at wiring time) |
| `--augment-panel` / `--adv-windows` | S2/p6-S5 | already wired (see baseline table above) | (no-op if flags absent) |

> **S2 note:** the track-b branch (`worktree-track-b-information-structure`) adds
> `--short-interest`, `--augment-out`, `--si-publication-lag` to `RunConfig` and a new
> `atx augment` subcommand. If S2 merged before S7 opens, those fields exist and need no
> work. If S2 is absent, document the gap in the research note and skip those rows.
>
> **S5 note:** the `conviction` bool is already wired (p6-S7 baseline). The new
> p7-S5 knobs are `--kelly-fraction` and conviction-weight fine-tuning flags defined in
> `combine/conviction.hpp` / `risk/kelly_sizing.hpp`. S7 adds `RunConfig` fields and CLI
> arms for these if S5 has landed.
>
> **Existing wires already in place:** all `// S7-1`, `// S7-2`, `// S7-3`, `// S7-4`
> labeled lines in `config.hpp`, `config.cpp`, `stage_discover.cpp`, `stage_run.cpp`
> are confirmed present. S7-1 through S7-4 below are additive (new p7 knobs only) or
> verification/hardening passes.

---

## Tasks

### S7-0 — Open ledger + verify green baseline

**Goal:** create the sprint ledger (`phase-s7-progress.md`), confirm all existing
determinism goldens and the discover digest slice pass on the current tree before S7
opens any hub file. Record the passing SHA as the S7 baseline.

**Wiring (file:line):**
- NEW `atx-engine/plans/p7/phase-s7-progress.md` — marker commit only.
- Read-only: `atx-engine/tests/factory/` (the `AtxImplDiscover` / `FactoryOos` suites);
  `oracle.hpp` untouched.

**Determinism:** this task makes NO source edits. It only runs the existing test suite
and records the result.

**Accept:**
- Marker commit lands: `docs(p7-s7-0): open sprint-7 wire-validate ledger`.
- `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` — green.
- `FactoryOos.MineIntoOffPathDigestUnchanged` — green.
- `AtxImplDiscover` determinism slice (seq==parallel) — green.
- All existing Pester tests (`build-tradeable-alphas.Tests.ps1`) — green.
- Baseline SHA recorded in the ledger header.

---

### S7-1 — Thread S1 deflation-gate knobs: `--require-split-stable`

**Goal:** wire the one S1 deflation-gate knob that is NOT yet threaded from
`GateConfig` through the CLI hub: `require_split_stable`. The other S1 fields
(`min_dsr`, `max_pbo`) already exist in `RunConfig` and `stage_discover.cpp` from
p6 (confirmed above). Add `--require-split-stable` as a new CLI valueless bool,
add `RunConfig.require_split_stable`, and wire it into `gc` in
`stage_discover.cpp`. Document the cumulative-trial-count behavior (it is automatic
via S1-4's `factory.cpp` change — no new flag required; document in the research
note).

**Wiring (file:line):**

- `config.hpp` — add after `max_pbo` field (around line 101):
  ```cpp
  // --require-split-stable (S7-1, S1 T3): when true, GateConfig::require_split_stable
  // is set, causing AlphaGate::admit to return RejectSplitUnstable for any candidate
  // whose holdout halves disagree in sign. false (default) => GateConfig inert, byte-identical.
  bool require_split_stable = false; // --require-split-stable (S1 T3 gate; false = inert)
  ```
- `config.cpp` — add to the valueless-bool fast path (alongside `protect-seed-elites`
  at line ~41):
  ```cpp
  if (flag == "require-split-stable") { cfg.require_split_stable = true; return atx::core::Ok(); } // S7-1
  ```
- `stage_discover.cpp` — in `run_discover_gated`, in the `gc` construction block
  (alongside `gc.rt_cost_bps` at line ~420), add:
  ```cpp
  gc.require_split_stable = cfg.require_split_stable; // S7-1 S1 T3
  ```
  Note: `GateConfig::require_split_stable` must be defined by S1 before this compile.
  If S1 is absent at merge time, guard with `// S7-TODO: depends on S1` and note the gap.

**Cumulative trial-count (S1-4 — automatic, no new flag):** S1-4 removes the
`static_cast<void>(trial_count)` no-op in `factory.cpp:983` and wires real cumulative N
into the cascade bound. This is automatic — the factory reads `trial_count` from its own
internal state. No CLI flag is needed; no `RunConfig` field is needed. Document this in
the research note: "trial-count tightening is automatic when S1-4 is merged; no new flag."

**Determinism (inert default):** `require_split_stable = false` → the
`require_split_stable` guard in `AlphaGate::admit` (S1-3) never fires → byte-identical
to today. The `min_dsr` and `max_pbo` fields already exist at their inert defaults
(`0.5` and `1.0` respectively from p6); S7-1 does not touch them.

**Accept:**
- `AtxImplDiscover` determinism slice stays byte-identical (inert defaults).
- CLI parse round-trip unit test: `--require-split-stable` parses to
  `cfg.require_split_stable == true`; omitted → `false`.
- Config-file round-trip: `require-split-stable=true` in a config file parses correctly.
- Wire test (compile-time): `gc.require_split_stable` is set from `cfg.require_split_stable`
  and confirmed in a `run_discover_gated` call on a toy panel.
- If S1 is absent: `// S7-TODO: depends on S1` marker present; gap recorded in ledger row.

---

### S7-2 — Thread S4 capacity/decay/tradeable-profile knobs

**Goal:** add four new p7-S4 CLI knobs that S4 exposed as engine-layer helpers but that
are not yet in `RunConfig` or the CLI hub:
1. `--tradeable-profile` — calls `tradeable_fitness_cfg()` from `factory/fitness.hpp`
   (S4-4) to set `sc.fitness` to the recommended tradeable defaults.
2. `--ema-alpha` — wires `EmaDecayPolicy.ema_alpha` into the combine stage's weight policy.
3. `--capacity-aum` — the target AUM for `compute_capacity_vector` (S4-2); replaces the
   constant-1.0 stub in `stage_combine.cpp` when `> 0`.
4. `--capacity-curve-report` — triggers `emit_capacity_scorecard` (S4-3) and appends
   the curve to the report KV block.

**Wiring (file:line):**

**`config.hpp`** — add after the existing capacity/corr fields (around line 235):
```cpp
// -- S7-2: p7-S4 tradeable-profile / EMA-decay / capacity-curve knobs (all inert at defaults) --

// --tradeable-profile (S7-2, S4-T4): when true, stage_discover applies
// factory::tradeable_fitness_cfg() to sc.fitness — sets turnover_penalty_slope=2.0,
// max_turnover_target=0.20. Inert default: false (FitnessCfg{} unchanged, byte-identical).
bool tradeable_profile = false;

// --ema-alpha (S7-2, S4-T1): EMA decay factor for EmaDecayPolicy in the combine stage.
// 1.0 (identity) = inert: EmaDecayPolicy with ema_alpha=1.0 is byte-identical to
// the stateless WeightPolicy path (S4-T1 determinism contract).
double ema_alpha = 1.0;

// --capacity-aum (S7-2, S4-T2/T3): target AUM for per-alpha capacity vector computation.
// 0.0 (default) = off: the existing constant-1.0 stub in stage_combine is preserved.
// When > 0, compute_capacity_vector is called (S4-T2) and fills the real capacity vector.
double capacity_aum = 0.0;

// --capacity-curve-report (S7-2, S4-T3): when true, emit_capacity_scorecard is called
// after the combine fit and the curve is appended to the report KV block. false = inert.
bool capacity_curve_report = false;
```

**`config.cpp`** — add parse arms:
- Valueless bool fast path: `tradeable-profile`, `capacity-curve-report` (alongside
  `augment-panel` at line ~43).
- Numeric block: `ema-alpha` and `capacity-aum` via `parse_double` (alongside existing
  numeric arms, e.g. after `capacity-floor` at line ~262).

**`stage_discover.cpp`** — in the `sc` / `fcfg` construction block:
```cpp
// S7-2: apply tradeable fitness profile if requested. tradeable_fitness_cfg() returns
// the recommended FitnessCfg with slope=2.0/target=0.20; the individual
// --turnover-penalty-slope / --max-turnover-target overrides still win if set_flags
// contains them (S7-1 baseline wires already apply; tradeable_profile is applied first,
// explicit flags override after).
if (cfg.tradeable_profile) {
    sc.fitness = factory::tradeable_fitness_cfg();
    // Now re-apply explicit CLI overrides (they won at parse time in set_flags).
    if (cfg.set_flags.count("turnover-penalty-slope"))
        sc.fitness.turnover_penalty_slope = cfg.turnover_penalty_slope;
    if (cfg.set_flags.count("max-turnover-target"))
        sc.fitness.max_turnover_target = cfg.max_turnover_target;
}
```
(Insert after the existing S7-1 `sc.fitness.*` wires at line ~873.)

**`stage_run.cpp` / `stage_combine.cpp`** — EMA-decay and capacity-curve wiring touches
the combine driver. The owned-file constraint for S7 is the four hub files; if
`stage_combine.cpp` is not in S7's exclusive list, coordinate or check whether it is
unowned by other sprints (S6 owns `stage_panel`, `serialize_panel`, `stage_load` —
`stage_combine.cpp` is not listed). Wire `cfg.ema_alpha` into the `EmaDecayPolicy`
construction site in `stage_combine.cpp` (if it exists post-S4); wire `cfg.capacity_aum`
to call `compute_capacity_vector` in place of the 1.0 stub (line ~589 of
`stage_combine.cpp`). Wire `cfg.capacity_curve_report` to call `emit_capacity_scorecard`
after combine. If `stage_combine.cpp` requires a co-owner check, document the seam in
the ledger row.

**Determinism (inert default):**
- `tradeable_profile=false` → `sc.fitness = FitnessCfg{}` (unchanged; byte-identical).
- `ema_alpha=1.0` → `EmaDecayPolicy{base, 1.0}` is byte-identical to the stateless path.
- `capacity_aum=0.0` → the `capacity_floor/target_aum` guard in `stage_combine.cpp`
  does not fire → constant-1.0 stub preserved → byte-identical.
- `capacity_curve_report=false` → `emit_capacity_scorecard` not called → byte-identical.

**Accept:**
- `AtxImplDiscover` slice stays byte-identical (all four flags at inert defaults).
- CLI parse round-trips: `--tradeable-profile` → `cfg.tradeable_profile==true`;
  `--ema-alpha 0.7` → `cfg.ema_alpha==0.7`; `--capacity-aum 1e8` → `cfg.capacity_aum==1e8`;
  `--capacity-curve-report` → `cfg.capacity_curve_report==true`.
- Wire test: `--tradeable-profile` → `sc.fitness.turnover_penalty_slope == 2.0` and
  `sc.fitness.max_turnover_target == 0.20`; `--tradeable-profile --turnover-penalty-slope 3.0`
  → `sc.fitness.turnover_penalty_slope == 3.0` (explicit override wins).
- If S4 is absent: mark with `// S7-TODO: depends on S4`; note gap in ledger.

---

### S7-3 — Thread S5 conviction/Kelly knobs

**Goal:** wire the p7-S5 knobs that `combine/conviction.hpp` and `risk/kelly_sizing.hpp`
expose. The `--conviction` bool is already wired (p6 baseline). S5 adds:
- `--kelly-fraction` — fractional-Kelly scaling applied at the book level.
- `--conviction-weight-floor` — minimum conviction weight before a name is zeroed out.

Add the new `RunConfig` fields, parse arms, and wire into the combine stage.

**Wiring (file:line):**

**`config.hpp`** — add after `conviction` (line ~240):
```cpp
// --kelly-fraction (S7-3, S5): fractional-Kelly multiplier applied to the final
// combined book weights. 1.0 (default) = full Kelly = no change = byte-identical.
// Values in (0,1) shrink the book toward cash; 0.0 would zero it (not useful; no guard needed).
double kelly_fraction = 1.0;

// --conviction-weight-floor (S7-3, S5): minimum per-name conviction weight to retain
// in the conviction-scaling block (stage_combine). Names with conviction < floor are
// zeroed before renormalization. 0.0 (default) = all names kept = inert.
double conviction_weight_floor = 0.0;
```

**`config.cpp`** — add `parse_double` arms: `kelly-fraction` and
`conviction-weight-floor` (near the numeric block, after the conviction bool arm at
line ~30).

**`stage_combine.cpp` (or the conviction call site)** — at the conviction block
(`cfg.conviction` guard), pass `cfg.kelly_fraction` and `cfg.conviction_weight_floor`
into the conviction API surface (the exact field names depend on what S5
`conviction.hpp` / `kelly_sizing.hpp` exposes; use the field names S5 defined — if they
differ, reconcile and note in the ledger row).

**Determinism (inert default):**
- `kelly_fraction=1.0` → the Kelly multiplier is the identity → book weights unchanged → byte-identical.
- `conviction_weight_floor=0.0` → the floor zeroes nothing → byte-identical to
  the `--conviction` path without the floor knob.
- At the default `conviction=false` (p6 baseline), neither field is consulted → doubly inert.

**Accept:**
- `AtxImplDiscover` slice stays byte-identical (inert defaults).
- CLI parse round-trips: `--kelly-fraction 0.5` → `cfg.kelly_fraction==0.5`;
  `--conviction-weight-floor 0.05` → `cfg.conviction_weight_floor==0.05`.
- Config-file round-trip for both flags.
- Wire test: `--conviction --kelly-fraction 0.5` on a toy combine fixture produces a
  combined book whose gross leverage equals `0.5 × (book without kelly-fraction)`.
- If S5 is absent: `// S7-TODO: depends on S5` markers; gap in ledger.

---

### S7-4 — Thread S2 information-breadth flags (conditional on track-b landing)

**Goal:** verify and complete the S2 CLI wiring if the track-b branch
(`worktree-track-b-information-structure`) has been merged. The track-b branch adds
`--short-interest`, `--augment-out`, `--si-publication-lag` to `RunConfig` and a new
`atx augment` subcommand (`stage_augment.cpp`). S7-4 audits whether these fields
survived the S2 merge into main, resolves any conflict residuals, and records the
result.

This unit is conditional:
- **If S2 merged:** audit that `config.hpp` carries `short_interest`, `augment_out`,
  `si_publication_lag`; that `config.cpp` parses `--short-interest`, `--augment-out`,
  `--si-publication-lag`; that `stages.hpp` declares `run_augment` and
  `dispatch.cpp` routes the `"augment"` subcommand. Fix any omission. Record "S2 wired:
  YES" in the research note.
- **If S2 not merged:** document "S2 absent: gap recorded; not blocking S7 smoke."

**Wiring (file:line, S2 present path):**

- `config.hpp` (if absent): add after `augment_panel` (line ~190):
  ```cpp
  // -- S2/track-b augment subcommand (FINRA short-interest) --
  std::string short_interest;         // --short-interest <csv-path>
  std::string augment_out;            // --augment-out <bin-path>
  long        si_publication_lag = 2; // --si-publication-lag (days; default 2)
  ```
- `config.cpp` (if absent): add string parse arms for `short-interest` and `augment-out`;
  `parse_long` arm for `si-publication-lag`.
- `dispatch.cpp` / `stages.hpp`: ensure `"augment"` routes to `run_augment` (the
  `stage_augment.cpp` entry point). S7 does NOT edit `stage_augment.cpp` (S2 owns it).

**Determinism (inert default):** `short_interest.empty() == true` (default) → `run_augment`
is never called on a normal `discover` or `panel` invocation → byte-identical. The new
subcommand is only active when `atx augment` is invoked explicitly.

**Accept:**
- If S2 merged: `Augment.*` (4 tests), `FinraShort.*` (4 tests) green; `--short-interest`
  parses; `atx augment --help` does not crash.
- If S2 absent: gap documented in ledger row and research note; no test failures introduced.
- `AtxImplDiscover` determinism slice byte-identical regardless (S2 wiring is on a separate
  subcommand path).

---

### S7-5 — Dev-panel smoke + `build-tradeable-alphas.ps1` `-Profile smoke` + Pester

**Goal:** extend `scripts/build-tradeable-alphas.ps1` with a `-Profile smoke`
path that runs the whole pipeline on `work/dev/dev-panel.bin` (600×501, the cached
dev panel) with loose gates and a small population so the entire discover →
combine → optimize → report chain completes in ≤ 5 minutes. Update the Pester
test to cover the new `-Profile smoke` argv construction. Confirm the smoke run
exits green, confirming end-to-end wiring — NOT edge correctness.

**Script changes (`scripts/build-tradeable-alphas.ps1`):**

The script already has `-Profile smoke|prod` per the verified p6-S7 harness. If
the current p7 script diverges from this, the update adds:

```
-Profile smoke  →  --panel work/dev/dev-panel.bin
                   --population 40 --generations 4
                   --min-sharpe 0.0 --min-fitness 0.0 --max-turnover 1.0
                   --workers <auto>
                   (fast loose gates: guaranteed admits on the dev panel)
                   (full knob list: threads the new p7 flags in their inert defaults)

-Profile prod   →  --panel work/accept/panel.bin (unchanged from p6-S7)
                   <full p7 knob set; operator-driven; NOT run in this sprint>
```

The smoke profile explicitly passes ALL new p7 CLI flags at their inert defaults
(so wiring is exercised without changing any output):
```
--require-split-stable false is omitted (inert = off)
--tradeable-profile omitted (inert = off)
--ema-alpha omitted (inert = 1.0)
--capacity-aum omitted (inert = 0.0)
--kelly-fraction omitted (inert = 1.0)
```
This ensures the smoke run validates CLI parsing and wiring without triggering any
opt-in behavior.

**`-DryRun` mode:** compose the argv arrays and `Write-Host` them; do NOT call the CLI.

**`-Stage` parameter:** accept `augment|discover|pipeline|all`; default `all`.
Smoke profile: `discover` and `pipeline` are fast enough to chain; no staged split needed.

**Pester test (`scripts/tests/build-tradeable-alphas.Tests.ps1`):**

Add (or update) tests that:
- `DryRun -Profile smoke`: script outputs argv containing the dev-panel path, loose gate
  values (`min-sharpe 0.0` or equivalent), and the new p7 flag set (all at inert values).
- `DryRun -Profile prod`: script outputs argv containing the full prod panel path and the
  active p7 knob set (`--tradeable-profile`, `--require-split-stable`, etc.).
- Does NOT require the binary, the real panel, or a compile.
- Validates that every expected flag appears in the composed argv (pattern-match on
  `-match`).

**Determinism:** the smoke run exercises the pipeline on the dev panel. It produces no
goldens. All flags are at inert defaults so the discover digest is byte-identical to
the pre-S7 baseline on the same panel (the smoke run's primary claim is
"pipeline runs; no segfault; gate returns ≥ 1 admit with loose gates").

**Accept:**
- `-DryRun -Profile smoke` prints the full argv without error.
- Pester (`build-tradeable-alphas.Tests.ps1`) — all tests green (including new
  smoke/prod DryRun tests).
- `build-tradeable-alphas.ps1 -Profile smoke` runs end-to-end on `work/dev/dev-panel.bin`
  in ≤ 5 minutes and exits 0. At least 1 alpha admitted (loose gates).
- No hour-long prod run is run. The prod profile is DryRun-verified only.

---

### S7-6 — Determinism slice + seq==parallel confirmation + research/validation note

**Goal:** confirm that S7's complete wiring (all new inert defaults) has not shifted any
existing golden, that the default `atx discover` is seq==parallel byte-identical, and
write the research/validation note documenting the sprint outcome and the V1 operator
handoff.

**Checks:**

1. **Default-path byte-identity (post-S7):** re-run the `AtxImplDiscover` determinism
   slice and the factory golden+digest slice with NO new flags asserted (all at inert
   defaults). Assert all green vs. the S7-0 baseline SHA.
   Passing test names:
   - `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`
   - `FactoryOos.MineIntoOffPathDigestUnchanged`
   - OOS goldens

2. **`seq==parallel` — default path:** run `atx discover` with `--workers 1` and
   `--workers N` (where N is `std::thread::hardware_concurrency()`) on the dev panel
   with no new flags. Assert the factory digest is identical across both runs.

3. **`seq==parallel` — smoke-profile path (opt-in knobs active):** if S1's
   `require_split_stable`, S4's `tradeable_profile`, or S5's `kelly_fraction` are active
   in the smoke profile, run the smoke profile with `--workers 1` and `--workers N`
   and assert digest equality. (The p7 determinism contract: opt-in flags may change the
   output but must be seq==parallel invariant.)

4. **Pester green:** `build-tradeable-alphas.Tests.ps1` all pass.

**Research/validation note** (`atx-impl/research/<date>-p7-wire-validate.md`):

The note covers:
- **Wiring audit:** for each p7 sprint (S1–S6), one row: flag threaded / field confirmed /
  gap (if sprint absent). Example:
  ```
  | S1 require-split-stable | threaded → gc.require_split_stable | green |
  | S1 cumulative trial-count | automatic (S1-4 factory.cpp) | no flag needed |
  | S4 tradeable-profile | threaded → tradeable_fitness_cfg() | green |
  | S4 ema-alpha | threaded → EmaDecayPolicy | green |
  | S4 capacity-aum | threaded → compute_capacity_vector | green |
  | S5 kelly-fraction | threaded → kelly_sizing | green |
  | S2 short-interest | [present/absent] | [green/gap] |
  ```
- **Determinism confirmation:** "Default path byte-identical: YES (goldens green,
  SHA `<hex>`). seq==parallel on default path: YES (digest `<hex>`). seq==parallel
  on smoke-profile: YES (digest `<hex>`)."
- **Smoke run result:** dev panel path, date-range, worker count, wall time, admit count,
  loosest-gate knobs used. Any flags whose wiring was not exercised (absent sprints).
- **V1 operator handoff** (documented, NOT run):
  ```powershell
  # After S1–S5 land on main, run once, overnight:
  .\scripts\build-tradeable-alphas.ps1 -Profile prod -Stage augment,discover `
      -WorkDir work/tradeable-p7 -Workers <auto>
  # Then:
  .\scripts\build-tradeable-alphas.ps1 -Profile prod -Stage pipeline `
      -WorkDir work/tradeable-p7
  # Output: atx-impl/research/<date>-production-book-results.md
  # Scorecard: book-level net-of-cost OOS Sharpe, DSR (cumulative-N), PBO,
  #            CPCV, walk-forward, capacity curve, N_eff/IR breadth.
  # North star: book net-of-10bps OOS Sharpe > 1.0 with ≥5 admitted alphas.
  # If missed: reject_histogram dominant bucket names the next sprint target.
  ```
- **Gap list:** any sprint that was absent at S7 merge time; the corresponding `// S7-TODO`
  markers in hub files; the plan for closing them in a follow-on S7b or operator-driven run.

**Accept:**
- Zero regressions in any existing test suite.
- `seq==parallel` confirmed on default path and smoke-profile path.
- Research note committed (not a placeholder — all rows filled in or explicitly marked
  as absent-sprint gaps).
- V1 command documented; NOT executed.

---

## Risks / guardrails

| Risk | Guardrail |
|---|---|
| S1/S4/S5 engine fields not yet landed when S7 starts | Wire what exists; mark each missing field with `// S7-TODO: depends on SN` and note gap in ledger row + research note. Do not block on absent sprints. |
| New flags accidentally shift the golden on the default path | Every new `RunConfig` field MUST default to the inert value. S7-6 catches any drift. `oracle.hpp` untouched. |
| p6-S7 wiring already present — double-wiring collision | Read the confirmed baseline table above before editing. If a `// S7-N` label already exists, do not re-add; verify the existing wire is correct and mark the unit as "wire confirmed." |
| `tradeable_profile` overrides `turnover_penalty_slope` set by an explicit CLI flag | Wire tradeable defaults first, then re-apply `set_flags` explicit overrides (see S7-2 pattern). |
| `stage_combine.cpp` is not in S7's explicit "Owns" list | Check S6's owned file list. If `stage_combine.cpp` is unowned by other sprints, S7 can edit it; document the seam. If ambiguous, add a note in the ledger and coordinate before editing. |
| S2 track-b conflicts with p6/p7 hub files | S2's merge conflict guidance (sprint-2-information-breadth.md §S2-1) resolves additively. If conflicts remain, keep both field sets; never drop existing p6 or p7 fields. |
| Smoke run takes > 5 min on dev panel | `--population 40 --generations 4` on a 600×501 panel with loose gates is the validated budget from the close-discovery-loop session precedent. If it exceeds 5 min, halve population further; document in ledger. |
| `ema_alpha=1.0` is not truly byte-identical due to floating-point order | S4-T1 guarantees `EmaDecayPolicy{base, 1.0}` is byte-identical to the stateless path (per S4 acceptance criteria). Verify this holds in the CLI-wired path; if not, file a regression against S4. |
| V1 prod run is accidentally triggered inside the sprint | The `-Profile prod` DryRun-only gate in Pester prevents this. The research note documents V1 as "run once after S1–S5 land." The sprint explicitly forbids running it. |
| No hour-long prod run is a gate | Enforced by design. Accept criteria for every unit are unit tests + ≤ 5 min smoke. No workaround permitted. |

---

## Bench / acceptance (sprint close)

| Criterion | Target | Evidence |
|---|---|---|
| Baseline goldens green pre-edit | zero failures | S7-0 |
| `require-split-stable` parse round-trip | `cfg.require_split_stable==true` | S7-1 parse test |
| `tradeable-profile` → fitness defaults | `slope==2.0`, `target==0.20` | S7-2 wire test |
| `ema-alpha` parse + inert identity | `1.0`→byte-identical combine | S7-2 parse + identity test |
| `capacity-aum` parse + inert off | `0.0`→stub preserved | S7-2 parse test |
| `kelly-fraction` parse + inert identity | `1.0`→book unchanged | S7-3 parse + identity test |
| `conviction-weight-floor` parse + inert | `0.0`→nothing zeroed | S7-3 parse test |
| S2 augment wiring (conditional) | fields present or gap documented | S7-4 audit |
| `DryRun -Profile smoke` argv correct | smoke flags in output | S7-5 Pester |
| `DryRun -Profile prod` argv correct | prod flags in output | S7-5 Pester |
| Pester all tests green | 45+ existing + new tests | S7-5/S7-6 |
| Smoke run ≤ 5 min, exits 0, ≥ 1 admit | wall time < 300 s | S7-5 execution |
| Post-S7 default path byte-identical | existing goldens green vs S7-0 SHA | S7-6 |
| seq==parallel (default path) | discover digest identical workers 1 vs N | S7-6 |
| seq==parallel (smoke-profile opt-ins) | digest identical workers 1 vs N | S7-6 |
| Research note committed (not placeholder) | all rows filled or gap-marked | S7-6 |
| V1 command documented, NOT run | command in research note; no prod output | S7-6 |

Sprint discipline: [../docs/sprint.md](../docs/sprint.md). Implementation quality:
[../docs/implementation-quality.md](../docs/implementation-quality.md).
