# ATX server and research workspace

Build directly in `C:/atx`; preserve existing research changes. New C++20
`atx-server` owns SQLite persistence, a bounded read-through memory cache,
durable scheduled jobs, bounded event channels, and the typed gRPC service.
`atx-front` is a dense docking/tiling browser workstation, inspired by the
provided QuantLab reference. A same-origin Node gateway translates browser
HTTP/SSE to gRPC; it never owns research state or executes calculations.

## Work ownership

- Server agent: core persistence/cache/scheduler/events, gRPC handlers, server
  entry point and tests, module CMake.
- Frontend agent: React/TypeScript docking UI, browser gateway, frontend tests.
- Research agent: validated structured artifact import, engine integration,
  explicitly labeled demonstration research and adapter tests.
- Root: shared protocol, root build/dependencies, integration, security review,
  end-to-end validation, operational documentation and launch tooling.

## Acceptance

- Native C++ gRPC server builds with repository warning gates.
- Jobs persist through restart, are bounded, cancellable, scheduled and
  idempotent. Interrupted work cannot be misreported as completed.
- Cache and event buffers have explicit capacity, thread ownership and expiry.
- Web workspace supports movable tabs, split panes and saved layouts with
  compact typography/spacing and accessible controls.
- A browser-submitted research request traverses the gateway and gRPC service,
  executes through the research adapter, persists results and updates panels.
- Real artifact results retain source/evaluation provenance; demo output is
  visibly synthetic. Missing data is never replaced with invented evidence.
- Authentication, safe binding defaults, request limits, shutdown/recovery,
  focused tests and runnable operator instructions are included.
- Database viewing uses a provider-neutral native API and dockable browser panel.
  The DuckDB adapter browses the actual atx-db serving catalog read-only, preserves
  exact numeric/NULL values, permits only configured lake roots, bounds query
  resources and releases connections between operations.
- UI styling follows a classic desktop research workstation: flat surfaces,
  neutral colors, square borders, compact controls and restrained typography.

The initial topology is a single service process with local SQLite storage;
horizontal database writers and distributed scheduling require a different
storage/lease design and are not implied by this implementation.

## Continued platform objective

The follow-on goal is an end-to-end equity long/short team research platform,
including named workspaces. The initial infrastructure acceptance above does
not close this broader goal. The implementation remains in this checkout with
three GPT-6.1-sol agents, coordinated by subsystem ownership.

| Capability | Required behavior and evidence | Current state |
|---|---|---|
| Workspaces | Durable named layouts and panel state; database-focused template; concurrent-save conflicts; restart and browser tests | Qualified and deployed on localhost |
| Data discovery | Provider-neutral catalog plus research dataset/field descriptions, immutable revisions, coverage and eligibility limitations | Read-only catalog and pinned research registry deployed |
| Alpha language | Same compiler policy drives editor validation, operator documentation and execution; bounded causal windows | Compiler, 35-operator catalog and editor validation deployed |
| Research library | Named hypotheses, immutable recipe revisions, tags, provenance and run links; no historical recipe overwrite | Qualified and deployed, including saved-revision execution |
| Real simulations | Registered checksum-bound TRAIN panels, causal fields/universe, delayed execution, explicit costs and result accounting | Published TRAIN HTTP/gRPC and browser qualification passed; deployed |
| Experiments | Bounded multi-simulation batches with parameter grids, repeatable recipes, job progress, cancellation and restart behavior | Atomic batches, composer, cancellation and scheduled restart qualified; deployed |
| Analysis | Aligned daily-return comparisons, correlation, yearly/rolling evidence and sensitivity checks; distinguish missing data and invalid evidence | Exact-period comparison deployed; canonical manifest-pin correction awaits upgrade |
| Portfolio research | Reproducible alpha combinations with constraints, turnover/cost accounting and risk diagnostics | Qualified on published TRAIN data and deployed, with hard-bound diagnostics and explicit identity geometry |
| Review and promotion | Immutable audit trail and explicit research/review/paper states with evidence requirements | Immutable reviews/audit, independent authorship guards and team browser workflow deployed; paper admission next |
| Paper operation | Scheduled target generation, position/fill ledger, reconciliation and monitoring using explicit simulated execution | Required |
| Teams | Identity, projects, membership and role permissions; actor attribution and shared/private resources | Individual identities and cumulative roles deployed for one shared project; private tenant isolation remains outside this topology |
| Operations | Health, bounded resources, durable schedules, data/runtime provenance, backup/recovery and operator documentation | Foundation shipped; recurring workflows and operations panels required |

