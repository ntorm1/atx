# FQ2 fundamental signal evaluation result

## Scope

Implemented the bounded `evaluate_fundamental_signals(store, options)` API and
`scripts/evaluate_fundamental_signals.py` entry point. Both require an explicit
complete FQ1 build run, unique evaluation run, label source, as-of date and UTC
run timestamp. The script opens an existing migrated warehouse directly and the
API fixes DuckDB to 256 MB and one thread. There is no implicit migration,
source refresh, activation or live warehouse execution.

FQ2 uses FQ1's public `validate_fundamental_signal_panel` helper to recompute
the same frozen panel digest, definitions, observed calendar, source contract,
and logical grains before label reads. It supports the frozen 1–32 signal family,
including zero-coverage signals. One decision date and signal at a time is
ranked in SQL; large membership and label rows stay in DuckDB. Entry is the
next observed FQ1 session; expected outcomes use the same observed market
source with an explicit evaluation cutoff.

## Label provenance prerequisite

Migration 0325 adds nullable `price_basis` and `calculation_version` to
`forward_returns_survivorship_safe`. Existing rows remain NULL. The bounded
canonical publisher writes the actual validated basis (`adjusted_close` or
`close`) and `forward_return_publication_v1` in the same stage/shadow/atomic
publication. The version identifies this SQL calculation path, including
selected bars, observed calendar endpoints, and delisting terminal stitching.
It does not certify vendor price adjustments or original historical delivery
vintages. Other sources retain their values and NULL provenance.

FQ2 selects the latest visible revision by
`(available_at,source_loaded_at,forward_return_id)` before checking validity;
it does not use `is_latest_revision`. It accepts only adjusted-close labels
with the recognized calculation version, finite return at least -1, exact
expected endpoint, valid observed/policy terminal source and outcome maturity.
Missing, invalid and unsupported-basis outcomes remain counted by date and
decile. Selected label IDs, basis, version and economic fields are digest
pinned. Individual selected label rows are not archived. A future verifier
could compare current labels with the stored digest, but no automatic read-side
comparison is implemented and prior row values cannot be replayed after source
replacement.

## Persisted schema

`fundamental_signal_evaluation_runs` stores status (`building`, `failed`,
`complete`), frozen config/build/calendar/sample/result hashes, run cutoff,
counts, blockers and `production_eligible=false`.
`fundamental_signal_evaluation_deciles` stores each requested
signal/date/horizon with pre-label membership, score tie boundaries, status,
label attrition, observed/policy terminal counts and mean returns.
`fundamental_signal_evaluation_label_evidence` stores one selected-label count
and digest per signal/date/horizon.
`fundamental_signal_evaluation_summaries` stores all 5/21/63-session by split
results, including untestable hypotheses, annual buckets, gross and
10/25/50-basis-point-per-side scenarios, terminal shares, calendar-aware
Bartlett HAC, and variable-family Holm correction on the entire frozen 21-day
family per split. Completed run IDs cannot be reused through the API. Failures
remain marked failed with error type only and any committed diagnostic chunks.

The default cohort is FQ1's dated US-common issuer-qualified set. It is not
price/ADV screened, borrow checked or capacity qualified. The fixed candidate
rule matches the CF1 research policy, but candidates remain research-only.
Overlapping horizon spreads are not daily trading PnL or Sharpe. The local
Holm procedure does not correct previous research searches. No alpha claim
has been made; no live evaluation or backfill was run.

## Validation and review

The FQ1 fixture uses the committed shared validator's exact six-field
diagnostic and blocker contract. The isolated FQ2 module passed 8/8 tests via
`run_memory_guarded.py` with a 1.5 GiB Windows job cap, exit code 0 and native
peak job memory 0.614 GiB (`fq2-tests7-memory.json`). This includes an actual
two-bar bounded publisher publication: adjusted and raw price arithmetic,
persisted basis/version, other-source retention and failed-swap atomicity.
The evaluator fixture exercises actual SQL inserts and sealing with 200 names,
label attrition, invalid latest revision suppression, a valid policy terminal,
tamper-sensitive result digest, immutability and failed-run diagnostics.
Scoped Ruff passed with a guarded exit code 0 (`fq2-ruff2-memory.json`).

An independent read-only review found no Critical issues and two Important
issues. Both are fixed: result sealing now includes all displayed decile and
summary fields, and terminal labels require coherent raw/terminal arithmetic,
dates, flags and observed/policy evidence. The docs now state precisely that
the stored selected-label digest does not automatically recheck current labels.
No live warehouse run, backfill or full migration bootstrap was performed.
