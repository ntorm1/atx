# Sprint 6 — Incremental Panel Append + Provenance

**Goal:** Two quality-of-life improvements to the data loop and run reproducibility: (1) add an
**incremental append path** to `stage_panel` so one new trading day no longer requires a full
rebuild from all `.seg` files — the append result must be **byte-identical** to a full rebuild over
the same date range; (2) **populate the three empty provenance fields** (`wall_ms`, `config_json`,
`engine_git_sha`) that every `PipelineRunRow` currently leaves blank, so every discover run is
auditable and reproducible.

**Determinism contract: ADDITIVE / opt-in (append) + METADATA-ONLY (provenance).**
- Incremental append is opt-in via an explicit API path; the default `run_panel` call continues
  to do a full rebuild and produce byte-identical `panel.bin` output. The new append path is
  correct *by construction* — it must pass a byte-identity test against full rebuild on the same
  fixture before it ships.
- Provenance additions (`wall_ms`, `config_json`, `engine_git_sha`) are metadata stored in
  `PipelineRunRow` inside the run DB. They must NOT alter the `panel.bin` digest or the discover
  search digest — those are binary outputs on the deterministic path. Verified by the determinism
  unit in S6-4.

**Owns (exclusive):**
- `atx-impl/src/stage_panel.cpp`
- `atx-impl/src/serialize_panel.cpp`
- `atx-impl/src/stage_load.cpp`
- `atx-engine/include/atx/engine/data/history_panel.hpp`
- `atx-impl/src/store_progress_sink.cpp`
- `atx-impl/src/stage_discover.cpp` — ONLY the `config_json` / `engine_git_sha` / `wall_ms`
  provenance lines (lines 540–541 and the `save_checkpoint` site at line 42). Do NOT touch
  flag-threading, CLI wiring, or any other discover logic — those are S7's exclusive domain.
- New test files under `atx-impl/tests/` or `atx-engine/tests/` (auto-globbed by CMake).

**Must NOT touch:**
- `atx-impl/src/config.hpp`, `atx-impl/src/config.cpp` — S7 exclusive.
- `atx-impl/src/stage_run.cpp` — S7 exclusive.
- Gate / fitness paths (`combine/gate.hpp`, `combine/metrics.hpp`) — S1 exclusive.
- Eval VM (`alpha/ts_ops.hpp`, `alpha/vm.hpp`) — S3 exclusive.
- `atx-engine/tests/factory/oracle.hpp` — untouchable by every sprint.

---

## Implementation quality handoff block (paste into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not
follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the
public API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not
leave TODO placeholders, fake success paths, unused APIs, or untested skeletons.

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

## Verified engine facts (confirmed via Read, cited by file:line)

### `stage_panel.cpp`
- `run_panel` starts at **line 20**.
- `build_history_panel(hc)` called at **line 58** (inside `ATX_TRY` macro).
- `write_panel(hp.panel, ...)` called at **line 72**.
- S7-3 augmentation marker comment at **line 60** (placed by p6-S5).
- **No incremental/append path exists.** Every call reads the whole seg partition.

### `serialize_panel.cpp`
- Magic constant `kMagic = 0x4C4E5041u` (`APNL` little-endian) at **line 18**; encoding comment at **lines 16–17**.
- Version field written at **line 126** (`put_u32(buf, kVersion)`).
- FNV-1a-64 digest computed at **line 157** over the full buffer, written as 8-byte trailer.
- `ofs.write(...)` single output write at **lines 168–169**.
- Format: `[magic u32][version u32][D u32][I u32][F u32][per-field names][D×I f64 date-major data][D×I u8 universe mask][fnv1a64 u64 trailer]`.

### `store_progress_sink.cpp`
- `wall_ms` hardcoded to `0` at **line 42** inside `save_checkpoint` (`/*wall_ms*/ 0`).

### `stage_discover.cpp`
- `PipelineRunRow` construction block at **lines 532–542**.
- `config_json = ""` at **line 540**.
- `engine_git_sha = ""` at **line 541**.