Existing repository holdout restrictions remain enforced. Reading a warehouse
table is not evidence of point-in-time certification. Data receipts must state
which properties are verified, modeled or unknown. Live broker order placement
requires a separately configured execution integration; paper operation must
never misrepresent a simulated fill as an exchange fill.

Qualification proceeds by vertical slice: native contracts and regression tests,
gateway error/cancellation coverage, browser interaction against actual service,
then restart the localhost workstation while preserving its SQLite data. Each
slice updates the status and records practical limitations. Completing this
goal requires auditing every row above and the user's full instructions.

## Integration record

- Debug and optimized Release builds and all 50 CTest cases pass in each:
  25 research, 19 core and six
  service/transport cases, including actual authenticated loopback streaming.
- NAV cancellation extension: four new tests plus twenty existing replay tests pass.
- Frontend: strict production build, formatting, 18 gateway/fan-out tests and all
  six Chrome browser cases pass, including the actual native research lifecycle.
  The production dependency audit has zero findings.
- Black-box actual HTTP/gRPC gate passes: engine execution, idempotency/conflicts,
  authentication, 24 concurrent cached reads, scheduling/cancellation, exclusive
  database ownership, traversal refusal and SQLite crash/restart durability.
  Forty-eight SSE clients receive events both before and after native restart.
  Release passes all thirteen integration checks over both plaintext loopback and
  verified TLS, including refusal of insecure external binding before DB creation.
- Review corrected subnormal-return histogram indexing, malformed manifest numeric
  conversions, contradictory NAV/return evidence, summary/detail UI loading,
  startup worker admission and scheduled-job fairness; regressions pass.
- Windows protobuf 6.33.4 needs a generated clang-cl/MSVC restrict-annotation ABI
  overlay. It is version/anchor checked and changes only service include paths.
  Server dependency installation is isolated at `deps/vcpkg-server`, because base
  manifest configuration removes optional gRPC packages from a shared install.
- Native transport has a 96-thread quota and 128 MiB gRPC buffer/reclamation
  budget, independent of the engine/cache/SQLite memory budget.
- Live browser execution verifies edited DSL, metrics/curves/histogram, 840 trade
  rows, provenance, scheduled cancellation and result retention after refresh.
  Draft and selected-run state are explicitly distinguished. SVG axes retain
  legible typography in narrow docked panels.
- Launcher Ctrl+C cleanup verified: both child listeners and the native process
  exited. The optimized service then reopened the same database with all six
  browser-test jobs preserved (three successful and three cancelled).
- Release workstation is running on `http://127.0.0.1:3000`, native loopback
  `127.0.0.1:50051`, using `atx-server/var/workspace.db`. Screenshot:
  `atx-front/artifacts/workstation-live.png` (generated, gitignored).

Qualification commands:

```powershell
powershell scripts/atx-build.ps1 -Preset server-dev -Ctest -L atx_server
powershell scripts/atx-build.ps1 -Preset server-rel -Ctest -L atx_server
python scripts/test-atx-workspace.py --server build-server-rel/bin/atx-server.exe --tls
npm --prefix atx-front test
npm --prefix atx-front run build
$env:ATX_E2E_LIVE = '1'
$env:ATX_E2E_BASE_URL = 'http://127.0.0.1:3000'
npm --prefix atx-front run test:e2e
```

The codebase graph MCP tools were unavailable in this session. The browser tool
failed during its bootstrap; Chrome/Playwright is the local visual test fallback.
All native builds use `scripts/atx-build.ps1`; new service targets compile without
PCH. Original unrelated working-tree changes remain outside this work.

## Database viewer extension

