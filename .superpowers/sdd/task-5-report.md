# Task 5 report: `populate_surface_db` — fit OPRA boards into SurfaceDb with per-symbol configs

## What was done

Implemented `atx::vol::populate_surface_db` and `write_populate_stats_csv`
per the pinned interface, plus the `mag7_surfdb_populate` CLI example. The
board→`PricedSurface` fit REUSES `build_corpus`'s exact per-board pipeline —
`FitSlot`/`fit_board` were extracted verbatim out of `corpus.cpp`'s anonymous
namespace into a new `src/`-private pair, `corpus_board_fit.{hpp,cpp}`, and
both `build_corpus` and `populate_surface_db` now call the same function.

### Recon finding that shaped the design (read this first)

The brief's implementation constraint says the per-symbol overlay is
`apply_symbol_config(cfg_for_symbol, inputs)` on "the board's SessionInputs."
I verified this is not just descriptive prose: `apply_symbol_config` sets
`band_k`, `deam.al_opts` (when `al_override`), `calendar_repair`, and (when
`pin_curve`) mirrors `curve.parametric` into `calib` — **none of which
`PricerConfig` (the type `fit_board`/`PricerFitter::fit` actually take) can
carry**. `PricerConfig` only round-trips `preset`, `curve` (when pinned), and
four `optional<bool>` knobs (`use_correction_cache`/`score_parity`/
`enforce_calendar_floor`/`use_deam_cache_for_fit`). A `PricerConfig`-only
translation would silently drop `band_k`/`al_override`/`calendar_repair`/the
calib mirror for any manifest config that sets them — a real correctness gap
the 6 brief-mandated tests wouldn't catch (they only exercise curve/pin_curve
and enabled), but a genuine violation of "apply_symbol_config reaches the
fit."

Fix: added one small, additive, backward-compatible parameter to
`PricerFitter::fit` — `const std::function<void(SessionInputs&)>&
session_overlay = {}` — invoked on the fitter's fully-resolved `SessionInputs`
immediately before the (first) `VolaSession::build` call. `fit_board` now
threads this straight through to `fitter.fit(*chain, session_overlay)` (one
line changed). `populate_surface_db` passes `[&resolved](SessionInputs& in) {
apply_symbol_config(resolved, in); }`. Every other `PricerFitter::fit(chain)`
call site in the codebase is untouched (default empty function ⇒ zero
behavior change; confirmed by the full `Corpus`/`PricerFitter`/`CurveFitParallel`
suites staying green).

One remaining subtlety: a symbol config's `pin_curve=true` must still be
immune to `PricerFitter::fit`'s auto-routed fallback ladder (the same
invariant `CorpusBoard::curve` already gets). The overlay hook alone doesn't
suppress the ladder (it only sees `PricerConfig::curve`), so
`populate_surface_db`'s `pricer_config_for_symbol` helper ALSO sets
`PricerConfig::curve` when `pin_curve` — mirroring `CorpusBoard::curve`'s
existing pin semantics — while the overlay hook covers the fields
`PricerConfig` cannot. Verified via `SurfaceDbPopulate.PinnedConfigHonored`
(a pinned `ConvexDense/node_cap 48` config reaches the served surface's
`kind_at(0)`) and reasoned through the `pinned_hft`/`auto_routed` logic in
`pricer_fitter.cpp` by hand.

### `PopulateSymbolStats.mean_oos_in_band` NaN rule

