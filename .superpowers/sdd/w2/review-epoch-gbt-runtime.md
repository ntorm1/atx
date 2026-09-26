# Independent E2/L2 runtime evidence audit

2026-09-26. Independently audited report/companion receipt `3718b266747624d5a6c2c3802683d0ca7cfd1ff5` against local artifacts under `C:/atx-wt/pool-2/build-equity`. **Approved as bounded runtime evidence.** No runs, builds, configuration changes or real payload reads were performed.

Verified every recorded XML, native receipt and log SHA256 in the companion JSON, all three build receipt/log bindings, and case-index SHA256 `c5dd670502014d01cf16fbdbd4020d9882e0e9530c147378ce1053f71ba25a66`. Parsed all testcase names/statuses independently: 42 run/completed executions, 41 distinct names, no skips. Each name has a final passing observation. Exactly one name repeats: `ImplIcEpochConfig.ConfigFileAndCliMergeUseTheSameGuardAndPreserveTrustedHead`, first failed and subsequently passed. All 18 new owning cases are present (GBT7, catalog5, prereg2, stage2, configuration2).

The first four cohorts were built at `3e467fce61de3538240d830aa33b05fb278111b2`, production/configured source `b6b80d2b7f5171ce8d3670e5ccfdc5a3519ae4bd`. Their Git difference is documentation only. The correction `42677f2431ee9a9db06a5e914efc4a1580d364cc` changes only the fixture: file parsing is followed by the existing merged-dispatch `validate_cross_flags` boundary. It does not change production algorithms or make equity-IC accept config files.

| Cohort suffix (`w2-epoch-gbt-…-qualified`) | Verified result |
|---|---|
| learn | 17/17 |
| catalog | 5/5 |
| flags | 5/6; initial failure preserved |
| stage | 13/13 |
| flags-fix | corrected case 1/1 |

Current unaffected executable hashes match the original receipts: engine W1 eval `4ed3f4627d42c0f7c9843fa9459d1ba4bba312e65ef2ca24b9ef67806b46eeb3`; engine IC screen `88b6a68a6f1a542175b1f2b17641ccc181524b1c408151ea3216baaa295e1211`; application W1 contract `75ce1c7e999de7c6fd50aa31882cb05f99a4837c50a098d5846770788c014acc`. The relinked application IC binary matches correction receipt `f6c497ac98e2b502c63afa9c27cbc37d3d8aede3a26a69368ecd230d1050c848`; the initial flags binary `f49c201bc2caa5434be724578595b8b1cbf88b9b3d0c0ddb2b4535f98523144f` is historical receipt attribution, not the current executable. No rerun of its other five cases is inferred.

Independently counted build actions: leaf7 CPP/0 links; combined52 CPP/6 links; correction1 CPP/1 link. No PCH or dependency compilation appears in these action lines. Bound durations are 51.4449585s, 239.6642778s and 24.4074442s respectively. Shared-host durations are not controlled speedup measurements.

This audit qualifies the bounded implementations and affected synthetic consumers only. Historical E2 imports/reconciliation, other-stage migration, cluster-N/DSR attachments, L2 inner-purged stopping/learned missing direction, large-scale performance and real alpha evidence remain open. Counts overlap prior qualification packets and must not be added to them.
