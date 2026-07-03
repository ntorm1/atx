# Sprint 5 — Wire, Deflate & Validate (the CAPSTONE)

**Goal:** thread the full new-knob surface that S1–S4 exposed (plus the deferred p7-S7 carry-forwards)
through the shared CLI hub (`config.hpp`, `config.cpp`, `stage_discover.cpp`, `stage_run.cpp`); make
deflation **blocking** in two places it is currently inert — the SEARCH objective (cumulative trial
count `N` folded into the fitness bar) and the LIVE library path (`GateDeflation` wired into
`library::verdict_for`, closing the p7-S1 dead-code carry-forward); build an automated **robustness
battery** that rejects the degenerate-alpha class of artifact (the measured `1/price` dimensional
tilt that passed every statistical gate); assemble the full stage graph
(augment→discover→riskmodel→combine→metabook→optimize→report) behind a `build-megaalpha-book.ps1`
harness; and produce the **first end-to-end V1 mega-book scorecard on real data**. Every new capability
defaults inert ⇒ the no-flag path is **byte-identical**. S5 runs **LAST** and is the ONLY sprint that
touches the four reserved hub files (mirrors the p6/p7 S7-last contract).

**Owns (exclusive):**
`atx-impl/src/config.hpp`, `atx-impl/src/config.cpp`, `atx-impl/src/stage_discover.cpp`,
`atx-impl/src/stage_run.cpp` (the CLI hub — reserved for S5 across all of p8),
`atx-engine/include/atx/engine/library/library.hpp` (`GateDeflation` → `verdict_for` seam),
`atx-engine/src/factory/factory.cpp` + `atx-engine/include/atx/engine/factory/fitness.hpp`
(cumulative-N deflation in the SEARCH objective; the `fitness.hpp` config field only — see the
ownership note below),
NEW `atx-engine/include/atx/engine/eval/robustness_battery.hpp` (+ `src/eval/robustness_battery.cpp` +
tests), NEW `atx-impl/scripts/build-megaalpha-book.ps1`
(+ `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1`), NEW research-doc template
`atx-impl/research/<date>-megaalpha-book-results.md`;
tests under `atx-engine/tests/{library,factory,eval}/` and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); every OTHER sprint's owned files —
`risk/factor_model.*`, `risk/{stat_factor_model,shrinkage,eigen_adjust,specific_risk,psd_repair,dead_factor,exposures}.hpp`,
`data/factor_model_artifact.hpp`, `atx-impl/src/stage_riskmodel.{cpp,hpp}`, `atx-impl/src/diag_risk.hpp` (S1);
`fund/*`, `combine/combined_source.hpp`, `atx-impl/src/stage_metabook.{cpp,hpp}` (S2);
`learn/*`, `combine/{combiner,regime_combiner}.hpp`, `atx-impl/src/{stage_combine,stage_regime}.cpp` (S3);
`risk/{capacity,optimizer,garleanu_pedersen}.hpp`, `cost/*`, `loop/*`, `exec/*`,
**`factory/fitness.cpp`** (the impact-in-selection BODY is S4-owned), `atx-impl/src/stage_report.cpp` (S4).
S5 CONSUMES the config-struct fields the feature sprints exposed and threads their CLI flags; it does
**not** re-implement their behavior.

> **Ownership reconciliation (binding, from the ROADMAP matrix):** `factory/fitness.hpp` is
> **S5-owned** for the cumulative-N deflation FIELD; `factory/fitness.cpp`'s impact-in-selection BODY
> is **S4-owned**. These are disjoint edits coordinated through the config struct: S5 adds/reads a
> `FitnessCfg` field (header), S4 fills the impact term (source). `factory/factory.cpp` is **S5-owned**
> (the `cascade_gate_passes` cumulative-N fold + the search-objective deflation), and on `main` this
> file has already partially landed the p7-S1-4 change (see the gap table) — S5 reconciles it, it does
> not re-add it from scratch. No two sprints edit the same file.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not
follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering,
crash/recovery semantics, and tricky domain rules. Do not comment obvious assignments or wrap
every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## The gap (verified file:line — the "main" state, not the stale worktree)

The working tree is on `feat/warehouse-parity`, which is **stale** vs `main` for `factory.cpp` and
`combine/gate.hpp`. All line numbers below are verified against **`main`** unless noted; the stale
worktree is called out where it differs so the implementer branches from the right base.

| Gap | File:line (main) | Evidence |
|---|---|---|
| Deflation gate is **dead code on the live library path** — `verdict_for` stops at the corr screen; it never consults `GateDeflation` | `library/library.hpp:408-447` (`verdict_for`); `AlphaGate::admit` DOES screen DSR/PBO/split at `combine/gate.hpp:201-215` | `verdict_for` ends `worst_corr > max_pool_corr → RejectCorrelated; return Accept;` — no DSR/PBO/split branch. Every live library caller (`factory::mine_into` → `Library::admit`) bypasses the S1 deflation screens. |
| `GateDeflation` + `GateConfig{min_dsr,max_pbo,require_split_stable}` exist but are library-orphaned | `combine/gate.hpp:72-89` (`GateConfig`), `:135-143` (`GateDeflation`, `kInertDeflation`) | The capability landed p7-S1 (merge `914ae7f`); the carry-forward is that `Library::verdict_for` was never wired to it. Confirmed: `AdmitKind` (`library.hpp:116-125`) has NO `RejectDsr`/`RejectPbo`/`RejectSplit` enumerator (only `RejectPriceScale`, `RejectDsrSubwindow` from p8-track R2/R3). |
| Search selection optimizes **UNDEFLATED** fitness on the STALE worktree — cumulative `trial_count` is voided | `factory.cpp:983` on `feat/warehouse-parity`: `static_cast<void>(trial_count); // reserved for a future tightening` | On the stale branch the cascade bound ignores `N` entirely. **On `main` this is ALREADY GONE** — the void is removed and `SR*_N = expected_max_sharpe(N,V)` is folded into the keep side (`cascade_gate_passes`). S5 reconciles the two — see the architecture note. |
| The `main` cumulative-N fold LOOSENS the bound with `N` (byte-identity-preserving), it does NOT make selection stricter | `factory.cpp` `cascade_gate_passes` (main): `keep iff sr_tr*factor + SR*_N >= min_dsr` | The main code's own comment flags the spec inconsistency: the S1-4 prose said "stricter with N" but the safe arithmetic ADDS `SR*_N` to the keep side (looser, skip-set shrinks). The blocking-in-SELECTION objective S5 wants is a DIFFERENT seam — the NSGA deflation column, not the cascade skip-bound (see S5-2). |
| PBO is **advisory, not blocking** | `factory.cpp:83-154` (`finalize_run_pbo`), `stage_discover.cpp:719-734` | `rep.pbo_gate_passed` is surfaced + a breach "emits a loud warning" but `--pbo-hard-block` (R3) is the ONLY escalation and it merely sets a non-zero EXIT — it never **un-admits** an alpha. `max_pbo`'s config comment (`config.hpp:97`) says the verdict is "ADVISORY-but-RECORDED". |
| No automated robustness battery | grep `atx-engine/include/atx/engine/eval/` | `cpcv.hpp`, `pbo.hpp`, `deflated_sharpe.hpp` (the machinery) exist; there is NO `robustness_battery.hpp` — no sub-universe rerun, no noise-replacement negative control, no alternate-neutralization rerun, no parameter-perturbation harness. |
| The V1 book-level scorecard has **NEVER run** | grep `atx-impl/research/*megaalpha-book-results*.md` | zero hits. p7-S7 (which would have run the analogous prod scorecard) was **PLANNED but never started** (empty branch, 0 commits — `p7/ROADMAP.md:99` names it; `p7/sprint-7-wire-validate.md` is the plan, no ledger). p8-S5 ABSORBS and supersedes its scope. |
| New S1–S4 config fields are unreachable from the CLI | `config.hpp:28-284` (`RunConfig`) | No `risk_model`/`dead_alpha_factors`/`group_neutralize` (S1), no `metabook`/`sleeve_method` (S2), no `combine_method=stack|regime-stack` (S3), no `impact_in_selection`/`capacity_curve` (S4), no `require_split_stable`/`blocking_pbo` (deflation). `min_dsr` (`:55`) and `max_pbo` (`:101`) already exist from p6/p7; S5 does NOT re-add them. |

