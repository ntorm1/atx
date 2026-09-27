# Evidenced fixed-cash claims in the actual DSL execution book

Status: source and seven postimplementation fixtures frozen; compilation/runtime pending root qualification. No build, real payload read, or research evaluation ran in pool4. This is a bounded W2-D2 dependency of the actual recent-data strategy, not completion of corporate-action coverage or proof of net Sharpe.

## Frozen packet

- Production `c0be402e102e1fbd3ad21a1578b1ae4290e31857`.
- Postimplementation fixtures `25ff0a88cc486878cd58369c92c075b9cbb80257`.
- Between-mark publication/decision correction `1ef2524da051c8ba4b9938f7f9ee3de8b601e0e6`.
- Corresponding seventh fixture `3d1ef20ab08a5e96bc690e506740291bf575617f`.
- Independent review refinement: `440d538f778bdb7e893ad4aec5514a74399361f6` reserves already-known locked positive equity from decision capital before formal claim recognition; this report commit updates its discriminating fixture and expands empty-route comparison to all numerical arrays.
- Owning test TU: `atx-engine/tests/factory/execution_cash_claim_test.cpp`; filter `ExecutionCashClaim.*`.
- New compiled TU: `atx-engine/src/factory/execution_cash_claim.cpp`. Root owns registration. No CMake/runner/shared AlphaStreams edits here.

## Trigger and evidence boundary

The original strict run stopped on a genuinely absent held endpoint; it did not complete a numerical trial. Root's pinned source inspection identifies securityID39621 as historical MDCO on January2/3, with last raw/adjusted close `84.900001525878906`, source factors1. This identity comes from root receipt `6b270e8b`, output SHA256 `dc0fd7742ae503acc736de7881e3404f63494f623d2aef36b9dbd0b70a944c3f`; pool4 did not re-read those rows.

