# Task B-2 report: field caps and worker cap

Lane B, worktree `C:/atx-wt/pool-10`, branch `feat/platform-v8-b-20260929`. Nothing was built or run.
Commit: the one carrying this report (on top of B-1).

## What was built

**Field caps** (strategy_ic_detail.hpp constants, one place each):
- `max_field_manifest_rows = 1024`: a pinned fields manifest may list 1..1,024 rows (was 64;
  strategy_ic_admission.cpp `bind_fields`). Refusal text gains the bound:
  `IC runner: <role> fields manifest field list (1..1024 rows)`.
- `max_extra_fields = 256`; `using FieldMask = std::bitset<256>`. `FieldPlan::needs/planned`,
  `FieldResidency::resident_`, `verify_fields(..., const FieldMask& needed, ...)` and `score_role`'s `needed` are
  FieldMasks (were u64). `field_plan` refuses a library referencing more than 256 distinct extra fields:
  `IC runner: at most 256 extra fields (the library references N)`.
- The Belady schedule is unchanged: every u64 operation maps to the same bitset operation (`>>f&1` -> `test`,
  `|=1<<f` -> `set`, `&=~(1<<f)` -> `reset`, `popcount` -> `count`, `!x` -> `none`), so for <= 64 extras the plan,
  loads, residency and progress lines are the same values.
- On-disk formats: no mask is serialised anywhere (manifests, sidecars, IC entries and summaries carry names,
  SHAs and counts only), so nothing needs an old-form reader. The header comment says so.

**Worker cap 4 -> 16**, one constant: `atx::engine::factory::max_research_ic_workers = 16` (ic_research.hpp), used
by `ic_screen.cpp` `prepare_cache` (was `> 4`; a `static_assert` keeps its refusal text "1..16" in step), by
`run_ic`'s bounded config, and by the frozen-TRAIN recipe bound on `vm_workers` (strategy_ic_admission.cpp, was
`> 4`; a 12-worker TRAIN artifact can now seed a validation-only run). Help text says `--workers 1..16`.
The admission envelope per worker is as coded (8 MiB + 64 KiB, 1,104 B/name, 64 B/date).

**Source pin.** `ic_screen.cpp` and `ic_research.hpp` are in `ic_result_sources`, so the digest is re-set to
`5bc47755...1954` (recomputed with the Python mirror of the test). No `ic_result_semantics_version` bump: a
wider worker bound schedules the same per-date row kernels; no IC bit changes. Cache keys are unchanged (neither
key hashes sources; workers are recorded, never keyed).

**Plan JSON** gains `max_working_bytes`, so `--plan-only` states both sides of the admission test.

## OD-2 (`--max-memory-mib 2560`): where 1,536 is enforced

The C++ runner already accepts 2,560: `dispatch_ic` allows `--max-memory-mib` up to 16,384, `run_ic` allows
`max_working_bytes` in [32 MiB, 16 GiB], and the frozen-TRAIN recipe check has the same bounds. No C++ change was
needed; `AdmissionReportsRequiredBytes` pins that the CLI admits 2,560 (and refuses 16,385). 1,536 lives only in
the drivers and specs (lane A / root files, not edited):
- `scripts/specs/v71.json:218-219` (`ic.flags`: `--max-memory-mib 1536`) and the same in v61, v61-ops, v70,
  v70-lo3, v7u-lo3;
- `scripts/specs/v7*.json` `runner.max_rss_mib: 1536` (e.g. v71.json:13), the bounded runner's RSS cap;
- `scripts/research_cycle.py:624` (fields step default `max_rss_mib` 1536) and its docstring (:66);
- `scripts/run_bounded_research.py:67` (`--max-rss-mib` default 1024).
Raising the admission to 2,560 needs both the IC flag and the bounded runner's RSS cap raised together.

## Tests (atx-impl/tests/strategy_ic_runner_test.cpp)

- `FieldCaps.Admits200RowManifestWith40Referenced`: 40 referenced fields (4 candidates x 10) with 200-row
  manifests score end to end (capacity 10, 40 loads); a 1,024-row manifest plans, 1,025 refuses before any output.
- `FieldCaps.RefusesLibraryReferencing257Fields`: 257 referenced fields refuse at library compile, before any
  manifest, role or output; 256 (bits 0..255) score end to end, the last candidate loading x250..x255.
