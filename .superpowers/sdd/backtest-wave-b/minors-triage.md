# Wave B — Minor findings triage (HEAD = 382fee2)

Source: `.superpowers/sdd/backtest-wave-b/progress.md`, every "Minor roll-up:" line
(T8 python-reader entry + T1–T9). Verified read-only against HEAD; no build, no test
run, no source edited.

**Totals: 31 rows — 7 STALE, 7 FIX-NOW, 16 DEFER (+1 duplicate row, T9-4 = T6-1).**

Legend for verdicts:
- **STALE** — a later task closed it, or it no longer applies at HEAD.
- **FIX-NOW** — small, safe, precisely describable; apply before branch close.
- **DEFER** — real, but needs a design decision / is blocked / cost > benefit.

---

## T8 — python reader hardening (12a6e4c)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T8-1 | Forged-input tests #10/#11 are regression locks, not RED-first (disclosed); branch-precise regexes | `atx-vol/python/tests/test_runarchive.py:371,381,391` | **STALE** — a TDD-methodology disclosure the T8 reviewer explicitly accepted ("reader already raised clean ValueError — probed"). No code artifact exists at HEAD to fix; the tests are correct and passing. |
| T8-2 | `#13` non-utf8 wrap scoped to `_string_table`; section-name and column-name decodes still raise a raw `UnicodeDecodeError` | `atx-vol/python/src/atxvol/report/runarchive.py:401` (section name), `:475` (column name); the guarded site is `:288-297` | **DEFER** — **the documented contract already holds**: `UnicodeDecodeError` derives from `UnicodeError` which derives from `ValueError`, so both unguarded sites already satisfy the module docstring's "raises `ValueError` on hostile input" (`runarchive.py:23`). Only the message is less contextual. Two `try/except` wraps (~8 lines) would buy message uniformity, nothing behavioural. Not worth reopening the python file at branch close. |
| T8-3 | Redundant `_recompute_header_crc` in the two version-gate tests (the version gate precedes the CRC gate) | `atx-vol/python/tests/test_runarchive.py:407,415` | **DEFER** — removing it would make both tests *depend* on the reader's gate ordering (version-before-CRC). Keeping the CRC fixed is the stronger, order-independent test. The "redundancy" is deliberate belt-and-braces; leave it. |

