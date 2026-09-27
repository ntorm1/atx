# T21 — fields-v5: fundamentals + me_company + industry group fields (v4 data, M-L)
Spec: T18 report (C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T18-report.md) §5 (fields path), §7 row T21 and "T21 details"; rulings in C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/v4-prereg.md §R2 (link rule, +1 declared
lag session via --fund-lag-sessions 1, staleness 200/400 d, primary lines only, me_company line-summed).
Edit atx-engine/tools/prepare_research_fields.py (+ test_prepare_research_fields.py). Existing fields and their
bytes must stay byte-identical when the new groups are not requested (existing tests pass). Inputs: T19 bridge
output dir (schema: module docstring of atx-engine/tools/prepare_identity_bridge.py — being written concurrently
in pool-4; code against the T18-declared rows (sr_id, cik, start, end_incl, available_at, primary, tier, basis)),
T20 events (schema contract atx-engine/tools/fundamental_events_schema.md — being written in pool-8; code against
T18 §7 T20 details; the controller will relay the committed schema). Group fields grp_sic2, grp_ff12, grp_ff49
(categorical, NaN when unlinked/stale 550 d). Budget <= 180 s / <= 1536 MiB per role run.
