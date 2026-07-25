# Wave B — final whole-branch review

Range reviewed: `git diff 6e3af60..382fee2` (14 files, +2326 / −400).
Reviewer read the full `review-final.diff` plus the HEAD state of every touched
file and the call sites/definitions they depend on. Read-only: nothing was
built, run, or edited.

---

## Verdicts

- **Spec compliance: ✅** — every Wave-B task landed as specified, and the
  extractions are faithful lifts. I traced each moved block against the code it
  replaced (build-schedule selection loop, cold projection, book VaR, the 25-column
  table, `hash_file`→`hash_archive_file`, the two `* 100.0` boundaries) and found
  **no economic delta**: same guards, same comparison directions, same defaults,
  same evaluation order, no off-by-one in the roll/DTE/date handling. See
  "Extraction fidelity" below for the specific things I checked and cleared.
- **Code quality: Request changes** — 3 Important findings. None of them is an
  economics bug on the current production path; all three are contracts that are
  claimed in the new public header but not actually delivered, or a resource
  regression the new seam forces on the caller.

**Findings: 0 Critical, 3 Important, 7 Minor.**

---

## Critical

None. I looked specifically for wrong economics, data loss/corruption, crashes,
and security defects and did not find any.

---

## Important

### I-1 — the M1 "warm-up lead-in" fix does not work end-to-end; the abort just moves downstream

- `atx-vol/src/listed_dispersion_pipeline.cpp:156-186` (`assemble_reconciliation_snapshots`)
- `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:92-121` (the contract text)
- `atx-vol/examples/spy_dispersion_backtest.cpp:561-569` (the production wiring)

**What is claimed.** The header states that feeding the reconciler the full
`clock.refs()` timeline plus trimming means "a warm-up session no longer aborts an
otherwise-valid corpus". The commit message and the CLI comment repeat it.

**What actually happens.** `assemble_reconciliation_snapshots` drops the leading
pre-roll sessions, so `reconcile_listed_dispersion` (which emits exactly one row
per snapshot — `atx-vol/src/listed_dispersion_reconciliation.cpp:251-332`) returns
`clock.size() − lead_in` rows. The very next statement in `run_backtest_command`
is:

```cpp
ATX_TRY_VOID(validate_listed_reconciliation_backtest(reconciliation, backtest));
```

and that function hard-requires equal cardinality
(`atx-vol/src/listed_dispersion_reconciliation.cpp:344-348`):

```cpp
if (!finite(absolute_tolerance) || absolute_tolerance < 0.0 ||
    reconciliation.rows.size() != backtest.size()) {
  return Err(ErrorCode::InvalidArgument,
             "listed reconciliation/backtest: invalid tolerance or row count");
}
```

`backtest.size()` is one row per clock ref (`record_every_n` defaults to 1), so
for any `lead_in > 0` the counts differ by exactly `lead_in`. Even if they
coincidentally matched, the per-row `rows[i].date != backtest.date[i]` check on
the next line would fire, because the reconciliation now starts at the first roll
date while the backtest starts at the warm-up date. `RunDir::verify()` carries the
same gate (`atx-vol/src/run_archive.cpp:1630-1634`).

**Failure scenario.** `run_spec.tsv` with `date_lo` one qualified session before
the first roll date (e.g. the corpus admits 2026-07-10 but the first roll defers to
2026-07-11 because coverage was short on day 0 — precisely the shape the new tests
construct). `build-schedule` succeeds. `run-backtest`:

- before this branch → `Err(InvalidArgument, "listed reconciliation: first snapshot must be first entry date")`
- after this branch → `Err(InvalidArgument, "listed reconciliation/backtest: invalid tolerance or row count")`

Net user-visible behaviour: still a hard abort, now with a message that points at
the wrong subsystem. The corpus that M1 was supposed to rescue is still rejected.

**Why the tests do not catch it.** `ReconcileListedSchedule_TrimsWarmupLeadIn`
(`atx-vol/tests/listed_dispersion_pipeline_test.cpp:340-421`) exercises the seam in
isolation and never calls `validate_listed_reconciliation_backtest`, so it is green
while the production path it models is still broken.

**Fix (pick one, both are small).**
(a) Make the trim symmetric: have `run_backtest_command` slice `backtest` to the
same date window before validating (or add a `first_date` argument to
`validate_listed_reconciliation_backtest` / align on dates rather than on index),
and relax the `RunDir::verify` count gate to `reconciliation.n_rows() <=
backtest.n_rows()` with a date-prefix check; **or**
(b) if aligned cardinality is a deliberate invariant, drop the trim and restore the
honest precondition — but then delete the "no longer aborts" claim from the header,
the CLI comment and the commit message, because it is false as written.
Either way, extend `ReconcileListedSchedule_TrimsWarmupLeadIn` to run
`validate_listed_reconciliation_backtest` over a matching `BacktestResult`, so the
test would have failed.