**The one sentence:** S1–S4 assemble the mega-book behind inert engine-config defaults; nothing is
reachable from the CLI, deflation is inert on both the search and library paths, PBO is advisory, there
is no robustness battery, and the V1 scorecard has never run. S5 threads the hub, makes deflation
blocking, builds the battery, and produces the first honest end-to-end scorecard.

---

## Architecture note — what "make deflation blocking" actually means (three distinct seams)

Deflation is NOT one switch. There are **three** places it can fire, and today it is inert or advisory
in all three that S5 owns. S5 must keep them distinct or it will double-count or break byte-identity:

1. **The live library admission path (`library::verdict_for`).** This is the p7-S1 dead-code
   carry-forward. `AlphaGate::admit` (`combine/gate.hpp:201-215`) screens `GateDeflation.dsr <
   min_dsr`, `pbo > max_pbo`, `require_split_stable && !split_stable` — but the Library facade's
   `verdict_for` (`library.hpp:408`) never calls those branches. **S5-1** appends the DSR/PBO/split
   screens to `verdict_for` (with new append-only `AdmitKind` enumerators) so the LIVE library caller
   fires them. Inert at `min_dsr=0.0`/`max_pbo=1.0`/`require_split_stable=false` — the same guards
   `AlphaGate::admit` already uses (`gate.hpp:201`, `:207`, `:215`), so the default library path is
   byte-identical.

2. **The search SELECTION objective (the NSGA deflation column + the cascade skip-bound).** Two
   sub-seams that must not be confused:
   - The **cascade skip-bound** (`cascade_gate_passes`, `factory.cpp`) is a per-candidate *early-exit*
     that provably-safely SKIPS hopeless candidates before the O(|pool|·T) sweep. On `main` it already
     folds `SR*_N` into the keep side — but LOOSENING (the skip set shrinks with `N`), because a true
     upper bound must never skip a candidate the gate-off run would have evaluated (that is the only
     direction that preserves `AdmittedSetUnchanged`). S5 does **not** make this stricter — that would
     break the digest invariant. It is already reconciled on `main`; S5 confirms it and removes the
     stale worktree's `static_cast<void>` if the merge base still carries it.
   - The **NSGA selection column** (`kObjDeflation = 6`, `fitness.hpp:187`; `deflate_selection`,
     `config.hpp:166`) is where making deflation *bite* in selection actually lives: when
     `deflate_selection` is set, `objectives[kObjDeflation]=dsr` and raw is multiplied by `dsr`
     (`fitness.hpp:178`). **S5-2** feeds the RUNNING cumulative trial count `N` into `FitnessCfg.trial_count`
     (`fitness.hpp:353`) so the `dsr` the selection column reads is deflated by the ACTUAL `N`
     (`eval::deflated_sharpe(sr, T, skew, kurt, N=trial_count, …)`, `fitness.hpp:485`), which rises
     monotonically with `N` (`expected_max_sharpe`, `deflated_sharpe.hpp:115`). This is the "search
     optimizes deflated, not undeflated, fitness" fix. It is opt-in behind `deflate_selection` (already
     inert by default), so byte-identity holds.

3. **The run-level PBO gate (blocking).** `finalize_run_pbo` (`factory.cpp:83`) computes the CSCV-PBO
   over the admitted SET; today `pbo_gate_passed` is advisory. **S5-2** adds a `blocking_pbo` opt-in
   that, when set, **un-admits** the run's marginal admits (or fails the run closed) instead of merely
   warning. Distinct from `--pbo-hard-block` (R3), which only flips the EXIT code. Inert at
   `max_pbo=1.0` (PBO never computed) — byte-identical.

The exposures/estimators are S1's; the impact term is S4's; the sleeve/stack methods are S2/S3's. S5's
whole job in the engine is these three deflation seams plus the robustness battery (S5-3). Everything
else S5 does is CLI threading (S5-0) and harness/validation (S5-4/S5-5).

---

## Determinism contract (Sprint 5) — own section

S5 inherits the p8 **opt-in / default-byte-identical** contract (ROADMAP §Determinism). Every CLI flag
S5 threads DEFAULTS to its inert value ⇒ the no-flag path is **byte-identical**:

- The pinned goldens stay UNCHANGED on the default path: `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, the OOS goldens, the `AtxImplDiscover` determinism slice
  (seq==parallel). `oracle.hpp` is FROZEN.
- **Library seam (S5-1):** the DSR/PBO/split screens in `verdict_for` are guarded by exactly the same
  conditions `AlphaGate::admit` already uses (`min_dsr > 0.0`, `max_pbo < 1.0`, `require_split_stable`).
  At the inert defaults the branches never fire ⇒ `verdict_for` returns the identical `AdmitKind` for
  every candidate ⇒ the library digest is unchanged. The new `AdmitKind` enumerators are **APPENDED**
  at the end (never reordered — the reject-histogram index is FROZEN, `library.hpp:113`).
- **Search seam (S5-2):** feeding cumulative `N` into `FitnessCfg.trial_count` changes the `dsr`
  column ONLY when `deflate_selection` is set (already inert by default). `blocking_pbo` changes the
  admitted set ONLY when `max_pbo < 1.0` (already inert). At the inert defaults both the F1 search
  digest and every golden are byte-identical.
- **The blocking changes the admitted set ONLY when the flags are set.** Blocking deflation is NOT a
  correctness fix that re-baselines a golden — it is a pure opt-in. (Contrast S4's P0 bugs, which DO
  re-baseline with the fix SHA as authority.)
- **The V1 prod profile turns opt-ins on as an explicit, documented non-default profile, never a golden
  re-baseline.**

**Four test classes per opt-in (mandatory):** (a) off-path byte-identity — the flag at its inert
default, digest unchanged vs the pinned golden; (b) on-path RED→GREEN — the flag active on a tiny
fixture where the deflated/battery verdict provably differs; (c) twice-run — same panel → same bytes;
(d) seq==parallel — where an admission path is touched (the per-generation cumulative `N` is captured
serially before the parallel_for, matching the `deflate_selection` precedent at `config.hpp:161-165`).

**Validation discipline (binding, from the ROADMAP):** no hour-long run is a sprint gate. Every unit
proves on unit tests + a dev-panel smoke ≤5 min. **V1** is the single operator milestone, run once,
out-of-loop, after S1–S4 land and S5 threads the hub.

---

## Dependency / wiring map — flag → feature sprint → engine field

```
S5-0 config.hpp/cpp  ── threads every field below; each defaults inert ⇒ byte-identical no-flag path
  ├─ (p7 carry-forward)  --short-interest / --augment-out / --si-publication-lag  → RunConfig (augment subcmd)
  ├─ (p7 carry-forward)  --kelly-fraction / --kelly-max-gross  → conviction/kelly sizing (combine)
  ├─ (p7 carry-forward)  --incremental-panel  → panel append + provenance
  ├─ (S1) --risk-model=diagonal|factor  → RiskModelConfig.kind        (stage_riskmodel/stage_optimize)
  ├─ (S1) --dead-alpha-factors           → RiskModelConfig.dead_alpha_factors
  ├─ (S1) --group-neutralize             → RiskModelConfig.group_neutralize
  ├─ (S2) --metabook                     → enable stage_metabook
  ├─ (S2) --sleeve-method=erc|hrp|invvol → MetaAllocatorConfig.method
  ├─ (S3) --combine-method=…|stack|regime-stack → CombineMethod enum (stage_combine dispatch)
  ├─ (S4) --impact-in-selection          → FitnessCfg impact term (S4-owned body; S5 threads the flag)
  ├─ (S4) --capacity-curve               → emit capacity scorecard (stage_report; S4-owned emit, S5 flag)
  └─ (deflation) --min-dsr / --max-pbo   → GateConfig (ALREADY in RunConfig from p6/p7 — NOT re-added)
                 --require-split-stable   → GateConfig.require_split_stable
                 --blocking-pbo           → NEW RunConfig.blocking_pbo (un-admit, not just warn)

S5-1 library/library.hpp:verdict_for  ── append DSR/PBO/split screens (GateDeflation → AdmitKind{RejectDsr,RejectPbo,RejectSplit})
       consumes: combine::GateConfig{min_dsr,max_pbo,require_split_stable} (gate.hpp:72), GateDeflation (gate.hpp:135)

S5-2 factory/factory.cpp  ── cumulative-N into the SELECTION objective (NSGA dsr column) + blocking PBO
     factory/fitness.hpp  ── FitnessCfg.trial_count is the seam (already exists :353); S5 ensures the
                             RUNNING N feeds it (reconcile the main SR*_N fold; retire stale :983 void)

S5-3 NEW eval/robustness_battery.hpp  ── sub-universe / alt-neutralization / noise-control / param-perturbation
       consumes: eval::deflated_sharpe (deflated_sharpe.hpp:138), the panel + a candidate's book

S5-4 stage_run.cpp:run_all  ── assemble augment→discover→riskmodel→combine→metabook→optimize→report
       + NEW scripts/build-megaalpha-book.ps1 (smoke + prod profiles), dev-panel smoke ≤5 min GREEN

S5-5 NEW atx-impl/research/<date>-megaalpha-book-results.md  ── the V1 operator scorecard template
```

---

## Tasks

### S5-0 — Open ledger + thread the full CLI flag surface (do first; all units depend on the config fields)

**Goal:** create the sprint ledger (marker commit); add every new `RunConfig` field the feature sprints
exposed + the p7 carry-forwards, with parse arms and config-file round-trip. Each field defaults inert
⇒ the no-flag path is byte-identical. This unit is pure plumbing — the fields exist and parse, and each
is wired into the corresponding engine config struct at its stage, but nothing changes output at the
defaults.

**Upstream dependencies:** every S1–S4 config struct field, plus the p7-S7 carry-forwards. If a feature
sprint is absent at merge time, wire what exists and mark the missing field `// S5-TODO: depends on SN`
in the hub, recording the gap in the ledger row (the exact p7-S7 discipline — never block on an absent
sprint).

**Root cause:** `RunConfig` (`config.hpp:28-284`) carries the p6/p7 knobs (`min_dsr:55`, `max_pbo:101`,
`conviction:240`, `augment_panel:190`, `adv_windows:189`) but NONE of the S1–S4 p8 knobs. The parse
layer (`config.cpp:19` `apply_flag_value`) has a valueless-bool fast path (`:25-43`), string arms
(`:46-63`), and `parse_double`/`parse_long` numeric arms — S5 extends each.

**Wiring (file:line + sketch):**
- `config.hpp` — append fields at the END of `RunConfig` (never mid-struct — aggregate-init order is
  load-bearing; the S1/S2/S3/S4 progress ledgers all appended). Grouped, each with an inert-default
  comment. Exact insertion line TBD by implementer; sketch:
  ```cpp
  // -- S5 (p8 hub): risk-model / meta-book / combine-method / cost / deflation knobs --
  // Each defaults to today's value so the no-flag discover/run path is byte-identical.
  std::string risk_model        = "diagonal";   // --risk-model=diagonal|factor  (S1; "diagonal" = inert)
  bool        dead_alpha_factors = false;        // --dead-alpha-factors          (S1; false = inert)
  bool        group_neutralize   = false;        // --group-neutralize            (S1; false = inert)
  bool        metabook           = false;        // --metabook                    (S2; false = single-panel combine)
  std::string sleeve_method      = "invvol";     // --sleeve-method=erc|hrp|invvol (S2; ignored unless --metabook)
  std::string combine_method;                    // --combine-method=…|stack|regime-stack (S3; "" = today's linear dispatch)
  bool        impact_in_selection = false;       // --impact-in-selection         (S4; false = impact stays report-only)
  bool        capacity_curve      = false;       // --capacity-curve              (S4; false = no curve emit)
  bool        require_split_stable = false;      // --require-split-stable        (deflation; false = GateConfig inert)
  bool        blocking_pbo        = false;        // --blocking-pbo               (deflation; false = PBO advisory as today)
  // -- p7-S7 carry-forwards (subsumed here) --
  std::string short_interest;                    // --short-interest <csv>        (augment subcmd; "" = inert)
  std::string augment_out;                       // --augment-out <bin>           (augment subcmd; "" = inert)
  long        si_publication_lag = 2;            // --si-publication-lag <days>   (default 2)
  double      kelly_fraction     = 1.0;          // --kelly-fraction              (1.0 = full Kelly = no change)
  double      kelly_max_gross    = 0.0;          // --kelly-max-gross             (0.0 = off = no gross cap)
  bool        incremental_panel  = false;        // --incremental-panel           (false = full rebuild as today)
  ```