## T1 — pipeline module foundation (4d12d96)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T1-1 | `core_min_names_per_roll{40}` has no example literal — inert; T3 must not silently activate a `<40` gate | `atx-vol/src/listed_dispersion_pipeline.cpp:197-211` (`accept_listed_schedule`); lock at `atx-vol/tests/listed_dispersion_pipeline_test.cpp:455-464` | **STALE** — closed by **f9fb4e2 (T3)**. `accept_listed_schedule` consults only `rolls.empty()` and `core_mode && rolls.size() < core_min_rolls`; the test builds 3 rolls of **2 names each** and asserts `EXPECT_LT(r.n_names, method.core_min_names_per_roll)` then `EXPECT_TRUE(accept_...)`, which locks inertness positively. |
| T1-2 | Fingerprint sensitivity test omits all 11 `admission` sub-fields and `query_route` | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:226-244` (bumps only `min_names_entry`, `core_min_dates`, `core_min_rolls`, `core_min_names_per_roll`, `occ_ess_authority`) | **FIX-NOW** — `policy_fingerprint()` *does* fold both (`listed_dispersion_pipeline.cpp:82-92` for admission, `:97` for `query_route`), but nothing pins it. Add two `EXPECT_NE` blocks: one perturbing an `admission` field (e.g. `admission.min_quotes`), one flipping `query_route` to a non-`ColdReference` value. **~6 lines**, test-only. |
| T1-3 | "order-independent identity" comment is imprecise — the key is fixed-order; the real property is padding/layout independence | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:67` | **FIX-NOW** — the `.cpp` already states it correctly ("keeps the fingerprint padding-free (no struct-layout dependence)", `listed_dispersion_pipeline.cpp:58-60`); only the header is wrong. Reword `:67` to "Deterministic, nonzero, layout-independent (padding-free) identity over all policy fields, composed in a fixed field order." **1 line, comment-only.** |
| T1-4 | Fixed-name test temp dir is not parallel-safe | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:183-189` (`fresh_dir()` → `temp_directory_path()/"atx-listed-pipeline"`, `remove_all` then create) | **DEFER** — codebase-wide pattern, not a Wave B regression (same shape at `atx-vol/tests/run_archive_test.cpp:766`, `atx-vol/tests/run_report_test.cpp` `fresh_dir`). Fixing one site is cosmetic; fixing all of them is a separate hygiene sweep. gtest does not run these in parallel today. |

## T2 — M1 reconciliation timeline trim (bb0e744)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T2-1 | RED anchor asserts `ErrorCode` only; `InvalidArgument` has 3 sources in that function, so it does not nail M1 | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:334` | **FIX-NOW** — the low-level reconciler's three `InvalidArgument` exits are distinguishable by message; the M1 one is `"listed reconciliation: first snapshot must be first entry date"` (`atx-vol/src/listed_dispersion_reconciliation.cpp:240-242`). Add an `EXPECT_NE(aborted.error().to_string().find("first snapshot must be first entry date"), std::string::npos);` after `:334`. **~2 lines**, test-only, matches the message-assert pattern already used at `:438`. |
| T2-2 | `start == 0` wrapper path and a ≥2-session warm-up lead-in are proven transitively, not asserted | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:369-388` (only exercises `start == 1`) | **DEFER** — `assemble_reconciliation_snapshots` has one linear scan and one `subspan(start)` (`listed_dispersion_pipeline.cpp:171-185`); `start == 0` is the trivial identity case and a 2-session lead-in is the same branch iterated once more. Adding either case needs another surface/`SurfaceSet`/quote-vector triple (~15 lines of scaffolding) for zero new branch coverage. |
| T2-3 | Returned snapshots' `quotes` **borrow caller storage via `std::span`**; the borrow is undocumented on the public seam | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:109-111` (and `:117-120`); the span member is `atx-vol/include/atx/vol/listed_dispersion_reconciliation.hpp:82` | **FIX-NOW** — genuine public-API footgun: `assemble_reconciliation_snapshots` returns `std::vector<ListedReconciliationSnapshot>` **by value**, which reads as owning, but each element's `quotes` is a `std::span<const ListedOptionQuote>` and `surfaces` is a raw `const SurfaceSet*`, both still pointing into the caller's `full_timeline` storage. Add a 2-line note above `:109`: the returned vector owns only the element structs; `quotes`/`surfaces` remain borrowed from `full_timeline` and must outlive the returned value. **~2 lines, comment-only.** |
| T2-4 | Nit: linear first-roll scan | `atx-vol/src/listed_dispersion_pipeline.cpp:171-177` | **DEFER** — O(n) over ~135 sessions, run once per `run-backtest`. Cost of change > benefit. |