The current atx-db data source is `atx-db/data/catalog.duckdb`; the legacy
`warehouse.duckdb` has been retired locally. Read-only inspection found 59 views
and two base metadata tables. Every view's external Parquet path lies under
`atx-db/data/alpha_panel/v1`, so one narrow operator-configured root is sufficient.
No warehouse files are created, migrated, copied, checkpointed or modified by
the viewer. Python DuckDB is used only to create isolated integration fixtures;
the production service uses the pinned native DuckDB 1.5.1 C API.

Native providers own their connection configuration. The shared protobuf exposes
source/capability listing, flat catalog objects, preview pagination and exact
typed string cells, allowing future provider adapters without database-specific
browser code. Only the DuckDB adapter is implemented in this extension.

Qualification of the extension:

- All 69 native CTest cases pass in the optimized Release build, including
  17 database-provider tests and two additional service authentication/empty-source
  cases. The 17 focused database tests also pass in Debug.
- The database black-box gate passes 15 checks over verified TLS using the
  optimized executable. It preserves BIGINT/DECIMAL, NULL, embedded NUL and UTF-8
  text; bounds rows and payloads; rejects writes and external reads; recovers
  from another process's writer lock; cancels work on browser disconnect; and
  leaves the fixture database hash unchanged. Evidence is retained beneath
  `.cache/database-integration-51xcl9lw` without the temporary private TLS key.
- The existing research black-box gate still passes all 13 checks over verified
  TLS, including 48 SSE subscribers before and after native restart.
- The Release workstation was restarted cleanly with its existing SQLite state
  and the configured `atx-db` source. Actual HTTP catalog browsing returns 61
  tables/views; the calendar view returns real Parquet-backed observations.
- The viewer is available directly at `http://localhost:3000/?panel=database`.
  Its neutral source/catalog/result API carries exact cell text and separate NULL
  and truncation flags. Provider capabilities can omit catalog/schema namespaces.
- All 24 gateway/channel tests and all 11 browser tests pass, with both native
  research and database checks enabled against the running optimized service.
  TypeScript, production assets, formatting and the production dependency audit
  pass. The audit reports zero vulnerabilities.
- Final Chrome screenshots show the actual catalog/calendar preview and native
  synthetic research results under the classic styling:
  `atx-front/artifacts/workstation-database-live.png` and
  `atx-front/artifacts/workstation-live.png`. Database tabs split, persist and
  reload correctly. The grid keeps a bounded number of visible DOM rows.
- Review found and corrected the DuckDB legacy C API's embedded-NUL conversion
  loss using modern VARCHAR chunk vectors. A separate adaptive SQL rendering
  bound prevents wide results from materializing unbounded output text before
  the response budget is applied.

The frontend additionally preserves source/preview identity on repeated selection,
virtualizes wide result grids, advances truncated preview pages by the number of
rows actually returned, and distinguishes a row limit from a text/byte limit.

## Portfolio integration

This is the next integration contract after the pinned TRAIN `real_dsl` slice is
qualified. It describes proposed work, not implemented server capabilities.
Completed NAV comparisons remain research analytics. A portfolio must evaluate
its constituent expressions on the same decision-time panel, combine their
signals, allocate one actual book, and replay that book's cash and holdings.

Reusable native interfaces:

