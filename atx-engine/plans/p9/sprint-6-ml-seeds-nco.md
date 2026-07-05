# Sprint 6 — ML Seed Source + NCO Allocator (Greenfield Capstone)

> **THIS SPRINT IS THE EXPLICIT CUT-POINT.** If budget tightens at any point,
> **cut S6 entirely.** S1–S5 + S7 (dead-alpha wire, factor-covariance-in-combine,
> GP trading, capacity/turnover objectives, book-level gates + synthetic smoke,
> and the corrected prod recipe) form a complete, shippable, honest series on
> their own — S7's recipe fixes do not depend on anything in this file. S6 adds
> the two purely-additive, independent greenfield levers (ML gen-0 seeding, NCO
> allocation); skipping it changes zero behavior anywhere else and leaves no
> dangling reference (S7's recipe simply omits `--ml-seeds`/`--sleeve-method nco`).

**Goal:** two independent, opt-in, byte-identical-off levers:

1. **ML seed source →  gen-0 pool.** Behind `--ml-seeds`, deterministically fit
   (or load a cached) `learn::fit_autoencoder_factors` + `learn::fit_tcn` model
   over the discover panel, materialize each model's per-(date,instrument) score
   as a new derived Panel field, and seed the GA's generation-0 population with
   trivial in-grammar DSL expressions referencing those fields. Today
   `learn/{autoencoder_alpha,tcn_alpha}` are built + engine-tested but have
   **zero call sites in `atx-impl`** (confirmed by grep — not even the
   combination path is wired end-to-end in the runnable pipeline); the only
   place they are exercised at all is one engine integration test that admits a
   planted NN alpha straight into a `library::Library` and stops there (never
   drives GA generation). See the grounding table below for the exact seam this
   sprint uses instead of a (structurally impossible) direct model→Genome wire.
2. **NCO (Nested Clustered Optimization) as an opt-in `fund::RiskBudgetMethod`.**
   Append `Nco` to the existing closed enum (InverseVol / EqualRiskContribution
   / HierarchicalRiskParity), reachable via `--sleeve-method nco`. Correlation →
   `atx::core::cluster::cluster` partition → intra-cluster HRP (reusing the
   existing `MetaAllocator::hrp_weights` kernel verbatim) → inter-cluster ERC on
   the reduced covariance (reusing `MetaAllocator::erc_log_barrier` verbatim) →
   recombine. Off (any other `--sleeve-method` value) ⇒ byte-identical.

Both levers are **opt-in behind an inert default**; the no-flag path is
byte-identical (pinned goldens `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`FactoryOos.MineIntoOffPathDigestUnchanged`, the `AtxImplDiscover` determinism
slice, `LibraryVerdict.AdmitKindEnumFrozenPrefix` all unchanged).

---

## Naming reconciliation (read before starting S6-2)

The p9 ROADMAP's SHARED CONFIG-FIELD REGISTRY names the enum
`fund::AllocatorMethod::Nco`. **No type named `AllocatorMethod` exists anywhere
in the codebase** (grep confirmed zero hits). The real type is
`atx::engine::fund::RiskBudgetMethod` (`meta_allocator.hpp:82-86`) — the type of
`MetaAllocatorConfig::method` (`meta_allocator.hpp:92`), i.e. exactly "the
`MetaAllocator`'s allocation method enum" the registry entry describes in
prose. This sprint appends `Nco` to `RiskBudgetMethod`. **Flagged for the
reconciler**: either the ROADMAP registry row should read
`fund::RiskBudgetMethod::Nco`, or a `using AllocatorMethod = RiskBudgetMethod;`
alias should be added if the generic name is load-bearing elsewhere — this plan
does not add that alias unprompted (no other file references
`fund::AllocatorMethod`).

---

## Owns (exclusive)

- NEW `atx-engine/include/atx/engine/factory/ml_seed_source.hpp` +
  NEW `atx-engine/src/factory/ml_seed_source.cpp` (the ML seed producer — calls
  `learn/*` frozen fit/predict functions, never edits them).
- `atx-engine/include/atx/engine/fund/meta_allocator.hpp` +
  `atx-engine/src/fund/meta_allocator.cpp` (append `RiskBudgetMethod::Nco` +
  the `nco_weights` kernel + `MetaAllocatorConfig::nco_clusters`).
- `atx-impl/src/config.{hpp,cpp}` (`ml_seeds`, `ml_seed_model_dir` RunConfig
  fields + `--ml-seeds`/`--ml-seed-model-dir` flags; extend the
  `--sleeve-method` closed taxonomy `{erc,hrp,invvol}` → `{erc,hrp,invvol,nco}`).
- `atx-impl/src/stage_run.cpp` (`sleeve_method_from_string`: map `"nco"` →
  `fund::RiskBudgetMethod::Nco`). **Refinement beyond the ROADMAP's terse S6
  ownership row** (which lists only `learn/*`, `factory/search_driver.*`/
  `genome.*`, `fund/*`, `config.*`) — flagged below and in the ledger, mirroring
  how p8-S1's own doc refined the top ROADMAP's ownership list with the actual
  file set once the real wiring was traced.
- `atx-impl/src/stage_discover.cpp` (the ungated `SearchDriver` construction
  site `~:1104` and the gated `FactoryConfig` assembly `~:591`/`~:953`) — same
  refinement flag as above; ML-seed injection is structurally an **upstream
  Panel + `seed_exprs` augmentation**, and this is the one file that assembles
  both before either search path runs. Without this file, `--ml-seeds` would
  parse and do nothing — a Potemkin flag, exactly what p9 exists to eliminate.
- Tests: `atx-engine/tests/factory/ml_seed_source_test.cpp`,
  `atx-engine/tests/fund/meta_allocator_nco_test.cpp`,
  NEW `atx-impl/tests/stage_discover_ml_seed_test.cpp`,
  NEW `atx-impl/tests/stage_run_sleeve_nco_test.cpp`.

## Must NOT touch

- `alpha/oracle.hpp` (untouchable every sprint).
- `learn/*` bodies — **call only**. Every AE/TCN fit or predict call goes
  through the existing public API (`fit_autoencoder_factors`, `fit_tcn`,
  `predict_ae`, `predict_nn`, `build_features`, `build_sequences`); none of
  those files are edited.
- `factory/search_driver.{hpp,cpp}` and `factory/genome.hpp` — **this sprint's
  central finding is that neither file needs to change at all.** See "The ML
  seed seam decision" below. `SearchDriver::init_population` (search_driver.cpp
  :393-486) already accepts arbitrary DSL source strings via `seed_exprs_`; a
  Genome is irreducibly `alpha::Ast` (genome.hpp:49-52) with no
  `unparse(Ast)` anywhere in the codebase (search_driver.hpp:49 comment) — so a
  fitted `LearnedModel`'s weights cannot become a DSL tree, and the honest seam
  is upstream of `SearchDriver` entirely (a derived Panel field + a trivial
  seed-expression string), not a change to how the driver builds gen-0.
- `MetaAllocator::allocate`'s public signature (`meta_allocator.hpp:125-127`) —
  unchanged. Only the private kernel dispatch (`risk_budget_weights`,
  `meta_allocator.cpp:338-352`) gains one `case`, and `MetaAllocatorConfig`
  gains one inert-default field.
- `MetaAllocator::hrp_weights` / `erc_log_barrier` estimation bodies — **called
  verbatim**, not re-derived (NCO is a composition of the two existing kernels
  plus one new small reduction step).
- S1–S5's owned files (`stage_optimize.cpp`'s risk-model/GP-trading regions,
  `stage_combine.cpp`, `stage_metabook.cpp`, `factory/fitness.{hpp,cpp}`,
  `risk/optimizer.hpp`, `factory/factory.cpp`'s battery surface, `loop/*`).

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use the surrounding engine headers as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering, and tricky
domain rules. Do not comment obvious assignments.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby engine code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## Grounding table (verified file:line) — the seam evidence

| Question | Finding | Evidence |
|---|---|---|
| Where does a learned alpha's per-(date,inst) score actually live today? | `learn::nn_to_candidate` walks a `SequenceTensor` and scatters `predict_sample(model, seq, s)` into `pos_flat[date_of[s]*n_instruments + inst_of[s]]` — a dense `[n_dates * n_instruments]` buffer, **period-major, inst-minor** — exactly `alpha::Panel`'s own column layout (`date * instruments + inst`, panel.hpp:69-70). | `nn_source.cpp:208-265`, `nn_source.hpp:160`, `panel.hpp:69-74` |
| Can a fitted `LearnedModel` become a `factory::Genome`? | No. `Genome` **is** `alpha::Ast` + cached `Analysis` (genome.hpp:36-52); there is **no `unparse(Ast)`** anywhere in the codebase (explicit comment, search_driver.hpp:49); an NN's encoder/decoder or TCN conv weights have no representation in the ~85-op DSL grammar. Mapping is not "hard", it is undefined — there is nothing to map TO. | `genome.hpp:36-69`, `search_driver.hpp:49-58` |
| Where do gen-0 seeds actually enter the pool? | `SearchDriver::init_population` parses each string in `seed_exprs_` via `alpha::parse_expr` + `analyze`, tags it `from_seed=true`, then pads to `cfg.population` (cycling or ramped-grammar fill). `seed_exprs_` is a plain `std::vector<std::string>` ctor argument (DSL source, not ASTs). | `search_driver.cpp:393-486`, `search_driver.hpp:363-388` |
| Does the CLI path already have a `seed_exprs` seam? | Yes, twice: the ungated path builds `SearchDriver` directly with `cfg.seed_exprs` (`stage_discover.cpp:1104`); the gated path (which `run_all`/`run_discover` with `--gated` always uses) copies the same list into `factory::FactoryConfig::seed_exprs` (`stage_discover.cpp:591`), which `Factory::mine_into` threads into its own internal `SearchDriver`. Both are plain string lists — no engine-side change needed to accept more entries. | `stage_discover.cpp:591,953,1104`, `factory.hpp:126` |
| Any live call site of `learn::fit_tcn`/`fit_autoencoder_factors`/`nn_to_candidate` in `atx-impl` today? | **Zero** (`grep -rn "nn_to_candidate\|fit_tcn\|fit_gru\|fit_autoencoder\|SeqLearnedSignalSource\|LearnedSignalSource" atx-impl/src` → no matches). | direct grep, this session |
| Any call site at all, including engine tests? | One: `LearnNnSourceIntegration.LibraryAdmit_PlantedNnAlpha` fits a TCN, calls `nn_to_candidate`, and admits the candidate straight into a fresh `library::Library` — proving the **combination** seam (a learned alpha CAN live in the library `combine::Stack` reads) but never touching GA generation. This is the "only used in combination via Stack" status quo the ROADMAP/design-spec describe. | `learn_nn_source_integration_test.cpp:1-44` |
| Is Panel appendable? | `alpha::Panel::create(dates, instruments, field_names, field_data, universe)` builds a fresh, self-contained Panel from raw column data (`panel.hpp:88-90`); `field_all(FieldId)` (panel.hpp:161-164) lets a caller re-read every existing column verbatim. So "original fields + N new derived columns, same `create()` call" is a first-class, already-precedented construction (`stage_discover.cpp`'s own `apply_capacity_screen` already builds and swaps in a derived Panel this way, `stage_discover.cpp:967-973`). | `panel.hpp:77-195`, `stage_discover.cpp:961-973` |
| Clustering helper for NCO? | **Yes, but at the `atx-core` layer, not `alpha::cluster_panel.hpp`.** `atx::core::cluster::cluster(const MatX& sim, ClusterConfig cfg={}) -> Result<Clustering>` (`atx-core/include/atx/core/cluster/cluster.hpp:605`, `ClusterConfig` :84, `Clustering{cluster_id, n_labels}` :112, `Algo` enum :77) is the raw, directly-reusable primitive. `atx::engine::alpha::cluster_panel.hpp`'s `build_cluster_panel` (`cluster_panel.hpp:180-369`) composes `rmt_clean` + `cluster` over a **returns `Panel` + a rolling window** — the wrong layer for `fund::MetaAllocator`, which owns no `Panel`/sample-count context, only an already-estimated `Ω` (`meta_allocator.hpp:21-22`: "Ω is an INPUT here... this unit estimates nothing"). `rmt_clean` needs a sample-ratio `q=N/T` NCO has no way to supply without changing `MetaAllocator::allocate`'s signature (out of scope) — so NCO clusters the **raw** correlation, exactly as `hrp_weights` already does (no RMT step there either, `meta_allocator.cpp:266-283`). | `cluster.hpp:77-112,605`, `cluster_panel.hpp:1-25,180`, `meta_allocator.hpp:21-22` |
| Existing kernel reusable for NCO's intra-cluster step? | Yes — `MetaAllocator::hrp_weights(const MatX& Omega)` (private static, `meta_allocator.cpp:266-333`) takes **any** covariance matrix, including a cluster submatrix. Called once per cluster, zero new HRP math. | `meta_allocator.cpp:266-333` |
| Existing kernel reusable for NCO's inter-cluster step? | Yes — `MetaAllocator::erc_log_barrier(Omega, b, iters)` (private static, `meta_allocator.cpp:84-114`), which never inverts Ω (CCD, fixed sweeps). Applied to the small `K×K` reduced (cluster-level) covariance with an equal budget. | `meta_allocator.cpp:84-114` |

---

## The ML seed seam decision (documented deviation from the literal brief)

The design spec's Root list names `factory/search_driver.*` / `factory/genome.*`
as where "inject learned seeds into the gen-0 pool" happens. Having traced the
actual mechanics (table above), **that is not where the seam lives, and neither
file needs to change.** `Genome` is irreducibly a DSL `Ast`; there is no path
from a fitted `LearnedModel`'s numeric weights into that representation, and
inventing one (e.g. a new DSL op that embeds a serialized NN and evaluates it
inline) would be exactly the "fragile wire" the brief warns against — a
brand-new VM op, a new `Expr::Kind`, new bytecode, new serialization for
`Ast` itself, all to represent a black box the DSL cannot introspect,
crossover, or mutate meaningfully (mutating one weight of a TCN conv kernel is
not a mutation the GA's operators are designed to reason about).

**The honest, scoped fallback this sprint takes** (exactly the brief's
suggested escape hatch): treat each learned model's score as **one more raw
Panel field the DSL can already reference**, and seed the GA with a trivial
in-grammar expression over it:

1. Fit (or load) the AE + TCN models once per discover run, deterministically.
2. Materialize each model's full `[dates × instruments]` score cross-section
   as a new derived Panel column (`__ml_ae_alpha`, `__ml_tcn_alpha`).
3. Build an **augmented Panel** — the original fields, in their original
   `FieldId` order (so every existing seed expression / DSL string that
   references `close`/`volume`/etc. keeps resolving to the exact same ids) plus
   the two new trailing columns.
4. Append two trivial seed expressions (`"zscore(__ml_ae_alpha)"`,
   `"zscore(__ml_tcn_alpha)"`) to the existing `seed_exprs` list.

From here, the **unmodified** `SearchDriver`/`Factory` machinery does
everything else: `init_population` parses the two new strings exactly like any
hand-written seed (`"rank(close)"` etc., the existing convention —
`cascade_trial_count_test.cpp:262`), they become real, mutable, crossover-able
`Genome`s tagged `from_seed=true`, and they compete/reproduce/get admitted
through every existing gate. The GA is now generating candidates that build ON
TOP OF the learned signal (e.g. `rank(zscore(__ml_tcn_alpha) + close_ts_mean_5)`
after a few generations of crossover) — which is a **stronger** integration
than a static admitted candidate, not a weaker one: the ML signal enters the
same evolutionary substrate every symbolic alpha does, rather than being
special-cased.

**One nuance requiring a NEW (small) function, not reuse of `nn_to_candidate`
verbatim:** `nn_to_candidate`'s `pos_flat` defaults every unwritten cell to
`0.0` (`nn_source.cpp:220-221`), conflating "no opinion" (pre-lookback warm-up,
out-of-universe, or an incomplete/invalid window) with a genuine `0.0`
prediction. Panel's own missing-cell contract is NaN (panel.hpp:71-74,
"missing... reads back as NaN"). Reusing `pos_flat` as-is would silently inject
a flat, non-missing `0.0` into the warm-up period across the whole universe —
a look-ahead-adjacent artifact the zscore/rank seed ops would treat as real
cross-sectional information. **S6-1's `build_ml_seed_fields` therefore performs
its own walk of the `SequenceTensor`** (initializing the score buffer to NaN,
writing only `sample_valid[s]==1` cells, exactly mirroring `nn_to_candidate`'s
walk order and its 3-line kind dispatch — `predict_ae` for `Autoencoder`,
`predict_nn` for `Tcn`/`Gru`/`Attn`) rather than calling the (unexported,
anonymous-namespace) `predict_sample`/reusing `pos_flat` directly. This calls
only the public `predict_ae`/`predict_nn` (`learned_source.hpp:327,347`) — it
does not reimplement either forward pass.

---

## Determinism contract (Sprint 6)

Both levers are **opt-in fields with inert defaults**:

- `RunConfig::ml_seeds = false` — inert; discover's Panel and `seed_exprs` are
  completely untouched (no new field build, no new fit, no new seed strings).
- `RunConfig::sleeve_method` stays `{erc,hrp,invvol}`-selectable as before;
  `"nco"` is a new, additional closed-taxonomy value. Any existing value ⇒
  `RiskBudgetMethod::{InverseVol,EqualRiskContribution,HierarchicalRiskParity}`
  exactly as today ⇒ `MetaAllocator::allocate` takes the exact pre-existing
  code path.

At these defaults, `run_discover`/`run_all`/`run_metabook`'s pinned digests and
`books.bin` bytes are unchanged.

**Four test classes per opt-in field (mandatory, both levers):**
(a) off-path byte-identity — `--ml-seeds` unset ⇒ discover digest unchanged;
`--sleeve-method` ∈ `{erc,hrp,invvol}` ⇒ `MetaAllocator::allocate` output
unchanged (byte-identical `CapitalWeights.c`).
(b) on-path RED→GREEN — ML seeds enter gen-0 as valid, `from_seed=true`
genomes deterministically; `Nco` allocation `==` `HierarchicalRiskParity`'s
output on a single-cluster reduction, and is more robust (lower participation
concentration / better-conditioned reduced covariance) on a genuinely
nested-cluster fixture.
(c) twice-run — same (panel, seed) ⇒ byte-identical augmented Panel bytes,
byte-identical seed genomes, byte-identical `CapitalWeights.c`.
(d) seq==parallel — the ML fit is a single-threaded cold-path fit (no
parallel dispatch to prove invariant over); NCO's clustering + kernel calls are
pure functions of `Ω` with no shared mutable state, mirroring the "seq==
parallel reduces to no shared mutable state" argument p8-S1's ledger already
established for the analogous per-window factor fit.

---

## Dependency / wiring map

```
learn/feature_matrix.hpp:build_features        ← S6-1a builds the F-dim raw-field FeatureMatrix
learn/sequence_features.hpp:build_sequences    ← S6-1a windows it into a SequenceTensor (L*F)
learn/autoencoder_alpha.hpp:fit_autoencoder_factors ← S6-1a fits the AE (frozen math, called only)
learn/tcn_alpha.hpp:fit_tcn                    ← S6-1a fits the TCN (frozen math, called only)
learn/learned_source.hpp:predict_ae/predict_nn ← S6-1b scores every (date,inst) into NaN-default buffers
NEW factory/ml_seed_source.hpp/.cpp            ← S6-1a/b/c: MlSeedConfig, build_ml_seed_fields,
                                                  augment_panel_with_ml_seeds, ml_seed_exprs,
                                                  save_learned_model/load_learned_model
alpha/panel.hpp:Panel::create + field_all      ← S6-1b assembles the augmented Panel (reuse, no fork)
atx-impl/stage_discover.cpp:591,953,1104       ← S6-1d: when cfg.ml_seeds, swap panel for the
                                                  augmented Panel and append ml_seed_exprs() before
                                                  either SearchDriver/FactoryConfig is built
factory/search_driver.{hpp,cpp}                ← UNCHANGED (seam is entirely upstream, see decision above)
factory/genome.hpp                             ← UNCHANGED
atx-core/cluster/cluster.hpp:cluster           ← S6-2 clusters the sleeve correlation (raw, no RMT)
fund/meta_allocator.cpp:hrp_weights            ← S6-2 intra-cluster kernel (reused verbatim)
fund/meta_allocator.cpp:erc_log_barrier        ← S6-2 inter-cluster kernel (reused verbatim, on the
                                                  small K×K reduced covariance)
fund/meta_allocator.hpp:RiskBudgetMethod       ← S6-2 appends Nco (append-only enum)
atx-impl/config.cpp:"sleeve-method"            ← S6-2 extends the closed taxonomy string set
atx-impl/stage_run.cpp:sleeve_method_from_string ← S6-2 maps "nco" -> RiskBudgetMethod::Nco
tests/factory/ml_seed_source_test.cpp          ← S6-1
tests/fund/meta_allocator_nco_test.cpp         ← S6-2
atx-impl/tests/stage_discover_ml_seed_test.cpp ← S6-1d / S6-3
atx-impl/tests/stage_run_sleeve_nco_test.cpp   ← S6-2 / S6-3
```

---

## Tasks

### S6-0 — Ledger + config fields + enum append (do first; all units depend on this)

**Goal:** create the sprint ledger (marker commit); add the two inert RunConfig
fields + CLI flags; append `RiskBudgetMethod::Nco` + `MetaAllocatorConfig::
nco_clusters`; extend the `--sleeve-method` closed taxonomy. No behavior change
— the fields/enum value exist, nothing reads them non-inertly yet.

**Wiring:**

`atx-engine/include/atx/engine/fund/meta_allocator.hpp` — append-only enum
value (index 3, after the existing 3):

```cpp
enum class RiskBudgetMethod : atx::u8 {
  InverseVol,             // w_s ∝ 1/σ_s (equicorrelation closed form / cheap fallback)
  EqualRiskContribution,  // Spinu log-barrier ERC (the default; CCD, fixed iters)
  HierarchicalRiskParity, // HRP (López de Prado 2016; never inverts Ω)
  Nco,                    // S6: Nested Clustered Optimization (López de Prado 2019;
                          // cluster -> intra-cluster HRP -> inter-cluster ERC -> recombine)
};
```

and, on `MetaAllocatorConfig` (append at struct end — aggregate-init order is
load-bearing per the p9 contract):

```cpp
  // S6: target cluster count for RiskBudgetMethod::Nco (ignored by every other
  // method). 0 (the default) => auto = max(1, round(sqrt(S))), the López de Prado
  // heuristic; a caller override must be >= 1 and is clamped to <= S at call time
  // (a k > S is meaningless — every sleeve becomes its own singleton cluster).
  atx::usize nco_clusters = 0;
```

`atx-impl/src/config.hpp` — append at struct end (before `set_flags`), mirroring
the existing S5-hub bool/string field style (`config.hpp:308-309,320`):

```cpp
    // -- S6 (p9): ML seed source + NCO sleeve method --
    // --ml-seeds (S6): behind this flag, discover fits/loads a deterministic
    // AE + TCN learned-alpha pair and seeds the GA gen-0 pool with trivial DSL
    // expressions over their materialized score fields (see
    // factory/ml_seed_source.hpp). false (default) = discover's Panel and
    // seed_exprs are untouched -- byte-identical to today.
    bool ml_seeds = false;
    // --ml-seed-model-dir (S6): directory the ML seed models are cached under.
    // "" (default) = fit fresh every run, in-memory only (no persistence).
    // Non-empty + missing/empty dir = fit fresh AND save the two models there.
    // Non-empty + already populated = LOAD the cached models (skip the fit).
    // Ignored entirely when ml_seeds is false.
    std::string ml_seed_model_dir = "";
```

`atx-impl/src/config.cpp` — new bool flag (mirrors `dead-alpha-factors`,
`config.cpp:44`) + new string flag (mirrors `panel`/`out`, `config.cpp:55-62`)
+ extend the existing `sleeve-method` validator (`config.cpp:142-149`):

```cpp
    if (flag == "ml-seeds") { cfg.ml_seeds = true; return atx::core::Ok(); } // S6-0
    ...
    if (flag == "ml-seed-model-dir") { cfg.ml_seed_model_dir = value; return atx::core::Ok(); } // S6-0
    ...
    if (flag == "sleeve-method") {
        if (value != "erc" && value != "hrp" && value != "invvol" && value != "nco") {
            return atx::core::Err(EC::InvalidArgument,
                "--sleeve-method must be one of erc|hrp|invvol|nco: got '" + std::string(value) + "'");
        }
        cfg.sleeve_method = value;
        return atx::core::Ok();
    }
```

**Determinism:** pure addition; `RiskBudgetMethod::Nco == 3` is a NEW enum
value appended after the existing three (does not renumber `InverseVol=0`,
`EqualRiskContribution=1`, `HierarchicalRiskParity=2` — no aggregate-initializer
breakage). Nothing dispatches on it yet outside a `static_assert`/pin test.

**Accept:**
- Project compiles (debug + release), all existing `fund_*`, `AtxImplConfig*`,
  `AtxImplDiscover*` suites green.
- `RiskBudgetMethodFrozenPrefix` (new, `atx-engine/tests/fund/`): pins
  `InverseVol==0`, `EqualRiskContribution==1`, `HierarchicalRiskParity==2`,
  `Nco==3` via `static_assert` (mirrors the `RiskModelKind::Diagonal==0` pin
  pattern from p8-S1).
- `AtxImplConfigMlSeeds` (new, `atx-impl/tests/`): `--ml-seeds` sets
  `cfg.ml_seeds=true`; `--ml-seed-model-dir <path>` sets the field verbatim;
  defaults are `false`/`""`; `--sleeve-method nco` parses to
  `cfg.sleeve_method=="nco"`; `--sleeve-method bogus` still rejects
  (`InvalidArgument`).
- Ledger marker commit (this file + a `sprint-6-progress.md` stub).

**Commit:** `git add atx-engine/include/atx/engine/fund/meta_allocator.hpp atx-impl/src/config.hpp atx-impl/src/config.cpp atx-engine/tests/fund/risk_budget_method_frozen_prefix_test.cpp atx-impl/tests/config_ml_seeds_test.cpp atx-engine/plans/p9/sprint-6-progress.md` then commit with trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Wrapper (run in one PowerShell call — subagent env does not persist):**
```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset dev
cmake --build --preset dev --target atx-engine-fund-tests atx-impl-tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset dev -R "RiskBudgetMethodFrozenPrefix|AtxImplConfigMlSeeds" --output-on-failure
```
Expected FAIL before the edit (undeclared `Nco`/`ml_seeds`/`ml_seed_model_dir`
— compile errors), PASS after.

---

### S6-1 — ML seed source: deterministic gen-0 injection

Four sub-steps (a: fit, b: score+augment, c: model-dir cache, d: CLI wiring),
landed as one reviewable unit sharing one test file plus the CLI integration
test.

**Files:** NEW `atx-engine/include/atx/engine/factory/ml_seed_source.hpp`,
NEW `atx-engine/src/factory/ml_seed_source.cpp`,
NEW `atx-engine/tests/factory/ml_seed_source_test.cpp`,
`atx-impl/src/stage_discover.cpp`,
NEW `atx-impl/tests/stage_discover_ml_seed_test.cpp`.

**Interfaces (exact signatures):**

```cpp
// atx-engine/include/atx/engine/factory/ml_seed_source.hpp
namespace atx::engine::factory {

// The ML seed source's knobs. Deliberately tiny (mirrors TcnAlphaCfg's own
// "tiny by design" convention, tcn_alpha.hpp:62) -- this is a SEED PROVIDER,
// not a tunable production model; the AE/TCN architecture sizes are fixed
// small constants in the .cpp (kAeFactors=2, kTcnBlocks=1, kTcnChannels=4,
// kTrainEpochs=12, kEnsembleSize=2 -- the exact literals learn's own
// tiny_tcn_cfg() test fixture convention uses, learn_tcn_gru_alpha_test.cpp:118-131).
struct MlSeedConfig {
  atx::u64 seed = 0;                    // deterministic fit seed (== SearchConfig::master_seed)
  std::vector<std::string> raw_fields{"close", "volume"}; // F raw Panel fields fed to AE/TCN
  atx::usize lookback = 8;              // L: TCN window depth / AE anchor step
  std::string model_dir;                // "" => fit fresh, no persistence (RunConfig::ml_seed_model_dir)
};

// One derived score field per learned model, dense [dates*instruments],
// NaN outside a model's coverage (pre-lookback warm-up / out-of-universe /
// incomplete window -- NEVER 0.0, matching Panel's own missing-cell contract).
struct MlSeedFields {
  std::vector<std::string> field_names;         // {"__ml_ae_alpha", "__ml_tcn_alpha"}
  std::vector<std::vector<atx::f64>> columns;   // parallel to field_names
};

// Fit (or load, see MlSeedConfig::model_dir) the AE + TCN models over `panel`
// and score every (date, instrument) cross-section. PURE in (panel, cfg) when
// model_dir is empty; when model_dir is set, has the documented cache
// side-effect (fits+saves if absent, loads if present) -- see
// save_learned_model/load_learned_model below. Errors propagate from
// build_features/build_sequences/fit_autoencoder_factors/fit_tcn verbatim
// (e.g. an unknown raw field name, or a panel too short for `lookback`).
[[nodiscard]] atx::core::Result<MlSeedFields>
build_ml_seed_fields(const alpha::Panel &panel, const MlSeedConfig &cfg);

// Build a NEW Panel carrying every field of `panel` (same FieldId order --
// every existing seed_expr / DSL string keeps resolving identically) plus the
// MlSeedFields columns appended at the end. Pure composition over
// Panel::create (panel.hpp:88-90) + field_all (panel.hpp:161-164); does not
// fork Panel's storage model.
[[nodiscard]] atx::core::Result<alpha::Panel>
augment_panel_with_ml_seeds(const alpha::Panel &panel, const MlSeedConfig &cfg,
                            MlSeedFields *out_fields = nullptr);

// The trivial in-grammar seed expressions referencing the augmented fields,
// one per field, wrapped in zscore(...) -- the same normalization convention
// every hand-written seed uses (cascade_trial_count_test.cpp:262 uses bare
// "rank(close)"; zscore is chosen here because a raw NN score has no natural
// cross-sectional scale, unlike a price field).
[[nodiscard]] std::vector<std::string> ml_seed_exprs(const MlSeedFields &fields);

// S6-1c: flat, versioned little-endian serialization of a LearnedModel
// restricted to the raw-field, non-augmented shape this module produces
// (aug.pca == nullopt AND aug.interactions.empty() -- ATX_CHECKed; a model
// carrying pool-alpha/latent augmentation is a programmer error here, not a
// runtime path, since build_ml_seed_fields never requests augmentation).
[[nodiscard]] atx::core::Status
save_learned_model(const learn::LearnedModel &m, const std::string &path);
[[nodiscard]] atx::core::Result<learn::LearnedModel>
load_learned_model(const std::string &path);

} // namespace atx::engine::factory
```

**S6-1a — fit.** `build_ml_seed_fields`:
1. `combine::AlphaStore empty_store{};` (no pool-alpha features — raw fields
   only, matching `LearnedSignalSource`'s own documented raw-field-only
   contract, `learned_source.hpp:413-419`).
2. `learn::FeatureSpec fs; fs.raw_fields = cfg.raw_fields; fs.horizons = {1};`
   `ATX_TRY(auto fm, learn::build_features(panel, empty_store, fs));`
3. `learn::SeqFeatureSpec ss; ss.lookback = cfg.lookback; ss.drop_incomplete = false;`
   (keep incomplete-window rows as `sample_valid=0` rather than dropping them —
   S6-1b needs the full (date,inst) grid to write NaN explicitly, not silently
   skip cells) — `ATX_TRY(auto seq, learn::build_sequences(fm, ss));`
4. If `cfg.model_dir.empty()` or the two model files under it are absent: fit
   fresh —
   ```cpp
   learn::AeFactorCfg aecfg;
   aecfg.k_factors = std::min<atx::usize>(2, cfg.raw_fields.size()); // kAeFactors, clamped <= F
   aecfg.train.master_seed = cfg.seed;
   aecfg.train.epochs = 12; aecfg.train.ensemble_size = 2;   // tiny_tcn_cfg()-precedent literals
   ATX_TRY(auto ae_model, learn::fit_autoencoder_factors(seq, aecfg));

   learn::TcnAlphaCfg tcncfg;
   tcncfg.blocks = 1; tcncfg.kernel = 2; tcncfg.channels = 4;
   tcncfg.cpcv.embargo = 0.0;
   tcncfg.train.master_seed = cfg.seed;
   tcncfg.train.epochs = 12; tcncfg.train.ensemble_size = 2;
   ATX_TRY(auto tcn_model, learn::fit_tcn(seq, tcncfg));
   ```
   else load both via `load_learned_model`.
5. If `!cfg.model_dir.empty()` and the fit branch ran, `save_learned_model`
   both models before returning (populate the cache for next run).

**S6-1b — score.** For each fitted model, allocate
`std::vector<f64> col(panel.dates() * panel.instruments(), NaN)`; walk
`seq.n_samples` ascending, and for `seq.sample_valid[s] == 1`:
`col[seq.date_of[s] * panel.instruments() + seq.inst_of[s]] =`
`(model.kind == Autoencoder) ? predict_ae(model, seq.x subspan at the trailing step)`
`: predict_nn(model, seq.x subspan at the L*F window)` — the same 3-line kind
dispatch `nn_source.cpp`'s anonymous-namespace `predict_sample` encodes
(`nn_source.cpp:90-102`), reproduced here because that helper is not exported;
both branches call ONLY the public `predict_ae`/`predict_nn`
(`learned_source.hpp:327,347`).

**S6-1c — model-dir cache.** `save_learned_model`/`load_learned_model` write/
read: `kind` (u8), `coeffs` (empty here — NN/AE arms use `forests`/`nn`, not
`coeffs`), `blend_w`, `horizons`, `feat_mean`, `feat_sd`, `trial_count`,
`n_base_features`, `oos_score_series`, `nn.{lookback,n_seq_features,arch_dims,
arch_params,member_states}` as length-prefixed `f64`/`u64` blocks behind a
4-byte magic + version header (mirrors `FactorModelArtifact`'s own "flat
little-endian header + payload" convention from p8-S1's ledger). A
`static_assert`-free runtime `ATX_CHECK(!m.aug.pca.has_value() &&
m.aug.interactions.empty())` guards the scope restriction; `forests.empty()`
for every model this module produces (Linear/Gbt-only field, unused here).

**S6-1d — CLI wiring** (`atx-impl/src/stage_discover.cpp`): immediately after
step 3 (`alpha::Library lib{};`, `~:976`) and before the field-name collection
(`~:1014-1018`):
```cpp
    // S6-1d: --ml-seeds. Fits/loads the deterministic AE+TCN seed pair over the
    // (post-capacity-screen) panel, swaps `panel` for the augmented Panel, and
    // appends the two trivial seed expressions to seed_exprs -- BEFORE fields[]
    // is collected (so the new columns are visible to both discover paths) and
    // BEFORE sc/fcfg.seed_exprs is read below. false (default) => panel and
    // seed_exprs are byte-identical to today; this whole block is skipped.
    std::vector<std::string> ml_seed_exprs;
    if (cfg.ml_seeds) {
        factory::MlSeedConfig mlcfg;
        mlcfg.seed = cfg.seed;
        mlcfg.model_dir = cfg.ml_seed_model_dir;
        factory::MlSeedFields mlfields;
        ATX_TRY(auto augmented,
                factory::augment_panel_with_ml_seeds(panel, mlcfg, &mlfields));
        panel = std::move(augmented);
        ml_seed_exprs = factory::ml_seed_exprs(mlfields);
    }
```
and, at the existing `cfg.seed_exprs.empty()` validation (`~:953`, ungated
guard) plus both existing `seed_exprs` consumption sites, append
`ml_seed_exprs` to the effective list (`std::vector<std::string> eff_seed_exprs
= cfg.seed_exprs; eff_seed_exprs.insert(eff_seed_exprs.end(),
ml_seed_exprs.begin(), ml_seed_exprs.end());`), threading `eff_seed_exprs`
into `fcfg.seed_exprs` (`~:591`) and the ungated `SearchDriver` ctor
(`~:1104`) in place of `cfg.seed_exprs`. **`cfg.seed_exprs.empty()` still
requires at least one user-supplied `--seed-expr`** — the ML seeds are
additive, never a substitute for a real seed (fail-loud: an ML-seeds-only
invocation with zero `--seed-expr` still rejects, matching the existing "at
least one seed must parse" contract, search_driver.cpp:391).

**Determinism:** `ml_seeds=false` ⇒ the new block is skipped entirely — `panel`
and `seed_exprs` are the exact pre-S6 objects, byte-identical discover digest.
On-path: `mlcfg.seed = cfg.seed` (never thread/time); `build_features`/
`build_sequences` are PIT + order-fixed (feature_matrix.hpp:24-32,
sequence_features.hpp:25-29); `fit_autoencoder_factors`/`fit_tcn` are
deterministic seed-ensemble fits (R1, both headers); the NaN-default score
walk is ascending-sample order-fixed; `Panel::create` is a pure function of its
inputs.

**Accept:**
- `MlSeedFieldsDeterministic` (twice-run, same panel+seed ⇒ byte-identical
  `MlSeedFields.columns`, element-wise `std::bit_cast<u64>`).
- `MlSeedFieldsWarmupIsNan`: every cell before the first valid sample date (or
  outside the panel's universe) is NaN, never `0.0` — the RED case this test
  is written against is "reuse `nn_to_candidate`'s `pos_flat` directly", which
  fails this test by construction (0.0-filled warm-up).
- `AugmentedPanelPreservesOriginalFields`: `field_id("close")` /
  `field_cross_section` on the augmented Panel are byte-identical to the
  source Panel for every pre-existing field; `num_fields()` grows by exactly 2.
- `MlSeedExprsParseAndAnalyze` (RED→GREEN): `alpha::parse_expr` +
  `alpha::analyze` on each string from `ml_seed_exprs(fields)` succeeds against
  the augmented Panel's field dictionary (this is the "valid genome" proof —
  RED before S6-1b/c exist, since the fields/exprs are undeclared; GREEN after).
- `ModelDirRoundTrip`: `save_learned_model` then `load_learned_model` on a
  fitted AE/TCN model reproduces byte-identical `predict_ae`/`predict_nn`
  output on a held-out feature row.
- `ModelDirCachesAcrossCalls`: first `build_ml_seed_fields` call with a fresh
  `model_dir` populates it (files exist after); a second call with the SAME
  `model_dir` produces byte-identical `MlSeedFields` WITHOUT re-fitting
  (assert via a fit-call counter or timing floor — the test asserts output
  equality, which is the load-bearing property; a stubbed "fit must not be
  called" assertion is out of scope if it requires a mock seam not otherwise
  needed — acceptable to assert equality only, documented if the call-count
  assertion is dropped).
- `StageDiscoverMlSeedsInertOff` (atx-impl, off-path byte-identity): `run_discover`
  digest with `ml_seeds=false` is byte-identical to a pre-S6 baseline run.
- `StageDiscoverMlSeedsInjectsGenomes` (atx-impl, RED→GREEN): with `ml_seeds=true`
  + a tiny synthetic panel + `--population`/`--generations` small, at least one
  `res.all_scored` genome has `from_seed==true` AND its rendered DSL string
  references `__ml_ae_alpha` or `__ml_tcn_alpha` (proves the seam is live end
  to end, not just unit-tested in isolation).

**Commit:** `git add atx-engine/include/atx/engine/factory/ml_seed_source.hpp atx-engine/src/factory/ml_seed_source.cpp atx-engine/tests/factory/ml_seed_source_test.cpp atx-engine/CMakeLists.txt atx-impl/src/stage_discover.cpp atx-impl/tests/stage_discover_ml_seed_test.cpp atx-engine/plans/p9/sprint-6-progress.md` (the CMakeLists.txt edit is the explicit `src/factory/ml_seed_source.cpp` line — that list is NOT globbed, per p8-S1's own confirmed-by-reading precedent) with trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Wrapper:**
```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset dev
cmake --build --preset dev --target atx-engine-factory-tests atx-impl-tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset dev -R "MlSeed" --output-on-failure
```
Expected FAIL first (undeclared `MlSeedConfig`/`build_ml_seed_fields`/etc. —
compile errors, confirmed via a full build+link before any `.cpp` body is
written, mirroring p8-S1-1's own RED verification), PASS after.

---

### S6-2 — NCO allocator

**Files:** `atx-engine/include/atx/engine/fund/meta_allocator.hpp`,
`atx-engine/src/fund/meta_allocator.cpp`,
NEW `atx-engine/tests/fund/meta_allocator_nco_test.cpp`,
`atx-impl/src/stage_run.cpp`, `atx-impl/src/config.cpp` (taxonomy string, done
in S6-0), NEW `atx-impl/tests/stage_run_sleeve_nco_test.cpp`.

**Interfaces (exact signatures):** one new private static on `MetaAllocator`
(`meta_allocator.hpp`, alongside the existing `hrp_weights`/`erc_log_barrier`
declarations):

```cpp
  // NCO (A2-successor, López de Prado 2019): (1) cluster the RAW sleeve
  // correlation (no RMT step -- MetaAllocator has no sample-size context to
  // supply rmt_clean's q=N/T, and hrp_weights already clusters raw
  // correlation-distance with no RMT step either, meta_allocator.cpp:266-283);
  // (2) intra-cluster weights = hrp_weights(Omega restricted to the cluster) --
  // REUSED verbatim, zero new HRP math; (3) reduce to a K x K inter-cluster
  // covariance (K = cluster count) via the intra-cluster weights; (4)
  // inter-cluster weights = erc_log_barrier(reduced, equal budget, iters) --
  // REUSED verbatim; (5) recombine: w_s = w_inter[cluster(s)] * w_intra[s].
  // K==1 (single cluster, e.g. a thin or highly homogeneous sleeve set)
  // collapses to EXACTLY hrp_weights(Omega) -- w_inter's single entry
  // normalizes to 1.0 by construction (any positive scalar divided by itself
  // is exactly 1.0 in IEEE754), so this is a provable identity, not an
  // approximation; the acceptance test below pins it bit-for-bit.
  [[nodiscard]] static atx::core::linalg::VecX
  nco_weights(const atx::core::linalg::MatX &Omega, atx::usize target_clusters,
              atx::usize iters);
```

and the `risk_budget_weights` dispatch (`meta_allocator.cpp:338-352`) gains the
exhaustive-switch arm the compiler otherwise flags as missing (the same
"append the enum, the switch tells you what to fix" discipline
`ModelKind`'s own header documents, `learned_source.hpp:83-92`):

```cpp
  case RiskBudgetMethod::Nco: {
    const atx::usize s = static_cast<atx::usize>(Omega.rows());
    const atx::usize k = /* MetaAllocatorConfig::nco_clusters, threaded through
                            allocate() as a new parameter to risk_budget_weights
                            -- see the signature-plumbing note below */;
    return nco_weights(Omega, k, iters);
  }
```

**Signature-plumbing note:** `risk_budget_weights` (`meta_allocator.cpp:
338-342`) does not currently receive `cfg` (only `Omega`, `sleeve_vol`, `b`,
`iters`) — its sole caller, `allocate()` (`meta_allocator.cpp:504`), has `cfg`
in scope. Add ONE trailing parameter `atx::usize nco_clusters` to
`risk_budget_weights` (both the private declaration in the header and the
`.cpp` definition), threaded from `allocate()`'s existing `cfg.nco_clusters`.
This is a private-method-only signature change (not `allocate`'s public
surface) — no caller outside this TU is affected.

**Implementation (`nco_weights`, `meta_allocator.cpp`, new function directly
below `hrp_weights`):**

```cpp
namespace {
// Raw Pearson correlation from Omega (unlike corr_distance, this returns rho
// itself, not the sqrt(0.5(1-rho)) distance transform -- the shared sigma_i/
// sigma_j computation is the only overlap with corr_distance, factored out
// for one-line reuse rather than duplicated verbatim).
[[nodiscard]] atx::core::linalg::MatX raw_correlation(const atx::core::linalg::MatX &Omega) {
  const Eigen::Index s = Omega.rows();
  atx::core::linalg::MatX rho(s, s);
  for (Eigen::Index i = 0; i < s; ++i) {
    const atx::f64 si = std::sqrt(Omega(i, i));
    for (Eigen::Index j = 0; j < s; ++j) {
      const atx::f64 sj = std::sqrt(Omega(j, j));
      const atx::f64 denom = si * sj;
      rho(i, j) = std::clamp(denom > 0.0 ? Omega(i, j) / denom : 0.0, -1.0, 1.0);
    }
  }
  return rho;
}
} // namespace

atx::core::linalg::VecX MetaAllocator::nco_weights(const atx::core::linalg::MatX &Omega,
                                                   atx::usize target_clusters,
                                                   atx::usize iters) {
  const auto s = static_cast<atx::usize>(Omega.rows());
  atx::core::linalg::VecX w(static_cast<Eigen::Index>(s));
  if (s == 0U) { return w; }
  if (s == 1U) { w[0] = 1.0; return w; }

  const atx::usize k_req = (target_clusters == 0U)
      ? std::max<atx::usize>(1U, static_cast<atx::usize>(std::lround(std::sqrt(static_cast<atx::f64>(s)))))
      : target_clusters;
  const atx::usize k = std::min<atx::usize>(k_req, s); // cluster() requires k <= s

  const atx::core::linalg::MatX rho = raw_correlation(Omega);
  atx::core::cluster::ClusterConfig ccfg;
  ccfg.algo = atx::core::cluster::Algo::Hierarchical; // deterministic, RNG-free (cluster_panel.hpp precedent)
  ccfg.k = static_cast<int>(k);
  const auto clustering = atx::core::cluster::cluster(rho, ccfg);
  if (!clustering.has_value()) {
    return hrp_weights(Omega); // degenerate partition -> HRP fallback (never throws, §0.8 spirit)
  }
  const std::vector<int> &label = clustering.value().cluster_id;
  const atx::usize n_clusters = static_cast<atx::usize>(clustering.value().n_labels);

  // Per-cluster member indices, ascending (order-fixed).
  std::vector<std::vector<atx::usize>> members(n_clusters);
  for (atx::usize i = 0; i < s; ++i) {
    members[static_cast<atx::usize>(label[i])].push_back(i);
  }

  // Intra-cluster weights (REUSE hrp_weights verbatim on each submatrix) +
  // the K x K inter-cluster reduced covariance.
  std::vector<atx::core::linalg::VecX> w_intra(n_clusters);
  atx::core::linalg::MatX reduced(static_cast<Eigen::Index>(n_clusters),
                                  static_cast<Eigen::Index>(n_clusters));
  for (atx::usize c = 0; c < n_clusters; ++c) {
    const auto &idx = members[c];
    atx::core::linalg::MatX sub(static_cast<Eigen::Index>(idx.size()),
                                static_cast<Eigen::Index>(idx.size()));
    for (atx::usize a = 0; a < idx.size(); ++a) {
      for (atx::usize b = 0; b < idx.size(); ++b) {
        sub(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) =
            Omega(static_cast<Eigen::Index>(idx[a]), static_cast<Eigen::Index>(idx[b]));
      }
    }
    w_intra[c] = hrp_weights(sub); // VERBATIM reuse, zero new HRP math
  }
  for (atx::usize c1 = 0; c1 < n_clusters; ++c1) {
    for (atx::usize c2 = 0; c2 < n_clusters; ++c2) {
      atx::f64 v = 0.0; // w_intra[c1]^T Omega[members[c1], members[c2]] w_intra[c2]
      for (atx::usize a = 0; a < members[c1].size(); ++a) {
        for (atx::usize b = 0; b < members[c2].size(); ++b) {
          v += w_intra[c1][static_cast<Eigen::Index>(a)] *
               Omega(static_cast<Eigen::Index>(members[c1][a]), static_cast<Eigen::Index>(members[c2][b])) *
               w_intra[c2][static_cast<Eigen::Index>(b)];
        }
      }
      reduced(static_cast<Eigen::Index>(c1), static_cast<Eigen::Index>(c2)) = v;
    }
  }

  // Inter-cluster weights (REUSE erc_log_barrier verbatim, equal budget 1/K).
  const std::vector<atx::f64> equal_budget(n_clusters, 1.0 / static_cast<atx::f64>(n_clusters));
  const atx::core::linalg::VecX w_inter =
      erc_log_barrier(reduced, std::span<const atx::f64>{equal_budget}, iters);

  // Recombine: w_s = w_inter[cluster(s)] * w_intra[cluster(s)][position of s].
  for (atx::usize c = 0; c < n_clusters; ++c) {
    for (atx::usize p = 0; p < members[c].size(); ++p) {
      w[static_cast<Eigen::Index>(members[c][p])] =
          w_inter[static_cast<Eigen::Index>(c)] * w_intra[c][static_cast<Eigen::Index>(p)];
    }
  }
  return normalize_sum1(std::move(w)); // defensive; already sums to 1 by construction
}
```

`atx-impl/src/stage_run.cpp` — extend `sleeve_method_from_string`
(`stage_run.cpp:23-27`):
```cpp
[[nodiscard]] static fund::RiskBudgetMethod sleeve_method_from_string(const std::string &s) {
    if (s == "erc") return fund::RiskBudgetMethod::EqualRiskContribution;
    if (s == "hrp") return fund::RiskBudgetMethod::HierarchicalRiskParity;
    if (s == "nco") return fund::RiskBudgetMethod::Nco; // S6-2
    return fund::RiskBudgetMethod::InverseVol; // "invvol" and any defensive fallback
}
```

**Determinism:** `sleeve_method ∈ {erc,hrp,invvol}` ⇒ the `Nco` case is never
reached; `nco_weights`/`raw_correlation` are never called; `MetaAllocator::
allocate` output is byte-identical (same code path as today). On-path: no RNG,
no clock; `cluster()`'s `Hierarchical` algorithm is deterministic
(`cluster_panel.hpp:76-83` documents this — the reason `SpongeSym` is NOT the
NCO default either); every reduction (correlation, cluster-submatrix build,
reduced-covariance quad form) is ascending-index order-fixed, matching every
existing kernel in this file.

**Accept:**
- `NcoInertOffPreexistingMethods`: `RiskBudgetMethod::{InverseVol,
  EqualRiskContribution,HierarchicalRiskParity}` produce byte-identical
  `CapitalWeights.c` vs a pre-S6 build on the same (Omega, sleeve_vol, caps)
  fixture (off-path byte-identity).
- `NcoEqualsHrpOnSingleCluster` (RED→GREEN, the load-bearing proof from the
  brief's acceptance bar): `nco_clusters=1` on any Omega ⇒
  `MetaAllocator{method=Nco}.allocate(...)` produces
  `CapitalWeights.c` **element-wise `std::bit_cast<u64>`-identical** to
  `MetaAllocator{method=HierarchicalRiskParity}.allocate(...)` on the same
  inputs. RED before `nco_weights` exists (undeclared); GREEN after, and the
  bit-exactness is the mathematically-provable identity documented above (the
  K=1 `w_inter` normalizes to exactly `1.0`), not a tolerance-based near-match.
- `NcoMoreRobustOnNestedClusters` (RED→GREEN): a synthetic `Omega` with two
  well-separated correlation blocks (high intra-block ρ≈0.8, low inter-block
  ρ≈0.05 — the textbook NCO fixture) plus one noisy near-singular direction.
  `Nco` with `nco_clusters=2` yields a strictly LOWER max per-sleeve weight
  concentration (`max_i c_i / Σc_i`) than plain `HierarchicalRiskParity` on the
  identical `Omega` — HRP's single-linkage tree can be misled by the noisy
  direction into an unbalanced bisection, while NCO's cluster-then-recombine
  isolates it to one small intra-cluster share.
- `NcoTwiceRunByteIdentical`: same `(Omega, sleeve_vol, caps)` ⇒ byte-identical
  `CapitalWeights.c` across two independent `allocate()` calls.
- `NcoClustersZeroAutoSqrtS`: `nco_clusters=0` on an `S=9` fixture resolves to
  `k=3` (`round(sqrt(9))`), verified via the resulting weight vector's cluster
  structure (or a targeted unit test on a k-derivation helper if factored out).
- `StageRunSleeveMethodNco` (atx-impl): `--sleeve-method nco` end-to-end through
  `run_metabook` produces a `books.bin` distinct from `hrp`'s on a fixture where
  they must differ (multi-cluster), and identical CapitalWeights-derived sizing
  to `hrp` on a single-cluster fixture (mirrors the two engine-level proofs at
  the wired layer).

**Commit:** `git add atx-engine/include/atx/engine/fund/meta_allocator.hpp atx-engine/src/fund/meta_allocator.cpp atx-engine/tests/fund/meta_allocator_nco_test.cpp atx-impl/src/stage_run.cpp atx-impl/tests/stage_run_sleeve_nco_test.cpp atx-engine/plans/p9/sprint-6-progress.md` with trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Wrapper:**
```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset dev
cmake --build --preset dev --target atx-engine-fund-tests atx-impl-tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset dev -R "Nco|SleeveMethodNco" --output-on-failure
```
Expected FAIL first (missing exhaustive-switch case is a compile error the
moment `Nco` exists without a `risk_budget_weights` arm — confirm this
compile-error RED before writing `nco_weights`'s body, mirroring how
`ModelKind`'s own header describes the enum-append discipline), PASS after.

---

### S6-3 — Integration tests: the four determinism classes, end to end

**Goal:** close the sprint with `atx-impl`-level tests that exercise BOTH
levers together through the real CLI surface (`run_discover`/`run_all` +
`run_metabook`), proving the four mandatory classes at the wired layer (S6-1/
S6-2's own accept tests already prove them at the engine-unit layer — this is
the integration capstone, mirroring p8-S1's "Bench / acceptance (sprint close)"
section).

**Files:** `atx-impl/tests/stage_discover_ml_seed_test.cpp` (extend from
S6-1), `atx-impl/tests/stage_run_sleeve_nco_test.cpp` (extend from S6-2), no
new production files.

**Tests (four classes, both levers, one small synthetic panel fixture shared
across the four — a tiny multi-instrument OHLCV panel serialized to a temp
`.bin`, per the p9 "short deterministic fixtures only" testing directive):**

(a) **Off-path byte-identity, combined:** `--ml-seeds` unset AND
`--sleeve-method hrp` (or any non-`nco` value) together ⇒ `run_all`'s folded
run digest (the 6-stage `fnv1a64` fold, `stage_run.cpp`'s `run_all`) is
byte-identical to a pre-S6 baseline run on the same fixture — proves neither
lever leaks into the other's off-path, and neither leaks into the surrounding
6-stage pipeline.

(b) **On-path RED→GREEN, combined:** `--ml-seeds` + `--metabook --sleeve-method
nco` together on the fixture ⇒ (i) at least one admitted alpha's DSL
references an `__ml_*` field (the S6-1 proof, re-run at the `run_all` layer);
(ii) `books.bin`'s sleeve capital weights differ from the same run with
`--sleeve-method hrp` (the S6-2 proof, re-run at the `run_metabook` layer) on
a fixture constructed to have genuine multi-cluster sleeve structure.

(c) **Twice-run:** the SAME `run_all` invocation (same `--seed`, same fixture,
both flags on) executed twice in-process (or as two separate process
invocations against the same temp dir) produces byte-identical `books.bin` and
run digest.

(d) **seq==parallel:** `--workers 1` vs `--workers 4` (both with `--ml-seeds`
set — the ML fit itself is single-threaded, but this proves the augmented
Panel + extra seed exprs do not perturb `SearchDriver`'s existing
worker-count-invariance contract, F2) produce byte-identical discover digests;
`MetaAllocator::allocate` has no thread dispatch of its own to vary (documented
as a structural "no shared mutable state" argument, consistent with how p8-S1
resolved the same class for its per-window factor fit).

**Accept:** all four tests green; full existing `atx-impl-tests` +
`atx-engine-factory-tests` + `atx-engine-fund-tests` suites re-run green (zero
regressions) after S6-0/S6-1/S6-2 land.

**Commit:** `git add atx-impl/tests/stage_discover_ml_seed_test.cpp atx-impl/tests/stage_run_sleeve_nco_test.cpp atx-engine/plans/p9/sprint-6-progress.md` with trailer `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Wrapper:**
```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
cmake --preset dev
cmake --build --preset dev --target atx-impl-tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ctest --preset dev -R "AtxImpl" --output-on-failure
```
Expected: full green, including every pre-existing `AtxImpl*` suite (the
regression gate) plus the new combined-lever tests.

---

## Sequencing

1. **S6-0 first** (ledger + config fields + enum append) — every unit reads
   `RunConfig::ml_seeds`/`ml_seed_model_dir` and `RiskBudgetMethod::Nco`.
2. **S6-1** and **S6-2** are fully independent (disjoint files: `factory/
   ml_seed_source.*` + `stage_discover.cpp` vs `fund/meta_allocator.*` +
   `stage_run.cpp`) — implement serially per the SDD rule, either order.
3. **S6-3** last — the combined-lever integration proof, depends on both.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| A learned model's warm-up/incomplete cells silently read as `0.0` instead of NaN if `nn_to_candidate`'s `pos_flat` is reused directly | A flat, non-missing signal contaminates the GA's cross-sectional zscore/rank seed ops during the warm-up window | S6-1b's own NaN-default walk (documented above) is the guardrail — the `MlSeedFieldsWarmupIsNan` test is the gate. |
| Fitting AE+TCN inline on every discover run is slow / non-deterministic if seeded from wall-clock | Violates the "no long sweeps" + F1 determinism contract | `mlcfg.seed = cfg.seed` (never thread/time); tiny fixed architecture constants (S6-1a); `ml_seed_model_dir` caching makes repeat runs skip the fit entirely. |
| `NcoEqualsHrpOnSingleCluster`'s bit-exactness claim is fragile to floating-point reordering | The RED→GREEN acceptance bar could flake | The proof is NOT "the two converge numerically" — it is `w_inter[0] == 1.0` exactly (any positive finite scalar divided by itself is exactly 1.0 in IEEE754 division), so `w_final == w_intra == hrp_weights(Omega)` termwise by the `nco_weights` recombination formula itself, not an approximation. Documented explicitly in the S6-2 section above. |
| `cluster()`'s `k` could exceed the (possibly small) sleeve count `S` on a thin fund | `atx::core::cluster::cluster` requires `k <= s` (per `cluster_panel.hpp`'s own `ccfg.k = min(cfg.k, m)` guard) | `nco_weights` clamps `k = std::min(k_req, s)` before calling `cluster()`, mirroring `cluster_panel.hpp:353`'s exact pattern. |
| `--ml-seeds` set with zero `--seed-expr` | Could silently produce an ML-only search with no human-authored seed at all, an unreviewed scope creep | The existing `cfg.seed_exprs.empty()` guard (`stage_discover.cpp:953`) is preserved unchanged — ML seeds are strictly additive to `seed_exprs`, never a substitute; discover still fails loud with zero seeds. |
| Ownership drift: the ROADMAP's terse S6 row omits `stage_discover.cpp`/`stage_run.cpp` | An implementer following only the terse row could ship a parsed-but-inert `--ml-seeds`/`--sleeve-method nco` — exactly the Potemkin-flag pattern p9 exists to fix | This plan's "Owns (exclusive)" section explicitly adds both files with a flagged rationale; the reconciler should update the ROADMAP's ownership row to match (same precedent as p8-S1's ledger correcting its own brief's file:line citations). |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, the `AtxImplDiscover`
  determinism slice, and `LibraryVerdict.AdmitKindEnumFrozenPrefix` all
  unchanged with `ml_seeds=false` and `sleeve_method != "nco"`.
