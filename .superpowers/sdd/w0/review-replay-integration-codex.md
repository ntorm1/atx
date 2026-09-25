# Independent W0 replay integration review

Reviewer: `w0_gate_audit`; implementation owner: `w0_replay_integration` in pool-4.
Reviewed production commit `8ba15b0efeaff53587cc0ac36493cd159247b126` and
test-fixture follow-up `4f257729d2949f082c818f53b42c2c7aca3b9cf9`.
This reviewer owns no replay code.

## Findings and disposition

- **Future-price leak, resolved statically:** Default terminal classification
  formerly scanned the full close series to identify a final print. Prefix NAV
  could change when future prices were added. Default first-missing handling no
  longer scans future prints; the scan is restricted to explicit
  `TerminalReturnExPostV1`. Events must be both due and available at valuation.
- **Unsupported short windfall, resolved statically:** Applying a negative asset
  return to an unevidenced missing short mark created gains, repeatable through
  gap/re-entry cycles. Default no-evidence handling now uses position-adverse
  stress, separately sourced as `AssumedMissingPriceAdverse`. Long and short
  positions lose value; assumptions and signed PnL are separately disclosed and
  report eligibility is explicitly false. Due, published terminal evidence still
  supports genuine terminal returns, including legitimate short gains.
- **Consumer integration, resolved statically:** The identified replay adapter
  no longer pins Abort. Corrected default and explicit Abort are forwarded by
  baseline and constrained-book stages. Legacy holding reports use causal V3;
  explicit V2 retains prior ex-post arithmetic.

## Acceptance limitation

The literal old B0 acceptance requiring negative returns for every unexplained
missing close is intentionally not met by the corrected default. Root explicitly
directed the adverse-stress correction because the old clause manufactures short
profits. The implementation report records the deviation. Stress liquidation is
not evidence that a delisting occurred and cannot qualify tradable alpha.

## Validation status

No remaining blocker found by static inspection. Independently inspected the
final source freeze, fixture-only follow-up and runtime logs:

- `pool-4/build-equity/w0-replay-book-final.log`: 128/128 book tests passed,
  including causal-prefix, publication timing, all venue/side stress and repeated
  short gap/re-entry cases (13.048 seconds).
- `pool-4/build-equity/w0-replay-impl-focused.log`: 32/32 consumer/config/legacy
  report checks passed (41.93 seconds).
- The holding-window fixture correction explicitly includes a next decision four
  sessions later and asserts that interval length; it repairs fixture scope
  without changing production arithmetic or relaxing the expected return.

**APPROVE for integration.** Independently checked the final scoped PCH-off
configure/check logs and cache: `ATX_USE_PCH=OFF`, isolated FetchContent, and all
six touched production translation units compiled successfully. The final report
is `47e5ef8e2972e460674e7c70afa7936d835335c1`; production and fixture SHAs are unchanged.
Whole-impl qualification is deferred to root's integrated gate, as directed.
No real data were read.
