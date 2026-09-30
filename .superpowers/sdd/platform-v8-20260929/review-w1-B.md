# Review W1, area B (IC runner, signals, composition, field and role builders)

Snapshot: `C:/atx-wt/pool-9` at 7af37e9d, change `ef11f462..7af37e9d`. Read-only; nothing built, run or edited.

**Stopped early at the owner's instruction.** This file holds what was verified up to the stop. Coverage is partial:
see the two lists at the end. No test file was read, so hunt item 7 (tests that cannot fail) is not covered at all.

Severity: **I** important, **M** medium, **m** minor. No **I** finding was established in the part that was read.

---

## Findings

### B-1 (M) Price-field reuse fingerprint does not cover the NYSE rule calendar it depends on

- Where: `atx-engine/tools/research_fields_price.py:48` (`from research_fields_sec import _Host, nyse_sessions`),
  `:194-210` (`extended_days` calls `nyse_sessions`), `:174-178` (`PRODUCERS`), `:671-679` (`reuse_inputs` and
  `entry_inputs` both return `{}`); `atx-engine/tools/code_fingerprint.py:36-37, 106-116`.
- What is wrong: `code_fingerprint.Module.reach` follows only names bound at module level in the one source it
  parses. An imported name is bound by its import statement, so the hash covers the text of the import and nothing
  of the imported function. `source_panel` reaches `extended_days`, which builds the pre-role session axis from
  `nyse_sessions`. That axis decides which session is "t-1-1260" for `ceq_iss_5y`, the 60 month ends of
  `coskew_60m` and the two pre-role sessions of `ret_overnight`. The function body lives in
  `research_fields_sec.py` and is in no price producer group. The module pins no inputs of its own.
- Failing scenario: someone corrects a holiday in `nyse_sessions` (an edit to `research_fields_sec.py` only). The
  price module's source, its LF SHA and its three vendor groups' fingerprints are unchanged, so `--reuse` copies
  `ceq_iss_5y`, `coskew_60m`, `ret_overnight` built on the old calendar and the manifest says "same producing code".
- Fix: move `nyse_sessions` access behind the host handle (`h.nyse_sessions`, exported by the builder namespace) so
  the host closure hashes it, or add the SEC module's `nyse_sessions` fingerprint to `reuse_inputs` / the entry.
  General rule for the record: `code_fingerprint` is blind to every first-party `import`; each field module's
  first-party imports need the same treatment.
- Verified by reading both files. Not run.

### B-2 (M) Marginal IC (K6) ignores the themes of an `ew-theme-std-v1` weights file

- Where: `atx-impl/src/strategy_marginal_ic.cpp:194-197` (theme names are read from `theme_redistribution.themes`
  only), `:221-222`, fallback `:146-147` (library row `theme`, else `family`), bound `:227-229`, method text `:516`.
- What is wrong: R-1 writes themes under `theme_standardise.themes` (`composition_rules.py:221, 249`). `read_themes`
  never looks there. With `--themes <R-1 weights>` the theme regressors are grouped by the library row's `theme`
  or, if the row has none, by `family`. Two consequences: (a) the grouping can differ from the one the blend used;
  (b) with family fallback and more than 10 weighted families the verb refuses with "1..10 weighted themes".
  Separately, the regressor is the un-reranked sum `sum s_k w_k r_k`, while under `standardise` the blend's theme
  component is the re-ranked composite, so the method string "the no-redistribution blend split by theme" is not
  true for an R-1 pool.
- Failing scenario: R-1 cell accepted, then `marginal --pool <R-1 combined> --themes <R-1 weights>` for the R-2
  screen: either a refusal, or K6 columns computed against theme groups that are not the book's.
- Fix: read `theme_standardise.themes` with the same precedence as `theme_redistribution.themes`; when the block's
  `rerank` is true, re-rank each theme row with `combine::for_each_centered_rank` before using it as a regressor
  and say so in `method.theme_composite`.
- Unverified part: whether library rows written by `generate_library.py` carry `theme` (file not read). If they do,
  consequence (b) does not occur and (a) reduces to a registry/weights disagreement. Confirm by reading one
  generated library row.