---

### I-2 — the `ListedArchiveLookup` borrow contract turns a bounded per-roll snapshot into O(n_rolls) resident boards

- `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:188-197` (the contract)
- `atx-vol/src/listed_dispersion_pipeline.cpp:352-358` (the borrow site)
- `atx-vol/examples/spy_dispersion_backtest.cpp:621-646` (the caller forced into an owning cache)

The pre-extraction loop loaded exactly one `MarketSnapshot` at a time and destroyed
it at the end of each roll iteration:

```cpp
for (const ListedScheduleRoll &roll : listed.rolls) {
  ATX_TRY(MarketSnapshot snapshot, MarketSnapshot::load(archive->second));   // local
  ...
}                                                                            // freed here
```

The new seam hands back a **borrowed** `const MarketSnapshot *` whose validity the
header requires "for the duration of the `project_listed_schedule` call". The only
way to satisfy that is the `std::map<std::string, MarketSnapshot> snapshot_cache`
the CLI now keeps, which retains **every** roll-date board until the whole
projection finishes. `MarketSnapshot` is a full heap deserialize
(`std::vector<PricedSurface> surfaces_` + provenance + prepared query tier —
`atx-vol/include/atx/vol/backtest.hpp:95-152`), not an mmap view, so peak RSS goes
from one board to `n_rolls` boards.

**Failure scenario.** The current production run (135 sessions → 7 rolls) is fine.
The stated next target is a multi-year OPRA corpus: ~monthly rolls over 10 years is
~120 boards of ~51 fully-reconstructed underlyings each, held simultaneously.
That is a linear memory blow-up on a path that previously had none, and it will
surface as an OOM / thrash on exactly the run this framework exists to do.

**Fix.** Scope the borrow to one roll instead of the whole call. Either change the
seam to a visitor —
`std::function<Status(std::string_view roll_date, const std::function<Status(const MarketSnapshot&)>&)>` —
or, simpler, have the lookup return an owning handle
(`Result<std::shared_ptr<const MarketSnapshot>>`, which is already the type
`SnapshotCache::load` produces) so `project_listed_schedule` drops each board after
its roll is priced. Both are drop-in for the current caller and restore O(1) peak.

---

### I-3 — `ListedDispersionMethodology` is a third copy of the thresholds, not the single authority it claims to be

- `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:47-70`
- `atx-vol/include/atx/vol/run_archive.hpp:563-569` (the competing copy)

The header states:

> One versioned methodology policy replacing the loose inline literals previously
> scattered across build-corpus / build-schedule / verify / run-projected-backtest.

Two of those four are not wired to it:

1. **verify.** `verify_command` (`atx-vol/examples/spy_dispersion_backtest.cpp:489`)
   calls `RunDir(run_dir).verify()` with default `RunVerifyOptions`, which carries
   its own `core_min_dates{60}`, `core_min_rolls{3}`, `core_min_names_per_roll{40}`
   (`atx-vol/include/atx/vol/run_archive.hpp:566-568`) and is consumed at
   `atx-vol/src/run_archive.cpp:1638-1649`. Nothing connects the two structs.
2. **run-projected-backtest.** The cold route reads `ProjectionConfig`
   (`spy_dispersion_backtest.cpp:803-805`), not `ListedDispersionMethodology`.

The consequence is a live duplicated-threshold hazard in the one struct whose
entire purpose was to remove duplication. Concretely: change
`ListedDispersionMethodology::core_min_rolls` from 3 to 4 and `build-schedule`
tightens while `verify` keeps accepting 3-roll archives — the two gates silently
disagree, and `policy_fingerprint()` (which is never called anywhere in production
— tests only) records the *new* value as if it governed both.

Relatedly, four of the struct's seven fields are dead: `admission`,
`core_min_names_per_roll`, `query_route`, `occ_ess_authority`. `query_route` is the
worst of them — it is a `QueryExecution::ColdReference` sitting next to
`ProjectionConfig::execution`, which is the actual cold-route authority. Setting
`method.query_route = QueryExecution::Configured` changes nothing; the projection
stays cold. That is exactly the "one of several copies" failure the seam was meant
to eliminate. (`core_min_names_per_roll` is at least *documented* as inert at
`listed_dispersion_pipeline.hpp:145-148`, which is honest.)