- `FieldCaps.V71FieldPlanUnchanged`: the committed v7.1 library (`fund_industry_ic_v71.json`, 40 extras) plans
  `resident_capacity` 6 and `planned_loads` 54, the values the receipted v7.1 u pass printed
  (`mega-v71-train-u-run1/stdout.log`, one grep of those two counters).
- `Workers.OutputsByteIdenticalAt4And8And16`: daily IC CSVs, planned targets, the saved blend and its masks,
  orientations rows and every role statistic identical at 4, 8 and 16; the recipe differs only in `vm_workers`
  and `research_ic_workers`.
- `StrategyIcRunner.AdmissionReportsRequiredBytes`: plan-only prints per-role `required_bytes` and
  `max_working_bytes`; 16 workers need exactly 12 per-worker envelopes more than 4; one byte below the
  requirement the run (and plan) refuses before any payload or output with `required_bytes=<n>`; the CLI admits
  `--max-memory-mib 2560 --workers 16` and refuses 16,385 MiB and 17 workers.
- Updated: `WorkerBoundsAndAdditionalMemoryAreAdmittedBeforePayload` now refuses 0 and 17 (was 0 and 5).

## How root verifies

Build: `atx-impl-strategy-ic-tests atx-equity-strategy-ic`. The engine change touches `ic_research.hpp`
(included by atx-engine factory TUs) and `ic_screen.cpp`.
gtest: `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:NoComposition.*:FieldCaps.*:Workers.*:StrategyIcComposition.*:IcScreen.*:ResearchIc.*`
(StrategyIcRunner 49, NoComposition 3, FieldCaps 3, Workers 1: 56 in strategy_ic_runner_test.cpp). If atx-engine factory tests are in the gate, also
`atx-engine-factory-tests` (ic_research_test.cpp still uses 2 and 4 workers).

Identity (brief step 3), v7.1 u and w at 4 and at 12 workers:
- Run the 12-worker passes **with an empty `--candidate-cache` directory** (or none): neither the signal nor the
  IC-result key contains the worker count, so on the warm v7.1 cache a 12-worker run hits every entry and
  exercises neither the VM pool nor the IC rows.
- **Admission at 12 workers exceeds 1,536 MiB on the v7.1 role.** Per worker the envelope is 8 MiB + 64 KiB +
  1,104 B x 5,627 names + 64 B x ~1,155 dates, about 14.1 MiB; 12 workers admit about 112 MiB more than 4. The
  v7.1 u pass admitted about 1,481 MiB at 4 workers, so 12 needs about 1,594 MiB: run it with
  `--max-memory-mib 2560` (OD-2) and a matching bounded-runner RSS cap, or first read `--plan-only`
  `required_bytes` at `--workers 12`.
- Compare between 4 and 12: `train_daily_ic.csv`, `train_planned_targets.csv`, `train_combined.f64`,
  `train_combined_member.u8`, `train_combined_finite.u8` SHA-equal; `orientations.json` `candidates` equal.
  `recipe.json` differs only in `vm_workers`/`research_ic_workers`, so `recipe_sha256` (and the hash inside
  `orientations.json` and `train_combined.json`) differ by design, as between 1 and 2 workers today.
- Record `stage_seconds` vm, ic and composition at 4 and 12 from `summary.json`.
- Flags off (default workers, <= 64 extras): every output is identical to B-1's; B-2 changes no value there.

## Deviations

- The worker bound lives in the engine header (`max_research_ic_workers`) and the runner uses it, instead of two
  literals, so the IC and runner caps cannot drift (P-15 "caps in 5 places").
- `max_working_bytes` added to the plan JSON (not asked; needed to state the admission test in one place).

## Cross-lane edits

- `atx-engine/include/atx/engine/factory/ic_research.hpp` (new constant; comment). In scope per the dispatch
  (`ic_screen.cpp`'s cap), but it is a shared engine header.

## Open risks

- Not compiled.
- The generators' comment `MAX_EXTRA_FIELDS = 64  # strategy_ic_runner: ... at most 64 extra fields`
  (atx-impl/strategies/generate_fund_ic_v4.py:57) is now stale; the generators are frozen legacy (lane A).
- `field_plan` is O(n^2 f) in metadata; at the new bounds (256 candidates, 256 fields) a pathological library
  could take on the order of a second to plan. Realistic libraries (48 x 40) are microseconds.
