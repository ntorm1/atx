# Platform core audit: the Python research surface (lane YARCH, Ruling PM8-12)

Base `798d3b23` (branch `feat/platform-v8-yarch-20261002`). Owner directive PM8-12: stop growing versioned Python
scripts; build reusable engine functionality and keep Python as a thin wrapper. This audit classifies every module of
the Python research surface, says where each numerical piece belongs in the C++ engine, and ranks the migrations.
The plan that executes it is `docs/plans/2026-10-02-platform-core-migration.md`.

Scope (line counts are `splitlines()` of the files at the base; the table is generated from disk by a classification
script, so the totals add up): `atx-engine/tools/*.py`, `atx-impl/strategies/*.py`, `scripts/research_*.py`,
`scripts/run_bounded_research.py`, `scripts/cycle_*.py` (imported by `research_cycle.py`), `atx-impl/tools/*.py`
(the fitter, composition rules, integrity and report tools). `atx-impl/tools/mega_report/` (8,241 lines, the pitch
renderer) is report glue (class A) and listed only as a total.

Classes:

* **A** orchestration, spec, receipt or report glue: stays Python.
* **B** numerical or research logic that belongs in the engine. Each row names the owning C++ module, whether it exists
  (EXISTS / NEW) and whether the Python duplicates C++ today (DUP).
* **C** dead or superseded.
* **T** tests (`test_*.py`, `conftest.py`): they follow their module; a B module's tests become fixture generators
  for the engine port and then retire with it.

## 1. Totals

| Class | Modules | Lines | Share of non-test lines |
|---|---:|---:|---:|
| B engine-bound | 27 | 22,352 | 58.8% |
| A stays Python (without mega_report) | 28 | 10,576 | 27.8% |
| C dead / superseded | 11 | 5,069 | 13.3% |
| non-test total | 66 | 37,997 | |
| T tests | 68 | 28,989 | |
| A `atx-impl/tools/mega_report/` (report renderer) | 11 | 8,241 | |

Reading: 27.8% of the research Python is the glue it should be. 58.8% is research logic in Python, and 3,378 of those
lines (the four composition mirrors, `backtest_integrity.py`, `dsr_total.py`, `mine_overlap_factor.py`,
`pit_fundamental_clock.py`, `horizon_stats.py`) re-implement C++ that already exists: two implementations of one rule,
kept equal (where at all) by Python-equals-C++ tests.

## 2. Classification

