# B1 text_native v2 bounded pilot

The authoritative corrected generation is `data/alpha_panel/v1/text_native_v2/pilot-20261005-r3`, manifest SHA-256 `5cbaf4efcaa1ee56325159d9042095a0bca3db3f808b8e98372d75fe43813f2b` (31,123 bytes). It is a completed pilot materialization, not full-cohort coverage or acceptance of every gate. Source producer commit `4407c94b` produced the document-fetch timing/memory measurements; Schema repair producer `9a6f986b` produced r2; complete-schema producer `36d87585` produced r3 by pinned retained-unit replay with zero HTTP after proving all 99 selected prior identities and clocks unchanged. Original metrics and receipts are retained values produced by the fetch producer, not newly computed by r3. Exact source snapshots and guarded receipts reside in `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/`.

The immutable original generation `pilot-20261005` remains preserved, including its initial C5 FAIL, manifest SHA-256 `1cc6760d06fc125d2de7af2e9b828cac5a4eafb3f0b8204902bcf7156a5b671e` (18,418 bytes). Arrow's first-row schema omitted real fiscal provenance in six 2019 rows. The repair writes an explicit union schema and explicit nullable producer fields; the validator still compares all public fields exactly. No provenance comparison was relaxed.

35 SEC master indexes cover 2018 Q2 through the partial 2026 Q4, within the 48-file allowance. Support begins 2018-06-30; primary scope begins 2020-01-01. Measured original-filing source horizon is 2026-10-02; census fetch clocks are 2026-10-05T23:26:35.954925Z through 23:27:04.401626Z. The current quarter has 14 originals. Census contains 212,561 originals, including 38,206 support and 174,355 primary filings. Master-index dates are filing dates, not authenticated acceptance clocks.

The deterministic whole-issuer rule ranks SHA-256(`b1-v2-whole-issuer:CIK`), requires both original form families and every observed primary year, and admits only whole issuers within the caps. CIKs 1673358, 1474098 and 1527166 supply 99 filings: 24 10-K-family and 75 10-Q-family, including 18 support filings. All 99 primary bodies landed; metadata-only/failed-body rows are zero. Authenticated output acceptance clocks span 2018-07-25T20:10:13Z through 2026-08-10T18:05:04Z. Null acceptance clocks are zero. Amendments never supply current or prior comparisons.

All four printed acceptance checks:

| Check | Result |
|---|---|
| I C5 manifest, exact public fields, pinned parts and year routing | PASS, 99 rows, zero excluded clocks |
| II landed/full census coverage | FAIL for every primary year: explicit pilot scope; exact acceptance-year census denominator unmeasured |
| III unchanged >=90% comparator gate | 2020 FAIL: 6/12 finite, six metadata-unknown; 2021-2025 PASS: 12/12 each; 2026 PASS: 9/9 |
| IV original family and strictly earlier prior clock | PASS, no invalid accessions |

| Year | Landed bodies by acceptance year | Census originals by filing year | Pilot share |
|---|---:|---:|---:|
| 2020 | 12 | 24,764 | 0.04846% |
| 2021 | 12 | 27,599 | 0.04348% |
| 2022 | 12 | 28,584 | 0.04198% |
| 2023 | 12 | 26,487 | 0.04531% |
| 2024 | 12 | 24,666 | 0.04865% |
| 2025 | 12 | 23,993 | 0.05001% |
| 2026 | 9 | 18,262 | 0.04928% |

Fiscal metadata is explicit original-primary DEI in 87 rows; 12 support rows remain `original_fiscal_period_unproven`. Allowed local FSDS supplied 96 matching accession records across 32 pinned files, but its minute-valued acceptance field differs from second-valued authenticated index clocks. Exact CIK/accession/form/report-period/filed/acceptance agreement is required; no period is inferred from later issuer state. Unresolved cases remain in the comparator denominator.

All 422 attempts and 1,076,845,235 transferred bytes are retained: 35 census, 200 detail-index and 187 primary-document attempts. The 187 body attempts include 88 repeats beyond the 99 final unique bodies; unique final body bytes are 531,363,998. This remains below 2,000 filings and 6 GB, including support, failures, probes and repeats. The malformed initial detail URL produced a 403; the canonical accession directory was corrected after the committed limiter reported no active cooldown/block. No server block, host identity or limiter was bypassed. SEC session rate remains <=5 requests/s.