- **Per-task RED→GREEN:** each opt-in (`MlSeedExprsParseAndAnalyze`,
  `StageDiscoverMlSeedsInjectsGenomes`, `NcoEqualsHrpOnSingleCluster`,
  `NcoMoreRobustOnNestedClusters`) RED before the wire, GREEN after.
- **Quantified NCO win:** on the nested-cluster fixture, record max per-sleeve
  weight concentration for `{HierarchicalRiskParity, Nco}` — NCO must show
  strictly lower concentration (the concrete, measured S6-2 claim, mirroring
  p8-S1's own "factor-model win, quantified" convention).
- **Twice-run + seq==parallel** on both levers (S6-3d).
- **Genuinely new call site, measured:** confirm via grep that
  `learn::fit_autoencoder_factors`/`learn::fit_tcn` now have >= 1 call site in
  `atx-impl/src` (S6-1's whole point — closing the "zero generation call
  sites" gap the ROADMAP's Potemkin-book table opens with).

---

## Out of scope

- Meta-labeling / triple-barrier / sample-uniqueness weighting (design spec
  §7, explicitly deferred — noted there as "lands only if cheap"; it is not
  cheap relative to this sprint's two levers and is not attempted here).
- A DSL op that embeds/evaluates a serialized NN inline (considered and
  rejected — see "The ML seed seam decision"; would require new `Expr::Kind`,
  bytecode, and `Ast` serialization for no proven benefit over the field-seed
  approach).
- RMT-cleaning the sleeve correlation before NCO's clustering step — would
  require a new sample-size parameter on `MetaAllocator::allocate`'s public
  signature (`Ω`'s originating `T`), which no existing caller supplies; `hrp_weights`
  already sets the no-RMT precedent this sprint follows.
- Any change to `factory/search_driver.{hpp,cpp}` or `factory/genome.hpp` —
  proven unnecessary (see grounding table + seam decision).
- Extending `--ml-seeds` to the `Gru`/`Attn` learned-alpha kinds (the design
  spec + ROADMAP name only `autoencoder_alpha`/`tcn_alpha`; `learn::fit_gru`/
  `fit_attn` are structurally reachable via the exact same seam — this is
  flagged as a natural cheap follow-up, not attempted here to keep the sprint
  bite-sized).
- Running `--ml-seeds`/`--sleeve-method nco` over a real full panel — the p9
  "no long sweeps" directive; V1 real-panel validation is the operator's step
  (per the top ROADMAP), and S7's recipe correction decides whether these two
  flags are enabled in the prod recipe at all (this sprint does not touch
  `build-megaalpha-book.ps1`).