| Class | Module | Lines | Owner in the engine / note |
|---|---|---:|---|
| B | `atx-engine/tools/prepare_research_fields.py` | 3,199 | research/fields (NEW, slice 1) + fields/sources; builtin builders: FINRA as-of = DUP of atx-impl asof_field.cpp and engine data/finra_short.cpp; factor_breaks = DUP of repair_role_factor_breaks.py; argv/manifest part stays a wrapper |
| B | `atx-impl/tools/fit_composition_weights.py` | 2,620 | research/admission (NEW: screen_v3/v4, Newey-West t, greedy redundancy; engine eval/hac EXISTS) + research/composition (rules EXIST in atx-impl strategy_ic_* over engine combine/group_*: fit-side DUP); load/store/argv stay a wrapper |
| B | `atx-engine/tools/build_fundamental_events.py` | 1,554 | research/fields/sources/fundamental_events (NEW); filing clocks overlap engine data/fundamental_clock_artifact |
| B | `atx-engine/tools/research_fields_holdings.py` | 1,475 | research/fields/builders (NEW); 13F/FTD/RegSHO/svx stage readers; no C++ today |
| B | `atx-impl/tools/backtest_integrity.py` | 1,295 | engine eval/deflated_sharpe, min_trl, pbo, trial_clusters (EXIST): DUP of PSR/DSR/MinTRL/CSCV/ONC; ledger rules -> research/ledger (atx-impl trial_ledger.cpp + eval/trial_registry EXIST) |
| B | `atx-impl/tools/alpha_report_card.py` | 1,215 | engine eval/cross_section_ic (EXISTS: Spearman IC, ICIR, autocorr, deciles): partial DUP |
| B | `atx-impl/tools/nav_summ.py` | 1,204 | engine eval/perf_metrics (EXISTS) + paired dSR / Memmel / CBB (NEW in eval); printing stays Python |
| B | `atx-engine/tools/export_fundamental_fields.py` | 1,059 | engine data/fundamental_fields (EXISTS; its header delegates fiscal-period arithmetic to this export) |
| B | `atx-engine/tools/research_fields_sec.py` | 996 | research/fields/clock (NEW): Calendar/Latest/Windowed are the clock-rule primitives; nyse_sessions partly DUP of engine quant/trading_calendar (2025-2035 only) |
| B | `atx-engine/tools/prepare_recent_research.py` | 993 | research/roles (NEW); membership rule research-prior63-usd-adv-topn-v1 computed here, re-validated by engine read_strategy_role; ADV/top-N overlaps data/universe |
| B | `atx-engine/tools/research_fields_price.py` | 784 | research/fields/builders (NEW); vol_126 ported in slice 1; source_panel/market overlaps engine data/history_panel |
| B | `atx-impl/tools/repair_role_factor_breaks.py` | 648 | research/roles (NEW): factor-break-v1, already DUP inside prepare_research_fields.factor_breaks |
| B | `atx-engine/tools/prepare_identity_bridge.py` | 615 | research/fields/sources/identity_bridge (NEW); dated links overlap engine point_in_time_universe link intervals |
| B | `atx-engine/tools/research_fields_v8.py` | 609 | research/fields/builders (NEW); issuer history, quarter index; overlaps engine data/fundamental_fields |
| B | `atx-engine/tools/research_fields_xdata.py` | 594 | research/fields/builders (NEW); dividend month, dvol beta, seasonality |
| B | `atx-engine/tools/research_fields_v9.py` | 569 | research/fields/builders (NEW); nt_first_126, earn_season_rank |
| B | `atx-impl/tools/composition_rules.py` | 390 | research/composition (EXISTS as atx-impl strategy_ic_composition + engine combine/group_rerank): DUP, Python-equals-C++ tests |
| B | `atx-engine/tools/research_fields_gold.py` | 374 | research/fields/sources (NEW); sealed gold-stage reader |
| B | `atx-impl/tools/composition_resid.py` | 365 | research/composition (EXISTS: strategy_ic_theme_resid + combine/group_residualise): DUP |
| B | `atx-engine/tools/pit_fundamental_clock.py` | 313 | engine data/fundamental_clock_artifact (EXISTS); clock resolution DUP (D3: 25 C++ + 19 Python checks of one rule) |
| B | `atx-impl/tools/mine_overlap_factor.py` | 299 | engine eval/hac (EXISTS): re-implements the verb's HAC t to derive tables compiled into strategy_mine_rule.hpp |
| B | `atx-impl/tools/composition_theme_erc.py` | 296 | research/composition (EXISTS: strategy_ic_theme_erc + combine/group_erc): DUP |
| B | `atx-engine/tools/research_fields_ohlc.py` | 291 | research/fields/builders (NEW); vendor bar fields |
| B | `atx-impl/tools/composition_ic_shrink.py` | 205 | research/composition (EXISTS: strategy_ic_shrink + combine/group_shrink): DUP |
| B | `atx-engine/tools/era_pool.py` | 175 | research/ledger (NEW): era pooling rules; no C++ today |
| B | `atx-impl/tools/dsr_total.py` | 158 | research/ledger + eval/deflated_sharpe (EXISTS) |
| B | `atx-impl/tools/horizon_stats.py` | 57 | mirrors aim-partial theta of strategy_target_replay.cpp (EXISTS) |
| C | `atx-impl/strategies/generate_fund_ic_v4.py` | 837 | frozen v4; carries a Python DSL grammar/parser/validator = DUP of engine alpha lexer/parser/typecheck (K1 replaced it) |
| C | `atx-impl/strategies/generate_fund_ic_v6.py` | 715 | frozen v6 (v5.1 revised by V6-L) |
| C | `atx-impl/strategies/generate_fund_ic_v70.py` | 670 | frozen v7.0 (v6.1 + wave 1; adds parser registry rows) |
| C | `atx-impl/strategies/generate_fund_ic_v71.py` | 560 | tip of the legacy chain (v7.0 + wave 2); library bytes reproduced by generate_library v71 and by the slice-2 spec |
| C | `atx-impl/strategies/generate_pv_fields_ic121_v3.py` | 535 | frozen price-volume + fields library |
| C | `atx-impl/strategies/generate_price_volume_ic96_v2.py` | 459 | frozen price-volume library (Python peak-slot estimator = DUP of the K1 plan) |
| C | `atx-impl/strategies/generate_fund_ic_v42.py` | 321 | frozen v4.2 (v4 + 3); a dead side branch (v5 builds on v4, not v4.2) |
| C | `atx-impl/strategies/generate_fund_ic_v5.py` | 315 | frozen v5.1 (v4 + 1) |
| C | `atx-impl/strategies/generate_fund_ic_v61.py` | 308 | frozen v6.1 (v6 + sv_flow) |
| C | `atx-impl/strategies/check_fund_ic_v6.py` | 204 | static check superseded by K1 --plan-only |
| C | `atx-impl/strategies/generate_slow_ic_48.py` | 145 | frozen 48-candidate library |
| A | `scripts/research_cycle.py` | 1,922 | cycle orchestration: spec -> argv -> receipts |
| A | `atx-impl/tools/book_diagnostics.py` | 1,565 | descriptive report (declared: gates nothing) |
| A | `scripts/research_mine.py` | 770 | mine orchestration; ~40 lines of table arithmetic (bonferroni_z, band/overlap factors) mirror strategy_mine_rule.hpp |
| A | `atx-impl/tools/compare_window_overlap.py` | 617 | audit report |
| A | `atx-impl/tools/book_monitor.py` | 584 | monitor flags (ops glue) |
| A | `atx-engine/tools/audit_tickerhistory_reconciliation.py` | 552 | read-only audit |
| A | `atx-impl/strategies/generate_library.py` | 543 | registry -> library glue; static validation already by the IC exe (K1 --plan-only) |
| A | `scripts/research_add_alpha.py` | 427 | add-alpha orchestration |
| A | `atx-engine/tools/prepare_tickerhistory.py` | 385 | input quarantine / QA receipt |
| A | `scripts/research_ledger.py` | 344 | ledger reader/appender glue; the counting rule it calls is backtest_integrity's (class B) |
| A | `scripts/research_spec.py` | 326 | spec templates |
| A | `atx-impl/tools/holdout_gate.py` | 322 | two-bit gate glue |
| A | `atx-impl/tools/era_data_audit.py` | 306 | audit report |
| A | `scripts/research_roles.py` | 284 | era loop orchestration |
| A | `atx-engine/tools/audit_recent_price_gap.py` | 283 | read-only forensics |
| A | `scripts/run_bounded_research.py` | 191 | bounded process runner + receipt |
| A | `atx-engine/tools/code_fingerprint.py` | 190 | AST fingerprints of Python producers; retires field by field as producers move (engine path: declared revision + build identity) |
| A | `scripts/research_gc.py` | 156 | cache gc |
| A | `atx-engine/tools/research_window.py` | 140 | Python side of the one research-window source (mirror of research_window.hpp) |
| A | `scripts/cycle_verdict.py` | 133 | cycle helper |
| A | `scripts/cycle_resume.py` | 101 | cycle helper |
| A | `scripts/cycle_admission.py` | 86 | cycle helper |
| A | `scripts/research_tree.py` | 83 | git / tree helpers |
| A | `atx-engine/tools/record_store.py` | 78 | content-keyed cache glue |
| A | `atx-engine/tools/prepare_research_fields_draft.py` | 53 | registration shim (becomes a registry spec entry) |
| A | `atx-engine/tools/prepare_research_fields_xdata.py` | 51 | registration shim |
| A | `atx-engine/tools/prepare_research_fields_ohlc.py` | 46 | registration shim |
| A | `atx-impl/tools/engine_tools.py` | 38 | import shim |

