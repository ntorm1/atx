# Dataset `compustat_std_parts`

A pinned export of every vintage in the published v10-based annual fundamentals file, projected to the 20 B3 columns. Part files are `parts/year=YYYY/funda.parquet`, partitioned by UTC `available_at`; NULL clocks are excluded with a counted reason. Restatements and leavers remain. Existing stages are never replaced.

Schema `atx.alpha-panel.compustat-std-parts/v1`; rule `compustat-std-funda-v1`; clock rule `cstd-filing-clock-v1`; staleness 400 days from datadate. The clock is inherited from upstream: acceptance, else filed + 46 hours, with FX rows at the applicable H.10 publication. Consumers use strict `available_at < cutoff` and open only permitted year parts.

Upstream manifest pin: `9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325`. Both manifest and source bytes are verified before export. Manifest records source pin, code identity, all part hashes, exclusions and counts, and is written atomically last.

Run `python -m atx_db.alpha_panel.compustat_std_parts build`, then `accept`, under the heavy memory guard (<=1 GB reservation, 25 GiB disk floor). Build refuses an existing nonempty stage; use a new stage name for future builds.

Acceptance foregrounds 2020 through measured source availability (2026-09-19 22:00 UTC), preserving original FY2009-2022 coverage and FY2009-2023 issuer tables. Nonfinancial means other and utility, excluding bank, insurer and REIT. No fiscal cohort is declared complete without source closure evidence. The latest availability year is partial. The deterministic 50-issuer SG&A sample prefers local FSDS filings stamped 2020 onward; classifications use explicit inclusion/exclusion labels and contiguous presentation arithmetic. Ambiguous R&D mentions are unresolved. Face/export SG&A comparisons require matching currency and no FX conversion; otherwise comparison is explicitly inapplicable. Acceptance stops after a failed self-check before querying rejected paths. Unknowns and agreement with exported SG&A are printed; this is a report, not a gate.

Measured export: 235,563 rows, 17 parts (2010-2026), zero NULL clocks. Upstream balance, rdq and sum4 remain FAIL. FY2009-2010 history remains thin; this export supplies no comparative backfill. See [B3 acceptance](../p9-al-data/b3-acceptance.md) for exact commands, numbers and manifest hashes.