### `atx-engine/include/atx/engine/data/history_panel.hpp`
- 12 canonical `kHistField*` constants at **lines 32–43**:
  `close`, `raw_close`, `volume`, `high`, `low`, `open`, `market_cap`, `sector`,
  `earnFlag`, `atmCenI_21d`, `atmCenI_126d`, `nEarnCnt_5d`.
- `HistoryDataConfig` struct at **line 48**.
- `compact_to_universe` is a config flag (line 57), not a standalone function.

### `stage_load.cpp`
- `created_at_nanos = 0` at **line 37** (hardcoded for determinism — correct, do not disturb).
- No date-major validation found in the current `run_load` scope.

---

## Wiring map

```
stage_load.cpp          → per-date .seg files (ORATS zips)
                          created_at_nanos=0 (determinism; frozen)

stage_panel.cpp         → build_history_panel(hc)           [line 58]  ← full rebuild today
  NEW append path       → append_history_panel(existing_panel, new_segs, hc)
                          write_panel(hp.panel, ...)          [line 72]
                          serialize_panel.cpp (APNL v1, fnv1a64 trailer)

store_progress_sink.cpp → save_checkpoint(...)               [line 42]
                          wall_ms = 0  ← REPLACE with real elapsed

stage_discover.cpp      → PipelineRunRow{...}                [lines 532–542]
                          config_json = ""   [line 540]  ← REPLACE
                          engine_git_sha = "" [line 541] ← REPLACE
```

---

## Tasks

### S6-0 — Open sprint ledger *(marker commit; no code)*

**Goal:** Freeze scope, create the `phase-6-progress.md` ledger under `atx-engine/plans/p7/`,
and land the marker commit so all subsequent units have a base SHA.

**Wiring:** No source changes. Creates:
- `atx-engine/plans/p7/phase-6-progress.md` (skeleton with S6-0…S6-5 rows, status=open)
- Updates `atx-engine/plans/p7/ROADMAP.md` `sprint-6-incremental-panel.md` status row to `⏳ open`.

**Determinism:** No binary outputs touched; no test changes.

**Accept:**
- Marker commit message: `docs(p7-S6-0): open sprint-6 incremental-panel ledger`.
- Ledger header has `Base: master @ <SHA>`.
- ROADMAP row for S6 updated.
- No source files modified.

---

### S6-1 — `append_history_panel` API + byte-identity unit test *(core correctness gate)*

**Goal:** Add a public `append_history_panel` function that, given an existing in-memory
`HistoryPanel` (already loaded from `panel.bin`) plus one or more new `.seg` files for dates
strictly after the panel's last date, produces an extended `HistoryPanel`. The result must be
**byte-identical** — same serialized `fnv1a64` digest — to a full rebuild from all segs over
the combined date range. This is the central correctness gate for the entire sprint.

**Design constraints:**
- The append function must preserve the existing panel's date-major row ordering and universe
  mask exactly. New dates are appended in non-decreasing order at the tail.
- If `new_segs` is empty, `append_history_panel` returns the existing panel unchanged (no-op,
  byte-stable).
- If any new seg date is ≤ the existing panel's last date, return `Err(InvalidArgument)` — the
  caller must not present overlapping or out-of-order segments.
- The function is a new free function, not a method on `HistoryPanel`. It does NOT modify
  `build_history_panel`; that path is frozen.
- The function lives in `history_panel.hpp` alongside the existing `build_history_panel`
  declaration, and its implementation in a new `append_history_panel.cpp` under
  `atx-impl/src/` (or alongside `stage_panel.cpp` — follow the local convention).

**Wiring:**
- `atx-engine/include/atx/engine/data/history_panel.hpp:48` — add `append_history_panel`
  declaration after `build_history_panel`.
- New `atx-impl/src/append_history_panel.cpp` (or inline in `stage_panel.cpp` if the local
  convention is to keep stage logic in one file — check and follow the pattern).
- `atx-impl/CMakeLists.txt` — add new `.cpp` to the source list if needed.

**Determinism:** The full-rebuild path (`build_history_panel` → `write_panel`) is not
touched. The append path is a new code path that produces byte-identical output — proven by the
byte-identity test, not by assertion.

