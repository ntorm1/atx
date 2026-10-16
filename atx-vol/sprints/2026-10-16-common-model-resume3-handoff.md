# Common American model: stopped-work handoff, October 16, 2026

The user explicitly requested **stop and write a handoff file**. The long-running
goal is **paused**, not complete. Resume only on a new user instruction. Do not
interpret the earlier continuous-work request as overriding this stop.

## Start here on an authorized resume

- Integrated code checkpoint: `b2b979c17ed93ec447e8b59f33c2202f46174245` on
  `feat/vol-resume3-20261016`. This handoff is committed after that checkpoint.
- Root worked in `C:/atx-wt/pool-33`, never the main `C:/atx` checkout. Its lease
  RunId was `vol-20261016-root3`, heartbeat keeper17564. Final lease/process
  disposition and unfinished agent checkpoints are appended below.
- A stable copy of this handoff is saved outside reusable worktree slots at
  `C:/atx-cache/quote-benchmark/2026-10-16-common-model-resume3-handoff.md`.
  A released pool slot is detached back to its old base and can be reused;
  recover committed work by branch/SHA, not by assuming its directory remains.
- Read this file first, then the linked sprint notes. The earlier
  `2026-10-16-common-model-next-parent-handoff.md` is historical and superseded
  for current state. Do not repeat already completed benchmark runs.
- On resume acquire a fresh leased integration tree at the current branch SHA,
  with a durable heartbeat and unique RunId/branch. Use that tree's absolute
  `scripts/atx-build.ps1` from its own working directory. Never raw ninja or
  `cmake --build`, and never rebuild `C:/atx` accidentally.

## User requirements and working constraints

Build a modular, reusable, high-performance American equity options fitting,
pricing and analytics library, intended eventually for HFT market making and
relative value. The user explicitly requires subagent-driven development,
implementation before tests, and no TDD. Backward compatibility is unnecessary;
existing APIs/tests may change. Routine questions are unwanted.

`srPrc` and `srVol` are evaluation labels only. Native fitting receives the
closed raw contract/quote/underlying/rate/dividend/clock schema. Do not introduce
vendor IVs, Greeks, synthetic spots, model flags or reference values as inputs.
No validation labels have been opened. Preserve mandatory no-arbitrage,
numerical admission, symbol/liquidity policy and bounded resource controls.
Never improve reported accuracy by hiding refused contracts or changing the
comparison population. No default was promoted from development scores.

Finite positive Markov/Snell consistency is not continuous-American accuracy.
The model has deterministic exercise layers, finite boundaries and a
limited-liability cash convention. Aggregate dividend PV cannot recover missing
cash ex-dates. Report approximation and unsupported cases explicitly. No world
speed record, production readiness or qualified latency claim has been made.

Read repository `CLAUDE.md`, `.agents/cpp/agent.md`, `atx-vol/CLAUDE.md` and the
root `.claude/skills/vol-dag/SKILL.md` when resuming. The user overrides its TDD
workflow. Graph MCP tools were unavailable, so rg was used for discovery.
Native collaboration tools were used under the user's explicit delegation
instruction. No pushes, PR publication, main merges or trading actions occurred.

Build presets: `vol-rel-avx2` (Release, AVX2, ThinLTO) and `vol-hygiene` (Debug,
PCH off). Root limited builds to two workers; host timings remain unqualified.
Owning object checks use `check <source.cpp>` or wrapper `build <object target>`;
`check` does not accept object paths. Fresh lease acquisition can report an
unrelated missing-module configure error after successfully publishing the
lease; inspect `.atx-lease` before acquiring again.

Python tool checks use `python -m pytest --noconftest <exact files>`: the unrelated
editable compiled extension resolves the main checkout. Standalone tool checks
must not be described as a compiled-extension or full release suite pass.

## Data and fixed baseline

Source parquet:
`C:/Users/natha/Downloads/OptionIntradayHist_30min_2026-10-14.parquet`.
Size1,916,931,890 bytes;34,011,291 rows; SHA256
`a0e0416f907d00de12620647eb4ec6bd7e08e41367319d70310ebdc8b3d85c7a`.

Reuse frozen `C:/atx-cache/quote-benchmark/20261014-dev200`:
72,348 selected/output rows,63,942 input-eligible, including716 unsupported
adjusted-root rows. Raw `quotes.csv`/`quotes.parquet` and `labels.parquet` are
separate. There are191 standard-deliverable snapshots. Two exceed the4096 raw
market quote cap, two have excessive carry dispersion. Preserve all refusals.