Two B rows are partly glue: `prepare_research_fields.py` (argparse, `run()` orchestration and the manifest assembly,
about 600 lines) and `fit_composition_weights.py` (loading, the record store and argv, about 1,100 lines) keep a
Python wrapper after their numerical parts move. The ranking in section 5 counts only the numerical part.

## 3. Duplications found (the "Python mirrors a C++ rule" pattern)

| Rule | Python | C++ | Kept equal by |
|---|---|---|---|
| FINRA as-of join (strict `available_at < date(session)`, 45-day staleness) | `prepare_research_fields.finra_field` + `parse_asof_csv` | `atx-impl/src/asof_field.cpp` (`build_asof_column`), engine `data/finra_short.cpp` (a third, parquet-fed variant) | the Python docstring ("replicates atx-impl asof_field.cpp exactly"); no shared test |
| factor-break-v1 | `repair_role_factor_breaks.py` and `prepare_research_fields.factor_breaks` | none | a port note (Python to Python) |
| ew-theme-std-v1, ic-shrink-v1, theme-resid-v1, theme-erc-v1 | `composition_*.py` (fit side, inside the fitter) | `atx-impl/src/strategy_ic_{composition,shrink,theme_resid,theme_erc}.cpp` over engine `combine/group_{rerank,shrink,residualise,erc}` | Python-equals-C++ fixtures (`test_composition_*.py` vs `atx-impl/tests/strategy_ic_*_test.cpp`) |
| PSR / DSR / MinTRL / CSCV PBO / ONC trial clusters | `backtest_integrity.py`, `nav_summ.py` (DSR again), `dsr_total.py` | engine `eval/deflated_sharpe.hpp`, `min_trl.hpp`, `pbo.hpp`, `trial_clusters.hpp` | none: the acceptance statistics of every cell come from the Python copy |
| Trial counting | `backtest_integrity.trial_counts` / `ledger_n`, `research_ledger.py` | `atx-impl/src/trial_ledger.cpp`, engine `eval/trial_registry.hpp` | none |
| HAC t of the mined statistic | `mine_overlap_factor.py` | engine `eval/hac.hpp` via `research_ic_fitness.cpp` | the Python derives constant tables the C++ compiles |
| Filing clock admission | `pit_fundamental_clock.py` | engine `data/fundamental_clock_artifact.cpp` | 25 C++ + 19 Python checks of one rule (D3) |
| DSL grammar, peak slots | `generate_fund_ic_v4.py` (and its copies through v7.1), `generate_price_volume_ic96_v2.py` | engine `alpha/{lexer,parser,typecheck}` | K1 (`--plan-only`) replaced it for v8 libraries; the legacy copies still run it |

