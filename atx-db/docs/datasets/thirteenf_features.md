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
| inst_shares;inst_shares_change | DOUBLE/shares | Complete cohort level; common membership prior holdings adjusted to current split basis |
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
