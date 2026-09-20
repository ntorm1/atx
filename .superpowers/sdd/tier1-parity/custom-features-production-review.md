# CF1 independent source review

2026-09-20. Reviewer: fresh Codex agent `review_custom_features`.

Verdict: **CLEAN — no Critical or Important finding.** This is the single
independent source review. Final task acceptance remains conditional on the
implementation commit, serialized migration registration and the implementer's
focused validation report. No second review is required absent a Critical finding.

The reviewed design is the corrected brief at
`cd539da110a4a916a5fe1b9d833c7e3185a4a8d4`: primary horizon 21 market
sessions, secondary 5/63 and a 63-session split embargo. Review covered the stable
new implementation before its commit, coordinated with its owner. The owner's
subsequent immutable-definition and terminal-provenance refinements were included
before closing this pass.

## Scope and evidence

Read `program.md`, `custom-features-production-brief.md`, the five new files below,
and the existing production forward-panel writer/schema and connection transaction
contract. No tests, live warehouse reads, bulk source scans, external LLM calls or
implementation edits were performed. The controller retains the heavy workload
slot for production prices.

Reviewed file SHA256 values:

| File under `atx-db/` | SHA256 |
| --- | --- |
| `src/atx_db/custom_features.py` | `6d49a53f22b4c96094c207213342531ae6eef10479932638b09cf6eab2833cb4` |
| `src/atx_db/migrations/bodies_0313.py` | `36846a5fc500fddcff9ae74a4e470dae6b495502e6da0c555ed96e19974ccc49` |
| `scripts/build_custom_features.py` | `130dac9df0a2664eec81d1a85521c86ae019cb5999a4f70025c110986e577545` |
| `tests/test_custom_features.py` | `ede6e0ce2f1f36088f20be291a891f6c5374c28ca99797111d028a3ad5cd7432` |
| `docs/CUSTOM_FEATURE_RESEARCH.md` | `cb20f07f96e3687ce21b9ae9c9108595d6b5fb58d15358cda9c5e4e6a1716a45` |

## Controls assessed

- Eight formulas and their positive directions are explicit. Calendar-range
  windows and exact endpoint joins preserve missing sessions. Adjusted prices and
  raw dollar volume have distinct roles; the implementation does not infer split
  events from the cumulative total-return factor.
- A decision at T22 UTC uses observations through the previous session and enters
  at the next session close. Maximum stored/input clocks include the conservative
  next-calendar-day noon delivery floor. Rows failing known-by-decision remain
  excluded from research formation. Future fields and current listing backcasts
  are not used.
- The named liquidity/price cohort is explicitly uncertified historical US common
  equity. Feature ranks and deterministic security-ID ties precede the label join.
  Minimum 200-name cross-sections produce at least 20 names per decile; constant
  cross-sections and excluded dates receive explicit status records.
- Realized labels join on entry date and exact calendar endpoint. Latest eligible
  revision selection precedes validity checks, including terminal provenance;
  invalid revisions cannot resurrect earlier favorable returns. Missing labels
  retain their formation denominator, and policy terminals remain separately
  counted. Missing required horizons fail explicitly.
- Train/validation/holdout boundaries purge crossing labels and embargo the first
  63 sessions of new splits. The HAC implementation uses exact session lags,
  including gaps, with lag horizon minus one. Missing or degenerate tests have no
  significance result and remain members of the complete eight-test primary Holm
  family. Secondary horizons cannot qualify a hypothesis.
- All eight features and all three splits/horizons persist results, including null
  and unsuccessful tests. Annual stability, label coverage and explicit four-side
  cost sensitivity contribute to the screening result. Production eligibility is
  separately false with specific listing, adjustment/identity, source-vintage,
  terminal-coverage and execution limitations. The output is described as horizon
  return spreads, without a daily trading PnL or annualized Sharpe claim.
- Build calculations stay in sequential SQL partitions; only compact aggregate
  spread series reach Python. The wide daily table avoids a full-panel ART index.
  Source/version replacement and its manifest publish atomically. Evaluation
  aggregates and their run manifest also publish atomically. Run IDs cannot be
  reused and an altered definition requires a new feature version.
- The CLI uses one DuckDB thread and a 1 GB default, avoids auto-initialization,
  checks the complete pending migration set and requires governed migration 0313.
  No activation/jobs/registry edits were mixed into the reviewed task scope.

## Acceptance limits

This pass does not establish full-universe runtime, peak memory, row counts,
economic adjustment accuracy, historical delivery, historical listing membership,
complete delisting coverage or profitable signals. The supplied price digest is
operator evidence and the adjusted-label basis is the existing production writer
contract; CF1 does not independently certify either source. These limits are
explicit in the implementation and prevent production promotion. The controller
should use the single guarded focused package and later measured production run
already planned, without adding a speculative cleanup or test cycle.
