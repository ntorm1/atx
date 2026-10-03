# Point-in-time filing cohort lineage

R16 distinguishes prior_visible_baseline_complete, contemporaneous_arrived_complete
and retrospective_inventory_complete. The last remains NULL in historical
feature rows; a final2026 inventory never supplies2020 manager membership or
qualification. A prior-visible baseline is a denominator of known prior-quarter
original reporters, plus separately recorded current visible entrants. It is
not proof of the entire contemporaneous reporting-manager population.

Native CLI: `python -m atx_db.alpha_panel.thirteenf_features --cohort-filings
filings.jsonl --input-manifest manifest.json --event-start <UTC> --event-end <UTC>
--max-events 1000`. Input metadata has accession/form/filer_cik/report_date/
available_at and explicit qualified_original/source_qualified flags. Missing
source qualification stays false. Every event preserves previously published
clock snapshots; a late prior report also refreshes next-quarter baseline state.
Unknown/unqualified amendments and notices block affected comparison cohorts.

Keys: period_q DATE and available_at TIMESTAMPTZ. Baseline/arrived/entrant/
outstanding IDs and source accessions are VARCHAR lists. Cohort ID is SHA256 of
canonical sorted baseline CIK IDs; comparison_population_managers is BIGINT.
Completion/zero flags are BOOLEAN. Whole-population completeness and economic
ownership zero are false. Rejected metadata is keyed by input_line, preserving
accession/reason and audit clock. Missing report/clock and inconsistent quarter
report dates never silently disappear.

128MBsinglethread/file-backed DuckDB; max10000events,50000metadata rows per
cohort snapshot. Bounds are explicit; no complete-source inference. Input hashes
and parent publication are pinned before/after. Stage remains UNQUALIFIED.
No real cohort build or source/economic admission is established. Public-source
closure, original verification, full native position assembly and review remain
open; these metadata products alone cannot certify ownership/buying/zero.

Completion requires affirmative `qualified_original` and `source_qualified` for both selected prior/current originals; missing/false qualifications remain incomplete. `source_lineage` is JSON containing each selected original and every visible relevant amendment/notice accession, public clock, form, manager and blocking reason; `source_accessions` includes all these contributors. Lineage preserves strict visibility before each snapshot.