| Interface | Existing behavior and integration boundary |
|---|---|
| `combine::AlphaCombiner::fit`, `atx-engine/include/atx/engine/combine/combiner.hpp` | Fits weights from only `[fit_begin, fit_end)` return rows; `Combination` retains that interval. `IcWeighted` currently uses trailing return Sharpe as a proxy, not signal IC. Fitted weights must apply after the fit interval, wholly inside permitted TRAIN data. |
| `combine::CombinedSignalSource`, `combine/combined_source.hpp` | Combines current constituent cross-sections with frozen weights. Prefer the explicitly versioned `StandardizedFixedGrossV2` rule: missing constituents contribute neutral opinions to fixed gross instead of amplifying survivors. The source borrows constituent lifetimes. |
| `risk::PortfolioOptimizer::solve`, `risk/optimizer.hpp` | Existing dollar neutrality, gross budget, per-name caps and turnover penalty. The basic solver is a projected/proximal construction; richer constraints dispatch to the augmented QP. Do not describe every positive basic `risk_aversion` value as a calibrated risk/return frontier: its documented gross normalization retains a directional tilt. |
| `risk::ConstraintSet::materialize`, `risk/constraints.hpp` | Existing `GrossNet`, `PositionCap`, `GroupCap`, `TurnoverBudget`, `BetaNeutral`, factor bounds and explicit dollar-liquidity `TradeParticipationCap`/`DaysToLiquidate`. Prefer dollar-liquidity inputs for published dollar ADV; the older participation API multiplies share ADV by price. |
| `risk::ConstrainedQpSolver::solve_with_cert`, `risk/qp_solver.hpp` | Returns actual weights and primal/dual/infeasibility certificates. Retain the certificate and independently measure all hard bounds before submitting targets; never silently return relaxed or infeasible targets. |
| `risk::FactorModelBuilder`, `risk/factor_model.hpp` | Existing trailing factored covariance estimation. Its caller must provide a physically truncated history and provenance for exposures; the current server has no admitted dated risk-model artifact. |
| `book::ReplayAllocationPolicy` / `ReplayIntentPolicy`, `book/replay.hpp` | Supplies actual drifted marked holdings, cash, NAV, frozen decision preferences and eligibility at each execution. Reuses the actual replay, cancellation seam and accounting. Explicit `HoldCurrent` preserves units without reconstructing them through a weight. |
| `book::SqrtImpactCost`, `book/replay_cost.hpp` | Prices nonlinear impact/spread/commission and caps actual filled dollars against execution-period liquidity known before the fill. Missing ADV refuses fills. Residual orders are retained and actual turnover/costs recorded. |
| `risk::PortfolioOptimizer::solve_surface`, `risk/optimizer.hpp` | Executable-cost optimization explicitly requires expected returns over the surface horizon. Dimensionless DSL ranks are not calibrated expected returns; refuse this mode until an admitted TRAIN-only calibration supplies horizon, units and source pins. |
| `risk::forecast_trajectory`, `risk/horizon.hpp` | Forecast decay uses only the current cross-section and explicit half-life; no future observations. Keep forecast decay separate from smoothing today's signal with already observed prior signals. |
| `book::DecayController`, `book/decay_monitor.hpp` | Existing realized-performance monitoring against a frozen admission baseline, with lifecycle hysteresis. Its current DSR cross-check is documented as PSR-equivalent; do not advertise a stronger selection correction. |

`book::BookPipeline` is a useful wiring reference, but its documented constituent
sources are scripted doubles. It must not be exposed as real market simulation
without replacing that seam with admitted DSL sources and real dated inputs.
The direct replay/policy interface is the smaller trustworthy integration path.

The first portfolio recipe freezes 2–12 embedded expressions and their exact
hashes; it does not accept unverified alpha/revision IDs. A later library adapter
must resolve immutable revisions on the server. The recipe also freezes one
dataset ID and manifest SHA; one formation cohort;
signal normalization and missing-opinion rule; signed manual component weights;
optional causal signal smoothing; delay/cadence; gross and per-name upper bounds;
turnover budget; and explicit modeled trading/borrow costs. Load the market panel
once. Evaluate only admitted expressions, combine in canonical component order,
and constrain the resulting preference before actual delayed replay. Keep the
existing `real_dsl` contract unchanged; introduce a separate versioned portfolio
mode after its tests pass. Manually specified blend weights require no fitted
performance claim. Learned blends need a separate frozen fit/apply contract.

Initially expose bounded signal construction with dollar neutrality, per-name
caps and turnover limits. A geometric projection may use identity covariance as
constraint geometry only, explicitly labeled as such; it is not an estimated
risk model. Accept sub-unit gross or a flat book when the alpha/caps permit no
larger feasible exposure. Measure achieved net/gross/name exposure, actual traded
dollars, costs, unfilled targets and all assumed liquidations per session. Report
any mismatch between intended and realized allocations; never substitute target
weights for held positions in performance evidence.

Group neutrality requires a new admitted daily group binding. The published
panel producer documents previous-session filing-clock visibility for SIC, but
the server does not yet verify/project that contract. Vendor snapshot `gics`
remains disallowed. Missing classification needs an explicit unavailable/excluded
policy and coverage statistics. Beta/factor bounds likewise require trailing
dated exposures with sufficient TRAIN warmup. Raise effective warmup to cover
all alpha, risk, beta, volatility and smoothing windows; reject unavailable
history rather than padding or crossing the locked evaluation boundary.