The admission decision itself (v4-prior-v1/v2: prior sign, 250 finite days, tau <= .70, Newey-West HAC t veto
below -2.0, greedy redundancy at |rho| <= .90 in (tier, roster) order) exists only in Python
(`fit_composition_weights.screen_v4`); `atx-impl/src/strategy_ic_admission.cpp` consumes its output file.

## 4. The versioned library generators (class C)

### Which one is live

The legacy chain is `v4 -> v5 -> v6 -> v61 -> v70 -> v71`: each generator imports its parent generator by file,
re-derives the parent's library and recipe bytes, checks them against pinned SHA-256s, and appends its wave.
`generate_fund_ic_v42.py` is a dead side branch (v4.2 = v4 + 3; v5 builds on v4, not on v4.2). Every one passes
`--check` at the base, so `generate_fund_ic_v71.py --check` executes 3,726 lines of Python (the whole chain) to verify
two committed files. `fund_industry_ic_v71.json` is the last library a legacy generator produces; v8.0 onwards are
`generate_library.py` + `alphas/registry.json` + `libraries/<name>.json`. Nothing outside the strategies directory
imports a legacy generator except one archived study (`.superpowers/sdd/mega-alpha-20260926/studies/v6_scorecard.py`).

### What differs between versions (diff summary)

| Step | Lines (-/+) | Docstring | Data (members, fields, prose) | Code | What the step adds |
|---|---|---:|---:|---:|---|
| v4 | 837 | 30 | 169 | 559 | the base: 37 members in 9 themes; a full Python DSL grammar, parser, field table, static validator and registry cross-check (the part K1 replaced) |
| v4 -> v42 | -737 / +221 | 20 | 34 | 230 | three v4.2 members; loads v4 (dead branch) |
| v4 -> v5 | -739 / +217 | 24 | 26 | 224 | one v5.1 member; loads v4 |
| v5 -> v6 | -200 / +600 | 40 | 168 | 441 | V6-L literature revision: roster rewrite, smoothing forms, expression builder |
| v6 -> v61 | -636 / +229 | 21 | 54 | 198 | sv_flow (FINRA shorting flow); field-table extension |
| v61 -> v70 | -208 / +570 | 35 | 175 | 407 | wave 1 (5 members) and six new parser registry rows (W2 DSL ops) |
| v70 -> v71 | -510 / +400 | 31 | 212 | 274 | wave 2 (4 members), 4 field declarations, a new theme, recipe prose |

Each step copies the previous step's loader, parser wrapper, validator and recipe assembly with small edits (the
-/+ counts above are mostly that copy), and puts the wave's data (DSL strings, tiers, citations, field declarations,
prose) in Python literals. The data part is what a spec holds; the code part is what `generate_library.py` already
does generically.

### What replaces the copies

* The library bytes: `generate_library.py --library v71` reproduces `fund_industry_ic_v71.json` byte for byte today
  (sha256 `787c802e...`, 39,317 bytes) from `alphas/registry.json` and `libraries/v71.json`.
* The spec: slice 2 (`atx-impl/strategies/generate_from_spec.py`, `atx-impl/strategies/specs/library-v71.json`) pins
  that output and everything the generation reads that can move under it, calls the IC exe's K1 plan for static
  validation (or reads a saved plan), and verifies the legacy recipe as a frozen artefact.
