# Lane S2 handoff (owner stop)

- Pool: `C:/atx-wt/pool-18`
- Branch: `feat/p9-s2-20261003`
- Base: `1239a5ff` (P9 integration after the D1 merge)
- WIP head: `cc882481` `wip(S2): ...` (see `git log`; this handoff file is committed on top)
- State: **not built, not tested** (lanes do not build C++). The two source tripwire gtests are
  known red until the re-pins below are done, and one existing runner test is stale.

Contract: `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/wave2-lane-dispatch.md`
(LANE = S2) plus the S2 brief and carry (P16, P13, S1-PIN). Method: no TDD, no C++ build, no
real data, files only via Write/Edit tools, 100-col lines, /W4 /WX clean by re-reading.

## Brief / carry status

| Item | Status |
|---|---|
| One constexpr per-opcode table (family, axis, needs_group, stateful), `static_assert` size == last opcode + 1, every predicate derived, lookahead_safe derived | DONE (`alpha/op_traits.hpp`; typecheck, VM, streaming, oracle, registry all read it) |
| Band-split serial VM kernels by chunk axis with per-worker scratch | DONE (`vm.hpp`: `BandScratch`, `run_bands`, `dispatch_banded`, `run_band`) |
| eval_lit_ts extracts columns once per instruction | DONE (per band j: `s.lit_col`, window slice or `s.lit_win`) |
| `IcScreenConfig::min_coverage` (default .8) keyed in IC-result scope | DONE engine + runner + scope key (`min_coverage_bits`, only when non-default) |
| IC HAC rule named/selectable beside eval/hac.hpp's (default unchanged, both printed) | DONE engine (`IcHacRule`, `estimate_daily_ic`, `ic_hac_rule_name`) + runner (`--ic-hac-rule`, "hac" block with both rules when the flag is given; scope key `hac_rule` when non-default) |
| `--label-terminal imputed-v1` (label-only price column, imputed terminal; missing-exit share; IC under both rules) | DONE engine (`LabelTerminalRule`, `ResearchIcTerminal`, `terminal_imputed_pairs`) + runner/admission (flags, label-role admit/bind, second label cache, `ic_missing_exit_v1`, `combined_ic_missing_exit_v1`, `label_terminal` role block, recipe keys + per-role pins, frozen-TRAIN pin check, memory term `label_terminal_bytes`) |
| `--eval-mode audit-exact` with cache under its own identity token | DONE (`vm_identity(true)` = identity + `_audit`; own cache root; `CacheKey::eval_mode` keyed, matched, written to sidecar; runner refusal removed) |
| P13 C++ seal consumer, IC half | DONE (`fields_seal` in admission `bind_fields`: refuse present-and-different `seal.exclusive_end` or malformed seal; absent seal or no exclusive_end = "absent", logged by `score_role` as `IC fields-seal role=<r> seal=absent accepted-as-legacy-manifest`) |
| P16 survival-label source = role builder's imputed close (label role; E-10 relaxed for that column only) | DONE (admit_label_role requires `universe.delisting.returns_applied` true; read only for labels, dropped before the VM) |
| S1-PIN: S2 owns ic_screen.{hpp,cpp} | respected |
| Source-digest re-pins | **NOT STARTED** (see next steps 1-2) |
| Gtests (all) | **NOT STARTED** (see next step 4) |
| Update stale runner test | **NOT STARTED** (next step 3) |
| Report `task-S2-report.md` | **NOT STARTED** |
| Carried S1 minors | not fixed: they live in S1's marginal-verb files (forbidden to S2) and `research_ic_fitness.cpp` (unowned) |

## Exactly where I stopped, and the next steps

I had just finished the runner (`strategy_ic_runner.cpp`) and admission edits and was about to
write the runner tests. In order:

1. **Re-pin `dsl_vm_sources_sha256`** in `atx-impl/src/strategy_ic_signal_cache.cpp` (~line 86;
   value still the old `fcf8021e...`; a comment above it already says "re-pin without bump"). The
   list (35 entries, `op_traits.hpp` inserted after `lit_ops.hpp`) is final. Digest recipe (see
   `expect_sources_pinned` in `atx-impl/tests/strategy_ic_runner_test.cpp` ~2223): SHA-256 over,
   per listed path in list order, `"<path>\n<byte length>\n<text>"` with CRLF normalized to LF
   (length is of the LF text). Compute with a small Python script in the scratchpad, reading the
   files from the pool. The test also fails if a listed file includes an unlisted
   `atx/engine/...` header (op_traits.hpp includes only registry.hpp: listed).
