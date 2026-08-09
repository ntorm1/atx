# Feature Factory + Corpus Labels (Sprint A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the BEV label factory emit the full `kFairVolFeatureSchemaV1` feature block beside its target, and ship the corpus-scale batch/QA tooling — producing the first trainable dataset for the `IFairVolModel` seam.

**Architecture:** All C++ work is an extension of the existing `examples/bev_label_factory.cpp` driver (and its NO_MAIN gate test) — no library-header changes except a doc-comment sync in `theo.hpp`. Feature values are produced with the exact same accessors the serving-side overlay uses (`surf.iv`, `surf.forward_at`, `surf.delta`, `realized_vol_panel`, `count_events_at`), so train-time and serve-time features cannot drift. Corpus orchestration and QA are Python scripts in a new `atx-vol/scripts/` tier (the C++ driver stays single-uid/single-tenor; the runner fans it out).

**Tech Stack:** C++20 (existing atx-vol toolchain, MSVC + Ninja via `scripts\atx-build.ps1`), GoogleTest, Python 3 + pytest (stdlib only — no pandas dependency).

**Context (read once):** roadmap `docs/research/2026-08-09-theo-ml-alpha-iteration-plan.md` §4 S1–S2; sprint summary `atx-vol/sprints/2026-08-08-theo-module-sprint-summary.md`. This sprint closes roadmap gaps G1 (no feature producer) and the tooling half of G6 (corpus wiring). Training itself (S3) is explicitly OUT of scope.

## Global Constraints

- `Result<T>` is tl::expected: `.has_value()`, `.error()`, `ATX_TRY` / `ATX_TRY_VOID`; `.ok()` does not exist.
- Designated initializers for aggregate construction in new code; any new multi-field config struct gets an `aggregate_arity_is_v` pin (see `breakeven.hpp` for the pattern).
- The label TSV's **byte-determinism gate is inviolable**: two runs with identical args must remain byte-identical (`memcmp` in the gate test). No timestamps, hostnames, or locale-dependent formatting in output. Doubles via the existing `append_double` (`%.17g` snprintf); NaN prints as `nan` and round-trips through `strtod` — emit NaN sentinels through `append_double`, never hand-formatted.
- Feature column names and order are **frozen to `kFairVolFeatureSchemaV1`** (`theo.hpp`): `log_moneyness, tenor_years, market_vol, rv_21d, rv_63d, iv_minus_rv, n_events_to_expiry, delta_abs`. `kFairVolFeatureCount == 8`.
- RV estimator for spot-mirror bars is **`RvEstimator::CloseToClose` only** (bars are O=H=L=C spot mirrors; range estimators are degenerate on them — YZ/GK/RS stay dormant until real OHLC lands, roadmap §5).
- Event-count semantics must match serving: `count_events_at(schedule, entry_ts_ns, T)` (`event_vol.hpp` — `(now, expiry]` boundaries, maturity synthesized from year-fraction T), NOT a hand-rolled count against the settle timestamp.
- clang-format gate: `clang-format --dry-run --Werror` clean on every touched file (new lines; pre-existing drift elsewhere in a file is out of scope).
- Tests: targeted runs only (standing user directive) — `& .\scripts\atx-build.ps1 -Ctest -R '<Suite>'` from repo root; never full lanes. Build: `& .\scripts\atx-build.ps1 build atx-vol-tests`. Note: quote the `-R` regex and invoke the script directly (`& .\scripts\...`), not through a nested `powershell` call — the nested call re-tokenizes `|`.
- Python: stdlib only (csv, json, argparse, math, subprocess, pathlib); tests via pytest, self-contained tmp-dir fixtures, no network, no real corpus dependency.
- Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `atx-vol/examples/bev_label_factory.cpp` | modify | Tasks 1–3: events loader + `--events`, spot-history pre-pass + RV panel, feature columns in `LabelRow`/TSV |
| `atx-vol/tests/bev_label_factory_gate_test.cpp` | modify | Tasks 1–3: gate coverage for each addition (same NO_MAIN textual-include mechanism — do not restructure it; controller ruling 2026-08-08) |
| `atx-vol/include/atx/vol/theo.hpp` | modify (comments only) | Task 3: ML-seam banner sync (the "TSV carries target + join keys only" text becomes stale this sprint) |
| `atx-vol/scripts/bev_corpus_run.py` | create | Task 4: manifest-driven fan-out of the driver across uid × tenor |
| `atx-vol/scripts/bev_corpus_run_test.py` | create | Task 4: pytest, stubbed executable |
| `atx-vol/scripts/bev_label_qa.py` | create | Task 5: QA report over label TSVs |
| `atx-vol/scripts/bev_label_qa_test.py` | create | Task 5: pytest, synthetic TSVs |
| `atx-vol/scripts/README.md` | create | Task 4: one-paragraph tier note (Python utility scripts; not part of the C++ build) |
| `atx-vol/CHANGELOG.md`, sprint summary | modify/create | Task 6 |