The implemented `real_portfolio` / `real-data-portfolio-v1` mode uses the existing
native fixed-gross signal combiner and constrained QP, followed by one actual cash
and holdings replay. Its geometry is explicitly identity signal projection,
with fixed 600 iterations and the native deterministic ADMM schedule: initial
rho 1, equality scale 1000, over-relaxation 1.6, and power-of-two rho adaptations
only at iterations 25, 75 and 150, with early exit disabled. The work admission
also reserves up to three numeric refactorizations. Independent 1e-6 checks
certify every accepted target. Targets are fractions of marked pretrade NAV at delayed
execution; achieved post-cost holdings are reported separately, and subsequent
price drift is not a continuous constraint guarantee. Mandatory missing-price
liquidations precede the discretionary turnover reference and remain separately
disclosed. Compulsory membership exits cannot silently relax hard limits.

All constituent evaluations and solves share a 250-million-unit work admission
budget, with aggregate panel/slot/signal/replay/constraint/factorization memory
reservation before any constituent evaluation. Allocation diagnostics retain at
most the configured result bound and 65,536 instrument cells, with explicit
truncation; complete daily returns remain available. Default scope is 128 names,
five-session cadence and 2020 Q1. Larger requests must reduce names, cadence or
window when the resource admission refuses them. The initial mode has no
estimated risk, industry/beta neutrality, impact, locate or capacity qualification.

The initial published TRAIN gate passed against the isolated native/gateway pair:
128 actual names, 12 warmup sessions and ten allocations in 2020 Q1. Maximum
absolute target net was 2.43e-17, per-name exposure 0.01859 against the 0.02 cap,
and primal residual 2.22e-16. Complete returns, source pins, modeled costs and
SQLite restart durability were verified; these are implementation qualification
results, not evidence of alpha quality or executable live trading capacity.

Do not enable impact/capacity/locate qualification from price and dollar ADV
alone. Spread, daily volatility, borrow availability, corporate-action identity,
source clocks and any surface calibration each need explicit dated inputs.
Preserve `historical_vintage_verified=false`, common-stock/universe-history limits
and modeled delivery clocks until those evidence contracts actually exist.

Required validation before adding the portfolio mode:

- Mutating observations after a decision cannot change earlier blended signals,
  fitted weights, allocations, costs or returns. Train/apply intervals and every
  source/revision pin must remain inspectable.
- Two opposing identical alphas cancel to a flat book; missing opinions obey the
  selected fixed-gross rule; component order and repeated runs are deterministic.
- Binding caps, neutrality and turnover hold on measured accepted targets; an
  infeasible set is rejected explicitly. Current marked holdings, rather than
  stale prior target weights, define turnover and partial trades.
- A membership departure still marks and closes actual held units. Unknown
  groups, guarded returns, missing liquidity and unavailable borrows cannot
  quietly become zero risk/cost or fabricated executions.
- Cost/participation limits affect actual fills and residual orders. Accounting
  reconciles NAV, returns, cash and holdings; all unverified assumptions persist
  in the result and stored experiment recipe.
- Aggregate parser, work, memory and output budgets cover every constituent and
  retained allocation. Cancellation stops data reads/solves/replay before result
  publication. Verify the actual authenticated gRPC/gateway path and restart
  durability against a bounded published TRAIN sample.


### Pure historical paper computation

The public `atx/server/paper.hpp` interface owns every bounded ledger input:
allocation targets and achieved weights, exact old/new TRI units for modeled
fills, unfilled targets, calendar-day financing, explicit mandatory liquidations,
complete valuation/position snapshots and the fixed paper-window calendar. The
backend computes a new, flat-cash window strictly after the caller-resolved
approved evidence ends. It permits only a frozen `real_portfolio` construction;
dates are the only changed recipe fields. The pure adapter does not attest
approval or publish a durable account.