2. **Re-pin `ic_result_sources_sha256`** in `strategy_ic_result_cache.cpp` (~line 44, still
   `2d758bff...`) the same way (ic_screen.{hpp,cpp}, ic_screen_config.hpp, ic_research.hpp
   changed). Decision: re-pin WITHOUT a semantics bump (S1-PIN / YOPS precedent: default
   outputs are byte-identical, new behaviour is keyed separately). Flag it in the report.
   Do the re-pins LAST, after any further engine edit.
3. **Update `StrategyIcRunner.AuditExactEvalModeIsRecordedAndRefusedWithTheCache`**
   (`strategy_ic_runner_test.cpp` ~3010-3038; rename e.g. `...AndCachedUnderItsOwnIdentity`):
   audit + `--candidate-cache` now succeeds; assert `cache/<ic_cache_vm_identity().identity>_audit`
   exists and `cache/<identity>` does not; summary `roles[0].candidate_cache.vm_identity` ends in
   `_audit`; a second audit run hits every candidate and its `train_daily_ic.csv` equals the
   uncached audit run's bytes; a ResearchFast cached run on the same dir hits nothing. Keep the
   CLI half (audit-exact 0, research-fast 2). Not changing an expected value to pass: the old
   expectation encoded the refusal the brief removes.
4. **Write the tests** (none exist yet; use named namespaces, Unity-safe):
   - `atx-engine/tests/alpha/alpha_op_traits_test.cpp`: `OpSig.PredicatesAgree` -- golden sets
     copied from the pre-S2 hand-written lists (`git show 1239a5ff:<file>`: typecheck.cpp
     `is_rolling_ts` switch, typecheck.hpp `is_shift_ts`/`is_cross_section`/`needs_group_arg`,
     vm.hpp old `chunk_axis`/`is_cs_op`/`is_instrument_op`, streaming_engine.hpp `is_ts_op`/
     `is_cs_op`/`is_grouped_cs`, oracle.cpp grouped list) pinned against the derived predicates
     for every opcode 0..kOpCodeCount-1; lookahead_safe derived == registry rows; axis per family.
   - `atx-engine/tests/alpha/vm_band_parity_test.cpp`: serial vs DetPool 1/4/16 byte parity for
     each banded kernel family (LoadField, Const, element-wise, lit map/cs/ts, as-of, recurrences
     TradeWhen/Hump/KalmanLevel/OuFilter/KalmanReg, Split2, ArgPack/GroupCross, CsBucket/CsResidOn),
     both eval modes, with a cross-section mask.
   - `atx-engine/tests/alpha/alpha_vm_slot_reuse_test.cpp` (edit): add `formulaic_ops()` rows to
     `catalogue_forms` (the Catalogue test's expected row count must then include them -- that is a
     scope change, not an expected-value edit to pass), per-opcode truncation-invariance and
     slot-reuse sweep over every opcode.
   - `atx-engine/tests/factory/ic_screen_test.cpp`: `IcScreen.MinCoverageConfig` (validate refuses
     0 / >1 / NaN; a series between .5 and .8 coverage is undefined at .8 and defined at .5;
     cache_matches compares it), `IcLabels.TerminalImputedClosedForm` (tiny panel, one name
     absent from session L+1 in the scored panel and present in the label panel at close[L]x(1+r):
     label = close_label[T]/close[entry]-1 for decisions with entry <= L < end; counts
     `terminal_imputed_pairs` closed form, a subset of missing_exit_pairs; MissingExitV1 unchanged;
     geometry refusal), an HAC rule test (HorizonAwareV3 SE == hac.hpp `mean_tstat` x T/n;
     Bartlett default bytes unchanged; `estimate_daily_ic` reproduces the evaluated estimate).
   - `atx-impl/tests/strategy_ic_runner_test.cpp`: label terminal run (fixture `role(...,holes)`
     with instrument 0 absent from d=420; label role = same with d=420 present, manifest patched
     with `"universe":{"delisting":{"returns_applied":true}}` via `json_file`; train-only cfg):
     recipe keys, `ic_missing_exit_v1` equals a plain run's IC (after erasing the two print-only
     coverage keys), `terminal_imputed_pairs` == finite_label_pairs(imputed) - finite_label_pairs
     (legacy); refusals: label role without returns_applied, pins without the flag, flag without
     pins, label close differing on a role-present cell. Min-coverage / HAC run: recipe key only
     when given; `hac.bartlett-2h-v1.standard_error` == plain run's `standard_error`; IC-result
     subdirectory differs under a non-default min-coverage; bad values refused; CLI parse errors
     exit 2. Seal: `fields_dir` manifest patched with `seal.exclusive_end` "2025-01-01" refuses,
     "2024-01-01" runs, absent runs and logs `seal=absent`.