**Accept:**
- `AppendHistoryPanel_ByteIdentical`: build a `HistoryPanel` over dates `[d0..dN]` via full
  rebuild; separately build over `[d0..dN-1]` then append `dN`. Serialize both via
  `write_panel` to in-memory buffers. Assert buffers are bitwise identical (same `fnv1a64`
  digest, same byte length). Use a tiny deterministic fixture (≤10 dates, ≤5 instruments,
  hand-set values in the seg data — no real ORATS data).
- `AppendHistoryPanel_EmptyNewSegs`: append with empty seg list returns the original panel
  byte-for-byte.
- `AppendHistoryPanel_OverlapRejectsInvalidArgument`: seg date ≤ last panel date returns
  `Err(InvalidArgument)`.
- `AppendHistoryPanel_MultiDayAppend`: append 3 new days in one call; byte-identical to full
  rebuild over all days.
- All pre-existing `stage_panel` / panel-read / pseed / alpha101 tests remain green.

---

### S6-2 — `run_panel` incremental flag (opt-in; default path frozen)

**Goal:** Wire the `append_history_panel` function into `stage_panel.cpp` behind an explicit
opt-in. When the opt-in is active and a valid `panel.bin` already exists at the output path,
`run_panel` loads the existing panel, identifies which seg files are new (dates strictly after
the panel's last date), calls `append_history_panel`, and writes the extended panel. When the
output file does not exist or the opt-in is not set, it falls back to the existing full-rebuild
path — byte-identical to pre-S6 behavior on the same inputs.

**Wiring (file:line):**
- `atx-impl/src/stage_panel.cpp:20` (`run_panel`) — add the opt-in branch. Pattern:

  ```cpp
  // S7-WIRES: cfg.incremental_panel will be declared in config.hpp by Sprint 7.
  // Until then the flag defaults to false and the existing path is taken.
  #if defined(ATX_PANEL_INCREMENTAL)
      if (/* existing panel.bin present */ !cfg.panel_out.empty() && path_exists(cfg.panel_out)) {
          ATX_TRY(auto existing, read_panel(cfg.panel_out));
          // identify new segs: dates > existing.panel.last_date()
          ATX_TRY(auto extended, append_history_panel(existing.panel, new_segs, hc));
          ATX_TRY(auto wd, write_panel(extended, cfg.panel_out, cfg.panel_universe));
          return make_stage_result(wd);
      }
  #endif // ATX_PANEL_INCREMENTAL
  // Default: full rebuild (unchanged from pre-S6)
  ATX_TRY(auto hp, build_history_panel(hc));   // line 58
  ATX_TRY(auto wd, write_panel(hp.panel, ...)); // line 72
  ```

- Add a `// S7-WIRES: cfg.incremental_panel: bool` comment at the top of `run_panel` so S7
  knows exactly which config field to declare.
- Include `atx/engine/data/history_panel.hpp` if not already included.

**Determinism:** The `#if defined(ATX_PANEL_INCREMENTAL)` guard is OFF in all default and CI
builds. The existing `build_history_panel` → `write_panel` path is untouched. No existing
digest or golden changes.

**Accept:**
- Default build (`ATX_PANEL_INCREMENTAL` not defined): `run_panel` produces byte-identical
  output to pre-S6 on the same inputs. Verified by running the existing stage_panel test or a
  small integration fixture.
- `ATX_PANEL_INCREMENTAL` build: `run_panel` detects existing `panel.bin`, appends the new
  seg(s), and the output digest matches a full rebuild over the combined range. (Can reuse the
  byte-identity fixture from S6-1 driven through `run_panel`.)
- `// S7-WIRES:` comment present at top of `run_panel`.
- All existing panel/alpha101/pseed tests green.

---

### S6-3 — Real `wall_ms` in `store_progress_sink`

**Goal:** Replace the hardcoded `wall_ms = 0` at `store_progress_sink.cpp:42` with the actual
wall-clock elapsed milliseconds for the checkpoint. The timing must be per-checkpoint (start of
generation to checkpoint call), not a global process start.

**Design:**
- The `save_checkpoint` call site already receives enough context to know "start of this
  generation." The simplest correct approach: add a `start_tp` parameter (of type
  `std::chrono::steady_clock::time_point`) to `save_checkpoint`, or have the sink capture
  `std::chrono::steady_clock::now()` at construction time and compute elapsed at each
  `save_checkpoint` call. Follow whichever pattern is already used elsewhere in the sink for
  timing — read `store_progress_sink.cpp` in full before choosing.
