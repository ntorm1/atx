# Platform core migration: the engine's research layer (lane YARCH, Ruling PM8-12)

Companion of `docs/plans/2026-10-02-platform-core-audit.md` (classification, duplications, ranking). This plan says
where the engine-bound Python goes, how Python drives it afterwards, how each move proves identity, and in which
slices it happens. Slices 1 and 2 are implemented on branch `feat/platform-v8-yarch-20261002`.

## 1. Target layout

One research layer in `atx-engine`, as small libraries with their own test targets. Each library links `atx::core`
and only the engine pieces it needs, so a change in one rebuilds one library and one test executable, never the whole
`atx-engine` archive or its PCH.

```
atx-engine/include/atx/engine/research/
  fields/        generic point-in-time field builder                     lib atx-engine-research-fields
    clock.hpp          day arithmetic, strict ISO dates, the reader-side seal (research_window.hpp)
    role_axes.hpp      pinned role: manifest SHA-256, axes, member mask, seal check
    role_rows.hpp      streamed, hash-verified role payload rows (volume.f64, present.u8, ...)
    field_writer.hpp   exclusive date-major f64 writer, canonical NaN, coverage, numpy-exact statistics
    field_spec.hpp     field definition, canonical JSON, formula_sha256 (the reuse fingerprint's formula part)
    asof_series.hpp    the as-of clock rule (latest row with available_day < session day, staleness)
    trailing_mean.hpp  the trailing-window rule (row t from sessions t-w..t-1 only)
    volume_mean_field.hpp, finra_asof_field.hpp   ported builders (slice 1); one header per builder group
    (next) sources/    parquet stage readers (TickerHistory3, SEC stages, 13F/FTD, fundamentals events, bridge)
    (next) manifest.hpp, reuse.hpp, registry.hpp   publish-last manifest, reuse decision, builder registry
  composition/   the composition rule registry                            lib atx-engine-research-composition
  admission/     gate statistics, sign rule, redundancy, marginal reuse    lib atx-engine-research-admission
  ledger/        trial counting, era pooling, DSR inputs                    lib atx-engine-research-ledger
  roles/         role writer, membership rule, factor-break-v1             lib atx-engine-research-roles
```

What each library takes over (owners from the audit):

| Library | From Python | Builds on (exists) | Test target |
|---|---|---|---|
| fields | `prepare_research_fields.py` builders, `research_fields_*.py`, `research_fields_sec.py` Calendar/Latest/Windowed | `core/sha256`, `data/research_window.hpp`; later `data/fundamental_fields`, `data/fundamental_clock_artifact` | `atx-engine-research-fields-tests` |
| composition | fit side of `composition_*.py` and `fit_composition_weights.py` (`ew_theme_*`, `fit_weights`) | the rule wrappers `atx-impl/src/strategy_ic_{composition,shrink,theme_resid,theme_erc}` move down; kernels `combine/group_{rerank,shrink,residualise,erc}` stay | `atx-engine-research-composition-tests` |
| admission | `screen_v3`, `screen_v4`, `newey_west_t`, greedy redundancy, `f_theta`; marginal reuse of `strategy_marginal_ic` | `eval/hac.hpp`, `combine/marginal_rank_ic` | `atx-engine-research-admission-tests` |
| ledger | `backtest_integrity` ledger rules and `trial_counts`, `dsr_total.py`, `era_pool.py` | `eval/trial_registry`, `eval/deflated_sharpe`, `eval/min_trl`, `eval/pbo`, `eval/trial_clusters`; `atx-impl/src/trial_ledger.cpp` moves down | `atx-engine-research-ledger-tests` |
| roles | `prepare_recent_research.py` role writer, `repair_role_factor_breaks.py` | `data/strategy_data` (reader), `data/universe` | `atx-engine-research-roles-tests` |

The composition move is a relocation, not a rewrite: YCOMB owns the rules in atx-impl; the slice that moves them
down is scheduled after the Y merge, and the atx-impl files become thin includes (no behaviour change, identity by the
existing `strategy_ic_*_test.cpp` suites).

## 2. The Python wrapper contract

Recommendation: **executables with a JSON spec in and a JSON receipt out**, as today
(`atx-equity-strategy-ic --plan-only`, `research_cycle.py` driving exes through `run_bounded_research.py`). One new
executable per library verb: `atx-research-fields build --spec SPEC.json --receipt OUT.json` (slice 3), later
`atx-research-admission`, `atx-research-ledger`. Python keeps argv parsing for operators, spec templating, the cycle,
the ledger file and reports.

| | Executables + JSON (recommended) | pybind11 module |
|---|---|---|
| Exists | yes: every strategy verb, `run_bounded_research.py` caps time and RSS per process | `python/` builds `atxpy._core` with scikit-build-core and a **pip** pybind11; pybind11 is not in `vcpkg.json` and no CMake preset builds it |
| Build cost on this host | one small exe per verb in the warm `build-equity` tree (target-scoped, ccache) | a second full engine build in the wheel's own tree (Release CRT, its own FetchContent), and /WX relaxed for the extension (its CMake says pybind11 trips the gate) |
| Isolation | a crash, an over-cap allocation or a leak ends one process; receipts are files root can pin | in-process: a fault kills the cycle; memory caps are cooperative only |
| Identity / provenance | argv + spec SHA-256 + exe build provenance are recorded in the receipt and the ledger | needs a separate provenance channel (module build id) |
| Data volume | payloads are files already (date-major f64), so no marshalling cost | zero-copy numpy views are its one advantage; not needed for file-to-file builders |