## T3 — schedule build extraction (f9fb4e2)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T3-1 | `PhaseTimer` not threaded into the builder — build-schedule diagnostics would lose per-phase granularity | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:172-185`; charges at `atx-vol/src/listed_dispersion_pipeline.cpp:233,240-241,251-252,256-260,263,272-273,300,310-311` | **STALE** — closed by **382fee2 (T9, obligation O4)**. The builder takes `PhaseTimer *timer = nullptr` and charges `selection` / `quote_join` exactly as the example measured them pre-lift; T9's reviewer verified "statement-for-statement timer parity". |
| T3-2 | Internal `hash_archive_file` duplicates the example's `hash_file` byte-for-byte | `atx-vol/src/listed_dispersion_pipeline.cpp:39-52`; removal documented at `atx-vol/examples/spy_dispersion_backtest.cpp:100-106` | **STALE** — closed by **382fee2 (T9)**. The example's `hash_file` is gone (only `hash_text`/`read_text` remain, which have other callers); the library helper is now the single implementation. |
| T3-3 | `ListedScheduleSpec` field-defaults mirror `RunSpec` on all 8 fields | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:129-138` | **DEFER** — inert duplication *by design*: the whole point of the POD is that the builder does not depend on the `RunSpec` layout (`:124-128`). Single-sourcing the defaults would re-introduce exactly the coupling the extraction removed. Would need a design decision to change. |

## T4 — cold projection + I1 parity (17b1477)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T4-1 | Report/header language overstates ("impossible by construction", "provably share ONE config") — at T4 it was a value-pin + kernel proxy | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:198-205` | **STALE** — the claim **became true** at **382fee2 (T9/O1)** and was gated at T10. `grep -rn "impossible by construction"` finds **no** hit in any Wave B source or header at HEAD (only unrelated files: `atx-vol/src/parity.cpp:180`, `atx-engine/...`, and the T4 review doc itself quoting it). The surviving header text says "recomputes its replay marks through the SAME constant (wired T9)", and T10 confirmed `projected_schedule` golden `d6793d46…` + `mark_divergence rows=0`. |
| T4-2 | Library function emits unconditional stdout/stderr diagnostics (impure) | `atx-vol/src/listed_dispersion_pipeline.cpp:457-460` (`std::printf` per roll), `:279-281`, `:296-297` (`std::fprintf(stderr)` on deferral) | **DEFER** — needs a design decision: introducing a diagnostics sink/callback is an API change across the whole `listed_dispersion_pipeline` seam, and the CLI currently *depends* on those lines being printed (documented at `atx-vol/examples/spy_dispersion_backtest.cpp:656`: "the per-roll stdout diagnostic line prints from within it"). Silencing them changes visible CLI output. Not a branch-close change. |
| T4-3 | (= obligation O2) `ListedArchiveLookup` missing-roll semantics | `atx-vol/src/listed_dispersion_pipeline.cpp:360-364` | **STALE** — closed by **382fee2 (T9/O2)**. Null `Ok` is guarded explicitly and returns the example's byte-identical message `"project-schedule: no qualified archive for roll date " + roll.roll_date`. Verified by T9's reviewer as byte-identical. |
| T4-4 | (= obligation O3) Lifetime: lookup returns a borrowed `MarketSnapshot*`, UAF if the CLI does not own snapshots across the whole call | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:189-196` (contract documented); CLI owner at `atx-vol/examples/spy_dispersion_backtest.cpp:630-646` | **STALE** — closed by **382fee2 (T9/O3)**. The CLI holds a `std::map` `snapshot_cache` whose node stability keeps every borrowed `MarketSnapshot` alive for the whole `project_listed_schedule` call; the header documents "the caller owns snapshot lifetime". |
| T4-5 | A `static_assert` canonical pin would beat the runtime `EXPECT` for `ProjectionConfig{}` | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:490-494` (`ProjectionConfigColdIsCanonical`, runtime `EXPECT` only) | **FIX-NOW** — `ProjectionConfig` is a literal aggregate (`bool` + scoped enum, `listed_dispersion_pipeline.hpp:206-209`), so `constexpr ProjectionConfig kCfg{}; static_assert(kCfg.analytic); static_assert(kCfg.execution == QueryExecution::ColdReference);` compiles. Add it above `:490`; the file already uses this exact pattern at `:204-205`. **~3 lines**, turns the I1 parity constant into a build-time gate. Keep the runtime test too. |

## T5 — dispersion_book_var (c2e5463)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T5-1 | Test pins structure only — no free economic invariants | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:650-655` | **FIX-NOW** — all three invariants are free (no new fixture, no new call). `ProjectedHistoricalVar` exposes `confidence / reference_value / value_at_risk / expected_shortfall / n_scenarios` (`atx-vol/include/atx/vol/historical_projection.hpp:47-53`), and `var->frames` is already in scope. Add: `EXPECT_GE(var->risks[1].value_at_risk, var->risks[0].value_at_risk)` (99% ≥ 95%), `EXPECT_GE(risk.expected_shortfall, risk.value_at_risk)` inside the existing `:653` loop, and `EXPECT_EQ(risk.reference_value, var->frames.back().value)` (pins the reference the library picks at `listed_dispersion_pipeline.cpp:521`). **~4 lines**, test-only. |
| T5-2 | The test's `kVegaVolPointToUnitVol` exercise is symbolic (`100.0 * kVegaVolPointToUnitVol`; the constant equals its own default, so it does not test the M9 swap) | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:609` | **DEFER** — true but unfixable without changing the constant's value. The value itself is already pinned twice (`static_assert` at `:204-205`, `EXPECT_EQ` at `:208`), and the real M9 swap sites live in the CLI (`spy_dispersion_backtest.cpp:966`, `:885`), pinned economically by the T10 gate (`final_nav=-456.5769067`). Nothing to add. |

