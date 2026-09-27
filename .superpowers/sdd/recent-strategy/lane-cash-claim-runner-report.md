# Optional cash-claim runner adapter

Source: `80ebf709`, corrected decision-clock support and early admission in
`5e1fa849`, and aggregate diagnostic admission in `5e6dcb75`.
Postimplementation fixtures: `a86b274d` and `8a9f254c`. This packet changes only
`atx-impl/src/strategy_runner.{hpp,cpp}` and the existing runner fixture TU.
Root owns the engine dependency imports, CMake, compilation and real-data runs.

The paired `--cash-claims PATH --cash-claims-sha256 SHA` opt-in admits exact
bounded JSON bytes (`atx.strategy-cash-claims/v1`). It rejects duplicate or
unknown keys, unsupported currency/settlement/evidence policies, duplicate event
or instrument IDs, malformed strict clocks and missing/zero provenance pins.
The document archive SHA must equal each role's admitted source snapshot. Only
then does the adapter bind each event to that role's verified manifest SHA for
the engine. These are caller-declared reconstructed publication records, not
authenticated historical delivery or original archived HTML.

The same events go through the engine's explicit claims context and extraction
for both TRAIN orientations, both fixed combined variants and subsequent roles.
No event is removed because it lies outside a role. The engine reports in-role,
pre-role, outside-axis and after-role use separately. A derived membership mask
excludes a name from VM cross-sectional operations and fixed blend contributions
only after both effective-by and availability clocks are strictly before that
decision. Claim valuation remains the engine-validated first eligible mark.
The immutable price, presence and member artifacts are unchanged; prior time
series history remains available. Extra mask and claim-output working memory is
charged explicitly before loading role payloads. The initial event-only reserve
missed retained orientation-summary multiplicity; independent review identified
that defect before compilation. The fix charges the entire run on each role:
`2*C + 2*roles + 2` summary slots, each with 8 KiB fixed plus 8 KiB per event,
times four for retained JSON, copies and serialization overlap. Counts are
bounded at C<=64, events<=256 and roles<=3 before this arithmetic. This is a
conservative declared admission envelope, not a measured RSS guarantee.

Enabled recipes use `atx.dsl-combined-execution/cash-claims-v2`, bind the exact
event-file SHA, archive SHA and decision-support policy, and retain the admitted
evidence JSON. Disabled runs retain the existing v1 recipe and plain extraction
route. No candidate, sign policy, family weight, cost parameter, cadence or
turnover definition changes. Monthly execution-date reporting remains intact.

Each successful trial discloses signed unsettled claim value, receivable and
payable balances, settled cash, NAV including claims, recognition bridge P&L,
modeled short-claim carry and event use. Combined trials also write a separate
cash-claim CSV. Payment timing stays unknown; no settlement or liquidation is
invented. The engine's research share-equivalent conversion is explicitly
distinguished from an observed broker entitlement ledger.

The four new source fixtures cover:

- A publication between mark and decision: retirement affects the current
  decision's fixed contribution denominator; valuation occurs at the next mark.
  Positive/negative orientations create opposite claim sides, negative claims
  carry modeled borrow, and a later role has no hypothetical opening claim.
- Two future-only announcement mutations: identity changes while every earlier
  combined CSV byte and frozen sign remains unchanged. The disabled baseline
  retains its v1 recipe and lacks claim report fields.
- Missing/mismatched external pins, mismatched archive, duplicate/unknown keys,
  zero evidence pins, unsupported basis, overlong identity and equality at the
  recognition clock fail before any output directory or trial is created.
- A two-candidate, 96-event report exceeds the 64 MiB configured envelope before
  payload loading. The fixture removes its synthetic close payload and checks
  the specific aggregate-summary refusal, requiring no large allocation.

`git diff --check` passed. No compiler, fixture, benchmark or real-payload run was
performed in this lane. Runtime approval is pending the root's focused batch;
no strategy performance, general corporate-action coverage or historical
evidence-quality claim follows from this source packet.