Known state to respect (from the 2026-08-08 sprint's final review): `collect_entry_date_jobs` already computes `actual_delta` (line ~346) and `sigma_entry_iv = surf.iv(*k, T)` (line ~356) per candidate — Task 3 reuses both, adding **no** new surface calls per candidate except one `forward_at(T)` per entry date. The I1 fix's one-`load_bev_path`-per-entry-date structure (`path_idx` into `owned_paths`) must be preserved.

---

### Task 1: Events calendar input — `--events` TSV + per-candidate event count

**Files:**
- Modify: `atx-vol/examples/bev_label_factory.cpp`
- Test: `atx-vol/tests/bev_label_factory_gate_test.cpp` (append)

**Interfaces:**
- Consumes: `EventSchedule` (`event_vol.hpp` — ctor takes `std::vector<std::int64_t>`, sorts), `count_events_at(const EventSchedule&, std::int64_t now_ns, double T)` (`event_vol.hpp:192-202` region).
- Produces (file-local, consumed by Task 3):
  - `BevFactoryArgs::events` (`std::string`, default empty = no calendar)
  - `[[nodiscard]] Result<std::optional<EventSchedule>> load_events_tsv(std::string_view path);` — `std::nullopt` when `path.empty()`.
  - `[[nodiscard]] Result<std::int64_t> iso_date_to_ns(std::string_view iso);` — `YYYY-MM-DD` → epoch ns at 00:00 UTC (civil-days arithmetic).
  - `PendingJob::n_events` (`double`; NaN when no calendar loaded).

Events TSV format (documented in the file banner and `usage()`): one ISO date per line, `#` comment lines and blank lines skipped, CR tolerated (reuse the file's existing `rstrip_cr`/`trim` helpers, ~line 168). Dates are the announcement dates for `--uid` (one file per underlier, like `--dividends`). Timestamps are midnight UTC of the announcement date — a deliberate day-resolution approximation, stated in the banner.

- [ ] **Step 1: Append failing tests** to the gate test:

```cpp
// Task F-1: --events calendar. (h) load_events_tsv parses dates + comments and
// count_events_at semantics reach the rows; (i) malformed date rejected;
// (j) empty path => nullopt (feature column NaN handled in Task F-3's test).
TEST_F(BevLabelFactoryGate, EventsTsvParsesAndCounts) {
  const fs::path p = tmp_dir() / "events.tsv";
  write_file(p, "# uid=SPY\n2026-03-05\n\n2026-06-04\r\n");
  const Result<std::optional<EventSchedule>> sched = load_events_tsv(p.string());
  ASSERT_TRUE(sched.has_value()) << sched.error().to_string();
  ASSERT_TRUE(sched->has_value());
  ATX_TRY_ASSERT(const std::int64_t d1, iso_date_to_ns("2026-03-05"));
  // 2026-03-05 00:00 UTC == 1772668800 * 1e9 exactly (civil-days check).
  EXPECT_EQ(d1, 1772668800LL * 1000000000LL);
  EXPECT_EQ((*sched)->count_between(d1 - 1, d1), std::size_t{1}); // (now, expiry] includes expiry
  EXPECT_EQ((*sched)->count_between(d1, d1), std::size_t{0});     // event at now excluded
}

TEST_F(BevLabelFactoryGate, EventsTsvRejectsMalformedDate) {
  const fs::path p = tmp_dir() / "bad_events.tsv";
  write_file(p, "2026-13-40\n");
  const Result<std::optional<EventSchedule>> sched = load_events_tsv(p.string());
  ASSERT_FALSE(sched.has_value());
  EXPECT_EQ(sched.error().code(), ErrorCode::ParseError);
}

TEST_F(BevLabelFactoryGate, EventsPathEmptyMeansNoCalendar) {
  const Result<std::optional<EventSchedule>> sched = load_events_tsv("");
  ASSERT_TRUE(sched.has_value());
  EXPECT_FALSE(sched->has_value());
}
```

If the fixture lacks `tmp_dir()`/`write_file` helpers, mirror how the existing gate tests create their temp SurfaceDb/TSV files (the dividends test (g) already writes a temp TSV — copy that exact mechanism). If `ATX_TRY_ASSERT` does not exist in the test lexicon, use `ASSERT_TRUE(r.has_value())` + `*r`.

- [ ] **Step 2: Run to verify failure.**
  `& .\scripts\atx-build.ps1 build atx-vol-tests` → compile error (`load_events_tsv` undeclared). That is the expected failure mode for a NO_MAIN-include test.

- [ ] **Step 3: Implement** in the driver, following the `load_dividends_tsv` function's placement and style:
  - `iso_date_to_ns`: parse `YYYY-MM-DD` with `std::from_chars`, validate ranges (month 1–12, day 1–31 with civil-days round-trip check: convert back and compare), days-from-civil arithmetic (Hinnant algorithm, ~10 lines of integer math), multiply by `86400LL * 1000000000LL`. `Err(ErrorCode::ParseError, "load_events_tsv: bad date '<line>'")` on failure.
  - `load_events_tsv`: empty path → `Ok(std::nullopt)`; otherwise read lines, skip `#`/blank, collect ns, return `Ok(std::optional<EventSchedule>{EventSchedule{std::move(v)}})`. Zero-date file is valid (an underlier with no scheduled events).
  - `BevFactoryArgs::events` + `--events` in `parse_args` (optional — no required-field check) + `usage()` line + meta echo `{"events", args.events}`.
  - In `run_bev_label_factory`: load once before the entry loop; pass `const std::optional<EventSchedule>&` down to `collect_entry_date_jobs`; per entry date compute `const double n_events = sched ? static_cast<double>(count_events_at(*sched, entry_ts_ns, T)) : std::numeric_limits<double>::quiet_NaN();` and stamp it on every `PendingJob` for that date (same value across the candidate lattice — count depends only on entry instant and T).
  - `PendingJob` gains `double n_events{0.0};` (positional-init risk: `PendingJob` is built with designated initializers at its one site — keep it that way).

- [ ] **Step 4: Run to green.**
  `& .\scripts\atx-build.ps1 build atx-vol-tests` then `& .\scripts\atx-build.ps1 -Ctest -R 'BevLabelFactoryGate'`. The pre-existing (a)–(g) tests must still pass (byte-determinism gate included — `--events` absent in their args, so output is unchanged).

- [ ] **Step 5: Commit** — `feat(vol): events-calendar input for the BEV label factory`

---

### Task 2: Spot-history pre-pass + per-entry-date RV panel

**Files:**
- Modify: `atx-vol/examples/bev_label_factory.cpp`
- Test: `atx-vol/tests/bev_label_factory_gate_test.cpp` (append)

**Interfaces:**
- Consumes: `MarketSnapshot::load(path)` / `uid_of` / `find` / `SurfaceRef->pricing().S` (exact per-session resolve pattern: `src/breakeven.cpp:213-244`, `load_bev_path`'s loop); `OhlcBar`, `realized_vol_panel(span<const OhlcBar>, RvEstimator::CloseToClose, 252.0)`, `RvPanel` (windows `{5,21,63,252}`; `vol[1]`=21d, `vol[2]`=63d) from `realized_vol.hpp`.
- Produces (file-local, consumed by Task 3):
  - `[[nodiscard]] Result<std::vector<OhlcBar>> load_spot_history(const Clock &clock, std::string_view uid, std::size_t first_needed_idx, std::size_t last_needed_idx);` — one `MarketSnapshot::load` per session in `[first_needed_idx, last_needed_idx]` of `clock.refs()`, emitting `OhlcBar{ts, S, S, S, S}` per session (spot-mirror bars; CtC-only by construction).
  - `PendingJob::rv_21d`, `PendingJob::rv_63d` (`double`; NaN when the trailing history is too short — `realized_vol_panel`'s own per-slot NaN contract).

History window definition (banner text): the RV panel for entry date *e* is computed over the trailing spot-mirror bars **ending at and including** *e*'s session close — the information set available at entry (labels accrue hedge P&L from the next session on). Window depth: `kRvHistoryBars = 253` sessions (the 252-window needs 253 closes; longer history is truncated by `realized_vol_panel`'s trailing-slice logic anyway).

- [ ] **Step 1: Append failing tests:**

```cpp
// Task F-2: spot pre-pass. (k) bars come out ascending, one per session,
// close == the fixture's own spot for that date; (l) a 3-bar history yields
// NaN 21d/63d slots (realized_vol_panel per-slot contract) — asserted at the
// panel level here, at the TSV level in Task F-3's test.
TEST_F(BevLabelFactoryGate, SpotHistoryMirrorsSessionSpots) {
  // Fixture corpus already exists for tests (a)-(d); reuse its SurfaceDb.
  ATX_TRY_ASSERT(const SurfaceDb db, SurfaceDb::open(db_root()));
  ATX_TRY_ASSERT(const Clock clock, Clock::from_surface_db(db));
  const Result<std::vector<OhlcBar>> bars =
      load_spot_history(clock, "SPY", 0, clock.refs().size() - 1);
  ASSERT_TRUE(bars.has_value()) << bars.error().to_string();
  ASSERT_EQ(bars->size(), clock.refs().size());
  for (std::size_t i = 1; i < bars->size(); ++i) {
    EXPECT_LT((*bars)[i - 1].ts_ns, (*bars)[i].ts_ns);
  }
  for (const OhlcBar &b : *bars) { // spot-mirror invariant: O==H==L==C, all > 0
    EXPECT_GT(b.close, 0.0);
    EXPECT_EQ(b.open, b.close);
    EXPECT_EQ(b.high, b.close);
    EXPECT_EQ(b.low, b.close);
  }
  // Panel over the first 3 bars: 5d slot falls back to whole span (valid),
  // 21d/63d/252d slots fall back to the same 3-bar span too (window > size
  // falls back to whole span, >= 2 bars, so they are NUMBERS not NaN) --
  // assert the documented fallback, not an imagined NaN.
  const Result<RvPanel> p3 =
      realized_vol_panel(std::span{bars->data(), 3}, RvEstimator::CloseToClose, 252.0);
  ASSERT_TRUE(p3.has_value());
  EXPECT_TRUE(std::isfinite(p3->vol[1]));
  // 1-bar history: every slot NaN.
  const Result<RvPanel> p1 =
      realized_vol_panel(std::span{bars->data(), 1}, RvEstimator::CloseToClose, 252.0);
  ASSERT_TRUE(p1.has_value());
  EXPECT_TRUE(std::isnan(p1->vol[1]));
}
```

(Adjust helper names — `db_root()` etc. — to the fixture's real spellings; the (a)–(d) tests construct the corpus, read them first.)

- [ ] **Step 2: Run to verify failure** (compile error: `load_spot_history` undeclared).

- [ ] **Step 3: Implement:**
  - `load_spot_history`: loop `clock.refs()` over `[first_needed_idx, last_needed_idx]`; per ref `ATX_TRY(const MarketSnapshot session, MarketSnapshot::load(ref.archive_path))`, resolve uid exactly as `load_bev_path` does (`uid_of` → `find`; `Err(NotFound, ...)` naming uid and ts on a missing surface); `const double S = surf->pricing().S;` guard `std::isfinite(S) && S > 0.0` with an `Err(InvalidArgument, ...)` naming `ts_ns` (mirror the loader's spot-guard message style); push `OhlcBar{.ts_ns = session.ts_ns(), .open = S, .high = S, .low = S, .close = S}`.
  - In `run_bev_label_factory`, before the entry loop: locate the index of the first entry ref in `full_clock.refs()` (linear scan matching `entry_clock.refs().front().date`), `first_needed = idx > 253 ? idx - 253 : 0`, last entry ref likewise; call `load_spot_history` ONCE for `[first_needed, last_entry_idx]`. A load failure here is a **hard error** (`ATX_TRY`) — a corpus that can serve surfaces but not spots is broken, and silent NaN-everything labels would be worse.
  - Per entry date: find the bar whose `ts_ns == entry_ts_ns` (walk an index pointer forward — entry dates ascend), slice `[max(0, i+1-253), i+1)`, `realized_vol_panel(slice, RvEstimator::CloseToClose, 252.0)`; on `Err` (contract: only possible via OHLC validation, which spot-mirror bars cannot fail — treat as entry-date skip with counter) or success, stamp `rv_21d = panel.vol[1]`, `rv_63d = panel.vol[2]` onto that date's `PendingJob`s.
  - New counter: `n_entry_dates_rv_short` (entry dates whose 21d slot came out NaN) — added to `RunCounters`, the zero-labels error string, the stdout summary line, and the meta block.

- [ ] **Step 4: Run to green** (`BevLabelFactoryGate` suite; (a)–(d) byte-determinism must still hold — new columns don't exist yet, and the pre-pass adds no output).

- [ ] **Step 5: Commit** — `feat(vol): spot-history pre-pass and per-entry-date RV panel in the label factory`

---

### Task 3: Emit the 8-feature schema block in the label TSV

**Files:**
- Modify: `atx-vol/examples/bev_label_factory.cpp`, `atx-vol/include/atx/vol/theo.hpp` (ML-seam banner comment only)
- Test: `atx-vol/tests/bev_label_factory_gate_test.cpp` (append)

**Interfaces:**
- Consumes: Task 1's `PendingJob::n_events`, Task 2's `PendingJob::rv_21d/rv_63d`; `surf.forward_at(T)` (same accessor the serving overlay uses, `theo.cpp` `build_features`); the already-computed `actual_delta` and `sigma_entry_iv` in `collect_entry_date_jobs`.
- Produces: TSV header becomes (existing 14 columns, then the schema block in schema order):

```
entry_ts_ns  uid  strike  expiry_ns  side  sigma_be  sigma_entry_iv  log_ratio
premium  vega  n_days  iters  flag  snapped
log_moneyness  tenor_years  market_vol  rv_21d  rv_63d  iv_minus_rv
n_events_to_expiry  delta_abs
```

plus meta key `feature_schema=1`. `market_vol` duplicates `sigma_entry_iv` by construction (same `surf.iv(K, T)` read) — emitted anyway so the schema block is a contiguous, self-contained slice a trainer consumes without joins; the file banner says so.

- [ ] **Step 1: Append failing tests:**

```cpp
// Task F-3: (m) header carries the schema block, names and ORDER frozen to
// kFairVolFeatureSchemaV1; (n) per-row spot-check: log_moneyness/tenor/
// market_vol/delta_abs recomputed from the fixture surface match the emitted
// values; iv_minus_rv == market_vol - rv_21d exactly; (o) no --events =>
// n_events_to_expiry column is "nan"; with a one-event calendar between entry
// and expiry it is "1"; (p) determinism gate still holds with all new columns
// (covered by re-running (a)-(d) unchanged — no new test needed, but the new
// columns mean the OLD golden expectations in (a)-(d) must NOT be
// column-count-sensitive; fix them here if they are).
TEST_F(BevLabelFactoryGate, FeatureBlockHeaderAndValues) {
  static_assert(kFairVolFeatureCount == 8); // schema drift tripwire
  // run the driver once (reuse (a)'s args helper), read the file:
  ...
  const std::string expected_tail =
      "log_moneyness\ttenor_years\tmarket_vol\trv_21d\trv_63d\tiv_minus_rv\t"
      "n_events_to_expiry\tdelta_abs";
  EXPECT_NE(header_line.find(expected_tail), std::string::npos);
  // parse one converged row, recompute from the fixture surface at that
  // entry date: EXPECT_NEAR(log_moneyness, std::log(strike / F), 1e-12);
  // EXPECT_DOUBLE_EQ(iv_minus_rv, market_vol - rv_21d); EXPECT_EQ(market_vol,
  // sigma_entry_iv) (same read, bit-equal); delta_abs in [delta_lo, delta_hi].
  ...
}
```

(The elided bodies follow the existing (a)–(d) tests' run-and-parse mechanics — read them and mirror; the parse helper splitting on `\t` already exists there or is 5 lines.)

- [ ] **Step 2: Run to verify failure** (header assertion fails — old 14-column header).

- [ ] **Step 3: Implement:**
  - `collect_entry_date_jobs`: compute `const double forward = surf.forward_at(T);` ONCE per entry date (guard finite/positive; non-finite forward → per-date skip counter bump + all candidates skipped, matching the serving overlay's refusal); per candidate stamp `log_moneyness = std::log(*k / forward)` and `delta_abs = ad` (already in scope) onto `PendingJob`.
  - `PendingJob` gains `double log_moneyness{0.0}; double delta_abs{0.0};` (Task 1/2 already added `n_events`, `rv_21d`, `rv_63d`). `LabelRow` gains all eight schema values (`tenor_years` = the pillar `T`, threaded through; `market_vol` = `sigma_entry_iv`; `iv_minus_rv` computed at row build as `sigma_entry_iv - rv_21d` — NaN-propagating when rv is NaN, which is correct and documented).
  - `append_rows_tsv`: extend header string + eight `append_double` fields per row, schema order.
  - `theo.hpp` ML-seam banner (~lines 249-256): replace the "Task 6's TSV supplies the TARGET and join keys only; the feature block is assembled offline by the trainer" sentences with: the label factory emits the full schema-v1 feature block beside the target as of this sprint (`bev_label_factory --events <tsv>`); `n_events_to_expiry` is NaN when no calendar is supplied; `rv_21d/rv_63d` are close-to-close (spot-mirror bars) until real OHLC lands. Comment-only change — no code, no test impact beyond the gate.
  - Meta block: add `{"feature_schema", "1"}`.
- [ ] **Step 4: Run to green** — `& .\scripts\atx-build.ps1 -Ctest -R 'BevLabelFactoryGate'`, then the neighbor suites the driver's TU touches: `-R 'Breakeven|BevPathLoader|TheoEngineTest'` (theo.hpp comment edit → recompile check only).
- [ ] **Step 5: Run the driver once against the real SPY corpus if present** (`C:/atx-data/surface-db-r2/spy-2019`, args as in the sprint summary) and eyeball the new columns — informational, not a gate; skip silently if the corpus directory does not exist on this machine.
- [ ] **Step 6: Commit** — `feat(vol): emit fair-vol feature schema v1 in BEV label TSV`

---

### Task 4: Corpus batch runner (`bev_corpus_run.py`)

**Files:**
- Create: `atx-vol/scripts/bev_corpus_run.py`, `atx-vol/scripts/bev_corpus_run_test.py`, `atx-vol/scripts/README.md`

**Interfaces:**
- Consumes: the driver's CLI (`bev_label_factory --db ... --uid ... --entry-start ... --entry-end ... --tenor-days N --delta-lo X --delta-hi X --dividends TSV [--events TSV] --out FILE [--threads N]`) and its `# key=value` meta header.
- Produces: CLI `python bev_corpus_run.py --manifest run.json --exe <path-to-bev_label_factory> --out-dir <dir> [--dry-run]`. Manifest JSON schema:

```json
{
  "defaults": {"delta_lo": 0.05, "delta_hi": 0.95, "threads": 0},
  "tenor_days": [30, 60, 90, 180],
  "runs": [
    {"db": "C:/atx-data/surface-db-r2/spy-2019", "uid": "SPY",
     "entry_start": "2019-01-02", "entry_end": "2019-12-31",
     "dividends": "C:/atx-data/div/spy.tsv", "events": ""}
  ]
}
```

One driver invocation per (run × tenor); output file `<out-dir>/<uid>_<entry_start>_<tenor>d.tsv`; a `manifest_out.json` summarizing per-invocation exit codes and the parsed meta counters (`n_rows_written` etc.); nonzero exit if any invocation failed. `--dry-run` prints the command list without executing.

- [ ] **Step 1: Write failing tests** (`bev_corpus_run_test.py`): (a) dry-run over a 2-run × 2-tenor manifest prints 4 correctly-formed command lines (assert exact argv lists); (b) live run against a **stub executable** (a tiny Python script the test writes to tmp, registered via `--exe`, that validates it received `--out` and writes a minimal `# n_rows_written=3` + header TSV, exiting 0) produces 4 output files and a `manifest_out.json` with `n_rows_written: 3` per entry; (c) a stub that exits 1 for one combination → runner exit code nonzero, `manifest_out.json` records the failure, other combinations still ran. Use `subprocess`-friendly stub invocation (`sys.executable stub.py ...`) so the test is platform-clean on Windows.
- [ ] **Step 2: Run to verify failure** — `python -m pytest atx-vol/scripts/bev_corpus_run_test.py -q` (module not found / functions missing). If the repo has an established pytest entry point (check `pyproject.toml`/CI config for one — `atx-db` has its own), use that spelling; otherwise plain `python -m pytest` from repo root is the convention this sprint establishes.
- [ ] **Step 3: Implement** (~150 lines): manifest load + validation (missing keys → clear error naming the field), command construction (omit `--events` when empty — the driver treats absent and empty differently only in that absent is also valid; pass nothing for empty), sequential execution (`subprocess.run`, capture stdout/stderr into per-invocation `.log` files beside the TSVs), meta-header parse (`# key=value` lines until the first non-`#` line), `manifest_out.json` write, exit-code aggregation. No parallelism (the driver is already internally threaded via `--threads`).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): corpus batch runner for the BEV label factory`

---

### Task 5: Label QA report (`bev_label_qa.py`)

**Files:**
- Create: `atx-vol/scripts/bev_label_qa.py`, `atx-vol/scripts/bev_label_qa_test.py`

**Interfaces:**
- Consumes: label TSVs (22-column format from Task 3, `#` meta header).
- Produces: `python bev_label_qa.py <labels.tsv>... --out-md report.md` — one markdown report over the union of input files:
  1. **Row accounting:** rows per file, total; rows by `flag` value (0=Ok and each nonzero `BevFlag`), by `snapped`.
  2. **Target distribution:** `log_ratio` mean/stddev/P5/P50/P95, overall and bucketed by tenor band (`tenor_years` ≤0.12 / ≤0.30 / ≤0.60 / >0.60) × `delta_abs` band (<0.25 / 0.25–0.5 / ≥0.5).
  3. **Feature coverage:** per feature column, count and fraction of NaN rows (`n_events_to_expiry` NaN = no calendar; `rv_*` NaN = short history).
  4. **Duplicate check:** duplicate `(entry_ts_ns, uid, expiry_ns, strike, side)` keys across all inputs → listed, and a nonzero exit code (duplicates mean a manifest double-covered a range — a real corpus-assembly bug).
  5. **Leakage tripwire (report-only):** Pearson corr(`log_ratio`, `iv_minus_rv`) and corr(`log_ratio`, `market_vol`) — printed with the note that |corr| near 1.0 suggests target leakage into features; no assertion (the trainer's leakage audit owns judgment, roadmap §4 S3).
- [ ] **Step 1: Write failing tests:** build two small synthetic TSVs in tmp (write exact 22-column rows with hand-chosen values, including one NaN rv row, one nonzero-flag row, and one deliberate cross-file duplicate key): (a) report contains the expected per-flag counts and NaN fractions (assert exact numbers); (b) duplicate detection exits nonzero and names the key; (c) bucketed stats: a hand-computed mean for one (tenor, delta) bucket matches to 1e-12; (d) `nan` strings parse (Python `float("nan")` handles the driver's `%.17g` output).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** (~180 lines, stdlib `csv` with `delimiter='\t'`, meta lines skipped; Welford or two-pass for stddev; `math.isnan` guards everywhere — NaN rows excluded from moment stats, counted in coverage).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): label-corpus QA report script`

---

### Task 6: Sprint closeout

**Files:**
- Modify: `atx-vol/CHANGELOG.md` (follow the existing entry style — new dated `### NEW` section at a clean entry boundary; the 2026-08-08 sprint's entry documents the layout AND its splice-repair history — do not repeat that mistake: verify with `git diff` that no pre-existing line changes)
- Create: `atx-vol/sprints/2026-08-09-feature-factory-sprint-summary.md`
- Modify: `docs/research/2026-08-09-theo-ml-alpha-iteration-plan.md` — §1 gap table: G1 closed (with the CtC-only and day-resolution-events caveats), G6 tooling half closed; §5 acquisition table unchanged (real calendars/OHLC still pending)

- [ ] **Step 1: Hygiene + targeted gate.** PCH-off compile check for the touched C++ TUs (driver + gate test — note the 2026-08-08 hygiene lane did NOT cover these two, recorded in that sprint's review; this closes it), then `& .\scripts\atx-build.ps1 -Ctest -R 'BevLabelFactoryGate|Breakeven|BevPathLoader|RealizedVol|TheoEngineTest'` and `python -m pytest atx-vol/scripts/ -q`. Record counts.
- [ ] **Step 2: Sprint summary** — shipped surface (columns, CLI, scripts), measured driver runtime on the fixture corpus (and the real corpus if present), the two data caveats (CtC-only RV, midnight-UTC events), residual register: real OHLC bars, point-in-time earnings calendar sourcing, trainer (S3) next.
- [ ] **Step 3: Commit** — `docs(vol): feature-factory sprint summary + changelog`

---

## Execution order & dependency graph

```
Task 1 (events) ──┐
                  ├─→ Task 3 (feature columns) ──→ Task 6 (closeout)
Task 2 (rv panel) ┘         │
                            ├─→ Task 4 (batch runner) ──→ Task 6
                            └─→ Task 5 (QA report) ─────→ Task 6
```

Tasks 1 and 2 both modify the same driver file — execute **sequentially** (1 then 2; order chosen so Task 2's counter work lands on top of Task 1's smaller diff). Tasks 4 and 5 are independent of each other and of C++ internals (they consume the frozen TSV format) — parallelizable if the harness supports it, but sequential is fine.

## Self-review notes

- Spec coverage: roadmap S1 = Tasks 1–3 (features beside target, serving-consistent producers, schema doc sync); roadmap S2 = Tasks 4–5 (corpus fan-out + QA artifacts: censor counts, target distribution, dedup). S2's "run the actual multi-year panel" is an ops action once real corpora/calendars exist — the tooling is the deliverable, per roadmap §5's acquisition table.
- Type consistency: `PendingJob` fields added in Tasks 1/2 are consumed by Task 3's `LabelRow` build verbatim; `OhlcBar`/`RvPanel`/`EventSchedule` signatures copied from headers read at plan time (`realized_vol.hpp:31-87`, `event_vol.hpp:137-154,192-202`).
- Judgment calls an executor must respect: (i) the gate test's fixture helper names in Task 1/2 test code are best-effort — read tests (a)–(g) first and adapt spellings, not semantics; (ii) `iso_date_to_ns`'s epoch check value (1772668800) — re-derive before trusting (`python -c "import datetime;print(int(datetime.datetime(2026,3,5,tzinfo=datetime.timezone.utc).timestamp()))"`); (iii) if `forward_at` is not a `PricedSurface` member under that exact name, grep `theo.cpp`'s `build_features` for the accessor it actually calls and use that — serving-side consistency is the requirement, the spelling is not; (iv) the driver banner's "CARRY-ONLY: does not join an earnings calendar" paragraph is made stale by Task 1 — rewrite it as part of Task 1 Step 3.
