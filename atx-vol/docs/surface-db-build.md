# `atx-vol-surface-db-build` — the production surface-database build CLI

Point it at an OPRA hive-v2 tree and a `SurfaceDb` root; it create-or-opens the
db, loads the date window, auto-generates the per-symbol fit configs, and
cell-aware-streaming-populates every `(symbol, date)` vol surface. One call over
`build_surface_db` (`atx/vol/surface_db_build.hpp`) wrapped in a hand-rolled arg
loop (`tools/surface_db_build_main.cpp`).

It is **fully resumable at every stage**: re-running over an unchanged hive fits
**zero** surfaces and spends nothing. See "Resume semantics" below.

Once a database is built, its companion tool **`atx-vol-surface-db`** inspects
and verifies it entirely from the command line — see
[Managing and verifying a built database](#atx-vol-surface-db--managing-and-verifying-a-built-database).
**No Python is involved in verification.**

## OPRA hive v2 — the on-disk layout it reads

```
<hive-root>/date=YYYY-MM-DD/data.parquet     # ALL symbols for that session
```

- True hive partitioning (`date=` key), so DuckDB / `pyarrow.dataset` also read
  the tree natively with partition-column inference.
- **Exactly one parquet file per session**, holding every underlying's rows. The
  date file is written atomically (tmp + rename) and is the **unit of resume**.
- Schema: 8 columns, unchanged from the corpus format —
  `ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz`.
  Prices are `int64` 1e-9 fixed-point; an unset side is `INT64_MIN`. The `date`
  partition value lives in the directory name, not as an in-file column.
- Rows are sorted by `underlying`, then `symbol`, so `underlying` column
  statistics support predicate pushdown for future selective readers. The C++
  loader does **one materialized read per date file** and splits by `underlying`
  in memory (one IO pass per date regardless of universe size); per-symbol
  row-group pruning is a documented future optimization, not built now.
- Snapshot minute is the fixed **19:55:00Z** hive convention (uniform with the
  existing corpus; the DST rationale is documented in the pull tool).

This replaces the old per-symbol tree `<root>/<symbol>/<date>.parquet` (one file
per `(symbol, date)`). To convert an existing per-symbol tree, see
[Sibling tools](#sibling-tools).

## Build

The tool target is **gated behind the cmake cache flag `ATX_BUILD_EXAMPLES`,
which is OFF by default** — the plain `configure` verb
(`pwsh scripts/atx-build.ps1 configure`, i.e. `cmake --preset dev`) builds the
library and tests but **omits** this CLI. Enable the flag explicitly at configure
time, then build the target:

```bash
# Configure with the tool enabled. The `configure` verb does not forward extra
# -D flags, so pass the flag through the wrapper's raw cmake path:
pwsh scripts/atx-build.ps1 --preset dev -DATX_BUILD_EXAMPLES=ON
# (equivalently, straight cmake in the MSVC dev env: cmake --preset dev -DATX_BUILD_EXAMPLES=ON)

# Build just the CLI:
pwsh scripts/atx-build.ps1 build atx-vol-surface-db-build
# -> build/bin/atx-vol-surface-db-build(.exe)
```

The exact cache flag is **`-DATX_BUILD_EXAMPLES=ON`**. It only ADDS the
example/tool targets (it does not change the library or tests). If you forget it,
the build fails with `ninja: error: unknown target 'atx-vol-surface-db-build'` —
that means the current build dir was configured without the flag; re-run the
configure line above.

## Usage

```
atx-vol-surface-db-build --db <root> --hive <root>
    --from YYYY-MM-DD --to YYYY-MM-DD
    [--symbols A,B,C] [--index SPY] [--preset populate] [--r 0.045]
    [--deep-selection] [--retry-disabled] [--pin-curve-family true|false]
    [--fit-workers N] [--report out.csv] [--max-failures N]
```

| Flag | Required | Meaning |
| --- | --- | --- |
| `--db <root>` | yes | `SurfaceDb` root. Created if absent, else **opened/resumed**. |
| `--hive <root>` | yes | OPRA hive-v2 root holding `date=<YYYY-MM-DD>/data.parquet`. |
| `--from` / `--to` | yes | Inclusive date window (every calendar date in range is enumerated). |
| `--symbols A,B,C` | no | CSV universe. **Omit (or empty) to discover** every underlying present in the window. Surrounding whitespace per field is trimmed. |
| `--index SPY` | no | Designated index leg — its config records the dense index recipe (bypassing per-board selection) instead of a classified family, for both config generation and the populate. **Under the default `--pin-curve-family false` that recipe never reaches the fit**: the stored `curve` is only consumed when `pin_curve` is set (`pricer_config_for_symbol`, and `apply_symbol_config`'s curve/calib assignment), so by default `--index` is a config-time annotation and the index leg auto-routes like every other symbol. It becomes load-bearing again with `--pin-curve-family true`. |
| `--preset NAME` | no | `fast` \| `accurate` \| `robust` \| `hft` \| `populate`. Default `populate`. Drives both the manifest seeding and the populate fit tier. |
| `--r RATE` | no | Flat continuously-compounded carry rate (`OpraHiveSpec.r`). Default **`0.0`**. **Must match the rate the hive's quotes were priced under** — read [Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing) before every run. Must be a finite number consuming the whole token; `abc`, `0.03x`, `nan`, `inf` and a missing value are all **exit 2**, never a silent `0.0`. Negative rates are accepted. |
| `--deep-selection` | no | Additionally run the full held-out `select_curve` OOS search per symbol and record its winner as that symbol's family (falls back to the fit-policy decision when the selector has no scorable holdout). Obeys `--pin-curve-family` like the cheap route does. |
| `--retry-disabled` | no | Re-attempt the symbols whose **stored** config is disabled instead of skipping them. Without it a fail-closed disable is permanent for the life of the database — the symbol is skipped as already-configured on every later run, so no fix to the loader, the hive or the selector can ever reach it. Enabled configs are still left untouched (unlike `overwrite_existing`), so a tuned config is never clobbered; it *does* re-enable a symbol an operator disabled by hand, which is why it is opt-in. The standing disabled names print on `config.failed_symbols` every run. |
| `--pin-curve-family true\|false` | no | Default **`false`**. Store the auto-selected curve family as a **hard pin** (`true`) or as the preferred route only (`false`). Pinning gives each cell exactly one family attempt and **disables both fallback ladders** — read [The curve family is a route, not a pin](#the-curve-family-is-a-route-not-a-pin) before setting it. Requires a value: `--pin-curve-family` with a missing or unrecognised value is **exit 2**, never a silent default. Accepts `true\|1\|on` and `false\|0\|off`. |
| `--fit-workers N` | no | Outer fit fan-out. `0` = auto (honors `ATX_VOL_FIT_WORKERS`); `1` = serial. |
| `--report out.csv` | no | Also write the four-section CSV report to this path. |
| `--max-failures N` | no | `32`. Cap on the printed `failed_cell` lines. Overflow is counted in `coverage.failed_cells_elided`, never dropped silently, and the `--report` CSV always carries the **full** list. `0` prints no per-cell detail at all. Same flag name, parsing and semantics as `atx-vol-surface-db verify`. |

**Discover-all vs explicit `--symbols`.** With an explicit list, exactly those
underliers are loaded for every date (a date whose file lacks a requested symbol
is a visible coverage hole — counted in `n_coverage_holes`, never a silent gap).
With no `--symbols`, the effective universe is the sorted distinct **union** of
`underlying` across every readable date in range; every date then spans the full
union, so the ingest grid is rectangular (date × union) with visible holes where
a symbol is absent from a given date.

Real hives have **non-uniform** per-date coverage (names list, delist, or simply
were not pulled that day), so a discover-all build over a wide universe reports a
**large `n_coverage_holes` and that is healthy** — it is the sparseness of the
grid, not a data defect. `n_load_errors` is the counter that means something is
wrong. The two are classified structurally by the loader (a hole is a present,
readable date file that does not carry that symbol), never guessed from an error
code, so real corruption can never hide in hole noise.

### The curve family is a route, not a pin

Stage 1 classifies each symbol's board and picks a curve family. It used to store
that choice as a **hard pin** (`SymbolFitConfig::pin_curve`) for **every** symbol,
unconditionally. **As of 2026-07-25 it does not pin unless you ask
(`--pin-curve-family true`).**

The default flipped because a pin is not just a preference — it is an
instruction to `PricerFitter` that the family must never be substituted. Inside
the risk pipeline, `auto_routed` is false whenever anything is pinned, and that
one boolean gates **both** recovery ladders:

- the **construction-failure** ladder (the fit did not build → try the next
  family), and
- the **admission-rejection** ladder (the fit built but failed a no-arbitrage /
  coverage gate → try the next family, each rung re-checked against the *full*
  admission contract).

So a pinned cell gets **exactly one curve-family attempt, with no recovery**. A
production build over real OPRA data lost **10 of 45 cells** that way, and the
per-cell reasons showed the failures were *marginal* — e.g. an SPY board rejected
on butterfly with a slack of `0.000107`, a hair over the boundary — exactly the
regime a different family is expected to recover. The ladders exist for this and
were off for 100% of cells.

Unpinned, nothing is lost: the chosen family is still written to the manifest
(`atx-vol-surface-db config --symbol SYM` shows it, and `--pin-curve-family true`
turns it back into a pin), the fitter re-derives the same policy decision per
board, and the ladder is there when the first attempt does not survive admission.
The "never silently substituted" immunity a pin buys is meant for an **operator's
explicit instruction**; a machine-generated per-symbol guess is not that.

**What `true` costs you.** It restores the old single-attempt behaviour exactly.
Use it when you need a symbol's family held fixed across dates — a controlled
comparison, or reproducing an older database.

**What `false` costs you — and this is the part to read twice: it changes the
fitted numbers, not just the runtime.**

*Numerically.* Unpinning does not merely re-choose the curve *family*; it hands
the fit a different **calibration**. `PricerFitter` only looks up the classified
profile's `calib` when its internal `decision_` is populated, and `decision_` is
populated **only on the unpinned branch**. So:

- **Pinned**, a cell fits with an essentially **default-constructed `CalibOpts`**
  (the fit-policy decision never fills `curve.parametric` — nothing in
  `fit_policy.cpp` writes that field — so the stored, faithfully round-tripped
  config carries calibration defaults), plus the handful of fields the preset and
  the risk-quality policy pin.
- **Unpinned**, the same cell fits with the **profile's tuned calib**. For an
  index/ETF-class board that is e.g. `max_outer_iter` 4 → 50, `huber_k` 1.5 → 2.0,
  `residual_disable` true → false, `residual_basis_kind` None → HingeQuad
  (`profile.cpp`'s `build_spy_like`), and `ConvexDense`'s `node_cap` is re-derived
  from the risk tier (40 → 60 at Balanced) instead of copied verbatim.

This is the auto route working as designed, and it is very likely a large part of
why coverage improves — but **expect fitted IVs, total variances and surface RMSE
to move**, not just to appear for cells that previously failed. Do not compare a
`false` database against a `true` one expecting bit-identical surfaces for the
cells that succeeded in both; they were fit by a different calibration.

*In time.* An *ambiguous* board (low classifier confidence) makes the fitter run
its own held-out selector at fit time rather than taking the stored family, and a
rejected board now pays for its ladder rungs. Both are extra work per cell, so a
build over a large universe can take longer than the pinned run did.

That is the intended trade: cells recovered, and a better-calibrated fit, for fit
time spent.

**The `LinearVariance` trap (only reachable with `true`).** The risk pipeline
refuses a `LinearVariance` *pin* outright — `InvalidArgument: invalid correctness
policy for requested risk surface`, on **every** cell of that symbol, every run.
The fit policy routes any ultra-liquid index/ETF profile to that family, so this
hits any index/ETF name in the universe that is not the single `--index` symbol.
With `--pin-curve-family true`, such a symbol is now rejected at **config** time —
stored **disabled** and named in `config.failed_symbols` — instead of silently
turning into an unexplained `cells_failed` count.

The guard is deliberately narrow, because over-rejecting loses cells that would
have fitted. It needs **both**:

- the **pin** — unpinned, the fitter substitutes the dense model on the auto
  route, so the family is harmless; and
- a **Risk** output in the symbol's policy — the refusal lives inside the *risk*
  build, which the fitter skips entirely for a mark-only request, and that mark
  path pins `LinearVariance` itself. `--preset hft` maps to MarketMark-only, so
  `--preset hft --pin-curve-family true` keeps those symbols enabled and fits
  them on exactly that family.

### Interest rate / carry — the single most likely way a build produces nothing

**`--r` sets the flat continuously-compounded carry rate, and it defaults to
`0.0`.** The default is only correct for a hive whose quotes were priced at
**zero** carry; with it, the implied forward is the spot.

**Pass `--r` on every real run.** If the hive's quotes embed a non-zero
funding/borrow rate (any real OPRA data does), leaving `--r` at `0.0` makes every
put-call-parity forward wrong and **every full fit fails, identically**. The
per-symbol config classification is tolerant enough to pass anyway, so the
failure shows up in the coverage counters, not in stage 1:

- `coverage.cells_ok` is **0** (or far below `cells_to_fit`),
- `coverage.cells_failed` carries the whole universe, with each
  `symbol.<S> ... ok=0 failed=N` row confirming it, and
- **no partition is written at all** — the database ends up empty.

**Do not guess which of these it is** — read the `failed_cell` lines (below).
Every failed cell prints the fitter's own reason, so a carry mismatch (every cell
failing the same way at the same gate) is distinguishable at a glance from a set
of genuinely marginal boards.

**This is no longer a silent green exit.** A build that scheduled work and
produced **nothing at all** — nothing fitted *and* nothing carried
(`cells_to_fit > 0`, `cells_ok == 0`, `cells_carried == 0`) — exits **3** and
prints a diagnostic on stderr naming the carry rate it used:

```
$ atx-vol-surface-db-build --db /db --hive /hive --from 2026-07-01 --to 2026-07-06
... (the full report still prints to stdout, and --report is still written) ...
atx-vol-surface-db-build: TOTAL FIT FAILURE: 9 cells scheduled, 0 fitted (9 failed).
  Most likely cause: the carry rate does not match the hive. This build used --r 0.
  If the hive's quotes embed a non-zero funding/borrow rate, every put-call-parity
  forward is wrong and every fit fails identically. Re-run with the matching --r <rate>.
$ echo $?
3
```

The decision lives in the library as
`is_total_fit_failure(const SurfaceDbBuildReport&)`
(`atx/vol/surface_db_build.hpp`), unit-tested in the `SurfaceDbTotalFitFailure`
suite — the CLI only maps it to an exit code.

**It is deliberately narrow, and the three neighbouring shapes stay green:**

- **Partial** failure (`cells_ok > 0` alongside some `cells_failed`) is **normal
  production output** — real hives carry unfittable boards. Exit `0`.
- **Nothing to do** (`cells_to_fit == 0`) is the **resume** path over an already
  complete database, and the un-pulled empty window. `cells_ok` is legitimately
  `0` because nothing was scheduled. Exit `0` — the build's convergence guarantee
  ("a re-run fits zero") depends on it.
- **Carried-only** (`cells_carried > 0` with `cells_ok == 0`) is the
  **converged-with-permanent-failures** steady state once a date holds a
  permanently-failing cell. That date is rewritten on every run and its failure
  retried forever (there is deliberately no persisted known-failed state), while
  its healthy siblings are *carried* rather than re-fitted — so `cells_ok` is
  legitimately `0` on a database that is otherwise entirely healthy. Exit `0`.
  Before carry-over those siblings were re-fitted and `cells_ok` was large, which
  is the only reason this shape never misfired.

  > **"Converged" is used in two senses in this document and they differ.** This
  > shape is converged in the sense that matters operationally — *no re-run will
  > ever produce anything new* — and it **warns on every run**. A **fully
  > converged** database, the one called "genuinely converged" fifty lines below,
  > additionally has `cells_failed == 0` and is **silent**. `prod-2026-07` is the
  > first kind, permanently. If your database warns every run, that is not
  > evidence it has failed to converge.

> **Known limit.** The exemption is keyed on `cells_carried == 0`, so **any** run
> that carried at least one cell is exempt from this predicate — a strictly wider
> set than "a converged database". In particular, a run whose every *scheduled*
> cell failed for a systematic reason (a bad config for a newly-added name, a
> loader regression) beside a large healthy carried population exits `0`, where
> before carry-over it would have exited `3`. The counters IDENTIFY that shape —
> `cells_ok 0` with `cells_failed > 0` and `cells_carried > 0` — but they cannot
> resolve it: the converged steady state prints exactly the same numbers. Neither
> the counters nor the exit code separate the two; only the per-cell reasons do
> (see the warning below). A wrong `--r` on a converged database
> likewise goes unflagged here, because carried cells are never re-fitted and so
> never re-fail; that question belongs to a stale-input check in the carry gate
> (comparing each stored record's `S`/`r`/`now_ts_ns` against the loaded board's
> `MarketEnv`), not to an exit-code predicate. The trade is deliberate: a false
> `TOTAL FIT FAILURE` on *every* healthy resume, telling the operator to change
> `--r`, would invalidate every surface in the database if followed.

**What was lost is the verdict, not the signal.** The ambiguous shape above gets a
stderr **warning** and exit **0**
(`is_carry_masked_fit_failure(const SurfaceDbBuildReport&)`, same header, pinned by
the `SurfaceDbCarryMaskedFitFailure` suite and the end-to-end
`BuildSurfaceDb.CarryMaskedFitFailureFiresOnTheAmbiguousShapeAndNotOnAConvergedDb`):

Transcribed from a real resume of a production-shaped database (51 symbols, the
2026-07-01 partition, whose MCD cell is one of the nine deliberately-left-failing
cells):

```
$ atx-vol-surface-db-build --db /db --hive C:/atx-data/opra-hive \
      --from 2026-07-01 --to 2026-07-01 --r 0.043 --index SPY
... (the full report still prints to stdout, and --report is still written) ...
atx-vol-surface-db-build: WARNING (exit 0): 0 cells fitted, 1 failed, 50 carried.
  Cells that failed (1 shown, 0 elided): 2026-07-01/MCD
  This run produced no NEW surface. Two very different runs look like this and the
  counters cannot tell them apart:
    (a) the converged steady state — a permanently-failing cell is retried on every
    run (by design; nothing is persisted as known-failed) while its healthy siblings
    are carried. Nothing is wrong.
    (b) every cell this run scheduled died for a SYSTEMATIC reason — a fitter or
    loader regression, or a bad config for a newly-added name — beside 50 carried
    cells that were never re-fitted and so could not re-fail. Before carry-over this
    run would have exited 3.
  Compare the list above with the previous run's: the SAME cells failing the same way
  is (a); a fresh name, or a new reason, is (b). The failed_cell lines on stdout carry
  each cell's own reason, and --report writes every one of them.
  Clearing it: fix the failing cell, or stop fitting the name entirely with
  `atx-vol-surface-db disable --db <root> --symbol <SYM> --yes` (its stored surfaces
  are kept). Disabling costs that symbol on EVERY date, so on a name that is healthy
  everywhere else it trades many good surfaces for one silenced line — usually a bad
  deal. If neither applies, this line is EXPECTED on every run and is not a defect on
  its own; what you watch is the list above CHANGING.
$ echo $?
0
```

The `Cells that failed` list is what makes (a) and (b) separable at a glance: it is
the only thing that distinguishes them, so it is on the warning rather than left
for you to reconstruct from the `failed_cell` lines. It is bounded by the same
`--max-failures` cap as those lines and elision is counted, never silent
(`--max-failures 0` prints `Cells that failed (0 shown, 1 elided):`).

It fires on `cells_ok == 0 && cells_failed > 0 && cells_carried > 0` and on nothing
else. Three consequences worth knowing:

- **It stays exit 0, permanently.** Shape (a) is a healthy production database and
  the steady state this feature exists to produce; a non-zero code for it is
  exactly the destructive false verdict the carry clause removed. The warning
  exists *because* the exit code cannot come back.
- **A FULLY converged database is silent.** Once no date has anything left to
  schedule, `cells_failed` is `0` and nothing prints. The warning recurs for as
  long as a cell keeps failing. (See the note under **Carried-only** above: a
  database that warns on every run is still converged in the sense that no re-run
  will produce anything new. Silence requires the stronger property.)
- **On a database with permanently-failing cells, this line is EXPECTED on every
  run, and there may be no action that clears it.** That is the honest statement
  and the manual used to imply otherwise. There are exactly two clearing actions
  and on the population that actually produces this warning, usually neither
  applies:
  1. **Fix the failing cell.** Only possible when the failure is a defect. It
     frequently is not — of `prod-2026-07`'s nine residual failures, three are
     genuinely arbitrage-violating boards (18–21 butterfly *and* 17–28 calendar
     violations) and the rest miss a no-arb bound marginally. There is nothing to
     fix in the code, and tuning the admission thresholds until the number is
     clean defeats the instrumentation.
  2. **Stop fitting the name** —
     `atx-vol-surface-db disable --db <root> --symbol <SYM> --yes`. See
     [Disabling a name](#disabling-a-name--the-remedy-and-its-real-price) for what
     this costs; the short version is that `enabled` is a per-**symbol** switch
     while these failures are per-**cell**, so it removes the name from **every**
     date, past and future. `prod-2026-07`'s nine failures are nine cells spread
     over eight symbols on eight dates, and every one of those symbols fits on its
     other sixteen dates. Silencing SPY's two marginal cells would cost SPY's
     other fifteen surfaces and the index leg with them.

  **When neither applies, the correct action is to accept the warning.** It is not
  a defect signal on its own. What you watch is the `Cells that failed` list
  **changing** — a name that was not there before, or a familiar name failing for
  a new reason — because that, and only that, separates reading (b) from reading
  (a). Compare it against the previous run's; `--report` gives you a diffable CSV
  of every cell if you want to automate the comparison.
- **It reads `cells_carried`, not `cells_carried_disabled`.** `cells_carried` is
  what grants the exit-3 exemption, so it is what the warning must track. A run
  that carried only *disabled* symbols' preserved surfaces is not exempt — it still
  exits 3.

So exit 3 answers exactly one question — "did this run get anything at all?" — and
**partial coverage is still your job to inspect.**

### The same trap one stage earlier — total CONFIG failure

`is_total_fit_failure` only sees cells that were **scheduled**, and stage 1 can
swallow the universe before anything is scheduled at all. If per-symbol **config
selection** fails for every symbol — every board classified unselectable — then
every config is stored **disabled** (fail-closed), `cells_to_fit` is `0`, and the
fit predicate reads that as the healthy *nothing-to-do resume*. The build used to
exit `0`, indistinguishable from a converged re-run, over a database with **no
enabled symbol** that will never hold a surface.

That is now exit **3** as well, via the sibling predicate
`is_total_config_failure(const SurfaceDbBuildReport&)`
(`SurfaceDbTotalConfigFailure` suite, plus the end-to-end
`BuildSurfaceDb.EverySymbolFailingSelectionIsTotalConfigFailure`):

```
$ atx-vol-surface-db-build --db /db --hive /hive --from 2026-07-01 --to 2026-07-06 --r 0.03
atx-vol-surface-db-build: TOTAL CONFIG FAILURE: 3 symbols seen, 0 enabled
  (3 disabled by a selection failure on this run, 0 already stored disabled).
  NOT ONE symbol in this database has an enabled config, so no cell was ever
  scheduled to fit; the database will stay empty. ...
... (the full report still prints to stdout) ...
config.n_configured 0
config.n_disabled_failed 3
config.n_disabled_existing 0
coverage.cells_to_fit 0
config.failed_symbols AAA BBB CCC
$ echo $?
3
```

True iff the config stage left at least one symbol **disabled** and **not one
enabled**, and the run produced no surface at all — neither fitted nor carried
(`cells_ok == 0 && cells_carried == 0`) — the same attempted-nothing-succeeded
shape as the fit predicate, read off the **standing** state rather than this run's
fresh verdicts:

```
disabled = n_disabled_failed + n_disabled_existing
enabled  = n_configured + (n_skipped_existing - n_disabled_existing)
=> disabled > 0 && enabled == 0 && cells_ok == 0 && cells_carried == 0
```

The `cells_carried` conjunct is unreachable today (nothing can be carried when no
symbol is enabled). It is there so both predicates read the **same** evidence for
"did this run produce a surface at all" — the coupling whose absence produced the
false `TOTAL FIT FAILURE` one stage down.

**Four neighbouring shapes stay green**, for the same reasons:

- **Partial** selection failure (some symbol enabled beside some
  `n_disabled_failed`): a real universe carries names whose board cannot pin a
  curve; they are disabled while the rest build. Exit `0`.
- **Nothing to do** (nothing disabled): every symbol was already configured and
  enabled (`n_skipped_existing`), or the window was empty. Convergence needs this.
  Exit `0`.
- **New names failing beside productive fits**: only newly-seen symbols failed
  selection while already-configured ones fitted. `cells_ok > 0`, so the run
  produced surfaces — partial, not dead. Exit `0`.
- **Carried-only**: the run re-emitted stored surfaces without fitting any
  (`cells_carried > 0`). The database served surfaces this run, so it is not
  dead. Exit `0`.

Both predicates map to the **same** exit `3`: a script asks "did this run produce
anything at all?", and the stderr diagnostic names which stage swallowed it. The
config check runs first, because when both fire the config stage is the upstream
cause and the thing to fix.

**A resumed all-disabled database is exit 3 too.** It used to be exit `0`:
"nothing was attempted, so nothing failed" — which meant the *first* run over a
hopeless universe failed loudly and every later run over that same dead database
reported green, because its disabled configs had turned into `n_skipped_existing`.
The predicate now reads the standing state (`n_disabled_existing`), so the answer
does not depend on which run you are on. `verify` still catches it independently:
a walk that selects zero cells over a db that holds partitions is a `FAILED`
verdict (see [The zero-cell verdict](#the-zero-cell-verdict--verify-cannot-pass-what-it-never-read)).

**A PARTIALLY disabled database stays exit 0** — one lost name out of fifty is a
partial build, not a dead one — but it is no longer silent. `config.failed_symbols`
carries the standing disabled names on **every** run (not just the one that first
stored the disable), the `--report` CSV carries them in its
`config_disabled_symbol` section, and a stderr callout names them and points at
`--retry-disabled`.

**Operator checklist:** a green exit no longer hides a *totally* dead build, but
it still does not mean full coverage. After any build, **inspect `cells_ok` vs
`cells_failed`, the per-symbol rows and the `failed_cell` lines** (or the
`--report` CSV's sections 2–4 — section 2 names every disabled symbol, section 4
every lost cell and why), then
run `atx-vol-surface-db verify --db <root> --min-cells <expected>` (below), which
turns "the database is the size and shape I expected, and every cell evaluates"
into a single exit code.

The finer fix for hives with a real term structure (rather than one flat rate) is
the per-cell **market-inputs** path (`OpraHiveSpec.market_inputs` /
`yc_pillar_t`/`yc_pillar_r`), which the CLI does **not** expose — `--r` is the
single flat rate only.

### Examples

```bash
# Explicit 3-symbol build over a July window, SPY as the index leg, CSV report:
atx-vol-surface-db-build \
  --db   C:/atx-data/surfdb-2026-07 \
  --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31 \
  --symbols SPY,AAPL,MSFT --index SPY --r 0.0425 \
  --report C:/atx-data/surfdb-2026-07/build_report.csv

# Discover the whole universe present in the window (no --symbols):
atx-vol-surface-db-build \
  --db C:/atx-data/surfdb-2026-07 --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31 --r 0.0425

# Deep per-symbol curve selection, serial fit (reproducible), robust tier:
atx-vol-surface-db-build \
  --db /db --hive /hive --from 2026-07-01 --to 2026-07-31 --r 0.0425 \
  --preset robust --deep-selection --fit-workers 1

# Restore the pre-2026-07-25 behaviour: the selected family is a HARD PIN, one
# curve attempt per cell, no fallback ladder.
atx-vol-surface-db-build \
  --db /db --hive /hive --from 2026-07-01 --to 2026-07-31 --r 0.0425 \
  --index SPY --pin-curve-family true
```

**Every example passes `--r`.** Omitting it is a build at zero carry — see
[Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing).

### Output and exit codes

Every scalar report field prints one-per-line to **stdout** as `key value`
(mirroring the CSV `key,value` section), followed by `config.failed_symbols`, one
`symbol.<S> attempted=.. ok=.. failed=.. disabled=.. carried=..` line per symbol,
and one
`failed_cell <date> <symbol> code=<Code> detail=<text>` line per **failed cell**.
With `--report`, the same data is written as CSV in **four** sections: a
`key,value` scalar section, a `config_disabled_symbol` row per disabled name, a
`symbol,n_attempted,n_ok,n_failed,n_disabled,n_carried` row per symbol, then a
`date,symbol,code,detail` row per failed cell (`detail` is RFC4180-quoted — it is
free text from the fitter and may contain a comma). Every section header is
emitted even when its list is empty, so the file's shape is constant.

### Why each cell failed — the `failed_cell` lines

`config.failed_symbols` has always named the symbols **config selection** refused.
The **fit** stage used to name nothing: a lost cell was a `+1` on `cells_failed`
and no more, even though `PricerFitter` had already built a full diagnostic for it
and the pipeline then threw it away. It no longer does — the fit `Error`'s message
travels from the fitter to the report intact:

```
coverage.cells_ok 33
coverage.cells_failed 9
coverage.failed_cells_reported 9
coverage.failed_cells_elided 0
failed_cell 2026-07-07 AAPL code=Unavailable detail=risk surface rejected: model=essvi mask=4 butterfly=2 butterfly_slack=0.0031 butterfly_k=-0.18 butterfly_slice=1 ... carry=ok inversion=failed
```

The `detail` text is the fitter's own, verbatim — for the risk pipeline it names
the failing gate, the offending slice, the log-moneyness and the slack, which is
what turns "9 cells failed" into an actionable next step.

Ordering is deterministic: **ascending by (date, symbol)**, and byte-identical for
any `--fit-workers` value. The list is appended by the single drain thread as it
walks dates in order, never by a fit worker, so completion order cannot reach it.

Printing is **capped** at `--max-failures` (default 32) because a wholesale
failure of a 51-symbol × 17-date universe is 867 cells. Truncation is **counted**,
never silent: `coverage.failed_cells_reported` + `coverage.failed_cells_elided`
always equals `coverage.cells_failed`, and the `--report` CSV carries every entry
regardless of the cap. This is the same contract `verify`'s `failures_reported` /
`failures_elided` pair already uses.

**This is diagnosis, not memory.** Listing a failed cell does **not** mark it
known-failed: there is deliberately no persisted failure state, so the cell is
retried on the next run exactly as before (see [Resume semantics](#resume-semantics)).

| Exit | When |
| --- | --- |
| `0` | Build succeeded — including **partial** coverage, a no-op **resume**, and a graceful empty-window no-op. Also the **carry-masked** shape (`cells_ok == 0`, `cells_failed > 0`, `cells_carried > 0`), which prints a stderr WARNING and still exits `0` — see [Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing). |
| `1` | A build error — malformed hive spec, or a db config/write failure. Message on stderr, **no report printed**. |
| `2` | A usage error — unknown flag, a missing required flag, an unknown `--preset`, or a malformed `--r` / `--pin-curve-family`. Usage on stderr. |
| `3` | **The build ran to completion and produced NOTHING** — either **total config failure** (`disabled > 0`, `enabled == 0`, `cells_ok == 0`, `cells_carried == 0`: every symbol was disabled by a selection failure, so nothing was ever scheduled) or **total fit failure** (`cells_to_fit > 0`, `cells_ok == 0`, `cells_carried == 0`: work was scheduled, no cell fitted, and nothing was carried either). One code for both — the script's question is "did this run produce anything?" and the stderr diagnostic names the stage. The full report still prints and `--report` is still written. See [Interest rate / carry](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing). |

`3` is deliberately distinct from `1`: `1` means *atx or the database broke* (no
report to read), `3` means *the tool worked and your inputs produced nothing* —
almost always a `--r` mismatch. A script can branch on that.

Note: a single unloadable or unselectable board never aborts the build — it is
tallied (and, for config, stored **disabled** = fail-closed) and the call still
succeeds. Partial failure is **not** exit 3.

## Report fields

**Config generation** (`generate_symbol_configs`, stage 1). The three
disposition counters partition the distinct symbols seen:
`n_configured + n_skipped_existing + n_disabled_failed == n_symbols`
(`n_disabled_existing` is a sub-count of `n_skipped_existing`, not a fourth class).

| Field | Meaning |
| --- | --- |
| `config.n_symbols` | Distinct symbols across the loaded boards. |
| `config.n_configured` | Freshly configured (or overwritten), enabled. |
| `config.n_skipped_existing` | Already in the manifest, left untouched (idempotent resume). |
| `config.n_disabled_failed` | Selection failed → stored **disabled** (never silently served). Also covers a `--pin-curve-family true` run whose chosen family is `LinearVariance`, which the risk pipeline refuses outright — see [The curve family is a route, not a pin](#the-curve-family-is-a-route-not-a-pin). |
| `config.n_disabled_existing` | A **sub-count of `n_skipped_existing`**, not a fourth class: how many skipped symbols carry a **disabled** stored config. Non-zero means the database is not serving that many requested names and will not start on its own — `--retry-disabled` re-attempts them. **Scope:** the config stage walks the symbols that have a **board in this run's window**, so a manifest symbol that is disabled *and* absent from the loaded window (dropped from `--symbols`, or no data on any date in range) is not counted or named here. `verify` is the backstop for that case — it walks the **manifest**, so its `disabled_symbol` lines name every stored disable regardless of the build window. |
| `config.failed_symbols` | Every name the database is currently **not** serving, sorted: the `n_disabled_failed` this run disabled **plus** the `n_disabled_existing` it found already disabled. Printed on every run, and written to the CSV's `config_disabled_symbol` section. |

**Populate coverage** (`populate_universe_streaming`, stage 2). Cells are
`(symbol, date)` pairs; the counters describe what the cell-aware resume did.

| Field | Meaning |
| --- | --- |
| `coverage.cells_loaded` | Boards handed to the populate (available parquet cells). |
| `coverage.cells_to_fit` | NEW `(symbol, date)` cells scheduled this run. A **config-disabled** cell is never counted — it can never be added, so counting it would keep its date pending forever. |
| `coverage.cells_refit` | Already-present cells that a same-date rewrite put back through the **fitter**. Carry-over and the disabled-preserve split this population **three** ways, not two: an already-present cell dragged into a rewrite is *carried* (`cells_carried`, the fingerprint still vouches for it), *preserved because its config is disabled* (`cells_carried_disabled`, never offered to the fitter), or *refit* — exactly one of the three, never two, and this row counts only the last. On a converged database this should be **`0`** — that is the invariant a cheap resume rests on, and it is the number to watch, not `cells_to_fit`. |
| `coverage.cells_carried` | Already-present cells re-emitted **verbatim** from the existing partition instead of being re-fitted, because the stored config fingerprint still matches. Byte-identical to what they replaced. Deliberately **not** counted in `cells_ok` — nothing was fitted — which is why both exit-code predicates read this counter explicitly. |
| `coverage.cells_carried_disabled` | Already-present cells whose config is **disabled** and whose stored surface was re-emitted verbatim so the rewrite would not **delete** it. `enabled = false` means *stop fitting this symbol*, never *delete what is already stored*. Deliberately **separate** from `cells_carried`, which both exit-code predicates read as evidence the run produced a serviceable database — a switched-off name's leftover bytes are not that evidence. Counted in neither `cells_refit` (never offered to the fitter) nor `cells_ok`. Unlike `cells_carried` this does **not** depend on the config fingerprint: the alternative to preserving is deletion, not a re-fit, so the fingerprint has no say. |
| `coverage.cells_already_present` | Skipped: symbol already in its date partition. |
| `coverage.cells_ok` / `cells_failed` | Fit outcomes over the (re)written dates. |
| `coverage.dates_total` | Distinct dates among the loaded boards. |
| `coverage.dates_written` | Dates that needed a (re)write this run. |
| `coverage.dates_skipped_complete` | Dates with **nothing left to add**: every loaded cell is either already present or config-disabled. |
| `coverage.dates_skipped_would_drop` | Dates skipped to avoid dropping an existing symbol (safety guard). |
| `symbol.<S> ...` | Per-symbol populate stats over the written dates. |
| `coverage.failed_cells_reported` / `_elided` | How many `failed_cell` lines were printed, and how many the `--max-failures` cap left out. The two always sum to `cells_failed`; the `--report` CSV is never capped. |
| `failed_cell <date> <symbol> code=.. detail=..` | One line per failed cell, ascending by (date, symbol) — the fitter's own reason. See [Why each cell failed](#why-each-cell-failed--the-failed_cell-lines). |

**The cell counters do not reconcile against `cells_loaded`** — do not read them as
a partition. A config-disabled cell that is absent from its partition on a
skipped-complete date appears in none of `cells_to_fit`, `cells_refit` or
`cells_already_present`, and the per-symbol `disabled=` column only covers the
dates this run actually wrote. `cells_loaded` is the input count.

**Hive ingest** — the first two counters describe distinct **dates**, the last two
describe **cells**:

| Field | Meaning |
| --- | --- |
| `n_dates_loaded` | Distinct dates that produced at least one board. |
| `n_dates_missing` | Distinct in-range dates that produced **none** (a fully absent OR fully unreadable date). The window is enumerated as **calendar** days, so every weekend and market holiday in range is counted missing — a July window always shows ~9. |
| `n_load_errors` | **Cell** count of real ingest **defects**: a present file that is unreadable/unparseable, has the wrong schema, or whose market inputs quarantined the cell. Never reaches the fit. **This is the counter to alarm on.** |
| `n_coverage_holes` | **Cell** count of **coverage holes**: the date file is present and readable, the symbol is simply not in it. Expected and healthy on a sparse universe; never reaches the fit. |

The two cell counters exhaust the loader's erroring cells
(`n_load_errors + n_coverage_holes == OpraBatchResult::n_error`) and are split by
the loader **structurally** — a hole is decided from the date file's own
distinct-`underlying` set, not from an error code (a hole and a wrong-schema file
both surface `InvalidArgument`, so a code test would report a corrupt date as
holes and let real corruption hide).

**Double-count, by design.** A date whose file is present but fully corrupt is
counted in **both** `n_dates_missing` (it produced no boards, so it is not a
loaded date) **and** `n_load_errors` (each of its cells is a present-but-
unparseable file). This is deliberate: `n_dates_missing` answers "how many
in-range sessions have no usable surfaces?" and `n_load_errors` answers "how many
present files failed to parse?" — a corrupt date legitimately answers yes to
both. A merely absent date (no file at all) bumps only `n_dates_missing`.

## Resume semantics

The build is idempotent and resumable because each stage independently no-ops on
work already done:

1. **Create-or-open db.** A manifest at `--db` root → open (resume); absent →
   create. A re-run reuses the same root.
2. **Config idempotence.** A symbol already in the manifest is left **untouched**
   (`n_skipped_existing`), so a re-run never clobbers an operator override. (The
   library exposes an `overwrite_existing` escape hatch; the CLI does not — it is
   deliberately non-destructive.)
3. **Cell-aware populate resume.** A partition (= date) is (re)written only when a
   loaded board adds a symbol the partition does not already carry. So as the
   OPRA pull dribbles in new `(symbol, date)` cells, only the new work is fit; a
   re-run over unchanged data fits **zero** (`cells_to_fit == 0`,
   `dates_written == 0`, `dates_skipped_complete == dates_total`) **once every
   loaded cell has either fitted successfully or been config-disabled** — a
   disabled cell is excluded from the pending tally, since it can never be added.
   A **grown** hive (new dates, or new symbols on existing dates) fits only the
   delta.
   - A cell that **fails to fit** is *not* suppressed: there is no persisted
     known-failed state, so it is retried on every run, which keeps its date in
     the rewrite set and re-fits that date's siblings. That is the deliberate
     cost of giving a transient failure another chance. A name that fails on
     **every** date can be taken out of the schedule with
     `atx-vol-surface-db disable --db <root> --symbol <SYM> --yes`, and then the
     database converges to silence. That is the only case where disabling is the
     right instrument: it is a per-**symbol** switch, so a name that fails on a
     few dates and fits on the rest loses the ones that fit too. See
     [Disabling a name](#disabling-a-name--the-remedy-and-its-real-price).
   - **Disabling a name never deletes what it already produced.** `enabled =
     false` means *stop fitting this symbol*; the surfaces it fitted before the
     disable stay in their partitions, keep loading, and are re-emitted verbatim
     through any later rewrite of those dates (`coverage.cells_carried_disabled`).
     That matters because the remedy above is the routine one: before this was
     fixed, disabling a permanently-failing name silently **destroyed** every
     surface it had already fitted, on the next rewrite of each of those dates —
     and the trigger was unrelated (any new enabled symbol arriving on the date).
     `--retry-disabled` therefore recovers both the config *and* the data.
     - **New failure mode, deliberate.** Re-emitting a stored record means
       *reading* it, so a record that cannot be opened or reconstructed
       (unreadable file, failed checksum, unparseable record) now **aborts the
       build** with that error instead of being silently dropped. The alternative
       is to write the date's partition without it — which is the deletion this
       preserve exists to prevent, performed on the one record already known to
       be broken. Nothing already built is lost: the date being rewritten has not
       been written yet, every earlier date was committed atomically, and a re-run
       skips them. Diagnose with `verify` (its `unmappable` / `checksum` kinds
       name the same bytes), then repair or drop that partition.
   - Safety guard: a date is **skipped, never rewritten**, if its partition
     already holds a symbol NOT present in this run's loaded set — a
     whole-partition rewrite would drop it (`dates_skipped_would_drop`). This
     cannot happen on the intended grow-only workflow but guards a
     narrower-symbol re-run from data loss. It is the *other* half of the same
     concern as the bullet above: that one covers a stored symbol this run
     **did** load but will not fit (disabled), this one a stored symbol it did
     **not** load at all.

An **empty window** (un-pulled days) is a graceful success: all-zero coverage,
the db still created. Absent dates in range are non-fatal (`n_dates_missing`).

The pull unit upstream is `(date, symbol-set)`: a present date file is read for
its on-disk symbol set (footer statistics, no data scan) and only the missing
symbols are pulled, then the date file is rewritten as the union — so the hive
never re-bills for data already on disk. See the pull tool below.

## Scale posture

Target shape: thousands of symbols × ~250 sessions/yr → ~1M surfaces
(≈ 4k symbols × 250 dates).

- **Per-date partition file**: 4k surfaces × ~2–6 KB ≈ 10–25 MB — comfortably
  inside `SurfaceArchiveV2` mmap + the LRU partition-view cache (16 resident
  partitions by default; O(1) `map_surface` probe per query).
- **Manifest** (`manifest.atxdb`): 4k × 256 B symbol records + 250 × 128 B
  partition records/yr ≈ ~1 MB, rewritten atomically — fine at this scale.
- **Asserted limits**: partition key ≤ 32 chars, symbol ≤ 32 chars.
- **Out of scope**: multi-year growth (manifest > ~100 MB or partitions
  > ~100k). The scaling seam is **one db root per year** (documented, not built —
  no sharding, YAGNI).

**Build-time peak memory scales with the date-range length in discovery mode.**
When `--symbols` is omitted, the hive loader runs a serial pre-pass that
materializes each date's table to compute the discovered union, and it **holds
each date's table in memory from that pre-pass until the date's panel pass
completes**. So the loader's peak RSS grows with the number of dates in the
requested window, not just with a single date. The downstream **populate** stays
`O(dates in flight)` (per-date fit → serialize → release), but the loader's
discovery retention is the memory ceiling for a wide window. For very long
ranges, build in date chunks (the cell-aware resume makes chunked runs stitch
losslessly) or pass an explicit `--symbols` list (which skips the discovery
pre-pass).

## `atx-vol-surface-db` — managing and verifying a built database

**Managing and verifying a built database requires no Python.** Everything below
runs from the
command line against a `SurfaceDb` root: what it contains, how each symbol is
configured, what one cell evaluates to, whether every selected cell still
holds intact bytes that evaluate to a usable number, and whether a name is fitted
at all (read
[what a green `verify` does and does not prove](#what-a-green-verify-proves-and-does-not-prove)
before you treat it as an acceptance gate). The
pybind11 wrapper is not needed, not installed, and not on the verification path
— the old production run-plan step "query check via python binding
(`map_surface` one cell)" is replaced by `atx-vol-surface-db query` and
`atx-vol-surface-db verify`.

The tool is a **thin shell**: every subcommand parses flags, calls exactly one
function in `atx/vol/surface_db_admin.hpp`, and prints. All logic — and the test
gate (`SurfaceDbAdmin`, `atx-vol/tests/surface_db_admin_test.cpp`) — lives in
that library, so a service or notebook consumes the same structs without parsing
this text.

### Build

Same `ATX_BUILD_EXAMPLES` gate as `atx-vol-surface-db-build` (see
[Build](#build) above — if that CLI is in your build dir, this one is too):

```bash
powershell scripts/atx-build.ps1 build atx-vol-surface-db
# -> build/bin/atx-vol-surface-db(.exe)
```

### Subcommands

```
atx-vol-surface-db <subcommand> --db <root> [flags]
```

`--db <root>` is required by every subcommand.

| Subcommand | Flags | Answers |
| --- | --- | --- |
| `info` | — | What is in this database? Generation, symbol/partition counts, surface count, bytes, then one `partition` line each. |
| `partitions` | — | One `partition` line per partition (the `info` tail on its own, for piping). |
| `partitions` | `--key KEY` | What does THIS partition actually hold? Reads the `.atxvsa` directory, not the manifest. |
| `symbols` | — | One `symbol` line per configured symbol. |
| `config` | `--symbol SYM` | One symbol's full stored fit config + provenance. |
| `query` | `--key KEY --symbol SYM --strike K --tenor T` | What does this cell evaluate to? iv / total variance / forward / uid / slices. |
| `verify` | `[--from KEY] [--to KEY] [--symbols A,B,C] [--include-disabled] [--probe-tenor T] [--max-failures N] [--min-cells N]` | Does every selected cell still map, checksum and evaluate? |
| `enable` | `--symbol SYM` | Resume fitting SYM on every date. **Writes the manifest.** |
| `disable` | `--symbol SYM --yes` | Stop fitting SYM on **every** date. Its stored surfaces are kept. **Writes the manifest.** |

`query` and `verify` both go through **`SurfaceDb::map_surface`** — the zero-copy
path production readers use (and the one the retired Python check made) — so a
green result is evidence about the path that actually serves.

**Six of the eight subcommands are read-only.** `enable` and `disable` are the
tool's only writes, and they change exactly one field of one symbol's stored
config (`SymbolFitConfig::enabled`) through the same
`SurfaceDb::upsert_symbol` the build path uses. There is deliberately no
`upsert` / `config --set` verb: a stored config is never clobbered here, matching
`atx-vol-surface-db-build`, which withholds the library's `overwrite_existing`
for the same reason.

> **Single writer.** Do not run `enable` / `disable` while a build is running
> against the same root. A build holds one in-memory manifest snapshot for its
> whole run and every partition write persists that snapshot's symbol table, so a
> mutation landing mid-build is silently overwritten. Nothing detects this — there
> is no lock file — so it is a scheduling rule. It is the same one
> `surface_db.hpp` has always stated for this database ("cross-process: single
> writer, many readers").

### Disabling a name — the remedy, and its real price

`disable` is the action the build CLI's carry-masked warning and the resume
semantics above both name. Read this before using it.

```
$ atx-vol-surface-db disable --db /db --symbol SPY --yes
symbol SPY
enabled_before 1
enabled 0
changed 1
generation 91
```

**What it does.** `enabled = false` means **stop fitting this symbol**. The
populate stops scheduling it on every date, forever, until it is enabled again.

**What it does *not* do.** It does not delete anything. Every surface the symbol
already fitted stays in its partition, still loads, still serves (nothing on the
read path gates on `enabled`), and is re-emitted **verbatim** through any later
rewrite of those dates — counted as `coverage.cells_carried_disabled` on the
build report. This is load-bearing enough to be pinned end to end by
`SurfaceDbAdmin.DisableThenRebuildPreservesTheStoredSurfaceBytes` (byte-identical
records either side of a real rewrite) and
`SurfaceDbAdmin.DisableEnableRoundTripRefitsTheSymbolWithNoDataLost`. Before this
was fixed, disabling a name silently destroyed everything it had produced on the
next unrelated rewrite of each of its dates.

**The price, stated plainly.** `enabled` is a per-**symbol** switch. There is no
per-cell disable and there is deliberately no persisted known-failed state, so:

> **Disabling a symbol removes it from EVERY date — the ones it fits as well as
> the ones it does not, and every future date until it is re-enabled.**

That makes it the **wrong instrument for a small number of bad cells on an
otherwise healthy name**, which is the common case. On `prod-2026-07`, SPY fails
on 2 of its 17 dates; disabling it to silence those two would cost the other 15
stored surfaces from the schedule and remove the designated index leg from every
future build. When a name fails on a *few* dates and fits on the rest, the honest
answer is to **accept the warning** — see
[the warning's clearing actions](#interest-rate--carry--the-single-most-likely-way-a-build-produces-nothing)
above. Disabling is the right instrument only when the name fails on
**substantially all** of its dates, or when you are deliberately fencing it out
of production for a reason unrelated to fitting.

**Flags and exits.**

- `--yes` is required by `disable` and by nothing else. It is checked with the
  other usage rules, *before* the database is opened, so a refusal cannot have
  written anything. Missing it is exit `2`.
- `enable` needs no confirmation: it is the recovering direction, and
  `--retry-disabled` already re-enables without one.
- Both are **idempotent**. Re-asserting the state a symbol is already in prints
  `changed 0`, writes nothing, does not bump `generation`, and exits `0` — so a
  converging operator script may assert the desired state unconditionally.
- A symbol the manifest does not configure is exit `1` (`NotFound`), not `2`: it
  is a fact about the database, not about the command line. Nothing is created.

**`enable` is not `--retry-disabled`.** `enable` restores the **stored** config
as-is and runs no selection. When the disable was yours, that is exactly right —
your config comes back untouched (proven by the config fold: a disable/enable
round trip returns the stored record to its original bytes). When the disable came
from a **failed config selection** (`config.n_disabled_failed`), the stored config
is the generic preset fallback that selection fell back to, and re-enabling it
fits *that* rather than a chosen one. Use
`atx-vol-surface-db-build --retry-disabled` for that case: it re-**selects** the
symbol as if it were new.

### What a green `verify` proves, and does not prove

Each selected cell passes **three gates**, in order, stopping at the first failure:

1. **map** — `map_surface`: the partition file opens, and the record's magic,
   framing and column bounds parse.
2. **checksum** — `SurfaceArchiveV2::validate_symbol`: the record's payload bytes
   still match the **CRC-32C the writer stored inside the record**.
3. **probe** — evaluate one ATM-ish point (`K` = that surface's own
   `forward_at(probe_tenor)`, `T` = `probe_tenor`) and require a finite, positive
   implied vol.

**It proves**, for every cell the flags selected: the file is there, the bytes are
the bytes that were written, and the surface produces a usable number at one point.

**It does not prove** the numbers are *right* — no oracle is consulted, and a
surface fitted from bad market data checksums perfectly and probes fine. It says
nothing about points other than the probe, nothing about cells the flags excluded,
and it cannot know how big the database was *supposed* to be — that is
`--min-cells`, which only you can supply. It is a **byte-integrity + liveness
check over a selected grid**, deliberately not more.

#### Why gate 2 exists

`map_surface` validates the header, the framing and the column bounds — **not one
payload byte**. So intra-record damage that preserves length (bit rot, a partial
copy, a hand edit) maps *cleanly*; and because the ATM probe only touches the
slices bracketing `probe_tenor`, a surface whose far-dated slice is shredded
returns a finite positive `iv` and used to print `verdict ok`. Whole-file
truncation was always caught (the header/metadata CRCs at open); this closed the
gap between those two. Corruption is reported as its own kind:

```
cells_checksum 1
fail 2026-07-01 AAA kind=checksum detail=ParseError: SurfaceArchiveV2::validate: record checksum mismatch
```

**Cost.** Gate 2 reads and checksums every selected record's payload, which is
strictly more IO than the map-and-probe walk (which touches only the header, the
column offsets and two slices). It is the **default anyway, with no opt-out flag**:
a health check that skips the checksum the format already carries is not a health
check, and this is the only gate that can see intra-record damage. If a
whole-universe verify ever becomes too slow to run as a gate, the right lever is
the one already there — narrow the grid with `--from`/`--to`/`--symbols` and verify
in shards — not a flag that makes the cheap answer the silent one. (`verify` is an
acceptance step, not a hot path; a `--no-checksum` flag would exist only to be left
on in the script that mattered.)

### Output format

Line-oriented and stable for scripting; **no JSON**. Two shapes only:

- **Scalars** print as `key value` (one per line), mirroring
  `atx-vol-surface-db-build`'s stdout.
- **Repeated records** print as `<record> <id> field=value field=value ...`.
  Record types: `partition`, `surface`, `symbol`, `fail`.

Fields never move and record lines never wrap, so
`grep '^fail '`, `awk '$1=="partition"'`, and
`... | grep '^cells_ok ' | cut -d' ' -f2` are all stable.

**`info`**

```
root <path>
generation <u64>            # manifest generation (++ on every rewrite)
symbols <n>                 # configured symbols
symbols_enabled <n>         # of those, not fail-closed-disabled
partitions <n>
partitions_missing <n>      # manifest entries whose file is NOT on disk
surfaces <u64>              # sum of per-partition surface counts
manifest_bytes <u64>        # sum of the sizes the manifest recorded at write time
bytes_on_disk <u64>         # sum of the sizes the files have NOW
partition <KEY> surfaces=<n> manifest_bytes=<n> bytes_on_disk=<n> present=<0|1> created_ts_ns=<n>
```

The two byte figures are **not** redundant. `manifest_bytes` is what the manifest
recorded when the partition was written; `bytes_on_disk` is what is there now. A
mismatch, or `present=0` / a non-zero `partitions_missing`, means the directory
was edited behind the manifest's back — exactly the corruption an inspector
exists to find.

**`partitions --key KEY`** — read from the archive directory:

```
partition <KEY> manifest_surfaces=<n> archive_surfaces=<n> manifest_bytes=<n> bytes_on_disk=<n>
surface <SYM> uid=<n> slices=<n> bytes=<n>
```

`manifest_surfaces` disagreeing with `archive_surfaces` means the manifest and
the file have drifted apart.

**`symbols`**

```
symbol <SYM> enabled=<0|1> preset=<name> pin_curve=<0|1> curve=<kind> provenance=<0|1>
```

`preset` uses the same vocabulary as the build CLI's `--preset`
(`fast|accurate|robust|hft|populate`), so a listing feeds straight back into a
rebuild. `curve` is BINDING only when `pin_curve=1`; with `pin_curve=0` (the build
CLI's default — see [The curve family is a route, not a pin](#the-curve-family-is-a-route-not-a-pin))
it records the family stage 1 selected as the preferred route, and the fitter
auto-routes with its fallback ladders live.

**`config --symbol SYM`** — `key value` scalars: `symbol`, `enabled`, `preset`,
`pin_curve`, `curve`, `band_k`, `policy.*`, `provenance` (0/1) and, when
provenance is present, `provenance.purpose`, `.quality_mode`, `.state`,
`.admitted`, `.validation_failures`, `.source_generation`, `.served_generation`,
`.legacy_format`.

**`query`** — `key value` scalars: `key`, `symbol`, `strike`, `tenor`, `iv`,
`total_variance`, `forward`, `uid`, `n_slices`. Doubles print at `%.17g`
(round-trip exact).

**`verify`**

```
partitions <n>              # partitions in range (the walk's rows)
partitions_in_db <n>        # partitions the manifest holds, IGNORING --from/--to
symbols <n>                 # symbols in the walk (the walk's columns)
cells_checked <n>           # == cells_ok + cells_unmappable + cells_non_finite + cells_checksum
cells_ok <n>
cells_unmappable <n>        # map_surface failed (file gone/corrupt, or symbol absent)
cells_checksum <n>          # mapped, but the payload no longer matches its stored CRC
cells_non_finite <n>        # bytes intact, but the ATM probe produced no usable number
symbols_disabled <n>        # manifest symbols the walk DROPPED (stored config disabled)
failures_reported <n>       # fail lines below (capped by --max-failures)
failures_elided <n>         # faults NOT listed -- truncation is never silent
fail <KEY> <SYM> kind=<unmappable|checksum|non_finite> detail=<message>
disabled_symbol <SYM>       # one per dropped column, so `ok` is never a blank cheque
min_cells <n>
verdict <ok|FAILED>
```

`symbols_disabled` / `disabled_symbol` do **not** change the verdict, and that is
deliberate: a fail-closed disable is a legitimate production state, not a corrupt
database, and failing every partially-disabled db would break every operator
script. What they close is the *reporting* hole — the default walk silently
narrows its own columns, so `verdict ok` over a database permanently missing a
requested name was byte-for-byte `verdict ok` over a complete one. Now the names
print (and a stderr note points at `--include-disabled` and the build's
`--retry-disabled`). The catastrophic case — **every** symbol disabled — is still
a hard `FAILED` via [the zero-cell verdict](#the-zero-cell-verdict--verify-cannot-pass-what-it-never-read).

The three `kind`s stay distinct so a fault names its own root cause: `unmappable`
is "the file or the record is not there / does not parse", `checksum` is "the bytes
changed under us", `non_finite` is "valid bytes, unusable surface". See
[the three gates](#what-a-green-verify-proves-and-does-not-prove).

`partitions_in_db` is range-independent on purpose: it is what lets the tool tell
*"this database is fresh"* from *"this database is full and your window matched
none of it"*.

### `verify` flags

| Flag | Default | Meaning |
| --- | --- | --- |
| `--from KEY` / `--to KEY` | unbounded | Inclusive partition-key range, compared lexicographically on the canonical (upper-cased) key. ISO dates sort correctly, so `--from 2026-07-01 --to 2026-07-31` is a July restriction. |
| `--symbols A,B,C` | manifest symbol table | Restrict the columns. Whitespace per field is trimmed, same rule as the build CLI. |
| `--include-disabled` | off | Include fail-closed **disabled** symbols. By default they are skipped, and what that skip hides depends on **when** the name was disabled. Disabled **before it ever fitted**: it is absent from every partition, so checking it would report a missing cell on every date of every healthy database — that is the skip this default exists for. Disabled **after it fitted**: it **keeps** its stored surfaces (`enabled = false` means *stop fitting*, never *delete* — see [Resume semantics](#resume-semantics)) and they still load, so the default walk leaves **real cells unverified**. Turn this on to walk both cases; the first reports the whole column `unmappable`, the second reports the cells that are actually there. It does **not** prove a disabled name is absent — it reports whatever is there, which is the point. |
| `--probe-tenor T` | `30/365` | Tenor for the per-cell ATM evaluation. Must be finite and > 0. |
| `--max-failures N` | `32` | Cap on `fail` lines. Overflow is counted in `failures_elided`, never dropped silently, and the `cells_*` totals stay exact. `0` prints no detail at all and elides everything. Same strict parsing as `--min-cells`. |
| `--min-cells N` | `0` | Fail when fewer than `N` cells were checked. **Strictly parsed** — see below. |

**Every value-taking flag requires its value.** A flag left at the end of the
argv — the shape a dropped shell variable produces — is a **usage error, exit 2**,
never an empty string silently read as a choice. This closed two live traps:
`partitions --key $KEY` with `KEY` unset used to mean *"list every partition"*
(you asked about one and got all of them, exit 0), and `--min-cells $EXPECTED`
with `EXPECTED` unset used to mean *"no floor at all"*.

`--min-cells` and `--max-failures` additionally require a **non-negative integer
consuming the whole token**: `abc`, `9x`, `-1` and a missing value are all exit 2,
never a silent `0`. `--min-cells` is the one flag whose entire job is to fail a
too-small database, so coercing a typo to `0` made it **fail open**:

```
$ atx-vol-surface-db verify --db /db --min-cells $EXPECTED    # EXPECTED unset
atx-vol-surface-db: --min-cells requires a value
$ echo $?
2
```

### The zero-cell verdict — `verify` cannot pass what it never read

A walk that selected **nothing** found nothing broken, so every counter is `0` and
no `fail` line prints — the exact shape of a perfect result. Over a database that
**holds partitions**, that is not health: real surfaces sat on disk and not one
byte was read. It is now a **`FAILED` verdict** (library-side, so every caller gets
it — `DbVerifyReport::selected_no_cells()`), with a stderr diagnostic naming which
of the three doors you came through:

```
$ atx-vol-surface-db verify --db /db
atx-vol-surface-db: verify checked 0 cells while the database holds 3 partitions
  -- nothing was read, so nothing could fail.
  3 partition(s) matched --from/--to, and 0 symbol(s) were selected. ...
partitions 3
partitions_in_db 3
symbols 0
cells_checked 0
verdict FAILED
$ echo $?
1
```

The three doors, all previously green:

- **every symbol is fail-closed disabled** — the default walk drops the columns, so
  a db with three populated partitions reported `symbols 0 / cells_checked 0 /
  verdict ok`. (This is exactly the database a resumed all-disabled build leaves
  behind, which the build's own exit correctly calls a no-op resume.)
- **`--symbols` named nothing the manifest configures.**
- **`--from`/`--to` matched no partition** — wrong window, or wrong db root.

**A genuinely fresh root (`partitions_in_db 0`) stays green**, deliberately: there
is nothing to be wrong about yet, and failing a newly created database would make
`verify` unusable as a post-`create` smoke test. The distinction drawn is between
*"there is no data"* and *"there is data and you read none of it"*. The operator's
question at a fresh root — *"this should not still be empty"* — is `--min-cells`,
which is the one number only the operator knows.

### The empty-database trap — why `--min-cells` still exists

A **fresh** database (`partitions_in_db 0`) has no broken cell and no data to have
read, so `verify` reports `verdict ok`. That is honest, and it is still a **silent
pass** for the one case the zero-cell verdict above deliberately excludes: a
database that was never built, was built over the wrong window, or lost partitions
to a later accident looks exactly like a database created five seconds ago.

```
$ atx-vol-surface-db verify --db /db
partitions 0
partitions_in_db 0
cells_checked 0
...
verdict ok            # there is nothing here, and nothing claims otherwise
```

Only the operator knows how big this database is supposed to be. In a script,
always assert it:

```bash
atx-vol-surface-db verify --db /db --min-cells 9   # -> verdict FAILED, exit 1
```

Three guards now stack, and they are complementary rather than redundant:

| Guard | Catches |
| --- | --- |
| build exit `3` | "this **run** produced nothing" — every symbol died at config selection, or every scheduled cell failed to fit. |
| zero-cell `FAILED` verdict | "this **run of verify** read nothing" over a database that holds partitions — all symbols disabled, an empty `--symbols`, or a window that matched nothing. Needs no flag. |
| `--min-cells N` | "this **database** is smaller than I expected" — including one that was never built. The only guard that can know the intended size, so it is the only one you must remember. |

**Keep `--min-cells` in every script.** It is no longer the *only* thing between a
broken database and `verdict ok` — forgetting it can no longer turn "read nothing"
into green — but it is still the only thing that knows what "big enough" means.

### Exit codes

| Exit | When |
| --- | --- |
| `0` | Succeeded. For `verify`: the walk **covered cells**, every one passed all three gates, **and** `cells_checked >= --min-cells`. For `enable`/`disable`: the symbol **is now** in the requested state, whether or not this run put it there (`changed 0` is a success — the verbs are idempotent). |
| `1` | A runtime failure (message on **stderr**, no `verdict` line) — db won't open, unknown partition/symbol, unreadable partition file, or an `enable`/`disable` naming a symbol the manifest does not configure. **Or** `verify` returned `verdict FAILED` on **stdout**: failing cells, too few cells, or a walk that selected none. |
| `2` | A usage error — unknown subcommand, unknown flag, a required flag missing (**`disable`'s `--yes` is one**), a flag left **without a value**, or a malformed numeric value. Every one of these is decided **before the database is opened**, so a typo'd subcommand against an unreadable `--db` reports the typo, not the open failure — and a `disable` refused for want of `--yes` provably wrote nothing. Usage on stderr. |

The `verdict` line disambiguates the two meanings of exit 1: a health failure
always prints one, a runtime failure never does.

### Worked session

```bash
$ atx-vol-surface-db info --db /db
root /db
generation 7
symbols 3
symbols_enabled 3
partitions 3
partitions_missing 0
surfaces 9
manifest_bytes 12288
bytes_on_disk 12288
partition 2026-07-01 surfaces=3 manifest_bytes=4096 bytes_on_disk=4096 present=1 created_ts_ns=...

$ atx-vol-surface-db partitions --db /db --key 2026-07-01
partition 2026-07-01 manifest_surfaces=3 archive_surfaces=3 manifest_bytes=4096 bytes_on_disk=4096
surface AAA uid=3061902210 slices=2 bytes=544

$ atx-vol-surface-db query --db /db --key 2026-07-01 --symbol AAA --strike 100 --tenor 0.0821917808
iv 0.24600158507884576
total_variance 0.0049739819064085963
forward 100.26404035644119

# Health gate, sized: nine cells, all mapped, checksummed and evaluated.
$ atx-vol-surface-db verify --db /db --min-cells 9 && echo HEALTHY
partitions 3
partitions_in_db 3
symbols 3
cells_checked 9
cells_ok 9
cells_unmappable 0
cells_non_finite 0
cells_checksum 0
verdict ok
HEALTHY

# After `rm /db/partitions/2026-07-02.atxvsa` -- every cell of that date is named:
$ atx-vol-surface-db verify --db /db
cells_checked 9
cells_ok 6
cells_unmappable 3
fail 2026-07-02 AAA kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
fail 2026-07-02 BBB kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
fail 2026-07-02 CCC kind=unmappable detail=NotFound: SurfaceArchiveV2::open_file: file not found
verdict FAILED
$ echo $?
1

# One byte flipped INSIDE a record: it still maps, still probes to a good iv,
# and the payload CRC names the exact cell.
$ atx-vol-surface-db verify --db /db
cells_checked 9
cells_ok 8
cells_unmappable 0
cells_non_finite 0
cells_checksum 1
fail 2026-07-01 AAA kind=checksum detail=ParseError: SurfaceArchiveV2::validate: record checksum mismatch
verdict FAILED
$ echo $?
1
```

**Operator checklist after a build:** `atx-vol-surface-db verify --db <root>
--min-cells <expected>`, then read `info` for `partitions_missing` and the
`manifest_bytes` vs `bytes_on_disk` agreement. That, plus the build's
`cells_ok` / `cells_failed` check above, is the whole acceptance path — no
Python. Remember what the green means: the bytes are intact and every selected
cell evaluates, **not** that the numbers are right or that the coverage is
complete (see
[what a green `verify` proves](#what-a-green-verify-proves-and-does-not-prove)).

## Sibling tools

The build CLI only **reads** a hive; two Python tools produce and maintain it:

- **`atx-vol/tools/migrate_opra_hive.py`** — converts the old
  `<root>/<symbol>/<date>.parquet` tree into the new `date=*/data.parquet` hive.
  Pure local IO ($0), atomic per date, idempotent (complete date files skipped),
  verifies row counts and schema equality per date, writes a migration manifest
  CSV.
- **`atx-vol/tools/pull_opra_hive.py`** — the v2 Databento pull targeting the new
  layout. Free `get_cost` preflight for missing cells only, hard `--cap` with
  degrade-to-top-N-by-weight, `--dry-run`, DBN cache, atomic writes, spend log.
  One `get_range` per date over the union of missing parents; output is the
  merged date file (per the resume/merge rule above).