## T6 — column single-source (b1cfd16)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T6-1 | `write_backtest_series_csv` is a 5th hand-kept copy of the 25 names (different order, CSV, unguarded); the header's "ONLY place" claim is overstated | copy: `atx-vol/src/run_report.cpp:78-104`; false claim: `atx-vol/include/atx/vol/backtest_series_columns.hpp:38-39` | **FIX-NOW (comment only) / DEFER (the dedup)** — see the dedicated analysis below. Narrow the header claim; do **not** touch the CSV this branch. |
| T6-2 | The parity test's 3 legs share the member pointer, so the member vertex is not an independent witness | `atx-vol/tests/run_archive_test.cpp:816-828` (`src = r.*col.member`, while TSV/archive are looked up by `col.name`) | **DEFER** — accurate, but real independence already exists elsewhere in the same file: the hand-written `backtest_dbl_cols()` map (`run_archive_test.cpp:639-668`, canonical order, no `backtest_series_columns()` dependency) drives `BacktestSectionRoundTripsValueExact` (`:690-702`), and `MatchesCommittedPythonFixture` compares against the committed fixture. Making the third vertex independent means re-copying the 25 names into the parity test — i.e. adding a copy to fix a copy. |
| T6-3 | Nit: fixture zero-valued columns evade value-swap detection | `atx-vol/tests/run_archive_test.cpp:620-634` (`zeros` reused for many columns) | **DEFER** — name/order is pinned independently and the T6 reviewer closed it via the verbatim-copy match. Changing the fixture values risks perturbing the committed python fixture sha256, which is an untouchable golden this wave. |

## T7 — determinism + fsync + count gate (9a24e78)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T7-1 | Determinism test covers fresh writes only; the merge path is untested (sound by construction) | `atx-vol/src/run_archive.cpp:1567-1589` (`created_ts_ns = static_cast<int64_t>(identity)`, then the merge branch at `:1589` reuses the same value) | **DEFER** — both branches derive `created_ts_ns` from the *same* `identity` local computed before the branch, so the merge path cannot diverge; T7's reviewer verified "merge recomputes". A merge-path determinism test needs a two-phase write fixture (~30 lines) plus a full build+run, which is exactly what is out of scope. The T10 gate already exercised merge-write in production sequence (union archive, 9 sections). |
| T7-2 | Inline fsync mechanism differs from 86f2210 (live-fd `_commit` vs reopen + `FlushFileBuffers`); dedup if `feat/pipeline-c` merges | `atx-vol/src/run_archive.cpp:463-474` (`ra_fsync_stream`), used at `:504` | **DEFER** — explicitly **blocked on an unmerged branch**. The shared primitive lives on `feat/pipeline-c`; deduplicating now would either fork the primitive again or create a merge conflict. Both mechanisms are correct (`_commit` on the live fd is the stronger form). |
| T7-3 | Parent-directory entry not fsync'd after the rename | `atx-vol/src/run_archive.cpp:513-536` (rename + bounded retry; no dir sync) | **DEFER** — needs a platform design decision: Windows has no portable directory-fsync equivalent, and the finding is explicitly "same scope as 86f2210", i.e. a pre-existing project-wide durability boundary, not a Wave B regression. Should be decided once, for both call sites, when `feat/pipeline-c` lands. |

## T9 — thin-CLI cutover (382fee2)