`fit_board`'s `FitSlot` gained one new field, `bool oos_in_band_available`,
set `true` only when `fitter.selection().has_value()` (the selector actually
ran and scored a chosen candidate). This is `false` both when a curve is
pinned AND when a board is auto-routed but not ambiguous enough to need
cross-validation (`PricerFitter::fit`'s `needs_cross_validation` gate) —
mirroring `CorpusEntry.oos_in_band`'s existing "0 if curve pinned" semantics
more precisely (it's really "0 if the selector never ran," pinning being one
cause). `populate_surface_db` sums `oos_in_band` only over slots with this
flag set and divides by that count; `mean_oos_in_band` is `NaN` when the
count is 0. Learned empirically: my first `StatsCsvShape` test draft assumed
an unpinned/auto-selected board always yields an OOS score — it doesn't
(direct-routed, non-ambiguous boards never invoke the selector) — so the test
was corrected rather than the implementation once I confirmed this matches
pre-existing `corpus.cpp` behavior (the `Corpus`/`CorpusAdmission` suites,
which exercise `oos_in_band` extensively, stayed green throughout).

## Tested

`atx-vol/tests/surface_db_populate_test.cpp`, 7 tests under `SurfaceDbPopulate`
(the 6 from the brief + 1 extra self-review gap-filler), fixture built with
the `make_board_spec`/`fit_board` synthetic-panel pattern from
`dispersion_test.cpp:76-129` adapted to emit `CorpusBoard`s (frame + env)
instead of a pre-fit `PricedSurface`; 2 symbols ("AAA","BBB") × 2 dates
("2026-03-02","2026-03-03"):

1. `FitsAndStoresPartitionsPerDate` — fresh db; `n_dates_written==2`,
   `db.partitions().size()==2`, both dates' surfaces load, `per_symbol` has
   both symbols with `n_ok==2`.
2. `HonorsDisabledSymbol` — `BBB` upserted `enabled=false`; both partitions
   contain only AAA (`open_partition(...)->map_symbol("BBB")` NotFound);
   `n_disabled==2` for BBB, `n_attempted==2` (attempted-but-skipped, not
   omitted from the denominator).
3. `SkipExistingResumes` — run twice; 2nd run `n_dates_skipped_existing==2`,
   `n_dates_written==0`, `db.generation()` unchanged (no upsert/write calls
   happen on a fully-skipped date).
4. `FailedFitRecordedNotFatal` — one board's frame zeroed; that (symbol,date)
   counts in `n_failed` (a `CorpusFitStatus::Skipped` empty-frame result maps
   to `n_failed` too — populate has no separate "skipped" bucket), the date's
   partition is still written with the other symbol.
5. `DateWithZeroSuccessfulFitsWritesNoPartition` (extra, added during
   self-review — the brief's 6 tests didn't cover this bullet) — both boards
   on one date corrupted; that date gets `NotFound` from `open_partition`
   and contributes 0 to `n_dates_written`; the other date is unaffected.
6. `StatsCsvShape` — exact header string; `AAA,2,2,0,0,1,nan` and
   `BBB,2,2,0,0,1,nan` exact rows (both pinned/direct-routed here, so both hit
   the NaN branch — see the recon-finding note above for why); `# run=test`
   meta line preserved.
7. `PinnedConfigHonored` — `AAA` pinned to `ConvexDense/node_cap 48`;
   `db.load_surface(date,"AAA")->kind_at(0) == ConvexDense`, proving the
   overlay reached the actual fit.

### TDD evidence

- **RED**: wrote `surface_db_populate_test.cpp` + wired both it and
  `src/surface_db_populate.cpp` (not yet created) into their respective
  `CMakeLists.txt` first. `cmake --build` failed at configure time
  (`_add_library`/`_add_executable`: source file does not exist) — confirms
  the test file and build wiring were in place before any implementation.
- **GREEN**: after implementing `corpus_board_fit.{hpp,cpp}`,
  `surface_db_populate.{hpp,cpp}`, the `PricerFitter::fit` overlay hook, and
  `examples/mag7_surfdb_populate.cpp`, `atx-vol-tests` built clean; first
  full run of the 6 brief tests caught the `StatsCsvShape` NaN-assumption bug
  above (`BBB,2,2,0,0,1,nan` unexpectedly matched `nan` too) — root-caused via
  the `pinned_hft`/`needs_cross_validation` logic in `pricer_fitter.cpp`,
  fixed the test's assumption, re-ran green. Added the 7th test after that,
  green immediately.
- `& .\scripts\atx-build.ps1 -Ctest -R "SurfaceDbPopulate|SurfaceDb|Corpus"`
  → **100% tests passed, 0 failed, 57 run** (2 pre-existing
  Disabled/Skipped: `SigmaInterpCorpus.BoardThroughput`,
  `CorpusGeneratedProperty.LongFixedSeedTenThousandBoardGate`).
- Regression check on the `PricerFitter::fit` signature change:
  `-R "PricerFitter|CurveFitParallel|Session\.|Dispersion\.|MultinamePipeline"`
  → **100% passed, 67/67**.
- `& .\scripts\atx-build.ps1 build mag7_surfdb_populate` → builds clean;
  smoke-ran it with no args → usage message + exit code 2 (no real OPRA hive
  available in this environment to exercise the full data path).

## Files

- Created: `atx-vol/include/atx/vol/surface_db_populate.hpp`
- Created: `atx-vol/src/surface_db_populate.cpp`
- Created: `atx-vol/tests/surface_db_populate_test.cpp`
- Created: `atx-vol/examples/mag7_surfdb_populate.cpp`
- Created: `atx-vol/src/corpus_board_fit.hpp` (private, `src/`-only — the
  extracted shared fit path: `FitSlot`, `fit_board`, `saturated_u32`)
- Created: `atx-vol/src/corpus_board_fit.cpp` (moved verbatim from
  `corpus.cpp`, plus `oos_in_band_available` + `session_overlay` threading)
- Modified: `atx-vol/src/corpus.cpp` — removed the now-shared
  `FitSlot`/`fit_board`/`collect_quality`/`count_two_sided_quotes`/
  `terminal_decision`/`saturated_u32` block (278 lines net removed), replaced
  with an `#include "corpus_board_fit.hpp"`; `archive_path_for` and
  everything else untouched. `admission_reason_mask` stays duplicated in both
  TUs deliberately (a 5-line pure constexpr, not "the fit block" — see Code
  Organization note in the brief).
- Modified: `atx-vol/include/atx/vol/pricer_fitter.hpp`,
  `atx-vol/src/pricer_fitter.cpp` — added the optional `session_overlay`
  parameter to `PricerFitter::fit` (see recon finding above).
- Modified: `atx-vol/CMakeLists.txt` — `src/corpus_board_fit.cpp` and
  `src/surface_db_populate.cpp` added to the `atx-vol` library;
  `mag7_surfdb_populate` registered in the `ATX_BUILD_EXAMPLES` block
  (comment names the gate test `SurfaceDbPopulate`).
- Modified: `atx-vol/tests/CMakeLists.txt` — `surface_db_populate_test.cpp`
  added.

Commit: `05bc3eb` — `feat(atx-vol): populate_surface_db - fit OPRA boards
into SurfaceDb with per-symbol configs`. Staged and committed only the
11 task-5-scoped files (file-scoped `git add`, not `-A` — the worktree
carries other agents' concurrent, unrelated changes).

## Self-review

- **enabled-skip**: `HonorsDisabledSymbol` — disabled boards are still
  counted in `n_attempted` (so `success_rate`'s
  `n_attempted - n_disabled` denominator is coherent) but never reach
  `fit_board`.
- **failure-recorded-not-fatal**: `FailedFitRecordedNotFatal` — a per-board
  failure never aborts the date; `n_failed` records it, the date's partition
  still gets written with the surviving symbols.
- **empty-date-no-partition**: `DateWithZeroSuccessfulFitsWritesNoPartition`
  (added specifically for this bullet — the brief's 6 named tests didn't
  cover it).
- **skip_existing resume**: `SkipExistingResumes` — a fully-skipped date
  never touches `db` at all (no `symbol_config`/`write_partition` calls),
  verified via unchanged `generation()`.
- **per-symbol stats incl. `mean_oos_in_band` NaN rule**: `StatsCsvShape`
  asserts the exact `nan` row text; the underlying `oos_in_band_available`
  flag is a faithful (in fact more precise) mirror of `fit_board`'s
  pre-existing OOS-availability semantics, not a new invention.
- **owning symbol storage**: `names`/`stamped` vectors are `reserve()`d to
  the date's board count before any `push_back`, so `SurfaceArchiveItem`'s
  non-owning `string_view`/`PricedSurface*` never dangle (the
  `surface_db_test.cpp:655-659` GOTCHA) — proven in practice since every
  test that loads a written surface back out (`load_surface`, `map_symbol`)
  passes.
- **Quality/YAGNI**: no speculative options; `write_populate_stats_csv` is a
  small independent twin of `run_report.cpp`'s `write_meta_body` shape (that
  helper is TU-private there, so it could not be reused directly — noted
  explicitly in a code comment rather than silently diverging).
- **Determinism**: within a date, boards fit in symbol-ascending order
  (input `order` sort is `(date, symbol, original index)`); parallel fan-out
  (when `n_threads>1`) writes disjoint `slots[k]`/`resolved_cfgs[k]` per
  worker, mirroring `corpus.cpp`'s own block-partition discipline — result
  is independent of thread count. `n_threads==0` means serial for THIS
  struct (documented as different from `CorpusConfig::n_threads`'s
  0-means-auto convention, per the brief's own doc-comment).
- **Build hygiene**: `/W4 /WX` clean throughout; no warnings suppressed.
- **build_corpus bit-identical guard**: full `Corpus`/`CorpusAdmission`/
  `CorpusBuildSession`/`CorpusQualityReport`/`QualifiedCorpus`/
  `CorpusGeneratedProperty`/`MultinamePipeline` suites green (35 tests) after
  the extraction — the only behavioral surface touched in `corpus.cpp` is a
  block deletion + one `#include`.

## Concerns

1. **`PricerFitter::fit`'s new parameter touches a widely-used core file.**
   I judged this in-scope and low-risk (additive, defaulted, backward-
   compatible — confirmed via a 67-test regression sweep covering
   `PricerFitter`/`CurveFitParallel`/`Session`/`Dispersion`/
   `MultinamePipeline`), but it is a step beyond "extract a function out of
   `corpus.cpp`" that the brief's Code Organization section names. Flagging
   per the task's "if the refactor looks riskier than the brief assumes,
   report" instruction — I did not stop, because the alternative (a
   `PricerConfig`-only translation) would silently drop `band_k`/
   `al_override`/`calendar_repair`/the pinned-calib mirror, which I judged a
   worse outcome than a well-scoped hook.
2. **CLI example is unexercised against real OPRA data** (none available in
   this sandbox) — verified only the argument-parsing/usage-error path and a
   clean build. The data-path (`load_opra_daterange` →
   `corpus_board_from_opra` → `populate_surface_db` → stats CSV) is exercised
   indirectly by the synthetic test suite (same `populate_surface_db`
   function) but not end-to-end through the CLI binary itself.
3. **`db.open_partition(date)` used for the `skip_existing` check** opens and
   CRC-validates the archive file when a date DOES already exist (not just a
   cheap manifest lookup) — deliberate (reuses `SurfaceDb`'s own
   canonicalization rather than reimplementing it), acceptable given resume
   is not a hot path, but worth knowing if a very-large-db resume run's
   startup cost ever matters.

## Fix report — overlay discrimination test

### The finding

Review flagged that no test in `SurfaceDbPopulate` discriminates "the
`session_overlay` hook reached `PricerFitter::fit`'s `SessionInputs`" from
"it would have looked the same anyway." `PinnedConfigHonored` pins
`ConvexDense`/`node_cap=48` and asserts `kind_at(0)==ConvexDense`, but the
synthetic dense boards auto-select `ConvexDense` regardless of any pin, and
`node_cap` isn't observable via `kind_at`. A silent regression that deletes
the overlay call in `surface_db_populate.cpp` would leave all 7 existing
`SurfaceDbPopulate` tests green.

### What the new test asserts

Added `SurfaceDbPopulate.SymbolConfigOverlayReachesFit`
(`atx-vol/tests/surface_db_populate_test.cpp`). It fits the SAME single
board ("AAA", `kDate0`) into three fresh `SurfaceDb`s:

- **db A**: symbol absent from the manifest → resolves to a `fallback`
  `SymbolFitConfig` with `preset=FitPreset::Fast`, `al_override=false`.
- **db B**: manifest entry with `preset=FitPreset::Fast` (the SAME preset as
  A — this pins `PricerConfig(A) == PricerConfig(B)` bit-for-bit, since
  `pricer_config_for_symbol` only round-trips `preset`/`curve`(when
  pinned)/four `optional<bool>` knobs, none of which differ between A and B)
  plus `al_override=true` and a distinctive `AlOpts{10, 20, 6, 1e-9}`.
- **db C**: the SAME manifest entry as db B, into a third fresh db (flake
  guard).

`al_override`/`al` is one of the fields `PricerConfig` categorically cannot
carry (confirmed by reading `PricerConfig`'s member list in
`pricer_fitter.hpp` — no `al`/`AlOpts` field exists), so the ONLY code path
that can ever move a manifest's `al_override` into
`SessionInputs::deam.al_opts` is `apply_symbol_config` running inside the
`session_overlay` lambda `populate_surface_db` passes to `fit_board`. Two
independent, non-tolerance-based oracles read back from
`db.load_surface(...)`:

1. **Structural**: `PricedSurface::pricing().al_opts` (stamped verbatim from
   the fit's resolved `SessionInputs::deam.al_opts` by
   `VolaSession::to_priced_surface`) — asserted bit-exactly `!=` between A
   and B on all four `AlOpts` fields, and bit-exactly `==` db B's manifest
   value (proves the VALUE reached the fit, not just "some field changed").
2. **Behavioral**: `fair_value(K=100, T=year_fraction(kDate0,"2026-04-17"),
   Side::Call)` at the same probe point on the same fitted board — asserted
   `!=` between A and B (a different Andersen-Lake discretization re-prices
   the identical point to a genuinely different American value).

The flake guard (db C vs db B) asserts both oracles are bit-identical when
the manifest config matches exactly, ruling out "A and B just happened to
differ this run" nondeterminism.

### RED evidence (overlay neutered, NOT committed)

Temporarily replaced the overlay-forwarding line in
`atx-vol/src/surface_db_populate.cpp`'s `fit_range` lambda:

```cpp
// before:
slots[k] = fit_board(board, pc, /*admission=*/nullptr, [&resolved](SessionInputs &in) {
  apply_symbol_config(resolved, in);
});
// after (temporary):
slots[k] = fit_board(board, pc, /*admission=*/nullptr, {}); // TEMP NEUTERED FOR RED EVIDENCE
```

Command: `ctest --test-dir C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\build --output-on-failure -R SurfaceDbPopulate`

Result: **88% tests passed, 1 tests failed out of 8** — every pre-existing
test (including `PinnedConfigHonored`) stayed green; only the new test
failed, exactly as predicted:

```
atx-vol\tests\surface_db_populate_test.cpp(380): error: Expected: (al_a.n_collocation) != (al_b.n_collocation), actual: 7 vs 7
atx-vol\tests\surface_db_populate_test.cpp(381): error: Expected: (al_a.n_quadrature) != (al_b.n_quadrature), actual: 16 vs 16
atx-vol\tests\surface_db_populate_test.cpp(382): error: Expected: (al_a.max_newton_iter) != (al_b.max_newton_iter), actual: 4 vs 4
atx-vol\tests\surface_db_populate_test.cpp(383): error: Expected: (al_a.tol) != (al_b.tol), actual: 1e-08 vs 1e-08
atx-vol\tests\surface_db_populate_test.cpp(386): error: Expected equality of these values:
  al_b.n_collocation
    Which is: 7
  distinctive_al.n_collocation
    Which is: 10
[... same pattern for n_quadrature/max_newton_iter/tol ...]
atx-vol\tests\surface_db_populate_test.cpp(398): error: Expected: (*fv_a) != (*fv_b), actual: 4.1809927518229264 vs 4.1809927518229264
[  FAILED  ] SurfaceDbPopulate.SymbolConfigOverlayReachesFit (983 ms)
...
88% tests passed, 1 tests failed out of 8
```

With the overlay neutered, db A and db B both fall back to
`FitPreset::Fast`'s own baseline `AlOpts{7,16,4,1e-8}` (identical), and the
probe `fair_value` is bit-identical (`4.1809927518229264` both) — proof the
manifest's `al_override` never reached the fit.

### GREEN evidence (overlay restored)

Reverted the neutering (`git diff --stat atx-vol/src/surface_db_populate.cpp`
shows no diff against the committed library file — confirms this fix is
test-only). Rebuilt and reran:

Command: `ctest --test-dir C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\build --output-on-failure -R SurfaceDbPopulate`

Result: **100% tests passed, 0 tests failed out of 8**:

```
1/8 Test #935: SurfaceDbPopulate.FitsAndStoresPartitionsPerDate ................   Passed
2/8 Test #936: SurfaceDbPopulate.HonorsDisabledSymbol ..........................   Passed
3/8 Test #937: SurfaceDbPopulate.SkipExistingResumes ...........................   Passed
4/8 Test #938: SurfaceDbPopulate.FailedFitRecordedNotFatal .....................   Passed
5/8 Test #939: SurfaceDbPopulate.DateWithZeroSuccessfulFitsWritesNoPartition ...   Passed
6/8 Test #940: SurfaceDbPopulate.StatsCsvShape .................................   Passed
7/8 Test #941: SurfaceDbPopulate.SymbolConfigOverlayReachesFit .................   Passed
8/8 Test #942: SurfaceDbPopulate.PinnedConfigHonored ...........................   Passed
100% tests passed, 0 tests failed out of 8
```

### Files changed

- Modified (test-only): `atx-vol/tests/surface_db_populate_test.cpp` — added
  `#include "atx/vol/american.hpp"` (for `AlOpts`) and the new
  `SurfaceDbPopulate.SymbolConfigOverlayReachesFit` test (~115 lines).
- No library code changed — `atx-vol/src/surface_db_populate.cpp`'s overlay
  wiring was confirmed correct (not broken), so the neutering used for RED
  evidence was reverted, not committed.