Paper admission permits 1..63 scored observations and at most 128 requested
names, within TRAIN. Existing research still requires at least three scored
observations. The paper-start prior-ADV formation rule selects a new cohort,
which callers must supply exactly, including order, after genesis. Full-window
work admission reserves all components, bounded solver refactors and complete
owned ledger memory. The evaluation panel is physically truncated to warmup plus
the requested prefix before any expression is evaluated; full pinned monthly
files may be verified to establish the unchanged source/calendar receipt.

The clock records completed pre-execution valuation through T. Execution at T
and its subsequent interval are pending until the next advance; no fabricated
future price makes a terminal target fill. Native aggregate trade fees are
authoritative cash debits. Per-fill modeled fees are attribution only, because
summing individually rounded fees can differ from the native aggregate. Borrow
uses the native D365/calendar-day convention. Missing held marks produce
explicit adverse stress liquidations, separate from fills and trade fees;
missing execution marks remain disclosed as unfilled targets.

Exact native sizing snapshots use the engine target resolver, rather than
reconstructing unit changes by dividing cash deltas by prices. Each completed
interval independently checks holdings, cash, fees, financing, liquidation
proceeds and NAV against full native accounting. Prefix tests compare every
owned valuation, allocation, fill and financing row against the same batch
simulation. Runtime content identity, immutable approval/currentness checks,
transactional append-only events and restart/CAS/idempotency enforcement belong
to the account service and must be qualified separately.

## Research-platform qualification, 2026-10-02

The optimized localhost instance was upgraded from SQLite schema 2 to 5 after
an integrity-checked online backup in `atx-server/var/backups`. All ten prior
jobs survived. It now runs a launcher-owned executable snapshot, allowing later
builds without locking the live build output. The atx-db catalog SHA256 remained
`d3a9e7a1aa5448d15c00f232884d67eedacde544173acf8a3b3f5cceb6d0574a`.

- The Release baseline passed 141 native cases, the 13-check research/workspace
  integration gate and the 15-check read-only database gate over verified TLS.
- `scripts/test-atx-platform.py --tls --keep` passed eleven additional actual
  HTTP/gRPC checks: concurrent layout CAS, immutable revisions and run lineage,
  atomic experiment admission, retry after alpha archival, queue capacity,
  scheduled restart preservation, cancellation and revision-checked deletion.
  Evidence: `.cache/platform-integration-u4ko2phh/evidence.json`.
- Published TRAIN qualification ran two opposing DSL recipes over 128 names,
  four checksum-pinned monthly files and 49 exact return periods in early 2020.
  Engine NAV reconciled, comparison aligned complete periods, stale pins and
  locked dates failed, and saved evidence survived native restart. Historical
  vintage, common-stock history, capacity and borrow availability remain
  explicitly unverified. Evidence: `.cache/research-integration-lp2tgwoy/evidence.json`.
- All 27 accumulated Chrome checks passed with native research, database,
  workspaces, TRAIN/library/comparison and experiment flags enabled on isolated
  port 3003. Production TypeScript/Vite builds passed. Actual screenshots are
  `atx-front/artifacts/workstation-train-live.png`,
  `atx-front/artifacts/workstation-comparison-live.png` and
  `atx-front/artifacts/workstation-experiments-live.png`.
- Live review exposed a comparison provenance key mismatch: source documents
  use `dataset_manifest_sha256`. A new failing native regression reproduced the
  missing pins and missing cross-revision warning. The corrected extractor
  retains canonical pins, supports the legacy key and rejects conflicting or
  malformed digests. All seven comparison cases pass in Release and Debug.
  This correction awaits the next native executable deployment.

Portfolio and governance/actor-attribution development follows this qualified
baseline. Team access is initially one explicitly shared local project with
operator-managed individual tokens and cumulative viewer/researcher/reviewer/admin
permissions. It does not imply private projects or tenant isolation. Those new
changes must pass integration and browser gates before the next localhost upgrade.

## Portfolio and governance deployment, 2026-10-03 UTC