The [issuer release](https://www.globenewswire.com/en/news-release/2020/01/06/1966725/0/en/Novartis-successfully-completes-acquisition-of-The-Medicines-Company-adding-a-potentially-first-in-class-investigational-cholesterol-lowering-therapy-inclisiran.html) is timestamped January6,2020 12:05ET (17:05UTC) and describes completed conversion into a right to receive USD85 per ordinary share, subject to its stated exceptions. It does not establish when a portfolio received cash. [Nasdaq's updated merger notice](https://www.nasdaqtrader.com/TraderNews.aspx?id=ECA2019-296) corroborates MDCO/CUSIP584688105, last trading January3 and completion before the January6 open. Its original December26 page date is NOT a verified publication clock for the later completion update; its prose also contains a suspension-year typo, so that prose is not used as the clock authority.

The new API labels supplied clocks as reconstructed source-publication research, not verified historical delivery. Curated evidence/source/identity/basis hashes are caller assertions bound into the recipe; the engine does not authenticate issuer pages or infer identity from a price. Root/pool5 own the actual evidence JSON, source-archive verification and role-manifest association.

## Small production contract

`factory/execution_cash_claim.hpp` is a lightweight typed event/API boundary. `execution_cash_claim_streams.hpp` wraps existing AlphaStreams only for explicit claim consumers, avoiding an AlphaStreams header change. `prepare_execution_objective_claims(...)` retains bounded owned event inputs; `extract_execution_signal_claims(...)` returns streams, six calendar diagnostics, event dispositions and a compact recognition ledger. Ordinary extraction refuses a nonempty claim context, preventing silent loss of claim reporting. Empty events retain the original hash stream and arithmetic, including disabled-borrow legacy calls; retained object overhead is not claimed byte-identical.

Each event pins the ordered security identity, role source SHA, independent identity/completion/basis evidence hashes, exact prior raw and adjusted marks, a completion interval, public availability, recognition mark and positive USD consideration per raw share. It explicitly asserts matching predecessor share basis and cash excluded from the adjusted-price return convention. The engine verifies the in-role prior row's physical presence and bit-identical raw/adjusted values. It does not infer a basis or use future last-print scans.

Completion's upper bound and public availability must both precede recognition strictly; recognition is the first eligible mark in the role. A bound represents before-open completion without inventing an exact closing instant. A completion published between a mark and its later decision already removes that line from the decision support, although valuation recognition waits for the next mark. That newly informed decision also excludes the currently accounted positive holding from investable capital; targets queued before publication remain frozen. It does not reinvest a locked asset or anticipate the valuation bridge. Known pre-role extinction closes the line without a hypothetical opening position/claim. Events outside the instrument axis or after the role are retained and hash-bound with explicit dispositions, not silently converted to a different identity.

At recognition, with previous marked signed dollars H and verified raw reference P, research share equivalents are H/P, signed fixed claim is (H/P)*cash consideration, and the mandatory valuation bridge is claim-H. The equity is retired and every pending target for that line canceled; settled cash is unchanged. This is a research reinvested-share-equivalent conversion, not proof of broker physical entitlements. Recognition is neither a market fill nor turnover; the bridge contributes to gross NAV change but has its own dollar diagnostic. Other missing or guarded held returns still refuse; no missing bar is replaced or assigned an invented zero return.

NAV remains settled cash + marked equities + signed fixed claims. Positive receivables are excluded from new decision target NAV. Negative claims remain liabilities and reserve settled cash; material negative cash after that reserve refuses. The last qualified modeled annual short-borrow rate continues against the fixed payable on actual elapsed calendar days until role end. The recognition interval retains the original equity borrow; subsequent intervals carry the payable rate, preventing double charging that interval. This reserve/carry rule is explicit conservative modeling, not evidence of a stock-loan termination, actual financing rate, or payment. There is no settlement API, interest earned, tax treatment, automatic terminal liquidation, or cash released from a claim.

The result reports signed claim, positive receivable, positive payable, recognition PnL, payable carry cost and settled cash separately on the same mature calendar. Each recognition records removed equity, research units, signed claim, bridge and carried rate; zero-held retirement can produce a zero-valued recognition record and must not be counted as an economically exposed liquidation. `event_uses` distinguishes in-role, pre-role, outside-axis and after-role input events. Unknown payment is explicit through role end.

## Bounds and compatibility

At most1024 events, unique event ID and instrument ID per role; every active string and digest is bounded. Admission adds event copies, name/period lookup, per-worker retirement/claim/rate state, six date arrays and bounded recognition/disposition records to the existing checked context/output/scratch budget. This is retained payload plus slack, not an RSS measurement. No additional D?N raw-price copy, archive scan, VM evaluation or backtest is introduced.

All supplied event fields and the fixed claim/carry policy extend the context hash only for nonempty events. Existing execution config, cadence, family weights/sign-selection recipe and cost surface math remain unchanged. Prior failed attempts are not rewritten. A nonempty claim context requires modeled-borrow mode so continuing liability carry cannot silently become zero-cost.

Existing `stage_equity_ic::EquityTerminalEvent` has no effective/publication clock and its label adapter uses last raw close; ordinary replay's DelistingEvent has only a period/table return. Neither is reused as a chronological evidence contract. The richer book transition/claim APIs currently admit only synthetic transitions and require a positive stock conversion/distinct successor; this slice does not silently broaden those old contracts.

## Postimplementation qualification source

Seven bounded fixtures cover independent long raw50/adjusted100 cash60 accounting (500 equity becomes600 receivable), signed short liability/calendar borrow and funding refusal, pending targets including delay3, receivable-excluded target sizing, exact clocks/basis/source/duplicates, unchanged numeric prefix under future event amount mutation, unrelated missing endpoint refusal, pre-role/out-of-axis/future dispositions, between-mark completion knowledge, and empty-event default digest/stream bit equality. The parity fixture is same-source branch comparison plus the independently specified initial ledger; it is not a claimed prepatch binary golden.

No runtime result is claimed here. Root must compile the private TU and this single owning fixture TU, execute the focused checks, independently review source, then import the runner adapter and re-run the retained strict rehearsal under its new explicit recipe. General D2 settlement/distribution/split/stock-merger handling, authenticated historical delivery, borrow availability, and economic performance remain open.
