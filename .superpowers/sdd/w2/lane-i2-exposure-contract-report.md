# W2-I2 dated exposure contract: bounded source slice

Status: production `09d5ff956ec28b9454151b851246a72fea7a891d`; six postimplementation fixtures `9e9a3b4c030b43325ee93596a064cc0f48486d6d`. Source-only; no compile, test execution, data access, artifact build, or empirical exposure claim. Root owns registration/qualification. Files are new `data/exposure_panel.hpp`, `src/data/exposure_panel.cpp`, and `tests/data/exposure_panel_test.cpp` (`ExposurePanel.*`). Independent source review pending.

## Implemented seam

Immutable shared ownership retains complete ordered session/decision/security-ID axes. Session labels are distinct from explicit decision timestamps. Canonical axis, recipe/parents, available evidence and normalized content hashes bind the object. The in-memory builder appends one date transactionally and freezes only after all dates are present. It does not publish a disk artifact. Caller evidence is declared, not authenticated by a loader.

The seven continuous descriptors are log size, log ADV63, Amihud63, cap-market beta252, residual daily volatility252, log momentum252 excluding21, and negative log-return21. Size is derived only from supplied qualified positive PIT cap. The other six unstandardized descriptors have explicit caller recipes; their market-price derivation is not implemented in this slice. FF49 remains a separate categorical 1..49 value, never normalized or replaced with a static vendor sector.

Numeric evidence requires observed-through <= available-at < decision. Membership and source presence have independent masks, qualification and knowledge clocks. Identity/classification validity starts and finite ends retain separate knowledge clocks; a future-known closure cannot erase an earlier value. Missing parent provenance or unavailable/unqualified input produces canonical missing data plus joint reasons. No cap weighting or industry fallback exists. Known member count and unknown-membership count are separate; source absence does not reduce the known-member denominator.

Each descriptor normalizes its contemporaneous qualified-member/cap cohort. Scaled raw moments avoid finite input overflow; at most16 equal-mean/population-SD passes clip raw values at +/-3 SD, followed by cap-weighted centering and equal-SD scaling. Final z is NOT promised within[-3,3]; zero spread produces flagged zero. Categories do not participate in continuous normalization. Complete/partial status is per date, so missing warmup cannot ban a later complete date.

`extract_exposure_date` checks externally supplied axis/content identities and exact decision time on every call. It returns original slots/IDs, row-major requested continuous values, raw positive USD caps, FF49, joint omission counts, denominator and degenerate-column metadata. Requested columns must be ordered/unique. The default requires complete member support; explicit dropping reports partial support and enforces configured name/coverage minima. Missing support is `Unavailable`, malformed identities/configuration are `InvalidArgument`, resource excess is `OutOfRange`. Synthetic evidence requires explicit consumer opt-in. Retained payload, transaction scratch and owned extraction are admitted with checked bounds and slack, not claimed as measured RSS.

Pool5's new residual-IC consumer owns intercept/industry contrasts, sqrt-cap statistical WLS, rank-deficiency refusal, label maturity and objective diagnostics. It retains this immutable panel, checks explicit axes and externally pinned price parent identity, and uses the typed extraction seam. I2 does not silently compact its original axis or substitute OLS on absent data.

## Prepared evidence and limits

Six synthetic cases cover independent planted normalization; future-known closure including equality; unavailable-value mutation; member/source absence; unqualified cap/industry and absent parent; unknown membership; partial warmup followed by complete date; final z beyond3; degenerate metadata; identity/config/clock/budget/geometry refusal; and append retry after rejection. These fixtures were written after implementation and have not run.

Existing `risk/exposures.hpp` uses equal-market beta/ADV20 and unclocked `PitSideInputs`; it is not repurposed as the requested I2 metrics. `fundamental_fields.hpp` weighted-average diluted shares and vendor `shares*raw_close` do not establish D3 PIT outstanding shares. Existing first-observation `sector_group_map` cannot establish D4 FF49 history. None are promoted to qualified prerequisites here.

Remaining original I2 work: authentic D3 shares/cap and D4 dated classification adapters; a causal price-descriptor producer with cap-weighted market and explicit history coverage; disk schema/publication and actual `stage_equity_exposures`; real2012-2019 t3000 artifact; actual downstream stage wiring; PIT sector and shared causality/runtime gates. Full I2 acceptance is open. No CMake, config, warehouse or application-stage changes were made by this lane.
