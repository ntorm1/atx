# Fundamental signal evaluation (FQ2)

FQ2 evaluates a completed, immutable FQ1 run against explicit survivorship-safe
forward labels. It records every frozen signal, including signals with no usable
decile dates. A result is consumable only when its manifest status is `complete`.
The run remains research evidence and `production_eligible=false`.

After migration 0325, publish the desired label source with
`refresh_survivorship_safe_forward_returns` using `price_basis='adjusted_close'`.
The bounded publisher writes `price_basis` and `calculation_version` on each new
row through the atomic stage and shadow path. `forward_return_publication_v1`
means the source row was calculated by the versioned SQL path using the selected
`equity_daily_bars` price column, observed XNYS calendar endpoints, and terminal
stitching. It does not certify the vendor adjustment economics or original
historical delivery vintage. Legacy rows retain NULL provenance and are excluded.
An unsupported newest visible revision suppresses an older usable revision.

Run with an already migrated warehouse:

```powershell
python scripts/evaluate_fundamental_signals.py --db-path warehouse.duckdb `
  --build-run-id completed_fq1 --run-id fq2_trial_1 `
  --as-of-date 2026-09-20 --run-at 2026-09-20T22:00:00+00:00 `
  --label-source atx_forward_returns_survivorship_safe_v1
```

The API is `evaluate_fundamental_signals(store,
FundamentalSignalEvaluationOptions(...))`. It revalidates FQ1's exact frozen
panel digest and grains before reading outcomes. Decision cohorts are dated
US-common issuer-qualified names with finite scores; there is no verified
price, ADV, borrow or capacity filter. Each date's memberships are fixed with
security-ID tie allocation before labels. Cohorts below 200 names and constant
cohorts are diagnosed. The label joins use the next observed session as entry;
5, 21 and 63 session endpoints share the observed market calendar.

Tables `fundamental_signal_evaluation_runs`, `_deciles`, `_summaries`, and
`_label_evidence` hold the manifest, per-date attrition, inference and a digest
of selected label IDs/basis/version/economic fields. Building and failed runs
remain diagnostic. The complete manifest seals configuration, build, sample and
result digests. Selected label rows are digest pinned, not archived. A future
reader can compare current labels with that digest, but no automatic read-side
comparison exists and prior individual labels cannot be replayed after source
replacement. Split rules are fixed: train through 2020, validation 2021–2023,
holdout from 2024; crossing outcomes are purged and the first 63 observed
sessions of later splits are embargoed.

New runs declare `evaluation_version=fq2_v2` and
`label_evidence_version=selected_label_v2` in their sealed configuration. The
selected-row digest includes `is_stitched`, which affects label validity, and
`source_loaded_at`, which orders otherwise tied revisions. Earlier `fq2_v1`
runs keep their original selected and sample hashes; those hashes have the v1
field coverage and must not be interpreted or recomputed as v2 evidence. The
publisher's `forward_return_publication_v1` label version is unchanged.

Daily equal-weight Q10–Q1 horizon spreads use calendar-aware Bartlett HAC with
lag horizon minus one. Holm correction covers the entire frozen primary 21-day
family in each split, including untestable hypotheses. Secondary 5/63-session
results cannot qualify primary candidates. Gross and 10/25/50 basis-point per
side net scenarios are horizon spreads, not daily portfolio returns or Sharpe.
Local Holm correction does not account for earlier research searches. No alpha
or live capacity claim follows from a completed run alone.

The prepared [decile acceptance query](../sql/research/fundamental-signal-decile-acceptance.sql)
displays the predeclared hypothesis family, coverage, cost scenarios and
blockers. Missing, incomplete and mismatched manifests receive explicit status
labels; only a matching complete run can surface a statistical candidate. This
inspection query does not revalidate digests and has not been run on live labels.
