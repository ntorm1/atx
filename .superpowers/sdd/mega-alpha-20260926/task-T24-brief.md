# T24 — library v4 generator (prior-signed, themed) (M)
Spec: C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/v4-prereg.md §R1 (roster by theme, one canonical variant per hypothesis, sign embedded, within-industry ranking via
group ops on grp_ff12 for firm-level ratios of themes 1-3, recipe carries prior_sign/theme/tier/citation) and
T18 report (C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T18-report.md) §6 table (DSL sketches, field names) + §7 row T24. Model the generator on the v3 generator
(atx-impl/strategies/ — find the v3 generator for pv_fields_ic121_v3.json) and its tests. Available role fields
today: close, raw_close, volume + fields-v4 names (see a fields-v4 manifest, e.g.
build-equity/recent-fast-train-2020-2022-v2-fields-v4/manifest.json); new v5 names from T18 §6 + grp_ff12,
grp_ff49, grp_sic2, me_company. PV/SI/IV/EAR members: canonical definitions (state each formula + citation);
do NOT look at v3 TRAIN results to pick variants. The generator must typecheck every DSL string with the
existing Python/DSL tooling if one exists (else document the assumption; group ops on grp_* need T22).