Cost of the recommendation: one `main` per verb (about 100-200 lines of argv/spec/receipt glue each, shared through a
small `research/cli` helper) and a process start per call (milliseconds, against builds of minutes). Cost of
pybind11: a vcpkg manifest change (or keeping the pip dependency outside the presets), a second build configuration
on a memory-starved host, and a relaxed warning gate. `atxpy` stays for interactive exploration; it is not a
production path.

## 3. Identity discipline

1. Every migrated builder or rule proves **byte-identity against the Python output on a committed synthetic
   fixture** before any Python is deleted: the fixture directory holds the synthetic inputs, a generator script that
   runs the EXISTING Python on them, and the Python's outputs; a pytest re-runs the generator and must reproduce the
   committed bytes (the Python side), and a gtest reads the same inputs and must reproduce the same bytes (the C++
   side). Slice 1: `atx-engine/tests/fixtures/research_fields/`.
2. What "identical" covers: payload bytes (f64 date-major, canonical NaN `0x7ff8000000000000`), SHA-256, every
   coverage count, min/max/mean and the five quantiles exactly (numpy's pairwise summation, axis-0 reduction order and
   linear quantile are reproduced, not approximated), the formula fingerprint (`formula_sha256`, the SHA-256 of the
   canonical definition, equal to the Python's for the same spec text). Manifest bytes are NOT claimed equal: the
   Python manifest carries the Python builder's own code identity and absolute paths; the engine manifest carries the
   engine's.
3. **A flag selects the engine path**; flag absent = the Python path, byte-identical outputs. For fields:
   `prepare_research_fields.py --engine-fields NAME[,NAME...] --engine-exe PATH` (slice 3) computes the named fields
   with `atx-research-fields` and the rest in Python, in one manifest. A field moves to the engine path by default
   only after root's identity run on the TRAIN role (payload SHA-256 equal to the v13 manifest's pin), and its Python
   builder is deleted one slice later.
4. Look-ahead: every builder gets a probe test (perturb one input dated D; no output row whose clock is at or before D
   may change) and the probe is shown to fail on a planted leaky variant, so the probe has teeth.
5. Nothing existing changes behaviour: new libraries and targets are `EXCLUDE_FROM_ALL`, new Python is beside the
   old, and no existing file's output moves.

## 4. Slices

| Slice | Content | Build targets added | Python retired after identity |
|---|---|---|---|
| 1 (done) | fields framework + `vol_126` (price/volume) + `si_shares`/`si_dtc` (publication-lagged as-of) | `atx-engine-research-fields`, `atx-engine-research-fields-tests` | none yet (identity on the fixture first) |
| 2 (done) | one spec-driven library generator (`generate_from_spec.py`), spec reproducing v71 | none (Python + the existing IC exe's K1 plan) | the eleven class-C generators/checker, 5,069 lines, after root's `--check` run |
| 3 | `atx-research-fields` exe (spec in, manifest + receipt out), publish-last manifest, reuse decision; `--engine-fields` flag in the Python builder | `atx-research-fields` | `finra_field`, `parse_asof_csv`, `read_schedule`, `volume_mean_rows` after the TRAIN identity run |
| 4 | parquet source readers (TickerHistory3 via vcpkg arrow/parquet), factor-break-v1, the price-module builders | (in the fields lib) | `research_fields_price.py`, `factor_breaks`, `repair_role_factor_breaks.py` |
| 5 | SEC clock primitives (Calendar/Latest/Windowed), SEC and holdings builders | (in the fields lib) | `research_fields_sec.py`, `_v9`, `_holdings` |
| 6 | composition relocation + fit verb | `atx-engine-research-composition(-tests)`, `atx-research-composition` | the fit side of `composition_*.py` |
| 7 | admission screens | `atx-engine-research-admission(-tests)` | `screen_v3`/`screen_v4` and their numerics in the fitter |
| 8 | ledger: counting, DSR inputs, era pooling (the acceptance statistics on the engine's eval code) | `atx-engine-research-ledger(-tests)` | `backtest_integrity.py` statistics, `dsr_total.py`, `era_pool.py` |
| 9 | roles: role writer and membership rule | `atx-engine-research-roles(-tests)` | `prepare_recent_research.py` numerics |
| 10 | fundamentals: filing events and fiscal-period arithmetic onto `data/fundamental_fields` | (fields/sources) | `build_fundamental_events.py`, `export_fundamental_fields.py`, `pit_fundamental_clock.py` |

Each slice: implement, fixture identity (pytest + gtest), root builds the named targets, root runs the identity on the
real TRAIN inputs where a payload exists, then the next slice deletes the superseded Python. Slices 3-10 wait for the
Y-lane merge and the v8 freeze gate (PM5-21 holds no executable or cycle-script change until then).

## 5. Risks

* Numeric identity with numpy is exact only where the reduction order is reproduced; slice 1 pins it with
  experiments (1-D sum = pairwise from 0.0 with 8 accumulators and 128-blocks; axis-0 sum of a (126, n) C-order array =
  sequential from 0.0 in row order when n >= 2, pairwise when n == 1; linear quantile = numpy's `_lerp`). A numpy
  upgrade can move the Python side; the fixture pytest catches it.
* The spec text of a field (units, clock, staleness, definition) is copied into C++ verbatim for the formula
  fingerprint; the fingerprint test pins the copy against the Python's value. One source for spec text (a JSON field
  registry both sides read) is slice 3 work.
* Composition and admission moves touch code the Y lanes own today; they are scheduled after the merge.