5. Re-read every edited C++ file for /W4 traps (unused, sign, shadow, narrowing), includes,
   100-col lines (`Grep` pattern `^.{101,}` over the touched files: the runner help text and a few
   admission refusal strings are the likely offenders), CMake entries (engine test groups are
   globbed, so new files under `atx-engine/tests/alpha/` need no CMake edit).
6. Commit in themes, write and `git add -f` `task-S2-report.md`, final SubagentHandback.

## Files touched (all S2-owned per brief + P16)

Engine: `alpha/op_traits.hpp` (new), `alpha/{typecheck,vm,streaming_engine,registry}.hpp`,
`src/alpha/{typecheck,oracle,registry}.cpp`, `factory/{ic_research,ic_screen,ic_screen_config}.hpp`,
`src/factory/ic_screen.cpp`.
Impl (P16 scope): `strategy_ic_runner.{hpp,cpp}`, `strategy_ic_detail.hpp`,
`strategy_ic_admission.cpp`, `strategy_ic_signal_cache.cpp`, `strategy_ic_result_cache.cpp`.
Cross-lane: none edited. Overlap risk: D2 (pool 20) may touch `strategy_ic_runner.cpp` /
`strategy_ic_admission.cpp` (composition) -- expect textual merge conflicts there, not semantic.
Behaviour reaching others: `bind_fields` is also used by `strategy_research_role.cpp`, so the seal
refusal applies to its consumers too (intended by P13).

## Decisions (and why)

- Table named `OpTraits`/`kOpTraits` in `alpha/op_traits.hpp`; `ChunkAxis` moved there from vm.hpp.
  Streaming `is_ts_op`/`is_cs_op` derive `&& !is_lit_op` so their sets are exactly the old ones
  (CsBucket/CsResidOn stay out): no behaviour change.
- Banding: Dates/Cells bands on the cs pool, Instruments bands on the ts pool; bands = min(n,
  4 x workers); shared state (TradeWhen/Hump state growth, LoadField param check) runs on the
  calling thread before bands. Pin and GroupDelay stay unbanded. Bit-identical by construction
  (disjoint writes, same per-cell arithmetic and order).
- Identity: every new flag absent = byte-identical recipe, summary, daily_ic.csv,
  candidates.jsonl, orientations, IC cache keys (scope keys only when non-default; result_json's
  extra keys only through `IcPrint` when a flag is given; IC record writes
  `terminal_imputed_pairs` only when nonzero).
- Imputed terminal = first session in (entry, end] where the label panel presents and the scored
  panel does not; return to that close, then cash. Label role must declare
  `returns_applied` true and share sessions/ids/dates/instruments/score window; on every
  role-present cell its presence and close must be bit-equal.
- Legacy (missing-exit-v1) IC under `--label-terminal` is evaluated every run, not cached; the
  `--no-composition` probe shortcut is disabled under the flag (the legacy IC needs the signal).
- `--ic-hac-rule` given (even = default) is recorded in the recipe and turns on the both-rules print.
- Frozen TRAIN: `expect_frozen_label_roles` mirrors `expect_frozen_fields` (TRAIN pin from the
  source; validation pin must equal this run's).

## Open questions for the PM

1. Re-pin both source digests without a semantics bump -- confirm (default bytes unchanged).
2. Should `exe_capabilities` (strategy_ic_rules.cpp + `tests/fixtures/composition_rules_list.json`,
   D-lane files) advertise `label-terminal`, `min-coverage`, `ic-hac-rule`, and change
   `eval-mode-audit-exact`'s meaning? Not touched (cross-lane).
3. Should the legacy IC under `--label-terminal` be cached in the default scope (it is exactly a
   default run's key)? Left uncached for simplicity.

## Traps

- Shell hook breaks heredocs: write files with Write/Edit only.
- `role()` test helper writes manifests without `universe`; patch via `read_json` + `json_file`
  (re-dumps sorted, sha recomputed).
- `read_strategy_role` refuses present cells with non-finite/<=0 close and absent cells with
  non-NaN values: a label-role fixture must keep that contract at the added termination cell.
- `RoleFields` gained `seal{}` (NSDMI) -- aggregate inits up to `declared` stay warning-free.
- `IcRunnerConfig::min_coverage` is `atx::f64` (the namespace has no unqualified f64).
- Never edit an expected hash/value to make a test pass; the re-pins are the documented
  tripwire procedure, flag them in the report.