- `config.cpp` — add valueless-bool arms (`:43` block) for `dead-alpha-factors`, `group-neutralize`,
  `metabook`, `impact-in-selection`, `capacity-curve`, `require-split-stable`, `blocking-pbo`,
  `incremental-panel`; string arms (`:63` block) for `risk-model`, `sleeve-method`, `combine-method`,
  `short-interest`, `augment-out`; `parse_double` for `kelly-fraction`, `kelly-max-gross`;
  `parse_long` for `si-publication-lag`. `apply_flag` records each canonical name in `set_flags`
  (so a CLI-present flag wins the run-mode merge, `config.hpp:278-283`).
- Wire each field into the owning stage's engine config struct (thin, at the existing construction
  block — the fields already exist from S1–S4): `--risk-model`/`--dead-alpha-factors`/
  `--group-neutralize` → `RiskModelConfig` in the `stage_optimize`/`stage_riskmodel` call site;
  `--metabook`/`--sleeve-method` → the `stage_metabook` enable + `MetaAllocatorConfig`;
  `--combine-method` → the `stage_combine` method dispatch; `--impact-in-selection` →
  `fcfg.search.fitness` (S4's body reads it); `--require-split-stable` → `gc.require_split_stable`
  (`stage_discover.cpp:414` `gc` block); `--blocking-pbo` → `fcfg` (S5-2 consumes it).

**Determinism:** pure addition; append fields at struct end (no aggregate-init breakage). At every
default the corresponding engine guard is inert (`risk_model=="diagonal"` routes to `diagonal_risk_model`;
`combine_method==""` keeps today's dispatch; `kelly_fraction==1.0` is the identity; `require_split_stable`
never fires the gate.hpp:215 branch). Existing discover/optimize/report goldens unchanged.

**Accept (named RED→GREEN tests + fixtures):**
- `ConfigParse.MegaBookFlags_RoundTrip` (new `atx-impl/tests/`): each new flag parses to its field
  (`--risk-model factor` → `cfg.risk_model=="factor"`; `--kelly-fraction 0.5` → `0.5`;
  `--require-split-stable` → `true`); omitted → the inert default.
- `ConfigFile.MegaBookFlags_RoundTrip`: the same flags in a `--config` file parse identically and a
  CLI-present flag overrides a file value (the `set_flags` merge, `config.hpp:305-310`).
- `AtxImplDiscover` determinism slice + `FactoryOos.MineIntoOffPathDigestUnchanged` — **byte-identical**
  with none of the new flags asserted (off-path byte-identity — this is the gate for the whole unit).
- Marker commit: `docs(p8-s5-0): open sprint-5 wire-deflate-validate ledger`.

---

### S5-1 — Wire `GateDeflation` into `library::verdict_for` (close the p7-S1 dead-code carry-forward)

**Goal:** make the DSR/PBO/split-stable deflation screens fire on the **live** library admission path.
Today `AlphaGate::admit` screens them but `Library::verdict_for` (the facade every live caller routes
through) does not. Append the screens to `verdict_for` with new append-only `AdmitKind` enumerators, so
a low-DSR alpha is REJECTED by the library when `--min-dsr > 0`; byte-identical at inert defaults.

**Upstream dependencies:** the `GateConfig{min_dsr, max_pbo, require_split_stable}` and `GateDeflation`
types (p7-S1, on `main` at `combine/gate.hpp:72,135`) — already merged, no sprint blocker. The
`--min-dsr`/`--max-pbo` CLI flags already exist (p6/p7); `--require-split-stable` is threaded by S5-0.

**Root cause:** `verdict_for` (`library.hpp:408-447`) ends at `worst_corr > cfg.max_pool_corr →
RejectCorrelated; return Accept;`. It never reads a `GateDeflation`, so the DSR/PBO/split branches
`AlphaGate::admit` runs (`gate.hpp:201-215`) are unreachable from `Library::admit`
(`library.hpp:179` → `verdict_for` at `:185`). `AlphaCandidate` (`library.hpp:92-106`) carries
`metrics` but no `GateDeflation` — the candidate must carry (or `verdict_for` must accept) the
per-candidate deflation scalars.

**Wiring (file:line + sketch):**
- `library.hpp` — extend `AlphaCandidate` (`:92-106`) with a `combine::GateDeflation defl =
  combine::kInertDeflation;` field (default inert, so existing constructors/tests are byte-identical),
  OR thread a `const GateDeflation&` param through `admit`/`verdict_for` (prefer the field — fewer call
  sites; the factory populates it where it already computes `dsr`/`pbo`, `factory.cpp:940-953`
  `deflated_sharpe`). Exact choice TBD by implementer; the field keeps `admit(c, gate)` signature stable.
- `library.hpp:116-125` — APPEND to `AdmitKind` (never reorder — frozen histogram index):
  ```cpp
  RejectDsr,      // S5-1: holdout DSR below gate.cfg.min_dsr (mirrors GateVerdict::RejectDsr)
  RejectPbo,      // S5-1: run-level PBO above gate.cfg.max_pbo
  RejectSplitUnstable, // S5-1: holdout halves disagree in sign when require_split_stable
  ```
- `library.hpp:446` — insert BEFORE `return AdmitKind::Accept;`, mirroring `gate.hpp:201-215`
  operators EXACTLY (same `>`/`<`, same inert guards):
  ```cpp
  // S5-1: deflation / selection-bias screens on the LIVE library path (the p7-S1
  // carry-forward). Inert at min_dsr=0.0 / max_pbo=1.0 / require_split_stable=false —
  // the exact guards AlphaGate::admit uses (gate.hpp:201,207,215) — so the default
  // library verdict is byte-identical. Placed AFTER the corr screen to preserve the
  // lazy-corr order the histogram/digest goldens pin.
  if (cfg.min_dsr > 0.0 && c.defl.dsr < cfg.min_dsr)                return AdmitKind::RejectDsr;
  if (cfg.max_pbo < 1.0 && c.defl.pbo > cfg.max_pbo)               return AdmitKind::RejectPbo;
  if (cfg.require_split_stable && !c.defl.split_stable)            return AdmitKind::RejectSplitUnstable;
  ```

**Determinism:** at the inert defaults every guard is false ⇒ `verdict_for` returns the identical
`AdmitKind` for every candidate ⇒ the library digest + reject-histogram are byte-identical. The
appended enumerators do not shift the existing indices (0..7 unchanged). The `defl` field default
(`kInertDeflation`) keeps every existing `AlphaCandidate` construction byte-identical.

**Accept (named RED→GREEN tests + fixtures):**
- `LibraryVerdict.LowDsrRejectedWhenMinDsrSet` (new `atx-engine/tests/library/`): a candidate with
  `defl.dsr = 0.10` under `gate.cfg.min_dsr = 0.5` → `verdict_for` returns `RejectDsr` (RED before the
  wire — currently returns `Accept`; GREEN after).
- `LibraryVerdict.InertDeflation_ByteIdentical`: with `min_dsr=0.0`, `max_pbo=1.0`,
  `require_split_stable=false`, a fixture pool of 8 candidates admits the IDENTICAL set + the IDENTICAL
  reject-histogram as the pre-S5-1 `verdict_for` (off-path byte-identity).
- `LibraryVerdict.PboRejectAndSplitReject`: `defl.pbo=0.9` under `max_pbo=0.5` → `RejectPbo`; a
  split-unstable candidate (`split_stable=false`) under `require_split_stable=true` → `RejectSplitUnstable`.
- `LibraryVerdict.AdmitKindEnumFrozenPrefix`: `static_assert`/test that `Accept==0 … RejectDsrSubwindow`
  keep their pre-S5 indices (the new enumerators are strictly appended).
- Differential test vs `AlphaGate::admit`: for a randomized-but-seeded candidate batch, `verdict_for`
  and `AlphaGate::admit` return the SAME verdict class (the facade now mirrors the gate).

---

### S5-2 — Cumulative-N deflation in the SEARCH objective + blocking PBO

**Goal:** make the search select on **deflated** fitness — feed the RUNNING cumulative trial count `N`
into the NSGA deflation column so the `dsr` selection signal is deflated by the ACTUAL number of trials
(stricter at larger sweeps); and make the run-level PBO **blocking** (un-admit) under `--blocking-pbo`
instead of merely advisory. Both opt-in ⇒ byte-identical at defaults.

**Upstream dependencies:** `FitnessCfg.trial_count` (`fitness.hpp:353`), `kObjDeflation`
(`fitness.hpp:187`), `deflate_selection` (`config.hpp:166`, threaded to
`fcfg.search.deflate_selection` at `stage_discover.cpp:437`) — all merged. `--blocking-pbo` field from
S5-0. The `finalize_run_pbo` machinery (`factory.cpp:83-154`) — merged. S4's `--impact-in-selection`
touches the SAME `FitnessCfg` (disjoint field; coordinate via the struct, not the file body).

**Root cause (two sub-seams — keep distinct):**
1. **Selection column.** On the STALE worktree, `cascade_gate_passes` voids `trial_count`
   (`factory.cpp:983`). On `main` the cascade skip-bound already folds `SR*_N` (LOOSER, byte-safe) —
   that is NOT the selection objective. The SELECTION signal is the `dsr` column
   (`objectives[kObjDeflation]=dsr`, `fitness.hpp:178`), computed via
   `eval::deflated_sharpe(sr, T, skew, kurt, N=trial_count, …)` (`fitness.hpp:485`). The fix is to
   ensure `FitnessCfg.trial_count` carries the RUNNING cumulative `N` (the factory already captures
   `res.trial_count` and the cross-run `cumulative_trials()`, `factory.cpp:347-349,607-609,1168-1170`)
   at the point the `dsr` column is computed, so a larger sweep deflates the selection signal more.
2. **Blocking PBO.** `finalize_run_pbo` sets `rep.pbo_gate_passed` but never un-admits
   (`factory.cpp:150-154`); `stage_discover.cpp:719-734` only warns / (with `--pbo-hard-block`) sets a
   non-zero exit. `--blocking-pbo` must actually DROP the run's marginal admits (or fail closed) when
   `pbo > max_pbo`.

**Wiring (file:line + sketch):**
- `factory.cpp` `cascade_gate_passes` — CONFIRM the `main` SR*_N fold is present and correct (the
  stale worktree's `static_cast<void>(trial_count)` at `:983` is retired on `main`). If the S5 merge
  base still carries the void, remove it and adopt the `main` fold VERBATIM (do not re-derive — the
  `main` comment block documents the byte-safety proof). This sub-seam is byte-identity-preserving; it
  is NOT the stricter-selection change.
- `factory.cpp` — at the `deflate_selection` selection-column computation (where
  `admit_fit.trial_count` is set from `prior_r1 + res.trial_count`, `:347-349` / `:607-609` /
  `:1168-1170`), ensure that value flows into the `FitnessCfg.trial_count` the `dsr` column reads for
  SELECTION (not only for the final admission report). Exact line TBD by implementer — the value is
  already computed; the wire is that the SELECTION `dsr` uses the running `N`, monotone-increasing.
- `factory.cpp` — add the blocking branch: when `cfg.blocking_pbo && cfg.max_pbo < 1.0 &&
  rep.pbo > cfg.max_pbo`, un-admit the marginal admits (or return `Err` and DO NOT persist the run's
  admitted set). Distinct from `--pbo-hard-block`'s exit-only escalation.
- `stage_discover.cpp:719-734` — thread `cfg.blocking_pbo` into the `fcfg` so the factory sees it; keep
  the existing advisory warning path when `blocking_pbo` is false.

**Determinism:**
- Selection column: `deflate_selection` is inert by default (`config.hpp:166`), so at the default the
  `dsr` column is never read and the F1 search digest is byte-identical. The per-generation `N` is
  captured serially before the `parallel_for` (the `deflate_selection` precedent, `config.hpp:161-165`)
  ⇒ seq==parallel holds.
- Cascade fold: byte-safe by construction (the `main` proof — the skip set only shrinks vs the
  N-ignoring bound, so `AdmittedSetUnchanged` is preserved).
- Blocking PBO: `max_pbo=1.0` (default) ⇒ PBO never computed ⇒ byte-identical; `blocking_pbo=false`
  ⇒ advisory path unchanged.

**Accept (named RED→GREEN tests + fixtures):**
- `CascadeTrialCount.SkipThresholdMonotoneInN` (new `atx-engine/tests/factory/`): the `dsr` selection
  threshold rises monotonically with `N` on a fixed-Sharpe fixture (`expected_max_sharpe(N,V)` monotone,
  `deflated_sharpe.hpp:115`) — the concrete quantified claim.
- `DeflateSelection.PassAtN1_RejectedAtN100`: a candidate whose raw fitness passes at `N=1` is NOT
  selected at `N=100` when its deflated `dsr` falls below the selection bar (RED before the running-N
  wire; GREEN after). A genuine keeper (high `dsr`) survives any `N`.
- `BlockingPbo.UnadmitsOnBreach`: on a fixture where the admitted set has `pbo=0.9`, `--blocking-pbo`
  with `--max-pbo 0.5` drops the marginal admits (or fails closed); `--pbo-hard-block` alone (no
  `--blocking-pbo`) leaves the set intact and only flips the exit code (the distinction test).
- `FactoryOos.MineIntoOffPathDigestUnchanged` + `NsgaSearch.ScalarRaw_ReproducesGoldenDigest` —
  byte-identical at the inert defaults (off-path).
- `DeflateSelection.SeqEqualsParallel`: the running-`N` deflation column is identical `--workers 1`
  vs `--workers N`.

---

### S5-3 — NEW `eval/robustness_battery.hpp`: automated admission-time robustness subsystem

**Goal:** an automated battery that rejects a candidate whose apparent edge collapses under
perturbation — the class the measured degenerate-alpha analysis flagged (an admitted alpha that was a
`1/price` dimensional artifact passing ALL statistical gates). The battery is a pure, deterministic
subsystem over a candidate's book + the panel; each check runs on a tiny deterministic fixture.

**Upstream dependencies:** the panel type + a candidate's book/PnL (existing); `eval::deflated_sharpe`
(`deflated_sharpe.hpp:138`). No feature-sprint blocker — this is greenfield eval code S5 owns. (It
CONSUMES S1's neutralization group_map for the alternate-neutralization check when present; degrades
gracefully to a no-op with a logged note when absent.)

**Root cause:** there is NO robustness harness in `eval/` — the degenerate `1/price` alpha passed
fitness, Sharpe, turnover, corr, DSR, and PBO because none of those probe whether the edge is a
DIMENSIONAL artifact or survives on RANDOMIZED inputs. The `--reject-price-scale` gate (R2,
`config.hpp:147`) catches the *specific* `1/price` correlation, but not the general class (a noise
signal that happens to correlate with any dimensional column). A noise-replacement negative control
catches the general case.

**Wiring (file:line + sketch):**
- NEW `atx-engine/include/atx/engine/eval/robustness_battery.hpp` + `src/eval/robustness_battery.cpp`.
  A `RobustnessBattery` with a config of independently-toggleable checks and a `run(candidate, panel,
  cfg) -> BatteryResult` returning per-check pass/fail + the surviving-edge ratio:
  ```cpp
  struct BatteryConfig {
    bool  sub_universe        = false; // rerun on TOP-N restricted universes; edge must survive
    bool  alt_neutralization  = false; // rerun under an alternate group_map; edge must survive
    bool  noise_control       = false; // NEGATIVE control: edge must COLLAPSE on randomized inputs
    bool  param_perturbation  = false; // perturb the candidate's params; edge must be stable
    atx::f64 min_survival_ratio = 0.5; // fraction of the base edge the check must retain
    atx::u64 seed = 0;                 // deterministic RNG for the noise-control draw (NEVER thread/time)
  };
  ```
- **sub-universe rerun:** re-evaluate the candidate's book on a TOP-N-by-ADV restricted universe;
  reject if the OOS `dsr` on the sub-universe falls below `min_survival_ratio × base_dsr`.
- **alternate-neutralization rerun:** re-evaluate after residualizing against an ALTERNATE group_map;
  an edge that is a pure sector/dimensional tilt collapses.
- **noise-replacement negative control:** replace the candidate's INPUT columns with seeded random
  draws of matched marginal distribution; the edge MUST collapse (if it survives on noise, it is an
  artifact — REJECT). This is the check that catches the `1/price` class generally.
- **parameter-perturbation stability:** jitter the candidate's numeric params within a small band;
  reject if the edge is a knife-edge (variance of `dsr` across perturbations exceeds a tolerance).
- The battery is admission-time SCREENING (never mutates state); all reductions order-fixed; the only
  RNG is the seeded noise-control draw (deterministic, seq==parallel invariant).

**Determinism:** the battery is off unless a `BatteryConfig` check is toggled; at the all-false default
it is a no-op (byte-identical). The noise draw uses `cfg.seed` (never thread/time) so the verdict is
reproducible run-to-run and seq==parallel.

**Accept (named RED→GREEN tests + fixtures):**
- `RobustnessBattery.NoiseControlRejectsArtifact` (new `atx-engine/tests/eval/`): a constructed
  `1/price`-style signal (edge is a dimensional artifact) SURVIVES on the noise-replacement control →
  the battery REJECTS it (RED: no battery exists; GREEN: `noise_control` flags it). A genuine
  cross-sectional signal collapses on noise → PASSES the control.
- `RobustnessBattery.SubUniverseCollapseRejected`: an edge concentrated in a handful of illiquid names
  falls below `min_survival_ratio` on the TOP-N universe → rejected; a broad edge survives.
- `RobustnessBattery.AltNeutralizationRemovesTilt`: a pure sector tilt collapses under the alternate
  group_map; an idiosyncratic signal passes.
- `RobustnessBattery.ParamPerturbationStable`: a knife-edge candidate (edge only at one param value)
  is rejected; a stable candidate passes.
- `RobustnessBattery.Deterministic_TwiceRun`: identical `BatteryResult` bytes on two runs with the same
  seed; identical across `--workers 1` vs `N`.
- `RobustnessBattery.AllChecksOff_NoOp`: the all-false config returns "pass, no checks run" and touches
  no state (off-path byte-identity for the pipeline).

---

### S5-4 — Assemble the full stage graph + `build-megaalpha-book.ps1` + dev-panel smoke ≤5 min

**Goal:** wire the full pipeline stage graph (augment→discover→riskmodel→combine→metabook→optimize→
report) through `run_all`/`dispatch`, write the `build-megaalpha-book.ps1` harness (smoke + prod
profiles, modeled on `build-tradeable-alphas.ps1`), and prove the dev-panel smoke runs green in ≤5 min
(loose gates guarantee admits — this validates WIRING, not edge).

**Upstream dependencies:** the new stages `stage_riskmodel` (S1) and `stage_metabook` (S2), the
`stage_combine` method dispatch (S3), the `stage_report` capacity-curve emit (S4). If a stage is
absent, `run_all` skips it (guarded by the S5-0 flag) and the smoke run degrades to the pre-p8 graph
with the gap noted — never a hard block.

**Root cause:** `run_all` (`stage_run.cpp:16-136`) hard-codes the SIX-stage graph
(load→panel→discover→combine→optimize→report, `:38-112`) and folds exactly six digests
(`:114-134`). The new S1 `stage_riskmodel` and S2 `stage_metabook` stages are not in the graph, and
the S3 `combine_method` / S4 `capacity_curve` knobs are not passed to their sub-configs. `dispatch`
(`dispatch.cpp:98-106`) routes each subcommand but has no `riskmodel`/`metabook` arm.

**Wiring (file:line + sketch):**
- `stage_run.cpp:49-100` — insert the new stages behind the S5-0 flags, each a no-op passthrough at the
  inert default so `run_all` is byte-identical when the flags are off:
  ```cpp
  // S5-4: riskmodel stage (S1) — build the factor covariance artifact BEFORE combine/optimize.
  // Skipped entirely when --risk-model=diagonal (the default), so run_all is byte-identical.
  if (cfg.risk_model == "factor") {
      RunConfig c_rm = cfg; c_rm.panel = (work/"panel.bin").string();
      c_rm.out = (work/"riskmodel.bin").string();
      ATX_TRY(auto d_rm, run_riskmodel(c_rm));   // new stage; S1-owned body
      /* thread the artifact into c_comb / c_opt below */
  }
  // ... metabook stage (S2) inserted between combine and optimize when --metabook ...
  ```
  The digest fold (`:114-123`) extends to the active stages ONLY when they run (a skipped stage
  contributes nothing — the six-digest fold is preserved byte-for-byte at the inert defaults).
- `dispatch.cpp:98-106` — add `if (sub == "riskmodel") return run_riskmodel(cfg);` and
  `if (sub == "metabook") return run_metabook(cfg);` arms; extend `kSubcommands` (`config.hpp:17`).
- Thread `cfg.combine_method` → `c_comb.method`; `cfg.metabook`/`cfg.sleeve_method` → the metabook
  sub-config; `cfg.capacity_curve` → `c_rep` (S4's emit reads it).
- NEW `atx-impl/scripts/build-megaalpha-book.ps1` — model on `scripts/build-tradeable-alphas.ps1`:
  a `[ValidateSet('prod','smoke')] $Profile` param, a `[string[]] $Stage` (`augment|discover|
  riskmodel|combine|metabook|optimize|report|pipeline|all`), a `[switch] $DryRun`, and one
  testable `New-*Argv` function per stage. `smoke` → `--panel work/dev/dev-panel.bin --population 40
  --generations 4 --min-sharpe 0.0 --min-fitness 0.0 --max-turnover 1.0` (loose gates ⇒ guaranteed
  admits; threads the new p8 flags at their inert defaults so wiring is exercised without changing
  output). `prod` → the full accept panel + the OPT-IN mega-book profile (`--risk-model factor
  --dead-alpha-factors --group-neutralize --metabook --sleeve-method hrp --combine-method stack
  --impact-in-selection --capacity-curve --min-dsr 0.5 --max-pbo 0.5 --require-split-stable
  --blocking-pbo`), operator-driven, NOT run in this sprint.
- NEW `atx-impl/scripts/tests/build-megaalpha-book.Tests.ps1` — Pester: `DryRun -Profile smoke` argv
  contains the dev-panel path + loose gates + the new p8 flags at inert values; `DryRun -Profile prod`
  argv contains the full opt-in mega-book flag set; no binary/panel/compile required.

**Determinism:** every inserted stage is skipped at its inert flag default ⇒ `run_all` is
byte-identical to today's six-stage graph. The smoke profile passes the new flags at inert defaults, so
the smoke discover digest equals the pre-S5 baseline on the same panel. The smoke run's claim is
"pipeline runs end-to-end; no segfault; ≥1 admit under loose gates" — WIRING, not edge.

**Accept (named RED→GREEN tests + fixtures):**
- `Pester build-megaalpha-book.Tests.ps1` — all green: `DryRun -Profile smoke` and `-Profile prod`
  emit the expected argv (pattern-match every expected flag); `DryRun` invokes no binary.
- `StageRun.MegaBookGraph_InertByteIdentical` (new `atx-impl/tests/`): `run_all` with all p8 flags off
  produces the byte-identical six-digest run digest as the pre-S5 baseline.
- `StageRun.RiskmodelMetabookStages_SkippedAtDefault`: `run_riskmodel`/`run_metabook` are NOT invoked
  when `--risk-model=diagonal` / `--metabook` absent (skip proof).
- Dev-panel smoke: `build-megaalpha-book.ps1 -Profile smoke` runs end-to-end on
  `work/dev/dev-panel.bin` in ≤5 min, exits 0, ≥1 alpha admitted (loose gates). Wall time recorded.
- `-Profile prod` is `DryRun`-verified ONLY — NOT executed in the sprint.

---

### S5-5 — The V1 operator scorecard harness + `<date>-megaalpha-book-results.md` template

**Goal:** the V1 operator scorecard template + the harness that emits the book-level honest numbers:
net-of-10bps OOS Sharpe, DSR (cumulative-N), PBO, CPCV, walk-forward, capacity curve, N_eff/IR breadth,
and the robustness-battery pass/fail matrix. V1 is the single operator prod run (out-of-loop, NOT a
sprint gate). This unit ships the TEMPLATE + the command; the operator runs it once.

**Upstream dependencies:** every prior sprint — this is the book the whole module assembles. The
scorecard READS the report KV block (`stage_report.cpp`, S4-owned) + the battery result (S5-3) + the
factory PBO/DSR (`factory.cpp`, S5-2). S5 owns the TEMPLATE and the harness invocation, not the
report-emit body.

**Root cause:** no `*megaalpha-book-results*.md` exists (grep → zero hits). The p7-S7 analog was never
run (empty branch). The scorecard has NEVER been produced on real data, so the north-star number is
unknown.

**Wiring (file:line + sketch):**
- NEW `atx-impl/research/<date>-megaalpha-book-results.md` — a TEMPLATE (all rows present, marked
  `<TBD — filled at V1>` where the operator fills the measured number), NOT a placeholder with empty
  sections. Sections: (1) run provenance (panel, date-range, seed, worker count, wall time, commit
  SHA); (2) book-level **net-of-10bps OOS Sharpe**; (3) **DSR** under cumulative-N deflation; (4) PBO
  (CSCV); (5) CPCV (`eval::cpcv_folds`, `cpcv.hpp:175`); (6) walk-forward OOS Sharpe (the `--walk-forward`
  telemetry, `config.hpp:245`); (7) **capacity curve** (edge vs AUM zero-crossing under √-impact); (8)
  N_eff/IR breadth (`eval/breadth.hpp`); (9) the **robustness-battery pass/fail matrix** (S5-3); (10)
  the reject-histogram + battery-failure dominant bucket (names the next module's target if the bar is
  missed).
- The V1 command block (documented in the template + the ledger, NOT run in-sprint):
  ```powershell
  # After S1–S4 land on main and S5 threads the hub — run once, overnight (the ONLY hour-long run in p8):
  .\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage augment,discover -WorkDir work\megaalpha
  .\atx-impl\scripts\build-megaalpha-book.ps1 -Profile prod -Stage riskmodel,combine,metabook,optimize,report -WorkDir work\megaalpha
  # Output: atx-impl\research\<date>-megaalpha-book-results.md
  # North star (p8 acceptance): book net-of-10bps OOS Sharpe > 1.0 with >=5 admitted, DSR>0 under
  # cumulative-N, PBO<0.5, turnover<0.20/day (cross-sleeve netted), capacity+ at >=$100M, survives
  # the robustness battery — OR a documented frontier naming the binding constraint. Honest null valid.
  ```

**Determinism:** the template is a doc; the harness invocation reuses S5-4's byte-identical wiring. V1
is out-of-loop — it never gates a sprint and never re-baselines a golden (the prod profile is an
explicit opt-in, `oracle.hpp` frozen).

**Accept (named RED→GREEN tests + fixtures):**
- `Pester build-megaalpha-book.Tests.ps1` — `DryRun -Profile prod -Stage riskmodel,combine,metabook,
  optimize,report` emits the staged argv without invoking the binary (the V1 command is composable).
- The research template committed (not a placeholder — every scorecard row present, `<TBD — filled at
  V1>` where the operator fills the number; the V1 command block present).
- Ledger records: V1 command documented, NOT run; the north-star bar; where the measured evidence will
  live (`atx-impl/research/<date>-megaalpha-book-results.md`).

---

## Sequencing

1. **S5-0 first** (config hub + ledger marker) — every downstream unit reads the new `RunConfig`
   fields and the CLI must parse before the harness can thread the flags.
2. **S5-1** and **S5-2** in parallel after S5-0 (disjoint files: S5-1 edits `library.hpp`; S5-2 edits
   `factory.cpp` + `fitness.hpp`). Both are the "make deflation blocking" pair.
3. **S5-3** (robustness battery) — greenfield `eval/` code, independent of S5-1/S5-2; can run in
   parallel with them.
4. **S5-4** after S5-0..S5-3 (the harness threads every flag and the stage graph invokes the battery).
5. **S5-5** last — the V1 template + command depend on the full harness being composable (S5-4).

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| Working tree is `feat/warehouse-parity`, STALE vs `main` for `factory.cpp`/`gate.hpp` | S5 re-adds an already-landed change or edits the wrong base | Branch S5 from **`main`**, not the warehouse-parity worktree. Confirm `cascade_gate_passes` already has the SR*_N fold (main) before touching it; the stale `static_cast<void>(trial_count)` at `:983` is gone on main. |
| Confusing the cascade skip-bound with the SELECTION objective | S5-2 makes the skip-bound stricter → breaks `AdmittedSetUnchanged` | Keep the two sub-seams distinct (architecture note §2). The skip-bound stays LOOSENING (byte-safe); the stricter-selection change lives ONLY in the `deflate_selection` NSGA `dsr` column. |
| New `AdmitKind` enumerators reorder the frozen histogram index | Digest/histogram goldens drift | APPEND only (`library.hpp:125`); the `AdmitKindEnumFrozenPrefix` test pins the pre-S5 indices. Never insert mid-enum. |
| `GateDeflation` not populated on the live candidate | S5-1 screens on `kInertDeflation` always → never fires even when `--min-dsr>0` | The factory computes `dsr`/`pbo` at `factory.cpp:940-953`; thread those into the `AlphaCandidate.defl` field at the admit call site. The `LowDsrRejectedWhenMinDsrSet` test is RED until this is wired. |
| A feature sprint (S1–S4) is absent at S5 merge time | Hub won't compile / stage missing | Wire what exists; guard each field `// S5-TODO: depends on SN`; skip the absent stage in `run_all` behind its flag; record the gap in the ledger + research note (the p7-S7 discipline). Never block on an absent sprint. |
| `factory/fitness.hpp` (S5) vs `fitness.cpp` (S4) collision | Merge conflict / double-wire | Disjoint edits: S5 adds/reads the `trial_count` FIELD (header); S4 fills the impact term (source). Coordinate via the `FitnessCfg` struct only. Confirmed disjoint in the ROADMAP ownership matrix. |
| Blocking PBO conflated with `--pbo-hard-block` (R3) | Double escalation or no-op | `--pbo-hard-block` flips the EXIT code only (`stage_discover.cpp:731`); `--blocking-pbo` UN-ADMITS. The `BlockingPbo.UnadmitsOnBreach` test asserts the distinction. |
| A new flag accidentally shifts the golden on the default path | Golden drift; determinism contract broken | Every new `RunConfig` field defaults inert; the `AtxImplDiscover` + `FactoryOos` off-path byte-identity tests are the gate. If they fail, a default leaked. |
| Noise-control battery uses thread/time RNG | Non-deterministic verdict; seq!=parallel | The noise draw uses `BatteryConfig.seed` (never thread/time); `Deterministic_TwiceRun` + seq==parallel tests pin it. |
| Smoke run > 5 min on the dev panel | Sprint gate slips into an hour-long run | `--population 40 --generations 4` on `dev-panel.bin` with loose gates is the validated budget (p6-S7 precedent). If it exceeds 5 min, halve population; document in the ledger. Never let V1 (prod) run as a gate. |
| V1 prod run accidentally triggered in-sprint | An hour-long run becomes a de-facto gate | `-Profile prod` is `DryRun`-verified only; the Pester gate + the ledger note forbid running it in-sprint. V1 is operator-driven, once, after merge. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** the pinned discover/optimize/report goldens, `AtxImplDiscover`
  determinism slice, `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, and
  `FactoryOos.MineIntoOffPathDigestUnchanged` all unchanged with NONE of the new flags asserted.
- **Per-task RED→GREEN:** each opt-in (library deflation screens, cumulative-N selection, blocking PBO,
  each battery check) has a test RED before the wire and GREEN after.
- **Deflation-blocking, measured:** on the S5-2 fixture, record the admitted-set size and mean `dsr`
  for {undeflated, cumulative-N at N=1, at N=100} — the deflated selection must drop the marginal
  overfit candidate at large N (the concrete quantified S5-2 claim).
- **Battery rejection, measured:** the constructed `1/price` artifact is REJECTED by the noise-control
  check while a genuine cross-sectional signal PASSES (the concrete S5-3 claim; the degenerate-alpha
  analysis is the motivating evidence).
- **Twice-run + seq==parallel** on every admission-touching path (library screens, selection column,
  battery).
- **Dev-panel smoke ≤5 min** GREEN with ≥1 admit under loose gates (WIRING, not edge).
- **V1 command documented, NOT run** — the scorecard template committed, all rows present.

---

## Out of scope

- Running the V1 full-panel prod book — operator milestone, once, after merge; never a sprint gate.
- Re-implementing any S1–S4 behavior — S5 CONSUMES their config fields and threads their flags; the
  factor estimator (S1), meta-book (S2), stacking/regime combiner (S3), and impact/capacity bodies (S4)
  are frozen from S5's perspective.
- The `factory/fitness.cpp` impact-in-selection BODY — S4-owned; S5 threads only the `--impact-in-selection`
  flag and the `fitness.hpp` config FIELD.
- `stage_report.cpp` scorecard-emit body — S4-owned; S5 threads `--capacity-curve` and consumes the KV.
- The cascade skip-bound stricter-in-N change — deliberately NOT done (it would break `AdmittedSetUnchanged`);
  the stricter selection lives only in the `deflate_selection` NSGA column.
- Editing `oracle.hpp` — frozen every sprint.
- NCO / meta-labeling / RMT-clustered sectors — ROADMAP future-work backlog, not p8.

Sprint discipline: [../docs/sprint.md](../docs/sprint.md). Implementation quality (mandatory for every
coding unit): [../docs/implementation-quality.md](../docs/implementation-quality.md).
