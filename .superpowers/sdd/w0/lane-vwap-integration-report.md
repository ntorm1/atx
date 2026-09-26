# W0-D0 raw VWAP integration — qualification pending

Owner: pool-4, `feat/w0-replay-integration-codex-20260925`. Implementation preceded tests. No real market data, warehouse access, download or post-2019 payload was used. Graph tools were unavailable; discovery used targeted source searches.

## Finding and corrected contract

The original D0 clause requires raw-price VWAP construction. The old adjusted typical-price proxy carried snapshot corporate-action factors into cross-sectional levels; labeling it a proxy did not satisfy that requirement. This repair is a substantive implementation correction, not an owner waiver or a claim that a daily close is observed intraday VWAP.

`VwapRule::RawDailyCloseV2` is now the default. Its daily-close proxy is `(raw_close * share_volume) / share_volume`, with price units. Finite positive raw price and volume are required; invalid or excluded cells are NaN. Algebraic cancellation avoids intermediate overflow/underflow and preserves raw-price bits. V2 replaces supplied stale VWAP in place. A generic `close` has unknown basis: absent `raw_close`, callers must explicitly declare `ClosePriceBasis::Raw` or receive an error. Geometry/cardinality/mask/product-overflow checks precede all column reads.

Explicit `AdjustedTypicalV1` preserves supplied VWAP or the exact former `(high + low + close) / 3` arithmetic and field order, including empty valid panels. Dollar-volume versioning remains independent in Alpha101 augmentation. The real-panel Databento adapter declares known raw input and exempts V2 VWAP from TRI scaling. History/real-panel tags follow the chosen rule; `stage_panel` refreshes tags after augmentation. Adjusted OHLC representation is unchanged. Raw daily values require the corresponding daily bar to be available; the proxy adds no intraday observations or stronger historical-vintage evidence.

`--vwap-rule raw-daily-close-v2|adjusted-typical-v1` reaches panel, mining and active discovery augmentation. Panel identity recipes bind rule/kind/basis and correctly describe dollar volume as `raw_close*raw_volume`; mining's hash-bound report records rule/basis and explicitly denies an intraday observation. Discovery binds the rule to persisted configuration and resume identity only when its capacity adapter is active. Review also exposed a pre-existing resume hole: the active minimum-price, minimum-ADV and ADV-window values now participate in both records. Inactive identity is unchanged by unused rule/window settings. DataContext exposes and moves the explicit rule/basis; empty-window raw lowering is unchanged.

## Frozen commits

- `deb939d25965fed0dd04f4493a8f91156a337e76`: derivation, adapters, metadata and CLI wiring.
- `c19e8a0a9182e8b450fa29480e3c93fd4dccd485`: DataContext policy forwarding and move state.
- `a90b428a7c45f8823b8dcf4d24be76f88e982f95`: validate geometry before raw-column reads.
- `95828c6e5da1d249b46a2032a25ca805a71462d5`: separate adjacent capacity-parameter resume repair.
- `6103ab692d9bc4bd5d63f2cef9c45dc2b6c94b1f`: preserve empty-panel legacy schema order.
- `d40441ad352aab84ead6faecb129da72a56c0496`, `a9b3d913300fe416fdb467092c0f5f5b3b844df6`: postimplementation tests and explicit synthetic input bases.
- `837f5e4fc44629a6b82c783e14d74841c99a6703`: existing widened-alpha/data-ingest benchmark fixtures explicitly pin V1. The WQ101 helper's identical pin is in `deb939d2`. Sizes, RNGs, workers and numeric fixture recipes were not changed.

## Qualification evidence and remaining gates

PASS: one isolated PCH-off production compile, `equity-hygiene -Jobs 1 check atx-engine/src/data/real_panel.cpp`, exit 0. Cache: Debug, `ATX_USE_PCH=OFF`, `FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/equity-hygiene`. Native command closure contained exactly one compiler action, with no PCH, library, worker or test dependency. Logs: `build-equity-hygiene/w0-vwap-hygiene-closure.log` and `w0-vwap-real-panel-hygiene.log`. Launch had 3.640 GiB free and no other compiler; follow-up had 3.078 GiB free. The compiler ended before the process sample, so peak RSS was not captured. This is a scoped include/build check, not full hygiene or runtime approval.

Read-only warm-dependency receipt `build-equity/w0-vwap-known-dependency-closure.json` identifies 121 existing affected objects: engine 5, data tests 16, book tests 1, impl core 22, impl tests 77. It inventories pool-4's existing compiled dependencies only; alpha/parallel/benchmark objects are not configured there and are not counted as zero work. No CMake changes or broad pool-4 rebuild were made.

Direct runtime owners are alpha/data/impl tests, plus `BookPipeline.*` for the changed DataContext interface. The frozen WQ101 helper also feeds parallel cache/DAG tests and the WQ benchmark; its semantics are covered by the independent fixture check. `alpha101_support.hpp` feeds alpha streaming and impl Alpha101/riskmodel/augmentation/capacity/discovery fixtures; its synthetic prices now materialize their known raw-close column. Ordinary adapter fixtures explicitly declare their constructed raw basis; legacy behavior checks explicitly select V1.

PENDING: owning builds and focused/whole runtime gates, coordinated on warm pool-5 alpha and root data/impl objects. Postimplementation checks cover invalid price/volume and geometry, stale-column replacement, unknown-basis rejection, explicit raw/legacy adapters, context moves before lazy construction, per-instrument factor resnapshot and VWAP rank invariance, metadata/hash binding, capacity resume sensitivity and empty V1 field order. `VwapLegacyFixture.N128MatchesFrozenPreD0InputBytesAndDigest` independently freezes the RNG and legacy derivation from `b185d056440704e7ebcfe2b9395601d7e5264269`, compares every field/order/mask/f64 bit at 2520 x 128, and prints the digest. It does not call current V1 on both sides. Its runtime result is still pending; no fixture-equivalence or performance result is claimed yet.

G0 impact, independently inventoried by its owner: all 14 frozen contexts are unaugmented and have no VWAP; no context rebuild is needed. Only full L9 needs a numerical rerun (43/101 fixture alphas, one literature seed and its grammar can consume VWAP). L7/L10, all 13 CP21 paths and native 2013 baseline do not consume it. Preserve old executable/artifact receipts and label the corrected L9 rule explicitly.

W2-A3 remains open: adjusted OHLC cross-sectional levels and mixed-basis expressions such as adjusted `close / raw vwap` are not certified causal by this fix. The mining report discloses that limitation. No alpha admission or broad historical-availability claim follows from this raw-field correction.
