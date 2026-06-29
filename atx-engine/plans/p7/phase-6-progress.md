# Phase 6 — Incremental Panel Append + Provenance — Progress Ledger

**Base:** `main @ e1c421f` (S1–S5 spine merged).
**Branch:** `feat/p7-s6`.
**Plan:** `atx-engine/plans/p7/sprint-6-incremental-panel.md`.
**Build:** unity-OFF (`-DATX_UNITY_BUILD=OFF`), `dev` preset; byte-identity gate
`atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*` must be green before+after every unit.

## Unit checklist

| Unit | Title | Status |
|---|---|---|
| S6-0 | Open sprint-6 ledger (marker commit) | complete (887be00) |
| S6-1 | `append_history_panel` API + byte-identity tests | complete (066d5ef) |
| S6-2 | `run_panel` incremental flag (opt-in; default frozen) | complete (4c8b3ee) |
| S6-3 | Real `wall_ms` in `store_progress_sink` | complete (251333b) |
| S6-4 | `config_json` + `engine_git_sha` provenance | complete (4514eab) |
| S6-5 | Determinism guard: provenance not in digests | complete (200f9bc) |

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

- **S6-0** (887be00): opened ledger + ROADMAP row. Byte-identity gate green baseline (18/18).
- **S6-1** (066d5ef): `append_history_panel` + 5 byte-identity tests (AppendHistoryPanel.*:
  ByteIdentical, MultiDayAppend, EmptyNewSegs, OverlapRejectsInvalidArgument, CompactRejected) all
  green; impl in `atx-impl/src/append_history_panel.cpp` (owned set, namespace atx::engine::data);
  decl appended after `build_history_panel` in `history_panel.hpp` (line 96 region; plan said "after
  build_history_panel"). Panel/load 8/8 green; oracle gate 18/18 green before+after.
- **S6-2** (4c8b3ee): `run_panel` incremental branch behind `#if defined(ATX_PANEL_INCREMENTAL)` via a
  new `acquire_history_panel` helper (anon namespace in `stage_panel.cpp`). Default path frozen
  (`build_history_panel`); guard OFF in all default/CI builds. `// S7-WIRES: cfg.incremental_panel: bool`
  marker placed at the build step. New-seg dir uses `cfg.out` (free on the panel path) until S7 declares a
  field. New test `AtxImplPanel.DefaultPathByteIdentical` (two default runs -> identical panel.bin bytes).
  Guarded branch compile-verified via throwaway `-DATX_PANEL_INCREMENTAL` reconfigure (reverted; no
  committed flag change). AtxImplPanel 5/5 green; oracle gate 18/18 green before+after.
- **S6-3** (251333b): real `wall_ms` via `steady_clock` captured at sink construction (`start_tp_`),
  elapsed-ms per `on_generation`. SCOPE NOTE: required adding a private member + `<chrono>` to the sink's
  own header `store_progress_sink.hpp` (paired with the owned `.cpp`; not in Must-NOT-touch). New test
  `AtxImplStoreDiscover.StoreProgressSinkWallMsNonZero` (RED: wall_ms==0 -> GREEN: >0 and <24h). Off-path
  discover byte-identity tests still green (wall_ms is provenance, never in the digest). Sink suite 6/6;
  oracle gate 18/18 green before+after.
- **S6-4** (4514eab): `config_json` = hand-rolled deterministic compact JSON over admission/scoring keys
  (no timestamps; non-finite gate sentinels as JSON strings; S6-PROVENANCE-SUBSET documented).
  `engine_git_sha` = CMake bake in `atx-impl/CMakeLists.txt` (D6): `git rev-parse HEAD` + porcelain dirty
  check + "-dirty"/"unknown" fallback -> PRIVATE compile def `ATX_ENGINE_GIT_SHA` (with `#ifndef` fallback
  in stage_discover.cpp). Verified bake: `251333b...-dirty`. New tests `AtxImplProvenance.*` (5):
  ConfigJsonNonEmpty, ConfigJsonRoundTrips, ConfigJsonDeterministic, EngineGitShaNonEmpty,
  EngineGitShaFormat — all green. Discover off-path byte-identity 6/6 (provenance never in
  manifest/digest); oracle gate 18/18 green before+after.
- **S6-5** (200f9bc): test-only tripwire `AtxImplProvenanceDigest.*` (3): PanelDigestUnchangedByProvenance,
  WallMsNotInPanelDigest (5ms wall advance -> same digest), ConfigJsonNotInPanelDigest (varied
  seed/gate fields -> same panel.bin). All green; oracle gate 18/18 green before+after.

## Final gate (sprint close)

- Full `atx-impl-tests`: **201 passed, 4 skipped, 0 failed** (the 4 skips are pre-existing
  `GTEST_SKIP()` in alpha101_orats / discover / single_alpha_capacity — synthetic-data / capacity gating,
  unrelated to S6).
- Twice-run determinism: S6 + touched suites (28 tests) identical across two runs.
- Byte-identity gate `atx-engine-factory-tests --gtest_filter=*Oracle*:*Golden*:*Digest*`: **18/18 green**
  at base AND after every unit.
- `atx-impl` executable links clean (git-SHA bake applied to atx-impl-core).
- Build: unity-OFF, `dev` preset, `/WX` warnings-as-errors clean. Guarded `ATX_PANEL_INCREMENTAL` branch
  compile-verified via a throwaway reconfigure (reverted; no committed flag change).

## Scope notes / deviations

- **store_progress_sink.hpp** (S6-3): added a private `start_tp_` member + `<chrono>`. The header is the
  sink unit's own interface (paired with the owned `.cpp`) and is NOT in the Must-NOT-touch list; the edit
  is the minimal mechanism the plan's S6-3 design prescribes (capture steady_clock at construction).
- **atx-impl/CMakeLists.txt** (D6): edited ONLY for (a) adding the new owned `append_history_panel.cpp`
  source and (b) the `ATX_ENGINE_GIT_SHA` git-SHA bake. No toolchain/flag/preset change.
- **stage_panel.cpp** new-seg dir: the guarded incremental branch repurposes `cfg.out` (unused on the
  panel path) as the new-seg directory until S7 declares a dedicated config field. Marked with the
  `// S7-WIRES: cfg.incremental_panel: bool` comment.
- **append_history_panel** is byte-identical by re-reading the combined partition (full-rebuild semantics)
  with a prefix self-check; a zero-re-read in-memory splice is deferred because `alpha::Panel` carries no
  symbol/date axis (documented in the header). This is honest incremental ergonomics, not a perf win on
  the historical re-read.