The schema-6 Release runs at `http://localhost:3000`. An integrity-checked online
backup preceded migration; all ten job IDs/statuses and the operator's saved
workspace document were compared exactly against that backup after restart.
The warehouse catalog SHA256 is unchanged. The qualified executable SHA256 is
`9c8dedd4f2af3e4b50c466f9a88a369ccf34c1301f6cdfaffec255e8d96614f7`.
Upgrade receipts are `.cache/governance-upgrade-before.json` and
`.cache/governance-upgrade-after.json`.

- All 183 native Release tests, 51 gateway tests and 33 Chrome tests passed.
  The browser suite covered all existing native flags plus individual team
  credentials, review decisions and the default 128-name portfolio builder.
- The prior workspace, durable-platform and DuckDB black-box gates remain
  green over verified TLS. Hardened team/TLS evidence is retained in
  `.cache/team-integration-ykv8p3su/evidence.json`; published TRAIN portfolio
  evidence is in `.cache/research-integration-l0ijixof/evidence.json`.
- A real-panel failure exposed inadequate convergence with constant ADMM rho.
  The adapter now uses the existing deterministic native schedule, with all
  schedule fields explicitly frozen and the receipt derived from those fields.
  Its aggregate reservation includes bounded numeric refactors. Independent
  hard-bound tolerance remains 1e-6; infeasible or uncertified results fail.
  Fourteen portfolio regressions include a volatile 128-name fixture.
- Authorship guards reject original creators, submission requesters, exact
  revision writers and earlier writers of the same canonical recipe, including
  after metadata-only edits. They resolve immutable library history even for
  older review evidence without the new fields. Independent reproduction of
  an unchanged recipe remains allowed. Expired person credentials cannot be
  repurposed as gateway credentials. Native stream faults close browser
  streams, forcing individual reauthentication on reconnect.
- The operator credential generator is `scripts/atx-team.py`; credentials are
  written only into a newly created private directory, separate from the
  digest-only policy. Native and frontend guides document shared-project roles,
  expiry, startup configuration and research-review limitations.
- Screenshots `workstation-portfolio-live.png`, `workstation-reviews-live.png`,
  `workstation-team-review-live.png` and `workstation-team-audit-live.png` under
  `atx-front/artifacts` show the actual classic, compact docking interface.

## Durable automation and historical paper accounts: next slices

Recurring workflows freeze one raw recipe or exact saved-alpha revision. They
use fixed UTC intervals of 60 seconds to 30 days, at most 1,000 admitted runs and
256 definitions. Configuration is immutable; lifecycle pause/resume uses CAS.
One active occurrence per workflow prevents overlap. Admission inserts its job,
unique scheduled-slot occurrence and cursor update in one transaction. Downtime
coalesces to the latest due slot; missed and skipped slots remain visible.
Queue backpressure leaves the due cursor pending. Counts derive from linked jobs.
Each automatic dispatch rechecks the creator's current research permission and
source-alpha availability. Expired, removed or demoted actors and archived source
alphas cause a durable pause. Permission callbacks run outside store/scheduler
locks. Completed workflows cannot reset their lifetime budget through resume.

The first paper account will be explicitly historical and simulated, with new
flat cash and a new window after its approved portfolio evidence ends, entirely
within the existing TRAIN admission boundary. It cannot relabel an old research
curve as paper performance. Construction, costs, limits and dataset pins remain
frozen; the paper-start cohort is selected by the same approved formation rule
and then frozen. Bound the window to 63 scored sessions and 128 names, subject
to the existing aggregate work/memory admission, and advance 1–5 sessions at a
time. The native replay supplies modeled TRI units, fills, fees, borrow, marked
holdings, cash and NAV; these are never broker shares or exchange executions.

Use bounded deterministic prefix replay from that fixed start, comparing every
already committed interval, fill and valuation before appending new rows. The
paper clock means valuation completed through T; execution at T belongs to the
next completed interval. Do not fabricate a future mark to execute a terminal
target. Store a new genesis, ordered hash-linked events and authoritative source,
approval and executable receipts. Cancellation before commit leaves the ledger
unchanged; after commit, idempotent retry returns the committed receipt. Review
withdrawal/staleness, source mutation, code-policy mismatch, failed reconciliation
or solver refusal blocks further advancement. Pausing preserves positions at the
last simulated valuation and does not imply liquidation. These designs remain
unimplemented until their own native, transport and browser gates pass.