| # | Finding | Anchor at HEAD | Verdict |
|---|---------|----------------|---------|
| T9-1 | project-schedule's `archive_load` diagnostics phase now reads 0/0 (folded into `cold_solve`) | phase list at `atx-vol/examples/spy_dispersion_backtest.cpp:608`; documented at `:656-659` | **DEFER** — needs a small design decision: dropping `"archive_load"` from the phase list is a 1-line edit, but it changes the `diagnostics` **section row set**, and there is value in a stable phase vocabulary across CLI versions for run-over-run comparison. The fold is already disclosed in-code, non-golden (diagnostics carries wall-clock `wall_ms` by design), and economically inert. Decide it with the diagnostics schema, not at branch close. |
| T9-2 | run-projected-var error path no longer writes partial frame/leg TSVs before erroring; the route is unpinned | `atx-vol/examples/spy_dispersion_backtest.cpp:986-996` (`dispersion_book_var` returns at `:986`, the first `ofstream` opens at `:992`) | **DEFER** — the new behavior is **strictly better** (an incomplete projection now leaves no truncated `projected_risk_*.tsv` behind instead of leaving two partial files). Pinning it requires a fixture that forces `frame.n_failed != 0` inside `PreparedHistoricalProjection::evaluate_into`, which is non-trivial to construct. Healthy path is byte-faithful and gated at T10. |
| T9-3 | run-projected-var `elapsed_seconds` now spans prepare + evaluate (was evaluate-only) | `atx-vol/examples/spy_dispersion_backtest.cpp:985-990`; consumed at `:1036` and `:1043` | **DEFER** — pure telemetry: it only feeds the `projections_per_second` column, which is wall-clock and therefore already non-deterministic run-to-run. Documented in-code at `:979-981`. No economics touch it. |
| T9-4 | (duplicate of **T6-1**) `run_report.cpp:78` 5th column copy untouched | see T6-1 | **DUPLICATE** — carried forward unchanged from T6; single verdict under T6-1. |

---

## The T6/T9 carried finding: can `write_backtest_series_csv` be driven off `backtest_series_columns()`?

**Short answer: yes, but only through an explicit permutation — a naive direct iteration
would change the CSV's bytes. And even the permuted version would not make the header's
"ONLY place" claim true, because three *other* copies of the CSV-ordered name list exist.
Verdict: fix the header comment now; defer the code dedup.**

### Evidence

**1. The two orders differ by exactly one displaced element: `nav`.**

Canonical (`backtest_series_columns.hpp:40-66`), positions 0-based:

```
0  pnl_total   1  pnl_delta  2  pnl_gamma   3  pnl_vega   4  pnl_vanna
5  pnl_volga   6  pnl_theta  7  pnl_rho     8  pnl_charm  9  pnl_unexplained
10 pnl_settlement 11 pnl_shares 12 financing 13 cost      14 nav
15 cash        16 gross_delta 17 gross_gamma 18 gross_vega 19 gross_theta
20 turnover_notional 21 turnover_vega 22 n_open_lots 23 n_unpriced_lots 24 n_unpriced_greeks
```

CSV (`run_report.cpp:79-103`): identical **set** of 25 names, identical relative order,
with `nav` moved from index 14 to index **1**. Formally:
`csv_order == [pnl_total, nav] ++ (canonical_order \ {pnl_total, nav})`.

So a direct `for (const auto& col : backtest_series_columns())` in `run_report.cpp`
would emit `nav` as CSV field 16 instead of field 3 — **every data row and the header
line change bytes.**

**2. That byte change is a contract break, not a cosmetic one.** The CSV order is pinned
in three further places, all in the nav-hoisted order:

- `atx-vol/include/atx/vol/run_report.hpp:46-55` — the documented column list, under an
  explicit "the column order … below are a BINDING interface, not an implementation
  detail" (`run_report.hpp:5-7`).
- `atx-vol/tests/run_report_test.cpp:129-134` — `kPinnedHeader`, asserted verbatim at
  `:209-210`.
- `atx-vol/tests/mag7_dispersion_report_test.py:52-57` — `SERIES_HEADER`, the fixture the
  mag7 renderer test writes and reads.

The only production caller is `atx-vol/examples/mag7_dispersion_backtest.cpp:243-246`.
The renderer itself (`atx-vol/tools/mag7_dispersion_report.py:365`) reads by column
**name**, so it would survive a reorder — but the two pinned-header tests would not.