- `std::chrono::steady_clock` (monotonic) is the correct clock. Do NOT use
  `system_clock` for elapsed time.
- The value must be > 0 in any real run (proven by the provenance unit test in S6-5).
- Do NOT change the DB schema or `PipelineRunRow` fields — `wall_ms` already exists as a
  column; this unit only populates it correctly.

**Wiring (file:line):**
- `atx-impl/src/store_progress_sink.cpp:42` — replace `/*wall_ms*/ 0` with the computed
  elapsed value.
- If a parameter is added to `save_checkpoint`, update its callers. Read the call sites before
  changing the signature.

**Determinism:** `wall_ms` is stored in the run DB (provenance), not in `panel.bin` or the
discover search digest. Panel binary output and search digest are unchanged. The provenance unit
test (S6-5) verifies `wall_ms > 0` without asserting any specific value.

**Accept:**
- `StoreProgressSink_WallMsNonZero`: construct a `StoreProgressSink`, run a synthetic
  discover cycle (or mock one via direct `save_checkpoint` call with a small `sleep` or by
  injecting a fake elapsed), and assert the recorded `wall_ms > 0`.
- The recorded value is plausible (> 0, < some large upper bound like 24h in ms) — not an
  exact assertion.
- All existing sink tests green.

---

### S6-4 — `config_json` + `engine_git_sha` provenance in `stage_discover`

**Goal:** Populate the two empty provenance strings in `PipelineRunRow` at
`stage_discover.cpp:540–541`:
1. `config_json` — a JSON snapshot of the `RunConfig` / `DiscoverConfig` at the time the run
   starts. Must round-trip: parsing the stored JSON re-produces the same config values.
2. `engine_git_sha` — the git SHA of the engine at build time, embedded via a build-time
   `#define`.

**Design — `config_json`:**
- Produce a compact JSON string from the discover-stage config fields (universe thresholds,
  adv_window, seed, gate thresholds, etc.). Minimally: all fields that affect which alphas are
  admitted or how they are scored, so a future reader can reconstruct the exact gate/seed
  environment.
- Use the existing JSON utility in the codebase (grep for `nlohmann` or `rapidjson` or
  `atx::json` before choosing — follow what the project already uses). Do NOT pull in a new
  JSON dependency.
- The JSON must not contain wall-clock timestamps (those are provenance, not config) — only
  config-parameter key-value pairs. This keeps the field deterministic across same-config runs.
- If the config serialization is non-trivial to implement completely, ship a minimal but
  honest subset: list the fields you cover in a comment and mark it `// S6-PROVENANCE-TODO:`
  for extension. Do NOT produce a fake `"{}"` or hardcoded string.

**Design — `engine_git_sha`:**
- Embed the git SHA at build time via CMake. In `CMakeLists.txt` (for the `atx-impl` target),
  add a `configure_file` step or `target_compile_definitions` that reads `git rev-parse HEAD`
  and bakes it into a `#define ATX_ENGINE_GIT_SHA "..."`. Alternatively, use the CMake
  variable `GIT_EXECUTABLE` + `execute_process` — follow the local pattern if one already
  exists (grep `CMakeLists.txt` for `GIT_SHA` or `GIT_COMMIT` first).
- If the working tree is dirty, append `-dirty` to the SHA.
- In `stage_discover.cpp:541`, replace `engine_git_sha = ""` with
  `engine_git_sha = ATX_ENGINE_GIT_SHA`.

**Wiring (file:line):**
- `atx-impl/src/stage_discover.cpp:540` — replace `config_json = ""`.
- `atx-impl/src/stage_discover.cpp:541` — replace `engine_git_sha = ""`.
- `atx-impl/CMakeLists.txt` — add git SHA bake step.