**Fix.** Either (a) wire it: give `RunVerifyOptions` a
`static_assert`/constructor from `ListedDispersionMethodology` (or make
`verify_command` build its `RunVerifyOptions` from the methodology) and delete
`query_route` in favour of `ProjectionConfig`; or (b) if the wiring is deliberately
out of scope this wave, correct the header comment to say which call sites it
currently governs (build-corpus entry floor + build-schedule date/roll floors, and
nothing else) and mark the remaining fields `// reserved, not yet consulted`.

---

## Minor

### m-1 — the T6 freeze guard pins names/order/dtype but nothing pins the member bindings, and the only independent oracle is blind on 23 of 25 columns

`atx-vol/tests/run_archive_test.cpp:603-635`, `atx-vol/src/run_archive.cpp:637-659`,
`atx-vol/include/atx/vol/backtest_series_columns.hpp:32-58`.

The `static_assert(backtest_series_matches_registry())` does exactly what its
comment claims: name, order and `F64` dtype against `kBacktestCols[2..26]`
(verified against `run_archive_schema.hpp:88-116` — 27 entries, `date`/`ts_ns`
first, 25 `F64`). It cannot, by construction, pin the `name → member-pointer`
binding. `{"nav", &BacktestResult::cash}` compiles and passes.

The new `BacktestSeriesColumnsTsvEncoderParity` test cannot catch that either — it
reads `r.*col.member` on both sides, so it is `f(x) == f(x)` with respect to the
binding. The only independent oracle is the pre-existing
`BacktestSectionRoundTripsValueExact`, which carries a hand-written name→member map
(`run_archive_test.cpp:639-668`) — but `make_encoder_fixture_result` sets 23 of the
25 columns to `{0.0, 0.0}`, so only a mis-binding involving `pnl_vega` or `nav`
would be detected. Not a regression (the old hand-kept arrays had the same blind
spot), and the current table is correct by inspection, but it is a one-line fix.

**Fix.** Give each column a distinct value in `make_encoder_fixture_result`
(e.g. `r.<col> = {i + 0.5, -(i + 0.5)}` for the i-th column). That converts the
existing oracle into a full 25-column mis-binding detector.

### m-2 — `BuildScheduleSymbolIsDeclared` cannot fail

`atx-vol/tests/listed_dispersion_pipeline_test.cpp:469-479`. Taking the address of a
named function and asserting it is non-null is a tautology; the test is really a
compile/link-time signature check, which the `using BuilderFn = ...` alias already
performs. Harmless, but it should not be counted as behavioural coverage of
`build_listed_dispersion_schedule` (whose economics are pinned only by the T10
fixture gate). Same shape at `:207-210` (`VegaVolPointConstantIs100` duplicates the
`static_assert` three lines above it).

### m-3 — `TwoRouteColdParity_LegMarksEqual` is self-parity, not two-route parity

`atx-vol/tests/listed_dispersion_pipeline_test.cpp:502-585`. "Route 2" calls the same
`make_listed_risk_lookup` that `project_listed_schedule` used internally
(`listed_dispersion_pipeline.cpp:376-377`), so the assertion reduces to
`f(x) == f(x)`. What it genuinely pins is worth having — that
`build_listed_dispersion_roll` stores the lookup's `model_mark` into
`leg.model_mark` unmodified, and that `residual_T` is reconstructible from the
projected roll — but it does not exercise the replay side.

I verified the I1 claim holds structurally by a different route:
`ListedDispersionStrategy::on_step` prices through
`surface->full_greek_seed(leg.strike, T, leg.side, price_options.analytic_greeks,
price_options.query_execution)` (`atx-vol/src/listed_dispersion_strategy.cpp:115-118`)
with an identical `residual_T` formula (`:22-34`), i.e. the same primitive with the
same two knobs — so one `ProjectionConfig` really does govern both. The genuine
end-to-end gate is the new Python e2e `test_projected_cold_union_and_zero_divergence`
(`mark_divergence rows=0`). Suggest renaming the C++ test to reflect what it pins,
so the parity claim rests on the test that actually establishes it.

### m-4 — project-schedule now emits a permanently-zero `archive_load` diagnostics row

`atx-vol/examples/spy_dispersion_backtest.cpp:608`. The `PhaseTimer` still declares
`{"setup_read", "archive_load", "cold_solve", "validate_write"}`, but the per-load
timing moved inside the library and nothing charges `archive_load` any more. Also
`validate_listed_dispersion_schedule` moved from `validate_write` into `cold_solve`.
Telemetry only — the diagnostics section schema is unchanged and the brief already
notes `wall_ms` is non-deterministic by design. Either drop `archive_load` from the
declared phase list or thread the timer through `project_listed_schedule` the way
T9/O4 did for the schedule builder.