**3. Formatting is already identical, so formatting is not the obstacle.** Both writers
use `snprintf("%.17g")` for doubles and `snprintf("%lld")` for `ts_ns`
(`run_report.cpp:110-113,132` vs `tearsheet.cpp:200-203,221`); the only difference is the
separator (`,` vs `\t`) and the `# k=v` meta prelude. A permutation-driven CSV would be
byte-identical to today's output.

**4. Therefore the byte-preserving dedup is possible and is small** — a `constexpr`
function in `run_report.cpp` returning `std::array<BacktestSeriesColumn, 25>` built as
"canonical order, with `nav` hoisted to index 1", replacing 25 hand-kept `{name, &r.member}`
pairs with one local literal (`"nav"`) plus a hoist rule (~12 lines). It would also
inherit the freeze guard transitively: a registry drift becomes a compile error in
`run_archive.cpp` (`:642-657`) before it can reach the CSV.

### Why it is still DEFER

- `run_report.cpp` is **entirely outside Wave B's blast radius** —
  `git diff --stat 6e3af60..HEAD` lists 14 files and `src/run_report.cpp` is not among
  them. Editing it now widens the branch's diff into an untouched module.
- Deduping only the `.cpp` copy leaves **three** copies of the CSV-ordered list
  (hpp doc, C++ pinned test, python fixture). The header's "ONLY place" claim would
  *still* be false afterwards, so the fix does not actually close the finding it was
  raised against.
- Verifying byte-identity requires a build plus the mag7 C++ and python suites —
  explicitly out of scope for this triage, and the whole point of the deferral.
- The two lists serve different frozen contracts: canonical order is pinned to the
  RunArchive registry `kBacktestCols[2..26]` and its schema hash `0xdcce47781ac8390d`;
  CSV order is pinned to the mag7 renderer interface. Unifying them is a schema decision,
  not a cleanup.

### What to do now (the FIX-NOW carve-out)

Narrow the overstated claim at `backtest_series_columns.hpp:38-39`. Replace
"This is the ONLY place the production column list lives." with a statement scoped to
what is actually true — that it is the only source for the two **registry-frozen**
serializers (TSV writer + archive encoder), and that `write_backtest_series_csv`
(`src/run_report.cpp:78`) deliberately keeps a separate, `nav`-hoisted CSV order pinned
to the mag7 renderer contract and is **not** derived from this table.
**~3 lines, comment-only, zero build risk.**

---

## FIX-NOW list, in application order

Comment-only first (no behavior, no test dependency), then the four test additions —
which all land in **one file** and can be verified by a single build + one gtest filter
run (`ListedDispersionPipeline.*`).

| Order | Item | File:line | Size |
|-------|------|-----------|------|
| 1 | **T6-1** — narrow the "ONLY place" claim; name `run_report.cpp:78` as the separately-ordered CSV contract | `atx-vol/include/atx/vol/backtest_series_columns.hpp:38-39` | ~3 lines, comment |
| 2 | **T2-3** — document that returned snapshots' `quotes`/`surfaces` stay **borrowed** from `full_timeline` | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:109` | ~2 lines, comment |
| 3 | **T1-3** — reword "order-independent" → "layout-independent (padding-free), fixed field order" | `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp:67` | ~1 line, comment |
| 4 | **T4-5** — `static_assert` the I1 parity constant (`analytic`, `ColdReference`) at compile time | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:490` | ~3 lines |
| 5 | **T2-1** — assert the M1 message string on the RED anchor, not just `ErrorCode` | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:334` | ~2 lines |
| 6 | **T1-2** — fingerprint sensitivity for an `admission` sub-field and `query_route` | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:244` | ~6 lines |
| 7 | **T5-1** — VaR invariants: 99% ≥ 95%, ES ≥ VaR, `reference_value == frames.back().value` | `atx-vol/tests/listed_dispersion_pipeline_test.cpp:655` | ~4 lines |

Total: 3 files, ~6 comment lines + ~15 test lines. Items 1-3 need only a compile;
items 4-7 need one `ListedDispersionPipeline.*` run. No golden, no fixture, no schema,
no economics touched — the T10 gate results stand unchanged.
