# D6 / R2 integration source review, 2026-09-26

Root reviewed the production changes before focused runtime qualification.
This is source approval, not an empirical acceptance or performance claim.

D6 core independent review is recorded at `6d5add74` (source `57362546`).
Its adapter hold is resolved in source by captured immutable HistorySourceIndex
(`8ec4abff` -> `585fe5ef`), final source-axis/path revalidation, exact destination
lookup checks, and once-admitted segment checksums. Disjoint files are rejected
or skipped using bounded header/time-axis inspection before payload mapping.
`702524f1` -> `a100160c` narrows the filename preflight to recognizable calendar
components; t3000 and build provenance suffixes no longer masquerade as years.
Unknown filenames still require sealed axis admission. The original f64 close
channel, independent presence/member masks, fixed union, bounded selected-window
consumer and explicit precision/source-vintage disclosures remain intact.
Root CLI/CMake integration is `c54985b0`, legacy aggregate defaults `d4185238`.

R2 source imports are `4de4cb81`, `8f7c13c9`, `5467efc2`, `d59a0d5a`,
`44d3a11f`, `2872cbdd`, `7585ff0c`, `727535b9` and format-only `cac29671`.
Review required symmetry/SPD validation before disabled eigen adjustment,
locale-stable recipes, factor allocation admission before matrix construction,
and distinction between unavailable statistical fitting and no requested fit.
VRA uses supplied prior forecasts with explicit clocks and unavailable status;
final-fit residuals are not presented as prior forecasts. Hybrid keeps thin names
through structural specific-risk fallback and refuses unknown current exposures
or caps. Diagnostics remain in model objects; full application serialization and
switching application defaults to V2 are not claimed.

Root rejected the first MRAD implementation because it labeled full-sample
absolute bias deviation as rolling MRAD. `80026ae6` -> `727535b9` now retains
12 contiguous forecast slots, invalidates windows containing missing slots,
requires a 21-session step, and excludes pooled residual deciles from this
time-series statistic. Full-sample deviation has its own name. Cohort aggregation
weights actual eligible book/window pairs. Postimplementation fixtures include
a regime-changing series with unit full-sample bias and positive rolling MRAD.
Method reference: USE4 Methodology Notes, Appendix A2, equations A4/A6, page37,
https://dmmn26wgpgtie.cloudfront.net/wp-content/uploads/2011/09/23120404/USE4_Methodology_Notes_August_2011.pdf
This bounded implementation does not establish empirical calibration or bands.

Build receipt `build-equity/w1-next-leaf-build-receipt.json` records source
`d4185238`, Jobs2, 36.581 seconds, exit1. Six production objects compiled; the
panel-store object failed on the deprecated Windows `_wfopen` API under /WX.
Root `ef2d900f` uses `_wfopen_s` with the same exclusive `wbx` mode and registers
R2 focused fixture TUs. Existing objects/PCH/dependencies remain reusable.
Runtime qualification of this batch is still pending. No actual data was read.