Completed immutable runs and detailed hashes are in
[the dev200 baseline note](2026-10-16-common-dev200-baseline.md):

| Run | Main result |
| --- | --- |
| `20261016-common-auto-funding-v1` |25,014/63,942 prices (39.1198%),18,978 identifiable IVs. Own24,581 price-label matches MAE0.500204628.162/191 fits;22 ordinary rollback-work refusals. |
| `20261016-common-storage-parity-v2` |All72,348 prediction bytes identical to baseline; expanded config and191 non-timing decisions also identical. |
| `20261016-common-midpoint010-v1` |Only midpoint weight0.1 changed. Same price coverage and24,581 scored rows: MAE0.500204628 to0.459036561; containment90.8291% to91.5887%. Price p99 worsened4.02242 to4.06426. |

Midpoint candidate serves19,018 IVs. On the exact18,867-row vol intersection,
MAE2797.746780 to2698.377824bp;111 baseline-only and151 candidate-only vols.
Against the older quote-slice model, common remains worse on their shared1252
price contracts: old0.174061385, default common0.461556331, midpoint common
0.403867558. Coverage gain is not a demonstrated matched-accuracy gain there.

Baseline/storage prediction SHA256:
`3f6965872f72e137956f8a8534adbd0e2755f6804023cec333860e7faf835cd3`.
Fixed storage receipt `20261016-common-storage-parity-v2.csv.run.json` SHA256:
`df85b627e2b4cf991249698840a8f90a0ccdd23183971b8a9310560cfe5a1a77`.
Native storage report SHA256:
`1575729e86f5c9dc2a04c6e19673f01471511d518328628f30791970c1963524`.

Old fixed-eSSVI versus automatic quote-slice figures use different matched
cohorts; do not mix them with common-model intersections. Original raw-label
protocol is separate from the legacy oracle benchmark that can use vendor IV.

## Integrated modules and recent commits

Earlier integrated work includes the owned interval fitter, raw common-market
preparation, concentrated sinh grid, owned liquidity policy, reference adapter,
exact-signature audit comparator, immutable shared model storage, native common
benchmark, durable progress/start receipts, quote-only seed API and fixed
development campaign driver. See `2026-10-16-common-model-resume3.md` and ledger
for the earlier commit chain. Current defaults remain:

- Sparse: one shared sigma, prior0.10,65 states/scale0.20.
- Ordinary: three shared state knots, prior0.01,129 states/scale0.10.
- Dense: five state knots per positive variance interval, prior0.001,
  129 states/scale0.10. Many expiries can exceed its parameter cap.
- All use interval loss and zero midpoint weight; explicit diffusion is default.

Recent integrated checkpoints:

| Root commit | Purpose |
| --- | --- |
| `5a2ac006` |Campaign review fixes: reserved `policies` trial name and durable Polars evaluator failures. Independently approved;22 checks. |
| `b2909ede` |Reviewed opt-in positive variance resolvent, author `1a2407321a2e929801e4b36f3a280c216c014f36`. |
| `340ae585ed68be2fea61d7c9c34fab290256e205` |Permanent resolvent tests, native policy/audit/probe selection and weighted-work diagnostics. Independently approved. |
| `87caebad`, `7a91141c` |Frozen E0/R104/R416 development plan/policies; local JSON LF attributes. |
| `8d4f9354` |Approved native quote-seed initializer, author `c9a4ad094521adbec964842d9c9a7b392941eeb0`. |
| `b2b979c1` |Permanent native seed helper registration and merged validation/docs. |

The resolvent is `(I-hQ)^-1` for the zero-drift birth-death variance generator,
implemented by two positive normalized Thomas sweeps and their transpose.
Calendar discount/carry/exercise times stay deterministic: this is not Carr's
continuous randomized-maturity American engine. Immutable factors are shared
only for exact variance-step bits within an interval. Explicit arithmetic and
default fingerprints are preserved. Prepared scalar/batch pricing allocates no
ordinary/aligned C++ storage on the measured panel.

Rows and work are separate: N logical rows per transition; rollback work N for
explicit/cash and2N for resolvent. Default max_transition_work8million, hard
16million; existing row/state/fit caps unchanged. PWL refuses resolvents.
Compile checks action on1 and original stock states, with fixed1e-12 per-state
mean tolerance, positive forward mass and no clipping/renormalization.
Independent review found and fixed an extreme-range/subnormal normalization
bug before integration. Do not replace direct-state probing with globally
normalized states. Detailed derivation/receipts:
[resolvent sprint](2026-10-16-american-markov-resolvent-sprint.md).

