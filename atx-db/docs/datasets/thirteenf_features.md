# Ownership feature vintage producer

Status: UNQUALIFIED; no actual source-certified publication or yearly coverage.
CLI: `python -m atx_db.alpha_panel.thirteenf_features --snapshots snapshots.jsonl
--bindings bindings.json`. Both inputs are explicit; no latest/default producer
bindings. Binding keys: universe, fundamentals_v11, thirteenf_quality. Each names
a complete manifest with exact SHA and all required output SHA/byte identities.
These are verified before/after computation; future evidence clocks fail closed.
Rows preserve security_id, period_q, available_at; vintages are distinct keys.

| Column family | Type/unit | Definition |
|---|---|---|
| inst_shares;inst_shares_change | DOUBLE/shares | Level requires independent entire-population completeness; change uses explicitly scoped identical qualified cohorts and current split basis |
| inst_ownership;inst_ownership_change | DOUBLE/fraction | Level and split-adjusted numerator change over same currently dated denominator |
| inst_n_holders;inst_breadth_change | BIGINT/managers | Positive SH holders and difference on identical complete cohorts |
| inst_new_positions;inst_closed_positions;inst_persistent_holders | BIGINT/managers | Positive/zero transition counts and retained positive holders |
| inst_hhi;inst_top5_share | DOUBLE/fraction | Share-weight HHI and top-five shares fraction |
| observed_shares;contributing_managers;cohort_complete | diagnostic | Incomplete observations never become qualified zeros |
| common_manager_share_change;change_reason | diagnostic | Comparable manager subset change or reason full cohort change is unsupported |
| ownership_above_one | BOOLEAN | Unexplained ownership above100% remains flagged and unclipped |
| denominator_shares | DOUBLE/shares | Explicit currently dated share-class denominator input |
| shares_by_filer_type;shares_by_manager_style | VARCHAR/JSON | Source-evidenced type or active/passive/unknown shares; no inferred active/passive labels |
| source_accessions;quality_status | lineage | Source keys and UNQUALIFIED_NORMALIZED_ADAPTERS |
| population_complete;reported_holding_zero;observed_n_holders | diagnostic | Full contemporaneous population proof, source-qualified table absence and observed positive reporters are distinct |
| comparison_cohort_id;comparison_population_managers | lineage | Stable sorted prior-population ID and explicit manager denominator |
| prior_visible_baseline_complete;contemporaneous_arrived_complete;retrospective_inventory_complete | cohort | R16 historical baseline/arrival state; retrospective flag remains NULL |

Full ownership levels are missing by default without independent contemporaneous
population completeness. A source-complete table absence only establishes
reported_holding_zero after exclusions/confidentiality/shared-manager ambiguity
checks; it never establishes economic ownership zero. Missing position defaults
are forbidden for changes without qualified table-absence proof on both dates.
Comparable changes require identical eligible manager populations and complete
qualified originals, dated mapping/units/splits. Observed levels and reporting
arrivals remain separately labeled diagnostics. Native metadata cohorts:
[thirteenf_cohorts.md](thirteenf_cohorts.md).

`dated_link_sql` retains every position/value and uses CUSIP report-date validity
plus strict link public availability. Multiple class candidates remain unmapped.
`denominator_sql` requires a positive finite source-backed denominator no later
than report date and known before cutoff; conflicts/multiple issuer share classes
stay missing unless source line basis is established. SQL uses common.connect's
UTC session; naive upstream clocks mean UTC per the published producer contracts.

Daily PIT: available_at strictly before previous session22UTC. Monthly PIT:
strictly before month-end-session22UTC. No return products. Staleness/history,
all-three-universe coverage, native event/cohort snapshot assembly, original source
admission, split/source handchecks and independent review remain release gaps.

Native coverage CLI: `--coverage-members specification.json --features
features.parquet --coverage-output output.json`. The specification contains
member_equity,univ_all_common,univ_r3000p entries, each with path/path_sha256 and
independent expected_keys/expected_keys_sha256. Both key populations must be
nonempty, typed DATE/integer, unique/nonnull and exactly equal. Actual membership
contains timezone-aware cutoff for every cell. Null,duplicate,missing,extra keys
reject the measurement. Independent release-source admission remains required.

Coverage keeps every benchmark cell; selects freshest visible report quarter
then latest vintage with strict clock; uses inherited150calendar-day staleness.
Reports levels/changes/breadth peryear2020+, nonnull/fresh/qualified/knownzero,
incompletecohort/missingfeature/stale counts separately. No return evaluation.
UNQUALIFIED source features cannot become qualified coverage or known-zero cells.

Coverage specifications additionally require `calendar`, `calendar_sha256` and `cutoff_policy`. The independently pinned calendar has unique DATE `session_date` and TIMESTAMPTZ `previous_session_close`. `daily_previous_session_close` requires exact previous-session 22:00 UTC; `monthly_month_end_close` requires the stamped date to be a calendar month-end and exact 22:00 UTC of that date, with calendar evidence for that month. Future and mismatched cutoffs reject measurement without shrinking denominators.
