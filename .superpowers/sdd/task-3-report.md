# Task 3 Report: `make_dispersion_strangle_spec` — dispersion-strangle strategy spec builder

## Status: DONE

## What I implemented

A pure config -> `StrategySpec` assembly function, `make_dispersion_strangle_spec`, in a new
`atx::vol` module (`dispersion_strangle.hpp`/`.cpp`). No pricing or snapshot access happens
here — it wires up the existing declarative DSL (`strategy.hpp`) exactly per the brief's
doc-comment contract, and resolution/pricing stays `resolve_spec_with_policy`'s job.

### `DispersionStrangleConfig` (header, verbatim from the brief)
`names`, `index_symbol{"SPY"}`, `target_abs_delta{0.40}`, `tenor_days{90.0}`,
`close_dte_days{10.0}`, `entry_every_n_days{1}`, `theta_per_name_daily{10.0}`,
`index_base_vega{10000.0}`, `missing{DropRenormalize, 4}`, `hedge{}`.

### `make_dispersion_strangle_spec` (src)
1. **Validation** (`InvalidArgument`, checked in this order): `names` empty; `index_symbol`
   empty; `index_symbol` present in `names` (`std::find`); `target_abs_delta` outside `(0,1)`
   (also rejects non-finite); `tenor_days <= close_dte_days`; `close_dte_days < 0` (also
   rejects NaN); `theta_per_name_daily <= 0`; `index_base_vega <= 0`; `entry_every_n_days ==
   0`; `missing.min_names > names.size()`.