### m-5 — the projected-VaR incompleteness gate moved ahead of the diagnostic TSV writes

`atx-vol/src/listed_dispersion_pipeline.cpp:504-512` vs
`atx-vol/examples/spy_dispersion_backtest.cpp:992-1025`. Previously the
`n_failed != 0` abort ran *after* `projected_risk_scenarios.tsv` /
`projected_risk_legs.tsv` were written, so a failed run left the failing frames on
disk for inspection. Now the library errors first and the CLI never opens those
files, which means (a) the diagnostic evidence for the failure is gone and (b) a
previous successful run's TSVs stay on disk next to a command that just failed,
looking current. Suggest truncating both files (or writing them) before returning
the error, or at minimum unlinking them on the failure path.

### m-6 — `assemble_reconciliation_snapshots`' returned vector borrows, and unlike its sibling seams that is not documented

`atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:109-111`. The returned
`std::vector<ListedReconciliationSnapshot>` elements hold `const SurfaceSet *` and
`std::span<const ListedOptionQuote>` into the caller's storage
(`listed_dispersion_reconciliation.hpp:78-83`). The copy itself is cheap and the
current caller is safe (`snapshot_owners` / `quote_owners` outlive the call), but
`make_listed_forward_lookup`, `make_listed_risk_lookup` and `ListedArchiveLookup`
all carry explicit "BORROWS ... must not outlive" notes and this one does not. Add
the same sentence.

### m-7 — the merge-write staleness guard can carry stale economics (pre-existing; outside this diff, but the brief asked)

`atx-vol/src/run_archive.cpp:1516-1565` + `1496-1514`. The guard compares
`header().run_identity_hash` against `RunDir::run_identity_hash()`, which folds
**only** `run_spec.tsv` and `universe_schedule.tsv` bytes. `surface_manifest.tsv`,
the `.atxvsa` surface archives and `trade_schedule.tsv` are not folded in, so
"the inputs are unchanged" (the comment's words) is weaker than it reads.

Scenario: rebuild the corpus and re-run `build-schedule` + `run-backtest` without
editing `run_spec.tsv` or the universe. Identity is unchanged, so the merge carries
forward the previous corpus's `projected_schedule` / `projected_cold` /
`mark_divergence` sections alongside the new `backtest` — one archive containing two
corpora's economics, and `RunDir::verify()` passes it (its only cross-section gate is
`backtest.n_rows() == reconciliation.n_rows()`, both of which are fresh).

This landed at `191e409` (Wave A), *before* the review base `6e3af60`, so it is not a
regression from this branch — but T7 does interact with it: making `created_ts_ns`
identity-derived removes the last wall-clock discriminator from the header, so a
mixed archive can no longer be dated either. Suggested fix when it is in scope: fold
`surface_manifest.tsv` (and ideally `trade_schedule.tsv`) into `run_identity_hash`,
or stamp a per-section producer identity so a carried section can be recognised as
belonging to a different corpus. Flagged Minor here rather than Important purely
because it is out of the reviewed range.

---

## Extraction fidelity — what I checked and cleared

These are the things the brief asked about that came back clean; recording them so
they are not re-checked.

**`hash_file` → `hash_archive_file`** (`listed_dispersion_pipeline.cpp:33-51`).
Byte-identical. The example's `read_text` opens with `std::ios::binary`
(`spy_dispersion_backtest.cpp:83-93`), and the new helper does too — no MSVC
text-mode CRLF/`0x1A` translation was introduced, so `surface_fingerprint` is
unchanged. The removal of the example's now-callerless duplicate is correct.

**build-schedule selection loop** (`listed_dispersion_pipeline.cpp:213-338` vs the
deleted `spy_dispersion_backtest.cpp` block). Line-for-line identical: the
`active_expiry != 0 && active_dte > roll_dte_days` DTE skip, the `authored`
requested-weight sum vs `selected->names` traded-weight sum, `coverage <
min_weight_coverage` with the `active_expiry == 0 ? continue : defer` split, the
`schedule.rolls.size() + 1u` cohort, the `spec.min_names` doing double duty as both
the `MissingNameSpec` floor and `selection_config.min_names`. All `if (timer)` guards
are pure additions on the telemetry path. `ListedScheduleSpec`'s defaults do in fact
mirror `RunSpec`'s (10 / 0.8 / 30 / 21 / 60 / 7 / 10000 / false — verified against
`dispersion_workflow.hpp:16-37`), and the CLI sets all eight fields explicitly, so no
default can leak in.