* Drift found: `generate_library.py --library v71 --check` FAILS at the base on `fund_industry_ic_v71.recipe.v2.json`:
  the recipe embeds the registry's mutable `house_budget`, and `max_roster` moved 56 -> 80 after v7.1 was written. The
  spec pins the house budget the library was generated under (`registry_pins.house_budget`), which makes v71's
  recipe.v2 reproducible again without editing the registry or `generate_library.py`.
* The legacy recipe `fund_industry_ic_v71.recipe.json` (160,162 bytes, pinned by `scripts/specs/v71.json`) embeds the
  legacy generator's own source SHA-256 (`generation.generator_sha256`), so no other program can regenerate it; it
  is a provenance document. The spec verifies it as a frozen artefact (bytes against the pin).
* Deletion: the eleven class-C files (5,069 lines) and their tests (`test_generate_fund_ic_*.py`,
  `test_check_fund_ic_v6_ops.py`) are DEPRECATED by this audit and stay in place until root's identity run of slice 2
  (`generate_from_spec.py --spec specs/library-v71.json --check` exit 0 on root's tree). Then they are deleted in one
  commit; their committed outputs (`fund_industry_ic_v4..v71*.json`, the price-volume libraries) stay as frozen,
  sha-pinned data.

## 5. Top 5 migrations

Score = lines retired x risk removed / effort. Risk: 3 = the code decides look-ahead, admission or acceptance, 2 = a
duplicated implementation kept equal only by tests, 1 = cosmetic. Effort: 1 = the C++ or the spec exists (mostly
deletion and a wrapper), 2 = partial C++ to extend, 3 = new C++ with new source readers.

| Rank | Migration | Lines retired | Risk | Effort | Score | Engine owner |
|---:|---|---:|---:|---:|---:|---|
| 1 | Legacy library generators -> one spec-driven generator (slice 2) | 5,069 | 2 | 1 | 10.1 | none needed: the IC exe's K1 plan validates; `generate_library.py` builds |
| 2 | Field builders -> `atx/engine/research/fields` (framework + vol_126 + FINRA as-of in slice 1) | 8,891 | 3 | 3 | 8.9 | research/fields (NEW library `atx-engine-research-fields`) |
| 3 | Composition mirrors -> the existing C++ rules called by a fit verb | 1,756 | 3 | 1 | 5.3 | research/composition (rules EXIST in atx-impl + engine combine/group_*) |
| 4 | Integrity statistics and trial counting -> engine eval + research/ledger | 1,953 | 3 | 2 | 2.9 | eval/deflated_sharpe, pbo, trial_clusters (EXIST), research/ledger (NEW) |
| 5 | Admission screens -> research/admission | 1,520 | 3 | 2 | 2.3 | research/admission (NEW; eval/hac EXISTS) |

Line bases: (2) `prepare_research_fields.py` + the eight `research_fields_*.py` modules; (3) the four `composition_*.py`
(1,256) plus the fitter's rule-fit code (`ew_theme_*_weights`, `fit_weights`, `shrink_solution`, about 500); (4)
`backtest_integrity.py` + `dsr_total.py` + the statistics of `nav_summ.py` (about 500); (5) `screen_v3`, `screen_v4`,
`newey_west_t`, factor construction and `fit`/`fit_prior` numerics of the fitter (about 1,520 of its 2,620 lines).
Next after these: fundamental clocks and events (2,926 lines, risk 3, effort 3: 2.9 per 1,000 lines but new readers)
and the role writer with factor-break-v1 (1,641 lines, risk 3, effort 3).

Why this order: (1) is nearly free and stops the copy pattern the directive names. (2) moves the look-ahead and seal
enforcement of every field into one tested library; its framework is the precondition for every later builder, so it
starts now even though it is the largest. (3) deletes a second implementation of rules whose C++ already decides the
book. (4) and (5) put the acceptance and admission statistics on the code the engine already tests; today the
statistics that accept a cell exist only in Python.

## 6. Notes for the Y lanes (no file of theirs is changed here)

* YSIG / YDATA own `research_fields_*.py`: this audit only reads them. Their next fields should land as C++ builders
  of the slice-1 framework once it is merged (plan section 4).
* YCOMB owns the composition rules: migration 3 adds a fit verb beside the rules, it does not rewrite them.
* YINFRA owns `scripts/wave_*.py`, YOPS `op_catalog.cpp`: outside this audit's scope.