Native policies accept `diffusion_method` (`explicit-euler` or
`variance-resolvent`) and `max_transition_work`. Fit reports include physical
factor/storage counts, original-state/mass residuals, per-interval actual clock
steps and `quote_prefix_rollback_work` for one evaluation of the retained model.
This is not summed work over all optimizer trials. Audit accepts
`--diffusion-method`; the standalone storage probe accepts `--method` and an
optional `--concentration-scale`, preserves fixture defaults if omitted and
retains individual compile/grid refusals.

The seed estimator in `american_markov_seed.hpp` uses bounded raw near-forward
positive-bid bid/mid/ask Andersen-Lake inversions with cumulative r/q and variance
clock. It refuses dated cash, IV-floor/unusable cases and incomplete forward
variance support. Shared seeds aggregate equally by positive variance endpoint;
interval seeds use conservative variance differences. No flooring/projection.
State knots receive flat initialization, not an inferred smile.

Native `initialization:{mode:"quote-seed",seed:{...}}` is opt-in. The private
`quote_bench_common_seed.hpp/.cpp` owns initial/prior vectors, materializes the
old configured prior before seeding and applies only complete in-bounds vectors.
Estimator errors/incompleteness retain configured initialization. The fitter
always receives all original quotes; failed seeded fits never retry. Default
configured initialization keeps the original numerical path. Diagnostic/error
arrays and iteration budgets are bounded. See
[native seed contract](2026-10-16-native-common-quote-seeds.md).

## Validation status

Parent focused Release gates passed60 pricing,16 fitter,14 seed and13 policy
cases. Resolvent seam initially passed101 native/audit checks, no skips; seven
owning integration objects compiled PCH-off. Final probe edit was rechecked.
The merged seed/resolvent native lane then passed107 cases; wrapper/campaign
passed110 cases, no skips. Four native source objects are current/pass PCH-off.
No source changes followed those gates except CMake registration/docs.

Latest parent receipts under `C:/atx-cache/quote-benchmark/`:

- `native-seeds-parent-20261016-merged-v1.xml`, SHA256
  `4e3e4f6f681121c49f2f02c6c804609303f086f9e6f3f05a65671fc8370a22d7`.
- `native-seeds-parent-20261016-wrapper-v1.xml`, SHA256
  `257025cdee2bcc1e6e2f3a7c7ed6d5c274acb0b1d0cd77c9eee9c98d613cb549`.
- `resolvent-seam-20261016-native-v2.xml`, SHA256
  `f48e712d963ce1c9c31281e20e9f5950c063276e231b10f38cf5e11d5aa45e14`.
- `resolvent-seam-20261016-probe-check-v1.json`, SHA256
  `ea8f34e96633f9688427f0cd9dc2cd9f4e7136afdc3106f6fd588e9f1fddaeea`.
- Independent seam review `resolvent-seam-340ae585-independent-review-v1/review.json`,
  SHA256 `7f5a2f6dd58ddd5e32a4b11a1b36b2ca09bfcc6dc82a4ec722ede69263657925`.
  Fifteen focused checks and a separate two-expiry prefix-work reconciliation.

Preexisting full-library issues remain: umbrella fixture-path scan flags five
absolute literals; catalog foreign-key test expects a different error mapping.
Only affected public-header inventory checks passed earlier. Do not claim a
passing umbrella, full fast suite, compiled extension or release qualification.

## Frozen binaries and next experiment

Do not overwrite any archive or run artifact.

- `common-auto-baseline-20261016-bin`: original default native SHA
  `8da12138b5a7718ac98fb488654fc99c4535e8a886a0eb55ebf1397a68184825`.
- `common-storage-progress-20261016-bin`: native SHA
  `d5a3d5e6f9f6aac4cb2dae58f225c2071677aba64cc85ddac7fb4e19f00593d0`.
- `common-resolvent-seam-20261016-bin`: frozen source340ae585, three tools and
  thirteen runtime DLLs. Native quote SHA
  `1caee582623e3284e5add50115ced5e28e2ea6af2064427faaf64637001fbdfd`.
  `build-provenance.json` explains that native sources were built at matching
  precommit content while CMake engine metadata still said b2909ede-dirty.

The latest root build executable includes seeds and is **not** the frozen
seed-free resolvent experiment executable. No separate seed-enabled full-data
experiment has been declared or run.

