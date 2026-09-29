# Task L9 report (WORK IN PROGRESS, stopped by the owner): V7-F plumbing and spec v71-lo3
**State:** branch feat/platform-v7-l9-v71lo3-20260929 in pool-10, based on pool-2 HEAD 9eddfd7e. It holds this report only; no code, spec or test was changed. Nothing was built, run or written to pool-2. C:/atx was read only to hash stage manifests.
**Done (research, read-only):**
- Binding text: v7-prereg.md "Universe trial U-lo3", paragraph V7-F. The cell is library v7.1 on role lo3 with fields-v9 rebuilt on lo3, same construction, L 1.247. It is accepted if paired S2 net dSR > 0 vs the better of the two accepted cells, AND mechanics hold. N + 1.
- Builder CLI (from `prepare_research_fields.py --help`):
  - SEC: `--sec-stages ROOT` (holds earnings_calendar/, insider/, sec_filings/), `--sec-identity-bridge DIR --sec-identity-bridge-sha256`, `--earnings-calendar-sha256`, `--insider-sha256`, `--sec-filings-sha256`.
  - Holdings: `--thirteenf / --ftd / --regsho-threshold / --security-master / --short-volume-ext DIR` plus `--<same>-sha256 PIN`.
  - Reuse: `--reuse DIR --reuse-sha256 PIN [--reuse-hardlink]`.
- Stage pins (manifest.json sha256 under V1 = C:/atx/atx-db/data/alpha_panel/v1). The live files equal the pins recorded in the lo1 fields-v9 manifest and in W5a's command:
  - earnings_calendar 9a4a976b03d0ad04672796f01abc129d0d09e3db23d62ddf57aea68ae3c7d769
  - insider dcd3f1aa4ba6e266c03ef78568ca131c1336f51ade477f03faff2e88a62ba061
  - sec_filings 5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693
  - export/identity-bridge-v2-pit 09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01
  - thirteenf 8974170f64b4a002cc1b131449c2abf0c7daaab23d4a256992afbdfc4105ffb0
  - ftd a76d69bed49829d9e14216f1abe2ef76b480c16c576fa49ae63fa2a28050f945
  - regsho_threshold 68f431f006d9a78bb26eb7d690f4f3d2a00aaee39e41f33a6474aba9e25ad694
  - security_master 3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a
  - short_volume_ext 7007a13c226a1730d0ef778d7201bf7c1d0e97ed61d4c12af32c1c4567444928
  - fundamentals (sic) 9f9b2f85f6bcd5c7f3a55aee097893094a5cb85ab2b4edbfb582297dab06816b
- Reuse source: build-equity/recent-fast-train-2020-2022-v2-lo3-fields-v7/manifest.json, sha256 2f14e20e3ff36b3a2d1fedaedc910f66465c5e308cc0138bd3d12923f1376e31. It is complete, has 41 fields, and is bound to the lo3 role (manifest 40e3d832, member 21d25d82). Its builder code sha256 (LF) 99e01265 equals the current prepare_research_fields.py, so REUSE_RULE should let those 41 fields be reused and only the 22 W5a/W5b fields computed (untested).
**Planned design (not implemented):**
- research_cycle.py:
  - New pinned INPUT_KEYS: sec_identity_bridge, earnings_calendar, insider, sec_filings, thirteenf, ftd, regsho_threshold, security_master, short_volume_ext, reuse_fields.
  - fields_step maps them to the flags above. `--sec-stages` is the common parent of the three SEC stage dirs; validate that they share it and that all four SEC inputs are present together.
  - `reuse_fields` gives `--reuse --reuse-sha256` from the spec, plus `--reuse-hardlink` when `fields.reuse_hardlink` is set. The CLI `--reuse-fields` is refused when the spec already pins reuse.
- scripts/specs/v71-lo3.json:
  - Base it on v70-lo3.json (identity_bridge v2-pit, fund_events, sic_events) plus the stage inputs above.
  - fields: the 63-name v9 list, output lo3-fields-v9. check: baseline = lo3 fields-v7, 41 identical, 22 added, short-volume pin.
  - Library v7.1. u on mega-candidate-cache-v71-lo3 (seed: `cp -al build-equity/mega-candidate-cache-v70-lo3 build-equity/mega-candidate-cache-v71-lo3`). u-compare against mega-v70-lo3-train-u-1, keys = `{input:baseline_library}` (v7.0).
  - Then fit, card, gate p1-v71-lo3 (4 rows, require any), w, nav -> mega-nav-v71-lo3-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247, monitor, summ (cells_from_ledger, dsr_n = ledger lines + 1 = 37 now).
  - reference_cell is one input that root points at the better accepted cell.
- Expected fields cost: 41 reused (hardlink) + 14 SEC + 8 holdings. By analogy with lo1 (W5a reuse run 59 s / 524 MiB working set; W5b 69 s / 581 MiB; lo1 v9 121 s / 571 MiB), about 100-130 s and ~600-650 MiB (estimate).
**Not done / untested:** all of the above, i.e. code, spec, tests and the dry-run plan. Also unverified: whether REUSE_RULE accepts the lo3 fields-v7 dir, and whether the builder refuses SEC or holdings args when their fields are not requested. Note: pool-2 merged the uncondensed L8 report (ac8a94de, 48 lines); the condensed bb303a9b is only on the L8 branch.
**Next three steps for whoever resumes:**
1. Implement the stage and reuse inputs in research_cycle.fields_step and validate_spec. Test with a hash-only plan (exact flag order) and one fake-builder run.
2. Write scripts/specs/v71-lo3.json, `lock` it read-only against pool-2 (the path map in l8_dry.py), and add spec-shape and plan tests modelled on the v71 ones.
3. Dry-run the plan against the real files. Then report the root lines: cache seed, plan, run, and the JSON rerun (`--json .../mega-nav-v71-lo3-summ-n<N>.json`).