2. **Assembly**:
   - One `LegSpec` per name (`make_name_leg` helper): `Strangle{Delta target_abs_delta call,
     Delta target_abs_delta put}`, `tenor.target_T = tenor_days/365.25`,
     `SizeSpec{TargetTheta, theta_per_name_daily, +1.0}`, `group = "basket"`.
   - One index `LegSpec` (`make_index_leg` helper): same structure/tenor,
     `SizeSpec{TargetVega, index_base_vega, -1.0}`, `group = "index"`.
   - `constraint = FlatVega{group_a="basket", group_b="index"}`.
   - `lifecycle.entry = EveryStep` iff `entry_every_n_days == 1`, else `EveryNDays` with
     `entry_every_n = entry_every_n_days`; `lifecycle.holding = CloseAtHorizon`;
     `lifecycle.roll_at_T = close_dte_days/365.25`.
   - `spec.missing = cfg.missing`, `spec.hedge = cfg.hedge`, `spec.name =
     "mag7_dispersion_strangle"` (size-agnostic literal, matches the fixture's 3-name test).

## Tests + results

New `atx-vol/tests/dispersion_strangle_test.cpp`, 3 tests (all from the brief, verbatim where
specified, plus extra validation-rule coverage):

1. **`DispersionStrangle.SpecShape`** — leg count/group/size-kind/sign/structure/tenor,
   index leg symbol/group/sign, constraint kind/groups, lifecycle holding/roll_at_T/entry —
   copied verbatim from the brief.
2. **`DispersionStrangle.RejectsBadConfig`** — the brief's 7 mutations verbatim, PLUS 4 extra
   mutations to cover every validation rule the doc-comment lists but the brief's literal test
   didn't exercise: `index_symbol = ""`, `target_abs_delta = 0.0` (lower boundary),
   `close_dte_days = -1.0`, `index_base_vega = 0.0`.
3. **`DispersionStrangle.EntryMath_EqualTheta_VegaFlat_FortyDelta`** — copied verbatim from the
   brief: resolves the spec against a real 4-surface `MarketSnapshot` via
   `resolve_spec_with_policy`, asserts every resolved leg reprices to `|delta| ~ 0.40` (call
   K>F, put K<F), each basket name's `|Σ qty*theta*mult|` equals `10*365.25` within `1e-6`
   relative, net cohort vega `<= 1e-9 * gross_vega`, and every index leg has negative `qty`.

Fixture: `load_fixture_snapshot()` builds one archive with 4 `make_surface`-built analytic
eSSVI surfaces (`AAA` uid 1 bump 0.00 spot 100, `BBB` uid 2 bump 0.06 spot 150, `CCC` uid 3 bump
0.12 spot 200, `SPX` uid 9 bump 0.02 spot 500 — exactly the brief's fixture spec) and
`MarketSnapshot::load`s it. `make_surface`/`write_archive` are copied from
`strategy_test.cpp`'s existing pattern (self-contained in this file's anonymous namespace, per
the brief's guidance to "adapt fixture helper names to real ones").

**Result**: `100% tests passed, 0 tests failed out of 47` for `-R
"DispersionStrangle|Strategy|Dispersion"` (3 new + 44 pre-existing `Strategy.*`/`Dispersion.*`/
`ListedDispersion*.*` all green, unmodified).

## TDD Evidence

**RED** — wrote the test file + both CMakeLists edits + the header (pure interface, no logic,
given verbatim in the brief) + an empty stub `.cpp` (declares the namespace, defines nothing),
then:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
```
```
lld-link: error: undefined symbol: class tl::expected<struct atx::vol::StrategySpec, class atx::core::Error> __cdecl atx::vol::make_dispersion_strangle_spec(struct atx::vol::DispersionStrangleConfig const &)
>>> referenced by \atx-vol\tests\dispersion_strangle_test.cpp:125
...
>>> referenced 12 more times
ninja: build stopped: subcommand failed.
```
The test TU compiled cleanly against the header (proving the interface/fixture wiring is
correct) and failed to LINK for exactly the missing-implementation reason — all 3 tests
reference the not-yet-defined function.

**GREEN** — implemented the function body, then:
```
& .\scripts\atx-build.ps1 build atx-vol-tests
```
Clean build, 0 warnings (repo builds `/WX`).
```
& .\scripts\atx-build.ps1 -Ctest -R "DispersionStrangle|Strategy|Dispersion"
```
```
100% tests passed, 0 tests failed out of 47
```

## Files changed

- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\include\atx\vol\dispersion_strangle.hpp`
  (new, 55 lines) — `DispersionStrangleConfig`, `make_dispersion_strangle_spec` declaration,
  doc-comments copied verbatim from the brief.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\src\dispersion_strangle.cpp`
  (new, 125 lines) — validation + assembly, `make_name_leg`/`make_index_leg` helpers.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\tests\dispersion_strangle_test.cpp`
  (new, 225 lines) — 3 `TEST(DispersionStrangle, ...)` cases + fixture helpers.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\CMakeLists.txt` (+1) —
  `src/dispersion_strangle.cpp` added to `add_library(atx-vol ...)`, next to `src/dispersion.cpp`.
- `C:\atx\.claude\worktrees\feat-atx-vol-mag7-dispersion\atx-vol\tests\CMakeLists.txt` (+1) —
  `dispersion_strangle_test.cpp` added to `add_executable(atx-vol-tests ...)`.

Commit: `922a6ad feat(atx-vol): dispersion-strangle strategy spec builder (equal-theta basket vs
vega-flat index)`.

## Self-review

- **Completeness against the doc-comment**: every bullet in the "Validated assembly into the
  declarative DSL" list is implemented 1:1 (leg shape, index leg shape, constraint, lifecycle,
  `missing`/`hedge` passthrough, `spec.name`). Every clause in the `InvalidArgument` list is a
  distinct `if` in validation order; all 9 rules are individually tested (7 from the brief's
  literal test + 4 I added to close the gap between the doc-comment's full rule list and the
  brief's abbreviated `RejectsBadConfig` test body).
- **Exact defaults**: header field defaults transcribed verbatim from the brief
  (`target_abs_delta{0.40}`, `tenor_days{90.0}`, `close_dte_days{10.0}`,
  `entry_every_n_days{1}`, `theta_per_name_daily{10.0}`, `index_base_vega{10000.0}`,
  `missing{DropRenormalize, 4}`).
- **Group/sign/constraint wiring**: verified against `strategy.cpp`'s
  `resolve_spec_impl`/`group_gross_vega` — `FlatVega{group_a="basket", group_b="index"}` scales
  `group_b` (index) qty so `gross_vega(index) == gross_vega(basket)`; opposite base signs
  (`+1` basket, `-1` index) net the cohort vega to ~0 in floating point exactly (single scale
  factor, `EXPECT_LE(|net_vega|, 1e-9*gross_vega)` passes). `ResolvedLeg::group` is populated by
  `expand_leg` from `LegSpec::group`, so the "group by uid" fallback the brief mentioned wasn't
  needed.
- **YAGNI**: no extra fields, no extra validation beyond the 9 documented rules, no
  speculative generalization (e.g., didn't parametrize the two hardcoded group-tag strings
  `"basket"`/`"index"` — the DSL's `CrossLegConstraint` already carries them and nothing in the
  brief asks for configurability there). `make_name_leg`/`make_index_leg` factored out only
  because both legs share 4 of 5 fields — kept private (anonymous namespace).
- **Quality**: matches the file's own established style (`dispersion.cpp`'s
  `Err`/`ErrorCode`/`Ok` imports, `[[nodiscard]]`, doc-comment conventions). Build is `/WX`
  clean for both the library TU and the full test executable link.

## Concerns

None. No blockers, no ambiguity in the brief required a guess — the interface, validation
list, and test code were all given exactly, and `resolve_spec_with_policy` (Task 2) was already
in place and matched the assumptions the acceptance-math test needed (group-based FlatVega
scaling over survivors, `ResolvedLeg.group` populated).