Approved [resolvent experiment plan](2026-10-16-common-resolvent-development-plan.md)
and policies are in `bench/experiments/2026-10-16-common-resolvent/`. E0 is
unchanged explicit auto; R104/R416 change only method and both maximum clocks
to1/104 or1/416. Priors, initial values, grids, classifier, midpoint0 and all
caps stay fixed. No quote seeds. Candidate clocks must not be retuned from labels.
Check policy hashes against `raw-feasibility.json`; after sequential
cherry-picks the parent restored exact committed LF bytes and verified hashes.

Run six predeclared synthetic controls at sparse65/.20 and ordinary129/.10,
upper4, clocks1/104,1/416,1/1664. Then a standalone E0 full dev200 run must match
the original prediction CSV byte-for-byte before either candidate launches.
The fixed two-trial campaign driver has no built-in parity gate; enforce E0
first. All runs need immutable fresh output/start receipts and journals.
No automatic resume, overwrite, holdout evaluation or default promotion.

Raw-only cap-feasible upper bounds: R104184 snapshots/45,070 rows; R416176/
34,528. R416's eight work refusals total10,542 rows and already failed E0.
These are work-feasibility ceilings, not predicted successes. E0 has17 fitted
snapshots above1% terminal upper mass, max0.1530446764; boundary error can
dominate. Report upper and zero mass as largest-expiry unconditional exposure,
not per-contract probabilities or price-error bounds.

Report whole72348/eligible63942 coverage, price/IV masks separately, exact
E0/R104, E0/R416, R104/R416 and three-way intersections, failures/unmatched
successes, own/shared quote containment and interval errors, raw strata and
boundary exposure. Never compare own-population MAEs as if masks were equal.

## Numerical-reference findings worth preserving

Independent QuantLib1.44 reference environment:
`C:/atx-cache/american-reference-venv/Scripts/python.exe`. Reference tools retain
per-contract durable SQLite checkpoints and exact full numeric signatures.
Separate stable/unresolved/unsupported statuses; no aggregate excludes failures
silently. State/grid/clock refinement changes error cancellation.

- Broad18 American funding/unequal-clock/high-vol rows are stable after a
  dedicated high-vol refinement. At domain8 and4800 steps,129/161/193 states
  give broad18 MAE0.001074/0.000592/0.000395. Finer time alone on a coarse grid
  worsened some American errors. See `2026-10-16-domain8-controlled-audit.md`.
- Resolvent author panel:120 supported ATM rows stable after refinement;
  48 local-vol/cash-collapse rows explicitly unsupported by the reference.
  At129 states/upper4, max-clock1/4 to1/1024 reduces martingale American error
 0.457821 to0.004246, high-vol1.187871 to0.012660, dated-cash0.229334 to0.003228.
  Coarse-step error remains material; no automatic method switch is justified.

Primary research already read: SpiderRock LiveVolSurfaces/OptionPricing docs,
QuantLib1.44 concentrating mesher and implicit Euler sources, Carr1998,
CTMC American valuation (Eriksson/Pistorius), Itkin hybrid-dividend work,
American nonlinear-stencil acceleration, Andreasen/Huge European interpolation,
Achdou American inverse problems. Links and interpretation are in sprint notes.
European interpolation speed claims do not establish American HFT latency.

## Pending agent lanes and shutdown receipts

All three agents stopped. No pending lane is integrated or approved merely
because it has a WIP commit. No new tests, benchmarks or research were started
while completing this handoff.

### common_market: experiment paused before E0

Pool36 was already released. No owned process or exec session remains.
Six synthetic runs finished before stop:42 models/3696 prices, zero prepared
allocations, scalar/batch bits equal, moment/mass residuals within1e-12 and
decoded-price no-arbitrage checks passed. **E0 never started. Neither raw-data
candidate nor new vendor-label scoring nor a new reference solver started.**

Artifacts:
`C:/atx-cache/quote-benchmark/common-resolvent-controlled-20261016-v1/`.
This contains frozen policies, exact340ae tool-source copies, six raw probe
outputs with per-run receipts, `analyze_synthetic.py` and
`synthetic-analysis.json`. The latter SHA256 is
`b55860e2ce37398f69aad88f6b606ecf6e89b4fc23b247273aadd8c4c0470a60`.
`frozen-plan.json` SHA256:
`a28da2e8f9e4f91ab6bd3c433c204b5c0acad6e5be0be8b9cc06f4ff9b246fb1`.
Detailed stop note `STOP-HANDOFF-20261016.md` SHA256:
`969aadba3c94aaa5d0bd8a74fa6658cd774fec0201c172e714f4735c9f566f24`.

