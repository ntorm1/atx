# `feat/pipeline-m` production review remediation

Date: 2026-07-26
Source review: `2026-07-25-pipeline-m-code-review.md`

## Decision

The production review's correctness, integrity, availability, scale, and product-contract
findings are remediated in this worktree. All C-series findings, all P-series findings, and
F-1 through F-7 have implementation changes and focused regression coverage. F-8 is no
longer represented as completed work: the remaining measurement- and fixture-dependent
items are explicitly deferred below with their acceptance conditions.

No timing result recorded during this remediation is a citable performance claim. The host
was shared. Performance findings were closed with bounded-memory/complexity changes,
algorithmic counters, structural routing assertions, or corrected measurement plumbing.

## Correctness, integrity, and availability

| Finding | Resolution |
|---|---|
| C-1 | Projected VaR resolves and sizes the immutable book at the last/as-of snapshot; route artifacts carry as-of and book identity. |
| C-2 | One shared multiplier-aware conversion now defines dollars of vega per vol point. |
| C-3 | Backtest/report data separates absolute gross vega from signed net vega. |
| C-4 | Vol-tick spread and market impact remain separate additive friction lanes. |
| C-5 | RunArchive carry identity includes exact spec, universe, manifest, trade schedule, dividend ledger, referenced archive content identity, schema, and binary version; any dependency change starts a fresh archive. |
| C-6 | Benchmark rows retain dates and require a finite, unique, ordered exact date join. |
| C-7 | Python option chains own a shared mutex; GIL-released readers use shared access and mutations use exclusive access under a documented lock order. |
| C-8 | Price frames carry portfolio provenance; bucket reduction rejects size, identity, revision, order, and key mismatches before modifying totals. |
| C-9 | SurfaceDb drain waiters also observe scheduler completion/error, so launch/setup failure wakes and returns a bounded error. |
| C-10 | Incremental SurfaceDb rewrites merge successful replacements and retain byte-identical prior cells after failed refits; destructive replacement is explicit. |
| C-11 | Corpus manifest/quality pairs use generation commit markers, durable publication, and restart recovery for abandoned checkpoint and final-index generations. |
| C-12 | The public report entry point prefers `run.atxrun`, matching the shipped CLI output. |
| C-13 | Loose TSV input records `columns_present`; missing required economics fail and optional omissions render unavailable rather than zero. |
| C-14 | Projected-VaR output is a staged data-first/summary-last generation with strict schemas, identities, dates, row counts, finite values, confidence levels, and recomputation checks. |
| C-15 | Projected VaR consumes the typed side and multiplier construction knobs. |
| C-16 | Report prose renders the configured delta band. |

## Performance and scale

| Finding | Resolution |
|---|---|
| P-1 | Projection loads the anchor first, uses sealed UID subsets, evaluates through a bounded provider, and caps live historical snapshots. |
| P-2 | Production OPRA ingestion loops outside `load_opra_daterange`: load one bounded date window, record evidence, fit/checkpoint, then release it. |
| P-3 | Scenario-grid exact scratch is compact `exact cells × successful unique contracts`; shape/task products are checked before allocation. |
| P-4 | Risk buckets maintain a key-to-slot index and preserve serial accumulation order, reducing lookup to `O(N + B log B)`. |
| P-5 | Framing-only checkpoint resume uses mapped archive opening and does not allocate/read a copied whole archive. |
| P-6 | The fitting benchmark times only `fit`; construction and admission/report inspection are paused, and phase counters average every admitted iteration. |
| P-7 | Corpus phase output records direct fit-fanout process CPU; the occupancy probe uses `fit_fanout_cpu_s / fit_fanout_s` instead of charging ingestion CPU to fitting. |
| P-8 | Generic American batches route homogeneous call and put chunks through side-native kernels with bounded scratch, ordered scatter, and status parity. |
| P-9 | A missed UID subset returns a timestamped, symbol-aware empty snapshot and materializes zero unrelated record bytes. |
| P-10 | Writers reserve unique same-directory temps, serialize same-destination publication, fsync the file and POSIX parent directory, and retain recoverable temps on final failure. |

## Product surface

| Finding | Resolution |
|---|---|
| F-1 | Corpus cash dividends publish as an authoritative, fingerprinted `share_dividends.tsv`; both production replay configurations load/deduplicate it into `FinancingConfig::share_dividends`, conflicts fail closed, RunArchive freshness includes it, and Python exposes the schedule. |
| F-2 | G2 now includes provenance-safe P&L buckets for every `PnlTotals` field and portfolio-level per-event `dP/dDiv` with explicit units, status, coverage, validation, and deterministic ordering. |
| F-3 | Python exposes validated term `YieldCurve` construction and the supported value-owning `CurveConfig` tree. |
| F-4 | Python exposes the material dispersion/listed/PIT/backtest controls and result fields, including liquidation NAV, absolute gross vega, reconciliation, and entry slippage. |
| F-5 | Surface grids expose family and per-Greek validity/status; American batch unsupported-regime status is distinct from `NotImplemented`. |
| F-6 | The shipped `build-schedule` uses the audited selection builder and durably publishes per-date `quote_rejects.tsv`, including no-basket attempts. |
| F-7 | `atx-build.ps1` preserves configure argv, invokes argument arrays without shell reparsing, defaults CTest evidence to serial, and has a machine-readable dry-run assertion. |

## F-8 acceptance boundary

The following items are deliberately **deferred**, not reported complete:

- A5 sigma-node/cache sampling remains a default-off optimization proposal. It is not
  required for correctness; enabling it needs an independently reviewed economic-golden
  decision.
- A6 timing evidence, the B7 platform baseline JSON, and a fresh T1 utilization number
  require a reserved quiet host. This shared-host remediation makes no throughput claim.
- B4/B6 selector/default-policy follow-ups remain outside the supported production
  contract; current effective policies are explicit and tested.
- Production-feed staleness remains unevaluable until a feed carrying the required
  timestamps is available. The acceptance report must continue to say “not evaluated”.
- The approximately 19 MB populated RunArchive fixture remains external. Archive-native
  report behavior is covered by source-built synthetic tests; the five heavyweight
  fixture tests continue to skip loudly when the explicit fixture is absent.

The counters gap is closed: `dev-counters` has a dedicated `build-counters` directory,
forces `ATX_VOL_COUNTERS=ON`, and has a serial `atx_vol` CTest preset. Its first run
reproduced the 12 documented counter failures; the missing gated boundary-solve bumps in
the American SIMD call/put lanes were repaired, and the complete counters lane then
passed.

## Validation

Validation was performed from `C:\atx-wt\wt-pipe-m` only.

- Default combined build: passed.
- Integrated changed-seam CTest set: 131/131 passed; one pre-existing diagnostic disabled.
- Python source-pinned suite: 188 passed, 5 expected external-fixture skips, 0 failed.
- Build-helper dry-run argv test: passed.
- Optimized fitting benchmark structural run: multiple admitted iterations were averaged
  (`phase_samples` greater than one); no timing claim is made.
- Counters-enabled serial `atx_vol` gate: 2,318 counted tests, 0 failed
  (730.38 seconds).
- Default serial `atx_vol` gate: 2,320 counted tests, 0 failed
  (672.10 seconds).
- Unfiltered 5,843-test repository run: stopped at user direction after more than 2,500
  tests with no observed failures. This partial run is not represented as a passed gate;
  the complete component, counters, Python, and changed-seam gates above provide the
  required remediation evidence.
- `git diff --check`: passed (line-ending notices only).
