# W1-D1 / D5 case provenance recovery

Status: preparation only, 2026-09-25. The original 19-session investigation is recovered and hash-verified. The original 82-case list is not recovered, and no case is claimed resolved. This pass read code, documentation, Git history, artifact directory entries, and preparation manifests only; no warehouse connection, data-row scan, payload download, or production edit occurred.

## Recovered 19-session source

Original file: `C:/atx/.worktrees/equity-platform/.superpowers/sdd/equity-platform-parent-goal/cp15-idgap-investigation.md`, **21,832 bytes**. Fresh SHA-256:

`fd4adfbac6c9fd558ac9370dddd8ab625cb130f546cf94fde1debda9960a8c9c`

This exactly matches the `data_quality_findings[0].pin` in `C:/atx/.worktrees/equity-platform/atx-engine/reviews/2026-09-20-point-in-time-universe-validation.json` (pin begins around line 1443). That receipt's fresh SHA-256 also matches the checkpoint-15 handoff:

`2c52c6a4c2d2e841d8ffe3b8e1ced3e8c79d40eeba5a2176bcfc75b06068c07e`

The investigation is absent from pool-3's tracked tree but survives in the old equity worktree. Its methods section identifies seven sealed 2012–2019 accepted archives and preparation manifests; the explicit date table is at lines 56–74. It describes selection by accepted-ID count below 70% of that year's median, with holiday labels. Preserve this exact historical table rather than reconstructing a generic holiday list.

Derived dates, in original order (9 in 2016, 8 in 2017, 2 in 2018):

```text
2016-01-15
2016-02-12
2016-03-24
2016-05-27
2016-07-01
2016-09-02
2016-11-23
2016-12-23
2016-12-30
2017-01-13
2017-02-17
2017-04-13
2017-05-26
2017-07-03
2017-09-01
2017-11-22
2017-12-22
2018-01-12
2018-02-16
```

Canonical derived-list bytes are exactly those 19 ASCII dates, separated by LF, with a final LF and no header. Computed SHA-256:

`0589dc9ae5c96e68d183820f4733ade7df94245d805e285e43e1bdf29c0fef60`

This hash describes the derived list, not this Markdown file or a pre-existing machine manifest. The recovery calculated it in memory; it did not write a standalone allowlist or remeasure corruption.

Freshly verified annual preparation manifests:

| Original path | SHA-256 |
| --- | --- |
| `C:/atx/data/tickerhistory_training_20160101_20161231_20260920/manifest.json` | `004ae1f1c615333f6c4fd49fa4896fa072ce7cc49ad78ee3fff35bf7bd52b872` |
| `C:/atx/data/tickerhistory_training_20170101_20171231_20260920/manifest.json` | `5e9cb5bd5221d67b2831c66b33e9a74b6b67a3aa857b9d816990afca5099bd95` |
| `C:/atx/data/tickerhistory_training_20180101_20181231_20260920/manifest.json` | `9b3026d569889c416bea4384cc614af87d6fc768fa0f3ec48d484700edc7d10f` |

All three record source `C:/Users/natha/Downloads/tbltickerhistory3_10y.zip`, member `tbltickerhistory3_10y.txt`, source SHA-256 `7d2b7a615a08ef3b8686608eda384c4e30e2ce23bdee294ea141b3c9334acd8f`. This is the **manifest-recorded** source hash; the source ZIP was not opened or rehashed. None of the annual manifests contains `qa_v2_dates`.

Directory inspection of `C:/atx/data/tickerhistory_training_20120326_20191231_qa2_20260922` found only `accepted.zip.partial` (1,606,608,804 bytes) and `conflict_fixture.zip.partial` (746 bytes), with **no completed manifest**. Their contents were not read. Recovering the date list therefore does not prove QA-v2 completion or close/volume validity across every affected row. R21-4 in the original equity `progress.md:200` requires that validation before repair; keep the new output and its manifest separate from these partial artifacts.

## Unrecovered 82-case provenance

The earliest located numeric claim is D-10 at `458d0bef:docs/plans/2026-09-24-alpha-engine-review-findings.md:122`, introduced in the W0 base commit. Exact Git-blob SHA-256 (38,762 bytes):

`78b5fea3de432fcd52ccc71eb7a36f44bb94751d66c97f551434bb736862a111`

Its sources section says the review included read-only warehouse queries restricted before 2020, alongside source review at `main@2e0d738f`. It does not provide the 82 IDs, query, result file, input snapshot hash, or per-case proof.

The focused search covered tracked plan/review documents; old equity-worktree `.superpowers/sdd/equity-platform-parent-goal` and `build-equity/audits`; root and pool-1 audit metadata; pool-1 W0/QPS archives; targeted tier1/brief/scratch documentation; relevant Git refs and all-ref history for the claim and identity/bridge changes. No reproducible 82-case artifact or generating query was found. This is a bounded search result, not a claim that no copy exists anywhere.

Earlier related history was checked: `75cbf9459cc8b5bd3c4f397bc01d2deb174ab04c` records the current-ticker bridge's survivor bias; `6b949d7f` revises the L10 fundamental-zoo report. That report discusses **49 of 536 unbridged 2014 names** matching current SEC tickers, including unrelated later issuers, and rejects that additional matching pass. This is a different population and cannot substitute for the 82 alleged wrong links. The small synthetic collision test likewise is not the original case list.

## Bounded pre-2020 regeneration contract

If the original case artifact remains unavailable, create a new, explicitly labeled audit rather than call a new list the recovered 82:

1. Freeze hash-bound vendor-ID/dated-ticker observations through 2019-12-31, the legacy bridge export being audited, and dated SEC issuer/security-line evidence with raw acceptance, revision, and availability provenance. Use sealed projections; do not open mixed-era rows or the warehouse as part of this recovery.
2. Enumerate collisions from that fixed population. Compare the legacy mapped CIK against independently evidenced issuer intervals. Preserve simultaneous share classes, symbol renames, reused symbols, and unresolved identity as distinct dispositions; a ticker match alone is not proof of either identity or error.
3. Emit `case_id,vendor_id,symbol,valid_from,valid_to,legacy_cik,evidenced_cik,evidence_available_at,evidence_ids,source_hashes,disposition,reason`. Record candidate, confirmed-mismatch, unresolved, and excluded counts. Keep retrospective confirmation separate from what was knowable at each decision; later evidence must not qualify earlier links.
4. Publish the population definition, source locators/hashes, exact query or code SHA, temporal bounds, case table, and manifest last. Report the resulting count without forcing it to 82. Track the original unverified claim separately until its actual list or query is recovered.

D5 can derive a versioned allowlist from the recovered report and hash above. It still needs the authorized row-level close/volume checks, repair receipt, and unchanged-period comparisons before claiming completion. Neither provenance recovery establishes tradeable-alpha or identity-coverage acceptance.