The completed analysis matches saved independent references by full numeric
contract/interval signatures and retains unsupported/missing rows. Parent has
read the handoff but has not reviewed all3MB of analysis. At ordinary129,
high-vol European max errors for104/416/1664 are0.0617286/0.0288563/0.0206347;
short-low-vol errors0.0191308/0.0190206/0.0189924 show persistent spatial error.
These do not authorize a different frozen raw clock or a default promotion.

### concentrated_grid: time-knot implementation is incomplete WIP

Preserved **unintegrated** commit:
`7f466d4d2ba435965c5c034c9bb4547d8acf736c`, branch
`feat/vol-markov-time-knots-20261016`, frozen baseb2909ede. Pool35 released and
heartbeat stopped. All builds completed before stop. Eight files are committed;
shared CMake changes were restored. Only three owning Release source objects
compiled. **No postimplementation tests were written or run, no candidate
parity/PCH-off gates, and no exact-source independent review occurred.**

The design was approved, but the code is not integration-ready. It adds explicit
`VarianceTimeKnots`, `variance_time_knots`, owned interval interpolation weights
and optional time-basis diagnostics. It replaces `interval_groups()`, so old
fitter tests and the parent's native JSON consumer will fail to compile until
migrated. Parent owns native parse/expanded-policy/output/seed-layout provenance
and permanent registrations. No backward compatibility shim is required.

Controls are absolute variance-year coordinates, log-sigma interpolation at
each original interval midpoint, flat tails, exact equal-control shortcut and
no schedule splitting. Paused intervals have inactive metadata. A bounded
streaming bidiagonal QR uses all positive-variance intervals through the latest
quoted expiry; fixed reciprocal-condition threshold1e-12, common sqrt(M)
normalization, support/norm diagnostics, no prior rescue. This tests the
represented interval map, not option-price identifiability. Time roughness
remains an index penalty. Interval-times-state scratch is preflighted against
the model volatility budget. Seed estimation explicitly refuses the new shape.

Continue from the WIP's
`atx-vol/sprints/2026-10-16-american-markov-time-knots-wip-handoff.md`.
A stable extracted copy is also saved at
`C:/atx-cache/quote-benchmark/2026-10-16-american-markov-time-knots-wip-handoff.md`.
Its pending list includes independent dense QR/conditioning checks, weak/rank
deficient columns, exact knots/tails/paused clocks, legacy operator/fit parity,
many-expiry caps, owned lifetimes, prior semantics and synthetic term recovery.
Existing8-case baseline receipt/source live in the cache as
`markov-time-knots-20261016-baseline.json` and `...-parity.cpp`; hashes are in
that WIP handoff. Do not call baseline capture a candidate test pass.

### reference_audit: sensitivity design only

Seed code was already integrated and its pool34 released. No active lease,
process or sensitivity implementation exists. Unfinished draft:
`C:/atx-cache/quote-benchmark/2026-10-16-markov-sensitivity-design-draft.md`,
SHA256 `5a743e728484864c8916c388466b49632ad7cd5f8fbca07957e6fb26ed1ba4c2`.

Tentative smallest lane: prepared scalar resolvent-only log-vol gradient with
frozen geometry/cash/carry, owned bounded tape/workspace; no optimizer change,
general spot Greeks or recalibration-response claim. Reverse normalized-factor
derivation is unverified. Weighted node-control binding, zero amplitudes,
exercise ties, tangent versus adjoint implementation and exact resource caps
remain undecided. Primary-source links are in the draft; no new research should
be inferred as an approved implementation plan.

### Resume order and final shutdown

Once explicitly resumed, lease a new root tree from the preserved integration
branch. Review the completed synthetic evidence, then run E0 byte parity and
the already frozen R104/R416 queue using archived seed-free binaries. In
parallel, re-lease the time-knot WIP, complete its postimplementation checks and
independent review before integrating; merge the parent's newer policy work-cap
validation rather than losing it. Only then add the native time-knot seam and
its owning checks. Independently review the sensitivity design before coding.

The root had no owned benchmark/build process at stop. Its only final changes
are this handoff and a ledger entry. The normal root lease release follows the
documentation commit; its actual receipt, final branch commit and external
handoff hash are written to
`C:/atx-cache/quote-benchmark/2026-10-16-common-model-resume3-shutdown.json`.
All source work remains in Git branches; main is untouched. The goal stays
paused after shutdown.
