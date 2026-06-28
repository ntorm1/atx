# Phase 6 — Incremental Panel Append + Provenance — Progress Ledger

**Base:** `main @ e1c421f` (S1–S5 spine merged).
**Branch:** `feat/p7-s6`.
**Plan:** `atx-engine/plans/p7/sprint-6-incremental-panel.md`.
**Build:** unity-OFF (`-DATX_UNITY_BUILD=OFF`), `dev` preset; byte-identity gate
`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*` must be green before+after every unit.

## Unit checklist

| Unit | Title | Status |
|---|---|---|
| S6-0 | Open sprint-6 ledger (marker commit) | open |
| S6-1 | `append_history_panel` API + byte-identity tests | pending |
| S6-2 | `run_panel` incremental flag (opt-in; default frozen) | pending |
| S6-3 | Real `wall_ms` in `store_progress_sink` | pending |
| S6-4 | `config_json` + `engine_git_sha` provenance | pending |
| S6-5 | Determinism guard: provenance not in digests | pending |

## Plan line-anchor drift (located by content)

The plan's "Verified engine facts" file:line anchors drifted since they were written.
Actual locations at base `e1c421f`:
- `stage_panel.cpp`: `run_panel` at line 20 (matches); `build_history_panel(hc)` at line 58 (matches);
  `write_panel(hp.panel, ...)` at line 72 (matches). A provenance sidecar block (S5-3) now occupies
  lines 74–136 between write_panel and the StageResult return.
- `serialize_panel.cpp`: `kMagic` at line 18, `kVersion` line 19; version write `put_u32(buf, kVersion)`
  at line 126 (matches); fnv1a64 digest at line 157; output write at lines 168–169.
- `store_progress_sink.cpp`: `/*wall_ms*/ 0` is at line 42 inside `on_generation` (NOT `save_checkpoint`;
  `save_checkpoint` is an engine-store method in `pipeline_progress.hpp` — out of Owns, must not change
  its signature). The sink passes wall_ms as the 8th positional arg.
- `stage_discover.cpp`: `config_json = ""` at line 540, `engine_git_sha = ""` at line 541 (match). The
  `save_checkpoint`/checkpoint site referenced for wall_ms is in `store_progress_sink.cpp` (above), not
  in stage_discover.cpp; no wall_ms edit needed in stage_discover.cpp.
- `history_panel.hpp`: `HistoryDataConfig` at line 48 (matches); `build_history_panel` decl at line 96
  (plan said "after build_history_panel" — appended there). Field constants at lines 32–43 (match).
- `stage_load.cpp`: `created_at_nanos = 0` at line 37 (matches; frozen, not disturbed).

## Decisions / design notes

- **D6 (CMake git-SHA bake):** `atx-impl/CMakeLists.txt` edited ONLY for the `ATX_ENGINE_GIT_SHA`
  build-time `#define` bake (execute_process git rev-parse + configure_file/compile-def) and to add any
  new owned `.cpp`. No toolchain/flag/preset change.
- **JSON:** no first-party JSON utility exists (only vendored nlohmann inside databento third-party). Per
  the brief, no new dependency added — `config_json` is a hand-rolled compact JSON object over the
  config-parameter keys that affect admission/scoring (no wall-clock timestamps -> deterministic).
- **wall_ms:** computed in the sink via `std::chrono::steady_clock` captured at sink construction;
  elapsed-to-now at each checkpoint. (save_checkpoint's signature already carries wall_ms.)

## Per-unit completion log

(to be appended as each unit lands)
