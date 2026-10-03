# Historical issuer incorporation

`issuer_history/` contains issuer-bound incorporation observations from original EDGAR accession headers. Archived submissions arrays supply accession discovery only; current incorporation profiles are never historical inputs.

The round selects the last issuer corporate filing before the first admitted NYSE session for the 1,454 missing-incorporation cohort, followed by bounded later observations. Original `STATE OF INCORPORATION` belongs to the matching CIK company-data block. Missing, conflicting, noncorporate, mismatched and explicitly unknown `XX` records stay unresolved. Codes use the [SEC EDGAR jurisdiction namespace](https://www.sec.gov/submit-filings/filer-support-resources/edgar-state-country-codes); `SC` means South Carolina here and `X1` means United States. Business-address states and FINRA exchange codes are separate inputs.

Availability is the later of original acceptance in Eastern time and the end of the original filing/change date in Eastern time, expressed in UTC. Ambiguous acceptance timestamps are rejected. A later header or redomiciliation never changes earlier sessions. Universe joins strictly before its previous-session 22:00 UTC cutoff. A dated cover observation wins an equal availability clock; otherwise the latest observation controls, with original acceptance ordering equal conservative stamps.

| Column | Meaning |
| --- | --- |
| cik | Issuer matched in the original header company block |
| accession | Original SEC accession |
| inc_state_country | Two-character EDGAR incorporation jurisdiction |
| accepted_at | Original acceptance converted from Eastern to UTC |
| available_at | Conservative filing/header availability in UTC |
| clock | Explicit clock construction |
| revision_risk | Header revision qualification |
| source_url | Original accession header URL |
| raw_sha256 | SHA-256 over original decompressed response bytes |
| fetched_at | Acquisition receipt time, never used to backdate the filing |

Raw objects and resumable outcomes live in `data/raw/sec_issuer_history/round2/`. Existing `sec_http` enforces the approved user agent and shared host limiter. A separate persisted limiter enforces at most one attempt per second and at most 3,000 combined round attempts, including retries and redirects for both new source stages. Every attempt is recorded before HTTP dispatch. The fetch/load split means loaders verify raw hashes and do not use the network. No new licensed source or synthetic fact is introduced; EDGAR is public, subject to SEC fair-access rules. Cadence follows issuer filings.

Commands run from `atx-db`, with `PYTHONPATH=src`, through the memory guard at no more than 1 GiB:

```text
python -m atx_db.alpha_panel.issuer_history prepare --cohort ../.superpowers/sdd/tier1-v4/t2-header-assessment.json
python -m atx_db.alpha_panel.issuer_history fetch --limit 1460
python -m atx_db.alpha_panel.issuer_history load
```

`plan.json`, `fetch_status.json` and the publish-last `manifest.json` identify source selection, remaining URLs, unresolved records, output hashes, code fingerprints and raw receipt provenance. A complete published artifact does not imply exhaustive historical source coverage. Sparse observations cannot prove no intervening redomiciliation occurred. Source coverage, measured rows and bytes are reported in Task 2 Fix round 2 after acquisition; French accuracy remains separately binding.

The output key is `(cik, accession)`. `cik` is int64; both clock columns are UTC timestamp microseconds; all remaining columns are strings. Jurisdiction codes, identifiers and hashes have no numeric unit. Source is the raw accession header named by `source_url`, verified against `raw_sha256`. `fetched_at` is a receipt timestamp and can be null when the store's compact receipt lookup does not retain it; it never supplies historical availability. Cadence and staleness follow issuer filings, with explicit partial historical coverage rather than a maximum-age guarantee.

The final round-2 publication contains 1,346 observations for 1,332 CIKs, with 13,506,881 source bytes for this plan's successful raw receipts. Availability spans 2017-10-04 through 2022-11-19 UTC. All 1,454 initial pre-eligibility headers were acquired; 123 lack a usable target incorporation state and remain unknown. The 8,085-observation plan retains 6,616 additional unfetched observations explicitly. Completed acquisition of the initial tranche does not complete this larger historical plan. Later original filings reused by the class lane add dated facts; no current issuer profile is backdated.

| Availability year | Observations | Distinct CIKs within year |
|---|---:|---:|
| 2017 | 812 | 812 |
| 2018 | 495 | 494 |
| 2019 | 28 | 28 |
| 2020 | 2 | 2 |
| 2021 | 3 | 3 |
| 2022 | 6 | 6 |

These years describe actual observation availability, not periods automatically certified by a later filing. The 1,346 unique `(cik, accession)` keys have zero clocks earlier than original acceptance. Output SHA/size, current code/plan/network/raw-ledger fingerprints and complete unknown inventory are checked in `t2r2-after-verification.json`.

The manifest's `fetch_status` preserves the last dedicated issuer-history fetch phase (1,460 cumulative attempts at that checkpoint). Later class acquisitions reused additional plan headers; final `unresolved` is regenerated against the final shared ledger and is the current pending inventory. The separate persisted attempt counter/integrity artifact records the final combined 2,998 attempts including hand sources. Phase-specific fetch checkpoints must not be interpreted as the final global count or current pending URL set.