**Literal → policy substitutions.** `51u` → `min_names_entry{51}`
(`spy_dispersion_backtest.cpp:324`), `60u` → `core_min_dates{60}` (`:429`), `3u` →
`core_min_rolls{3}` (`listed_dispersion_pipeline.cpp:205-206`), `* 100.0` →
`kVegaVolPointToUnitVol == 100.0` at both boundaries (`:886`, `:966`). All
value-identical. `dispersion.multiplier = 100.0` correctly stayed a literal (it is a
contract multiplier, not a vega scale).

**Cold projection** (`listed_dispersion_pipeline.cpp:340-469`). Verbatim, including
the guard order (valuation-ts match → `residual_T > 0` → `legs.size() == 2*(1+n_names)
&& >= 2`), the `strike = forward_at(residual_T)` restrike with `K > 0` check, the
zero-spread synthetic quote (`bid = ask = seed.greeks().price`), the retained
`raw_symbol`/`instrument_id`/`source_fingerprint`, the `i = 2; i + 1 < legs.size(); i
+= 2` member pairing, and `build_cfg` sourced from the *frozen* roll. The one
ordering change — `validate_listed_dispersion_schedule` moved inside the call, ahead
of the file writes rather than after — is strictly safer. The `Ok(nullptr)` →
`Err(NotFound, "project-schedule: no qualified archive for roll date ...")` mapping
preserves the exact original message.

**I1 single-authority wiring.** `grep` over the example confirms **zero** remaining
hardcoded `ColdReference` / `analytic` literals in either cold path
(`spy_dispersion_backtest.cpp` hits at 650/652/757/794-805 are all comments plus the
one `ProjectionConfig` read). `config.price.analytic_greeks = cold_cfg.analytic` is
economically a no-op: `RunConfig::price` is
`PriceOptions{/*n_threads=*/0, /*analytic_greeks=*/true}` (`backtest.hpp:309`), so the
explicit set matches the prior default. The comment saying so is accurate.

**RunArchive fsync/rename** (`run_archive.cpp:481-536`). The rename is genuinely
unreachable unless write **and** sync **and** close all succeeded — `wrote`,
`synced` (short-circuited on `wrote`) and `closed` are all tested before the publish
block, and the temp is removed on that path. The retry loop is bounded (8 attempts,
5→320 ms, ~635 ms total) and preserves the temp on final failure, leaving the prior
good destination intact. `std::filesystem::rename` does replace an existing regular
file on Windows, so dropping the prior `remove(dst)` is correct and closes the
former "removed but not yet renamed" window.

**Deterministic `created_ts_ns`** (`run_archive.cpp:1567-1589`). `identity` is forced
nonzero (`:1513`), so the `created_ts_ns == 0 ⇒ system clock` fallback at `:426` is
unreachable and the bits round-trip intact through the `uint64` header field (the new
`WriteIsByteDeterministic` test asserts exactly that). This follows the already-reviewed
content-derived-stamp precedent for surface archives (`07950ec` / `66de2ae`).
`ArchiveContentIdentity` still discriminates content, because `header_crc32c` covers
`metadata_crc32c`, which covers the directory, which carries the per-section payload
CRCs — so the cache-key concern raised for surface archives does not recur here.

**Merge-write section set.** `carried` excludes every name in `incoming`, so the
merged vector can never trip `write_run_archive`'s duplicate-name check
(`run_archive.cpp:198-202`). New-wins on collision is implemented as documented, and a
per-section framing failure correctly aborts the merge and starts fresh
(`:1553-1561`).

**Python reader hardening** (`runarchive.py:284-296`, `539-568`). The `close()`
rework is correct: the mapping legitimately outlives the descriptor on both POSIX and
Windows (the `mmap` module dups the handle), so releasing `_fh` in a `finally` while
retaining `_mm` for live views is sound, and `close()` stays idempotent. Note that
`UnicodeDecodeError` is already a `ValueError` subclass — the new test correctly
guards against that with `assert not isinstance(ei.value, UnicodeDecodeError)`, so it
is not a vacuous assertion.

**Tests that do pin behaviour.** `PolicyFingerprintStableAndSensitive` (five
independent single-field perturbations), `BuildSchedule_RejectsEmptyAndSubThreshold`
(including the deliberate proof that no <40-names floor was activated),
`DispersionBookVar_SplitsConfidences`, `VerifyRejectsCountGateMismatch`,
`WriteIsByteDeterministic`, `BacktestSeriesColumnsMatchRegistryOrder`, the five new
forged-archive Python tests, and the full-pipeline
`test_projected_cold_union_and_zero_divergence` would all fail on the behaviour they
name. That is a genuinely good test set; the criticisms in m-1..m-3 are about three
specific tests, not the batch.
