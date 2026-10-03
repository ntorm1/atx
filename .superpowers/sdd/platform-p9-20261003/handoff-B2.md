# Handoff B2 (owner stop, 2026-10-03)

- Pool: `C:/atx-wt/pool-14`. Branch: `feat/p9-b2-20261003`. Base: `1239a5ff`.
- WIP commit: `a1b05ef8` "wip(b2): ..." (this handoff is committed on top of it). NOTHING HAS BEEN COMPILED (lanes do
  not build); no gtest written yet; no pytest run on lane changes yet (no Python touched yet).
- Estimate: about 45% of the lane.

## Required carry items

### SQL-7 chain-head function: DONE (code; untested by gtest)
Header `atx-engine/include/atx/engine/research/ledger/chain.hpp`, namespace `atx::engine::research::ledger`,
source `atx-engine/src/research/ledger/chain.cpp`. Library target to be named `atx-engine-research-ledger` (CMake
NOT yet written, see below).

```cpp
inline constexpr std::string_view kLedgerSchema = "atx.trial-ledger/v1";
inline constexpr std::string_view kChainGenesis = "000...000"; // 64 zeros
inline constexpr std::string_view kChainRule = "atx-ledger-chain-fold/v1"; // SQL4 ledger_state.head_rule
struct ChainHead { std::string head_sha256; usize lines{}; std::string rule; };
[[nodiscard]] core::Result<ChainHead> chain_head(std::span<const std::string> lines);          // any prefix
[[nodiscard]] core::Result<std::vector<std::string>> prefix_heads(std::span<const std::string> lines); // head after each line
[[nodiscard]] core::Result<ChainHead> verify_chain(std::span<const std::string> lines);        // reader's _walk
// helpers: strip_line_end, is_blank_line, line_sha256, fold_head, is_chained_line
```
Rule (one copy, = backtest_integrity.chain_head/_walk): head starts at 64 zeros; per non-blank line in file order: a
line whose top-level JSON object has key `prev_sha256` -> head = sha256(line bytes without line end); otherwise
head = sha256(ASCII(head + line_sha256(line))). Blank (whitespace-only) lines skipped; trailing CR/LF stripped.
verify_chain also checks schema, prev link == head-before, no unchained line after a chained one (1-based physical
line numbers in messages). Python NaN/Infinity tokens read as null (`src/research/ledger/json_line.{hpp,cpp}`).

### SQL-9: kept. Nothing writes the ledger in C++ except the moved R9 IC ledger (`trial_ledger`, unchanged bytes).

### P11 (B2 writes the eval_tie gtests): NOT STARTED (see Tests).
### T1 minor (keep eval_tie checker runnable when deleting Python stats): NOT STARTED (deletion not started).
### Carried B1 minors: not touched (admission lib, out of B2 files; recommend report them as not fixed).

## Brief deliverables