**Determinism:** `config_json` and `engine_git_sha` are stored only in `PipelineRunRow` (run
DB). They are NOT written into `panel.bin` or included in any search/discover digest
computation. The determinism contract is verified by S6-5.

**Accept:**
- `Provenance_ConfigJsonNonEmpty`: a synthetic discover run (or direct `PipelineRunRow`
  construction) produces a non-empty `config_json`.
- `Provenance_ConfigJsonRoundTrips`: parse the stored `config_json` back via the same JSON
  library; assert the key gate/seed fields round-trip to their original values.
- `Provenance_EngineGitShaNonEmpty`: `engine_git_sha` is non-empty (the `ATX_ENGINE_GIT_SHA`
  macro is populated at build time; assert `strlen > 0`).
- `Provenance_EngineGitShaFormat`: the SHA is a valid hex string of length 40 (or 40+8 for
  `-dirty` suffix check).
- All existing discover / pipeline-run tests green.

---

### S6-5 — Determinism guard: provenance must not enter digests

**Goal:** Prove that the provenance additions from S6-3 and S6-4 do not alter any existing
binary digest — specifically `panel.bin`'s `fnv1a64` trailer and the discover search digest
(whatever hash the discover stage uses to fingerprint its output). This unit is a dedicated
test-only unit; it ships no new production code.

**Rationale:** The determinism contract is the spine of the engine's reproducibility story.
S6-3 and S6-4 operate on the run DB (metadata), not on the binary outputs — but a future
refactor could accidentally wire them into a digest. This unit provides a regression tripwire.

**Wiring (file:line):**
- New test file: `atx-impl/tests/provenance_digest_test.cpp` (auto-globbed by CMake).
- Reads `panel.bin` digest before and after a run with provenance populated; asserts identical.
- Reads the discover output digest (if the discover stage exposes one) before and after; asserts
  identical.
- If no discover digest API exists, assert instead that the `panel.bin` produced by a
  provenance-populated run has the same `fnv1a64` trailer as one produced with empty provenance
  (same input data, same config).

**Determinism:** This unit only adds tests. No source changes.

**Accept:**
- `PanelDigestUnchangedByProvenance`: two `run_panel` calls on the same synthetic fixture
  (one pre-S6-3/S6-4 code path, one post) produce identical `panel.bin` bytes.
