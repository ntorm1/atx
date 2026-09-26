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

New runs declare `evaluation_version=fq2_v3` and
`label_evidence_version=selected_label_v2` in their sealed configuration.

- **Selected-row digest.** It includes `is_stitched`, which affects label validity,
  and `source_loaded_at`, which orders otherwise tied revisions.
- **`fq2_v1` runs** keep their original selected and sample hashes. Those hashes
  have the v1 field coverage and must not be interpreted or recomputed as v2
  evidence.
- **`fq2_v3` compared with `fq2_v2`.** The only change is inference (below).
  Deciles, label evidence, sample digests and every summary point estimate are
  digest-identical.
- **Label version.** The publisher's `forward_return_publication_v1` label version
  is unchanged.

**Inference (`fq2_v3`).**

- **Primary test.** Each daily equal-weight Q10–Q1 horizon-spread mean is tested
  with the R3a robust test, `atx_db.research.stats.mean_inference`. It uses an
  equal-weighted-cosine (EWC) long-run variance with fixed-b inference and a
  Student-t(B) reference. The spread series is positioned by decision session
  (gaps kept), and `horizon_periods` is the horizon in sessions.
- **Summary columns.** They carry the robust test, and
  `config_json.inference.columns` records the mapping:

  | Column | Content |
  |---|---|
  | `p_value` | `robust_p_value` |
  | `z_statistic` | normal-equivalent z |
  | `hac_standard_error` | robust standard error |
  | `hac_lags` | B, the cosine terms (also the t degrees of freedom) |
  | `ci95_low` / `ci95_high` | Student-t(B) interval |
  | `holm_p_value` | Holm over the robust p |

- **Reported only.** Newey–West (R3a lag rule) and the legacy calendar-HAC p are
  recorded per hypothesis in `diagnostic_json.inference_comparison`. They never
  drive status, Holm or candidates.
- **Holm.** Correction covers the entire frozen primary 21-day family in each
  split, including untestable hypotheses (unchanged). Secondary 5/63-session
  results cannot qualify primary candidates.
- **Power at long horizons.** B = min(⌊0.4 T^(2/3)⌋, ⌊T/(2h)⌋), so short splits
  at 63 sessions have few degrees of freedom and little power.

**Legacy inference.** `fq2_v1` and `fq2_v2` runs used calendar-aware Bartlett HAC
with lag horizon minus one and a normal p. That test over-rejects under
overlapping labels: R3a measured about 11% size at a nominal 5% for the
21-session spread. Such runs remain readable. Their p-values are labeled
`inference_overconfident_legacy`
(`atx_db.fundamental_signal_evaluation.inference_status`), and they never pass the
desk-Q5 significance gate. That gate reads only an `fq2_v3` robust Holm p ≤ 0.05
on the primary hypothesis of the month's split.

**Limits of the results.** Gross and 10/25/50 basis-point per-side net scenarios
are horizon spreads, not daily portfolio returns or Sharpe. Local Holm correction
does not account for earlier research searches. No alpha or live capacity claim
follows from a completed run alone.

The prepared [decile acceptance query](../sql/research/fundamental-signal-decile-acceptance.sql)
displays:

- the predeclared hypothesis family, coverage, cost scenarios and blockers;
- the run's `inference_version` and `inference_status`.

Missing, incomplete and mismatched manifests receive explicit status labels. Only
a matching complete run can surface a statistical candidate, and a legacy-inference
run cannot. This inspection query does not revalidate digests and has not been
run on live labels.