The final fetch/parse pass took 233.078 seconds (236.1 seconds guarded), approximately 0.425 filings/s. Guard peak was 153,559,040 bytes, with minimum disk headroom 78.2811 GiB. The cumulative first-attempt wall clock is 1,694.134 seconds including engineering interruptions/queue waits: 0.05844 filings/s. Recorded cumulative extrapolations for 212,561 originals are 42.1 days and 2.312 TB; they include repeats and interruptions. A final-pass-only extrapolation is about 5.8 days and 1.14 TB of unique bodies, excluding new-cohort variability and additional failures. Full-cohort execution remains unmeasured and owner-gated.

Original retained output measured 1,260,809 bytes and raw census 122,093,510 bytes. The recorded full persistent projection is 3,051,719,685 bytes, below 16 GB, including raw census and 256 MiB issuer scratch allowance. Processing/replay is per issuer and per part; compressed profiles are disk-backed, capped, pruned and removed after completed units. Bodies are discarded and filing receipts retained. This three-issuer long-history sample does not establish full-cohort throughput, represent shorter/new issuers, or prove the disk projection for all text sizes.

Focused synthetic tests: 23 passed in 3.83 seconds, one file, xdist disabled. r2 guarded rematerialization: 5.1 seconds, peak 127,225,856 bytes, no HTTP. Original artifacts and FAIL evidence are immutable. Full landing and Task 10 were not executed. The exact resumable future FULL command is recorded in `task-9-report.md` under the plan workspace and requires owner authorization for a different absent generation.

The additional guarded local precision probe examined 218,559 rows in those 32 source files: every acceptance timestamp had zero seconds. Selected index/FSDS deltas ranged from -29 to +30 seconds. This observed precision does not prove the rounding convention or authorize treating unequal authenticated clocks as equal; the unresolved cases and 2020 FAIL are retained.


Round-one fixes (BASE `62ea7163`): I1 removes the unsupported 365 +/-45-day report-gap filter; same-period/previous-fiscal-year, strict prior acceptance and inclusive 180-550-day bounds remain. A 300-day transition original is covered by regression. I2 declares all 53 public Arrow fields, including every section metric/count and prior_form; NULL-only and populated parts share exactly the same schema, and acceptance enforces it. I3 moves this evidence to the required docs/p9-al-data destination and fixes links. M1 uses ASCII 2021-2025. Fix commits 36d87585 and 7be335ab; 26 focused tests passed 5.19 seconds.

The preserved r2 manifest pin is `323efb3f009f7844cfd8a9e92cfddc952e85ab6e168fce903378fa0c43844ea6` (18,912 bytes). Its historical C5 PASS used its pinned earlier validator; r3 adds the complete-schema check. No published original/r2 file, pin or acceptance receipt was replaced. The r3 comparator audit is pinned in its manifest and separately retained as task9-r3-comparator-audit.json. Reuse is rejected if a prior identity or clock changes; every preexisting public value remains exact, and only absent declared fields gain NULL. Fiscal uncertainty and the 2020 FAIL remain unchanged.

r3 guarded replay took 6.1 seconds with 127,344,640-byte peak after 298.5 seconds shared-slot admission wait. No HTTP or profile recomputation occurred; cumulative 422 attempts/1,076,845,235bytes remain unchanged. Fetch/parse throughput and cost estimates above still belong to the original producer, and full execution remains unmeasured and owner-gated.

Independent guarded r2/r3 comparison proves every original field and filing receipt unchanged in all 99 rows. Canonical SHA-256 values match on both sides:

| CIK | Rows | Original fields and receipts, identical r2/r3 SHA-256 |
|---|---:|---|
| 1673358 | 33 | d62e56825c447a568bec09b86bcb9b2b28fb14597fbdba0055768d2044da34c1 |
| 1474098 | 33 | 7be5ddab5f0af47d412643660f970fb56ecfb5474e79dffaf9c8fa7821d3285f |
| 1527166 | 33 | 7d4edc55f3a95765b39273b5091c6c7c51296104ff891c35c229fd2d2fb8d9de |

The independent verifier reproduced I PASS (99 rows, zero excluded clocks), II pilot FAIL for every year, III 2020 FAIL at 6/12 with six unresolved rows and later years PASS, IV PASS. It rechecked all three manifest pins unchanged. Evidence is retained as task9-r3-final-evidence.json; guarded run 2.0 seconds, peak 70,860,800 bytes. r3 producer bytes: 44,760, SHA-256 `88ac7d4514eb54f10502af818320a790696da042a5b9b832418d08236aea53e1`. r3 persistent bytes: 1,291,018; complete-schema full persistent projection: 3,162,448,937 bytes including census and scratch allowance, below 16 GB with the same sample limitations.