- `WallMsNotInPanelDigest`: a run with non-zero `wall_ms` and a run with zero `wall_ms`
  produce the same `panel.bin` digest. (Trivially true if `wall_ms` never touches the panel
  serializer — this test is the explicit proof that it doesn't.)
- `ConfigJsonNotInPanelDigest`: two runs with different `config_json` strings (e.g., one with
  a field changed that doesn't affect panel construction) produce the same `panel.bin` digest.
- All new tests use the `ATS_TEST(...)` framework. No real-data dependency.

---

## Sequencing

```
S6-0  marker commit (no dependencies)
  │
S6-1  append_history_panel API + byte-identity test  ← core gate; must land first
  │
S6-2  run_panel incremental flag (depends on S6-1)
  │
S6-3  wall_ms (independent of S6-1/S6-2; can parallel with S6-2 if agents are separate)
  │
S6-4  config_json + engine_git_sha (independent of S6-1/S6-2/S6-3; parallel with S6-2+S6-3)
  │
S6-5  determinism guard tests (depends on S6-3 + S6-4 being in the tree)
  │
close commit
```

S6-1 is the regression gate for the panel path. Do not ship S6-2 until S6-1's byte-identity
test is green. S6-3 and S6-4 are independent of the panel path and can be worked in parallel
with S6-2, but S6-5 depends on both S6-3 and S6-4 being present.

---

## Risks / guardrails

| Risk | Mitigation |
|---|---|
| Append byte-identity fails due to field ordering or universe-mask divergence | S6-1 byte-identity test is the explicit gate. Must be green before S6-2 ships. If it fails, the divergence will be visible in the buffer diff — inspect at byte level. |
| Append path activated by accident in a non-incremental build | `#if defined(ATX_PANEL_INCREMENTAL)` guard (S6-2) is OFF in all default and CI builds. The flag is never set unless the caller explicitly defines it. |
| Overlapping / out-of-order seg dates passed to append | `append_history_panel` returns `Err(InvalidArgument)` immediately; tested in S6-1 Accept. |
| `wall_ms` clock choice produces overflow or non-monotone reads | Use `std::chrono::steady_clock` (monotonic). Elapsed computed as `duration_cast<milliseconds>(now - start).count()`. No rollover risk in any realistic discover run. |
| `config_json` is a wall-clock timestamp accidentally (making it non-deterministic) | Config JSON must contain only config-parameter keys — no wall-clock time. Document explicitly in the `stage_discover.cpp` comment. S6-4 round-trip test catches field drift. |
| `engine_git_sha` bake step fails in environments without git | CMake `execute_process` should provide a fallback string `"unknown"` if git is not available; verified in CI. |
| `config_json` or `engine_git_sha` leaks into a panel digest path via a future refactor | S6-5 determinism tests are the explicit tripwire. They must be kept in the test suite permanently. |
| Serializer format version bump required for append | No. Append produces the same `APNL v1` format; the version field at `serialize_panel.cpp:126` is unchanged. The byte-identity test in S6-1 is the proof. |
| S7 `config.hpp` changes invalidate the `cfg.incremental_panel` stub comment | The `// S7-WIRES:` comment is informational, not code. S7 will declare the real field; the `#if` guard compiles to nothing until then. |

---

## Bench / acceptance summary

**Sprint gate (must be green before close commit):**

| Test | File | What it proves |
|---|---|---|
| `AppendHistoryPanel_ByteIdentical` | `atx-impl/tests/append_panel_test.cpp` | Append == full rebuild on same date range |
| `AppendHistoryPanel_EmptyNewSegs` | same | No-op on empty seg list |
| `AppendHistoryPanel_OverlapRejectsInvalidArgument` | same | Overlap detection is fail-closed |
| `AppendHistoryPanel_MultiDayAppend` | same | Multi-day batch append is byte-identical |
| `StagePanel_DefaultPathByteIdentical` | `atx-impl/tests/stage_panel_test.cpp` (extend) | `#if` guard off → no regression |
| `StoreProgressSink_WallMsNonZero` | `atx-impl/tests/store_progress_sink_test.cpp` | wall_ms > 0 in a real checkpoint |
| `Provenance_ConfigJsonNonEmpty` | `atx-impl/tests/provenance_test.cpp` | config_json populated |
| `Provenance_ConfigJsonRoundTrips` | same | JSON round-trips key gate fields |
| `Provenance_EngineGitShaNonEmpty` | same | git SHA baked at build time |
| `Provenance_EngineGitShaFormat` | same | SHA is 40-char hex (± `-dirty`) |
| `PanelDigestUnchangedByProvenance` | `atx-impl/tests/provenance_digest_test.cpp` | panel.bin digest is provenance-free |
| `WallMsNotInPanelDigest` | same | wall_ms never enters serializer |
| `ConfigJsonNotInPanelDigest` | same | config_json never enters serializer |

**Twice-run check:** run the full test suite twice on the same build; all digests and test
outcomes must be identical. Record the test binary name + total/fail/skip counts in the ledger.

**No hour-long production run is a gate.** Per p7 validation discipline, the only production
run in this module is the operator V1 milestone after S1–S5 spine sprints land.

**Dev-panel smoke (optional, ≤5 min):** after all units green, run
`scripts/build-tradeable-alphas.ps1 -Profile smoke` on the cached `work/dev/dev-panel.bin`
(600×501). Confirms wiring compiles and runs end-to-end. Not a correctness gate — the
byte-identity and provenance unit tests are the gate.

---

## Out of scope (S7)

- `cfg.incremental_panel: bool` CLI flag declaration in `config.hpp`.
- `--incremental-panel` CLI argument wiring in `config.cpp` / `stage_discover.cpp`.
- mmap or zstd read path (future sprint, out of p7 scope).
- Any changes to `oracle.hpp`, `combine/`, `eval/`, or `factory/` paths.