### B-3 (M) Nothing refuses a role with imputed delisting returns as the signal/IC role (Ruling E-10 is procedural only)

- Where: `atx-engine/src/data/strategy_data.cpp:89-125` (the reader validates schema, clock, membership recipe and
  byte recipe; it never looks at `universe.delisting.returns_applied`); `atx-impl/src/strategy_ic_admission.cpp:211-260`
  (`admit` reads only dates, instruments and the score window); writer `atx-engine/tools/prepare_recent_research.py:891-902`.
- What is wrong: F-0 lets `linked-operating-v1` take `--delisting-returns`. The patch sets `close[T]`,
  `raw_close[T]`, `present[T] = 1` on the termination session from a return whose cause "may be classified after T"
  (the rule's own text, `:157-163`, ends "must not feed a T-dated signal"). E-10 rules that such a role serves the
  NAV replay's labels only. No code enforces it: the IC runner and the marginal verb load that role like any other.
- Failing scenario: a spec (B0c or later) passes the delisting-returns role as `--train`. Every close-based DSL
  term evaluated at T reads the imputed close; the patched cell is present, so it also enters cross-sectional
  operators and market aggregates at T. IC labels of positions entered before T pick up the imputed return too.
  Nothing fails; the numbers silently mix a hindsight-classified value into signals.
- Fix: in `read_strategy_role` expose `returns_applied` on `StrategyRoleData`; `run_ic`, `run_marginal_ic` and the
  fields builder refuse it (or require an explicit `--allow-terminal-returns` that only the NAV/targets verbs pass).
- Verified for the C++ reader and `admit`. Unverified: whether `prepare_research_fields.Role` or the cycle spec
  wiring (area C) refuses it; neither was read for this.

### B-4 (M, unverified threshold) The 1,024-row fields manifest cap sits behind a 1 MiB file limit

- Where: `atx-impl/src/strategy_ic_library.cpp:40-52` (`metadata_text` refuses a file over 1 MiB),
  `atx-impl/src/strategy_ic_admission.cpp:326` (the fields manifest is read through `pinned_json`), `:341`
  (row cap), `atx-impl/src/strategy_ic_detail.hpp:56`; same limit in `strategy_marginal_ic.cpp:40, 68-76, 237`.
- What is wrong: B-2 raised the row cap 64 -> 1,024, but the manifest still has to be under 1 MiB. Real field
  entries carry definition text, sources, coverage and caveats. 1 MiB / 1,024 rows is about 1 KB per row including
  the `files` receipts, which a real entry is unlikely to meet. The effective cap is the byte limit, and its
  refusal text ("metadata file empty or over 1 MiB") does not name the row cap.
- Failing scenario: fields v10/v11 (70 to 75 rows) or a later wider build crosses 1 MiB; the u pass refuses at
  admission although the row count is far below 1,024. `FieldCaps.Admits200RowManifestWith40Referenced` is
  described in the lane report as using synthetic rows, so it would not see this.
- Fix: give the fields manifest its own byte limit (for example 16 MiB) in `bind_fields`, `legacy_manifests` and
  the marginal verb's `read_fields`, and name both bounds in the refusal.
- Unverified: bytes per row of a real manifest (no `build-equity` file may be opened). Confirm with the size of the
  fields-v9 `manifest.json` divided by 63.

### B-5 (m) The marginal verb carries its own copy of the return guard and the label recipe

- Where: `atx-impl/src/strategy_marginal_ic.cpp:345-361` (`return_guard`), `:365-391` (`research_labels`).
- What is wrong: H-3 lifted the guard to `engine::data::research_return_guard` (`role_panel.cpp:34-55`) and exposed
  the prepared labels as `ResearchIcCache::labels()` "so every research verb scores with the same recipe". The
  marginal verb still has private copies. They agree today (compared line by line), but neither source pin nor any
  test read here ties them; a change to the engine label or guard would leave K6 `ic21` on the old recipe.
- Fix: call `research_return_guard` and `prepare_research_ic` + `labels(1)` (horizon index of h 21), delete the copies.

### B-6 (m) The marginal verb accepts a cache entry of another VM build without saying so

- Where: `atx-impl/src/strategy_marginal_ic.cpp:254-277` (`accept_sidecar` records `vm_identity` but never compares
  it), `:569-570` (roots: this build's identity directory, then the bare cache directory).
- Scenario: this build's identity directory has no entry for a candidate (or `--fields` filtered it out), the bare
  root has a v2 entry written by the legacy-identity build: it is used. The output records only `build_vm_identity`.
  There is also no check that a member payload is the one the pool was blended from (the pool manifest lists none).
- Fix: require `vm_identity` of the sidecar to equal the root it was found under, and write each entry's
  `vm_identity` into its candidate row.

### B-7 (m) K6 t-statistics use a shorter HAC window than the runner's for the same h 21 series

- Where: `atx-impl/src/strategy_marginal_ic.cpp:39` (`hac_lag = 21`), `atx-engine/src/combine/marginal_rank_ic.cpp:210-228`
  (undefined dates compacted) against `atx-engine/src/factory/ic_screen.cpp:203` (`max(2h, rule of thumb)` = 42,
  calendar spacing kept, SE floored by the lag-0 SE).
- This is what K6 registers ("Bartlett, lag 21"), so it is not a fidelity defect. The effect: with 21-session
  overlapping labels the Bartlett weight on lag 20 is 1/22, so `ic21_hac_t` and `marginal_hac_t` are larger than
  the runner's statistic on the same daily series. A card that shows both should label them. Report-only.

### B-8 (m) `max_abs_rho` is taken over every other library candidate, not over book members

- Where: `atx-impl/src/strategy_marginal_ic.cpp:490-494`.
- The key beside it is `max_rho_member`. For a screened candidate the maximum can name another screened or
  rejected candidate (or a copy in the same screen), which is not redundancy with the book. Either restrict the
  maximum to `themes.theme_of[m] != npos` when `--themes` is given, or add `max_abs_rho_book` and keep both.

### B-9 (m) Delisting manifest counter still says 2025

- Where: `atx-engine/tools/prepare_recent_research.py:863` (`rows_available_on_or_after_2025_dropped`), filter at
  `:860` is `avail >= prf.SEAL_NS` (2024-01-01 since W0-1).
- The count is right, its name is wrong. Renaming changes lo3 manifest bytes, so decide once: rename to
  `rows_available_on_or_after_seal_dropped`, or leave and note it in the F-0 report.

### B-10 (m, known) Every pytest in `atx-engine/tools` runs under the superseded seal

- Where: `atx-engine/tools/conftest.py:15` (`research_window.bind(research_window.superseded())`).
- All field, role and event builder tests in that directory, including the F-0, F-1 and F-3 tests and
  `test_seal_partitions.py`, see seal 2025-01-01. Only what `test_research_window.py` runs in fresh interpreters
  exercises the repository seal. The ledger lists the redating as a deferred minor; recorded here because it
  bounds what those tests can say about hunt item 1. Unverified: whether `test_seal_partitions.py` spawns fresh
  interpreters (file not read).

### B-11 (m) The seal value is no longer part of any producer fingerprint

- Where: `atx-engine/tools/prepare_research_fields.py` (`SEAL = rw.SEAL`, about line 104) with
  `code_fingerprint.py:78-86` (the statement's AST is hashed, not the value it evaluates to).
- Before W0-1 the literal date was in the hashed AST; now a change of `research_window.json` changes no fingerprint.
  For one role this cannot change a payload (the prior is bound to the role, and a role reaching the seal is
  refused), so no stale copy was constructed. It is a pin that stopped tracking an input; add
  `rw.WINDOW_ID`/`SEAL_DATE` to the reuse key or to the manifest comparison in `load_prior`.

### B-12 (m, unverified effect) Panel extent of the price module depends on which fields are requested together

- Where: `atx-engine/tools/research_fields_price.py:717-729` (`compute`: `pre` is 1,261 with `ceq_iss_5y` or
  `coskew_60m`, else 2; the 490-day lookback only with `ceq_iss_5y`), `:331-335` (`factor_breaks` runs over that
  panel). `compute` is declared orchestration and is in no fingerprint; the co-requested set is in no reuse key.
- If `factor-break-v1` classifies a step using anything outside the step's own session (for example a session-wide
  count over the panel), `ret_overnight` bytes differ between a build that also asks for `ceq_iss_5y` and one that
  does not, and `--reuse` would not notice. Unverified: `factor_breaks` was not read. Confirm by reading it, or by
  building `ret_overnight` alone and with `ceq_iss_5y` on the fixture and comparing bytes.

---

## Checked, no finding

- B-3 move-only split: multiset line comparison of the base file against the six files at `bdca4c3d`. The only
  differences are the ones the lane report lists (default arguments moved to declarations, `inline constexpr`,
  `PartialFile` bodies out of line, `lit_ops.hpp` pin, K1 rows). No function defined twice across the units.
- Source pins: the `atx/engine` include closure of the 33 `dsl_vm_sources` paths and of the 10 `ic_result_sources`
  paths contains no unlisted engine header. `group_rerank.hpp` is outside both, correctly: composition output is
  never cached.
- Signal and IC-result cache keys (`strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`): role manifest,
  geometry, VM identity, DSL, per-field payload SHAs; IC scope adds member and guard content hashes and every
  configuration field. No path found by which another role, window, field payload or DSL is a hit. The
  `--no-composition` probe trusts the sidecar's payload SHA without hashing the payload; the lane report states it.
- Worker logic: IC rows (`ic_screen.cpp:441-489`) and both composition paths write disjoint rows per band and
  keep one scratch per worker; `ranked_count` is reset per row, so results do not depend on the band split.
  Admission's per-worker term (1,104 B per name) covers the 48 B IC row scratch and the 16 B composition row.
- Seal in C++: `read_strategy_role` refuses `score_end_ns > seal` and any sealed session label before any payload
  other than the two axis files; the marginal verb reads the role through it.
- `ew-theme-std-v1`: the runner's per-date rule (weighted signed rank sum, NaN sentinel for "no member present",
  centred tied re-rank over present names, `W_theme` times rank, missing neutral) and the fitter's weights
  (`tier_weights`, `member_cap`, re-grade table) match the five registered rules as written. The Python side holds
  no second implementation of the per-date blend, so there is nothing to disagree with; its diagnostics are declared
  as computed without the re-rank (`composition_rules.py:73-74`).
- `research_fields_price.py` clocks: every producer reads session t-1 or earlier (`a = prefix + t - 1`); `vol_126`
  writes row t before reading session t; the market query and the source scan both stop at
  `min(role last day, seal - 1)`. Coskewness and composite issuance formulas match the draft's section 3 text.

## Table

| ID | sev | file:line | one line |
|---|---|---|---|
| B-1 | M | `atx-engine/tools/research_fields_price.py:48`, `code_fingerprint.py:106` | price reuse fingerprint blind to imported `nyse_sessions`; stale calendar payloads copied |
| B-2 | M | `atx-impl/src/strategy_marginal_ic.cpp:194` | K6 ignores `theme_standardise.themes`; wrong theme regressors or refusal on R-1 weights (part unverified) |
| B-3 | M | `atx-engine/src/data/strategy_data.cpp:89`, `strategy_ic_admission.cpp:211` | delisting-returns role accepted as signal/IC role; E-10 not enforced in code |
| B-4 | M | `atx-impl/src/strategy_ic_library.cpp:44`, `strategy_ic_admission.cpp:326` | 1 MiB manifest limit makes the 1,024-row cap unreachable (threshold unverified) |
| B-5 | m | `atx-impl/src/strategy_marginal_ic.cpp:345` | private copy of guard and label recipe beside the engine's |
| B-6 | m | `atx-impl/src/strategy_marginal_ic.cpp:254` | cache entry of another VM build accepted silently |
| B-7 | m | `atx-impl/src/strategy_marginal_ic.cpp:39` | K6 HAC lag 21 vs runner 42; t values not comparable |
| B-8 | m | `atx-impl/src/strategy_marginal_ic.cpp:490` | `max_abs_rho` over all library rows, not book members |
| B-9 | m | `atx-engine/tools/prepare_recent_research.py:863` | counter named 2025 under a 2024 seal |
| B-10 | m | `atx-engine/tools/conftest.py:15` | directory's tests run under the superseded seal (known, deferred) |
| B-11 | m | `atx-engine/tools/prepare_research_fields.py` (~104) | seal value dropped out of producer fingerprints |
| B-12 | m | `atx-engine/tools/research_fields_price.py:717` | co-requested fields change panel extent; effect on bytes unverified |

Counts: I 0, M 4 (B-2 and B-4 partly unverified), m 8.

## Files fully reviewed (whole file at 7af37e9d unless noted)

- `atx-impl/src/strategy_ic_runner.cpp`, `strategy_ic_detail.hpp`, `strategy_ic_admission.cpp`,
  `strategy_ic_library.cpp`, `strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`,
  `strategy_ic_composition.cpp`, `strategy_ic_composition.hpp`, `strategy_marginal_ic.cpp`
- `atx-impl/tools/equity_strategy_ic.cpp` (diff), `atx-impl/src/strategy_ic_runner.hpp` (diff)
- `atx-engine/src/combine/marginal_rank_ic.cpp`, `include/atx/engine/combine/marginal_rank_ic.hpp`,
  `include/atx/engine/combine/group_rerank.hpp`
- `atx-engine/src/data/role_panel.cpp`, `include/atx/engine/data/role_panel.hpp` (diff),
  `include/atx/engine/data/research_window.hpp`, `src/data/strategy_data.cpp`
- `atx-engine/src/factory/ic_screen.cpp`, `include/atx/engine/factory/ic_research.hpp` (diff)
- `atx-engine/tools/research_window.py`, `atx-impl/strategies/research_window.json`, `atx-engine/tools/conftest.py`,
  `atx-impl/tools/engine_tools.py`
- `atx-engine/tools/prepare_recent_research.py` (lines 60-993), `research_fields_price.py`, `code_fingerprint.py`,
  `record_store.py`
- `atx-impl/tools/composition_rules.py`
- CMake diffs of `atx-engine`, `atx-impl`, and both test lists

## Read as diff only (functions around the hunks not read)

- `atx-engine/tools/prepare_research_fields.py` (all hunks, including `reuse_module_fields`, `load_prior`, the CNMS
  directory rule; not read: `factor_breaks`, `FieldWriter`, `Role` beyond the seal hunk, the legacy `reuse_fields` body)
- `atx-engine/tools/prepare_identity_bridge.py`, `atx-engine/tools/build_fundamental_events.py`

## Files and areas not reached

- Python: `research_fields_sec.py` (`k8_item402_63`, its `reuse_inputs`), `research_fields_holdings.py`,
  `research_fields_v8.py` (`grp_ff12f49`), `fit_composition_weights.py` (fit store, `f_theta`, the
  `ew-theme-std-v1` dispatch), `alpha_report_card.py` (card store, `ic_theta`, K6 columns), `horizon_stats.py`,
  `generate_library.py`, `alphas/registry.json`
- C++: `strategy_marginal_ic.hpp`, the H-3 racing/stride helpers (`parallel/lockstep_grid.hpp`, `factory/fidelity`)
- Every test file, C++ and Python (hunt item 7 not started)
- Hunt item 3 for the fit and card stores and for SEC/holdings producer reuse; MAX_PATH of the record stores
- Field definitions against draft section 8 and E-20 for `grp_ff12f49`, `k8_item402_63`, `xrd0_ttm` beyond the price
  module; `gscore7_lowbm` and `eps_consist_4y` were not looked for
- Lane reports not read: W0-1, C-1, C-2, C-3, F-0, F-1, F-2, H-3; `.agents/cpp/agent.md` not read