| item | state |
|---|---|
| `research/ledger` lib: chain, ledger_file (read+verify), trial_counts (trial_counts, ledger_n, N_tot via trial_breakdown, dsr_variance, construction_ids, construction_prefix_length, history reads, defect rule), era_pool (check ids/windows, segment_starts, pooled_sha256, pool_label, trial_id, pooled_trial_id) | written |
| `trial_ledger.cpp` moved down to `atx-engine/src/research/ledger/trial_ledger.cpp` + header `research/ledger/trial_ledger.hpp` (namespace changed); `atx-impl/src/trial_ledger.hpp` = include + `using` re-exports into `atx::impl`; `atx-impl/src/trial_ledger.cpp` = comment + include only (keeps atx-impl's source list untouched) | written |
| engine `eval/pbo.{hpp,cpp}`: `PboSplitTrace` + `pbo_cscv_traced(perf, n, splits, rule, trace&)`; body moved into one `run_cscv` (no 2nd CSCV) | written (cross-lane-ish: engine eval file, no owner lane) |
| `eval/deflated_sharpe_house.hpp`: `HouseDsrMethod` (house_v1, tot, cross, lo, onc), `house_dsr`, `lo_sampling_variance` | written |
| `research/numpy_reduce.hpp` (numpy pairwise sum/mean/var/std, linear quantile) | written, port verified bit-exact vs numpy in scratch |
| eval lib (`research/eval/`): numpy_rng (default_rng PCG64 port), house_moments (moments, sharpe, psr_report, dsr_at, dsr_rows, lo_null_dsr), onc_house (MLAM ONC + effective_trials + effective_n_dsr; `KmeansDraws` interface for replay), paired (Memmel, CBB percentile, LW studentized; numpy reduction orders), conditional (AKM), cscv_report (house PBO summaries over `pbo_cscv_traced`), eval_series (CSV `session_ns,net_return` reader, seal refusal, align_many) | written |
| `eval_report.hpp` (EvalRequest, evaluate, eval_text; JSON layout documented in the header) | header written; **`eval_report.cpp` NOT written** |
| `eval_cli.{hpp,cpp}`, `eval_main.cpp` (exe `atx-research-eval`) | not started |
| CMake tail blocks: `atx-engine/CMakeLists.txt` (ledger lib: atx::core + nlohmann_json only; eval lib links atx::engine PRIVATE for pbo; exe), `atx-engine/tests/CMakeLists.txt` (two test exes + registration), `atx-impl/CMakeLists.txt` tail: link atx-impl lib to `atx-engine-research-ledger` (cross-lane, C2 owns) | not started |
| `nav_summ.py --engine-stats` adapter | not started |
| `wave_stage_record.py`: record stage writes ledger head (`ledger-head.json` with head, lines, rule) into the sprint dir (cross-lane, E2 owns) | not started |
| Python statistics deletion as a separate prepared commit | not started |

### Where exactly I stopped / next step
`eval_report.cpp` was next. Design (implement as written in my notes; mirrors Python dict keys so the nav_summ
adapter can assign blocks directly):
- read cells (`read_net_series`), unique labels; moments per cell in FILE order; `dsr_rows(moments, dsr_n)` (cross).
- ledger: `read_ledger(path, MissingLedger::Empty)` (Python ledger_read semantics), doc `{path, exists, lines,
  chain_head, chain_rule, window_id}`; `ledgered = construction_ids(records)`; per cell in_ledger = its `--trial`
  id in ledgered; history = label in `--history`.
- per cell `dsr.house_v1` = nav_summ.ledger_dsr keys exactly (n = ledger_n(records, in_ledger||history), window_id,
  variance_sr, cells, variance_source, legacy_variance_sr, legacy_cells, n_rule text as Python, dsr, sr0_daily,
  sr0_annual, legacy_dsr, legacy_sr0_annual); `dsr.tot` = dsr_total.ledger_rows keys (window_id, in_ledger,
  variance_sr, cells_in_v, variance_source, parts{by_kind, ledger_trials, campaign_registry, n_tot}, tot, v8; n_tot =
  parts.n_tot + (in_ledger?0:1), n_c = ledger_n(records, in_ledger)); counts are i64 (do not cast before the n<2
  check); `dsr.cross`, `dsr.lo` (lo_null_dsr), `dsr.onc` (effective_n_dsr when --onc).
- paired per cell vs `--paired-of LABEL=REF` else `--paired REF`: align_many of the two; <3 common -> `paired: null`
  + `paired_note` "paired skipped: N session(s) in common with the reference (3 needed)"; else paired_stats; JSON =
  nav_summ keys (sessions, sr, sr_reference, dsr, rho, memmel_se, t, cbb_ci95, cbb_valid_draws, lw{dsr_population,
  se, ci95, p_value, p_one_sided, valid_draws}, draws, block, seed) + K-P9-5 `se` (= lw.se), `p_one`, `p_two`,
  `reference`. NaN serializes as null: the adapter must turn null back into nan for sr, sr_reference, dsr, rho,
  memmel_se, cbb_ci95[*], lw.dsr_population, lw.se, and drop lw.p_one_sided unless one_sided.
- conditional: LineageCell list = `--step LABEL=DSR,SE` steps then cells with finite paired dsr and memmel_se>0;
  `cumulative_conditional(list, alpha)` (sums SELECTED cells only); per-cell rows from it.
- onc/pbo: series = `--onc-series` / `--pbo-series` or the cells; align_many; ONC needs T>=3; PBO = cscv_report
  (exhaustive only, blocks<=16; refuse a Python `--pbo-max-splits` in the adapter). PBO JSON = cscv_pbo keys
  (seed null, exhaustive true, logit_quantiles p5..p95, logit_distribution{oos_rank, logit, splits}, logits) +
  labels + engine diagnostics (adapter keeps only Python keys so `--pbo-json` bytes do not change).
- hand: `--hand TRIAL_ID=CSV`: prefix k = construction_prefix_length (refuse none, Python text), ledger_rows on
  records[:k] with in_ledger true, prefix_chain_head = chain_head(lines[:k]).
- top level: schema `atx.research-eval/v1`, config, ledger, cells, onc, pbo, hand, conditional, and K-P9-5 mirrors
  psr / min_trl / dsr / paired = cells[0]'s.
- CLI flags planned: `--daily [LABEL=]CSV`(rep), `--paired`, `--paired-of`(rep), `--protocol v7|v8` (v8: draws
  4999, seed 20260929 unless given), `--draws --block --seed --dsr-n --psr-benchmarks --psr-alpha --onc
  --onc-series(rep) --onc-init --onc-seed --pbo --pbo-series(rep) --pbo-blocks --ledger --window-id --trial
  LABEL=ID(rep) --history LABEL(rep) --hand TRIAL_ID=CSV(rep) --step LABEL=DSR,SE(rep) --threshold
  --conditional-alpha --json --out`. Exit 0 / 1 refused / 2 usage (admission CLI idiom).

## Tests to write (none written)
`atx-engine/tests/research/`: EvalVerb tie test (build 12 CSVs from `eval_tie/series.f64` (500x12 date-major),
ledger JSONL from `ledger_records.json` adding `"schema":"atx.trial-ledger/v1"`, window `eval-tie-v1`, trial ids
`000...1..c`; compare to `expected.json` per its `tie_rules` tolerances: 1e-12, integer counts/clusters exact),
replay of `inputs.onc_picks` through `KmeansDraws`, ResearchEvalRng (cbb_starts.u16 = default_rng(20260929)
integers(0,500,(4999,24))), `EvalVerb.JsonSchema`, `ResearchLedger.TrialCountsEqualPython` (+ a Python fixture
generator calling backtest_integrity + pinning pytest), `Conditional.ClosedFormNormalCase` (null bias at c=0 =
se*sqrt(2/pi); median of N(mu,se)|X>c inverts to mu; c far below -> raw -/+ 1.96 se), chain prefix-head tests.

## Evidence (scratchpad, NOT in repo)
`C:\Users\natha\AppData\Local\Temp\claude\c--atx\0960fa49-6bf7-4358-9781-d55e083b02da\scratchpad\`:
`np_rng_port.py` (PCG64 port = numpy, incl. fixture CBB starts), `np_reduce_port.py` (0 mismatches),
`ref_eval.py` (loop transcription of the C++ eval lib: every eval_tie value within 1e-12, moments bit-exact, ONC
clusters and LW p-values exact), `cond_check.py` (conditional: medians invert to mu within 4e-13, untruncated CI =
raw -/+ 1.96 se, null bias exact). Copy them if the next agent wants them; they may vanish.

## Decisions
- house ONC (MLAM) is its own named method beside engine `onc_cluster` (different algorithm; engine's does not
  reproduce the house's clusters). PBO reuses the ONE engine CSCV via the new trace.
- numpy RNG ported in C++ so the verb draws CBB starts / ONC picks itself (identity with Python prints).
- `house_v1` in a new header so `deflated_sharpe.hpp` includers do not recompile.
- verb input = `session_ns,net_return` CSV written by nav_summ (Python repr; from_chars exact).
- conditional: SE = Memmel, threshold 0, alpha .05; printed only (no rule reads it).

## Open questions for the PM
1. "Separate prepared commit" deleting the Python statistics: a commit on this branch after root identity, or a
   patch / side branch? (Root's identity needs both paths, so it cannot be merged before identity.)
2. K-P9-5 `paired.se`: I map it to the LW studentized SE (memmel_se is separate). Confirm.
3. eval_tie fixture ledger records lack `schema`; the tie test will add it when writing the JSONL (no expected
   value touched). OK?

## Traps
- Not compiled: re-read every new file for /W4 (unused, sign, shadow) before root builds; `eval_series.cpp` uses
  `atx::engine::data::` fully qualified on purpose; `cscv_report.cpp` uses ATX_TRY with a typed decl.
- `atx-impl/src/trial_ledger.cpp` is now a stub; atx-impl must link the ledger lib (CMake tail not done) or the
  IC trial ledger symbols go unresolved.
- Python `splitlines()` splits on more separators than LF; the C++ reader splits on LF (+CR strip). Fine for ASCII
  JSON lines.
- conditional: raw barely above the threshold (e.g. 1e-9 se) cannot bracket the median -> `clamped` true and the
  estimate empty (expected: the estimate tends to minus infinity).
- test_no_python_mirror.py: the adapter must not define any function named like a RULE_SYMBOL (psr, min_trl,
  deflated_sharpe, expected_max_sr, lo_null_dsr, effective_n_dsr, cscv_pbo, onc, onc_base, effective_trials,
  dsr_rows, memmel_se, cbb_indices, paired_stats, dsr_at, ledger_rows), methods included.
- Identity runs must not append to a real ledger (use a copy / omit `--ledger`).
