# Explicit reviewed identification contract

`bench/spy_acceptance.py` defaults to
`--identification-contract frozen-wave1-v1`. Its unchanged coverage rule requires
at least 12,183 identified rows and no newly PriceOnly baseline-identified row.
The original `spy-wave1-identification-v1.json` is unchanged.

The explicitly selected `reviewed-feasible-floor-v1` contract can accept only
the two reviewed changed-target classifications, row IDs 27418697 and 27419115.
They are calls at strikes 120 and 230 in expiry 21568. Their original asks are
below their terminal European floor. Both old fitted targets had valid IVs;
the new feasible-midpoint targets changed by less than $1e-7 and lie within the
deterministic zero-volatility reporting band. Correct final queries therefore
return PriceOnly, reason `floor_vol`, no point volatility, and interval [0, .005].
The numerical investigation is in
[raw-price-target-experiment.md](raw-price-target-experiment.md).

The review fixture `bench/fixtures/spy-reviewed-feasible-floor-v1.json` binds
the original input SHA-256, unchanged baseline content and evidence hashes,
both complete original rows, model/market/carry identity, old targets and IVs,
new actual API targets and metadata, and the independent cold-reference receipt.
The helper pins the fixture's canonical JSON SHA-256, so editing the evidence
cannot quietly enlarge the contract. Canonical JSON retains 64-bit integer
identifiers exactly and avoids platform newline differences. Original file hashes
are also retained as provenance.

For every newly PriceOnly row, the audit requires the reviewed identity, actual
`joint_feasible_mid` method, no fallback, the selected terminal-floor recovery
policy, and the exact reviewed classification metadata. The current raw quote
must equal the pinned original row. Current published query and node prices
must match the reviewed target within
`32 * epsilon * max(1, spot, strike, abs(actual), abs(expected))`.
This allowance covers only floating-point representation, approximately $5.5e-12
for these contracts; it is neither a spread budget nor permission for future
target changes.

The audit reconstructs effective rate and yield from the current market, actual
per-strike carry and aggregate funding PV. It independently recomputes the
deterministic exercise supremum and its ceiling-based band, verifies the original
ask is below the terminal floor, requires the old target to be above the band
and the new target inside it, and preserves the strict target-shift bound of
$1e-7. The pinned cold receipt establishes the old valid root and the sigma-floor
forward price. Current point-IV accuracy and floor semantics for every identified
row remain separate mandatory gates.

Every report retains `strict_coverage_passed` and the complete
`new_price_only_row_ids` diagnostic. It separately exposes
`reviewed_accepted_row_ids` and each recomputed proof. For the reviewed candidate,
the strict diagnostic remains false with exactly two IDs. Only the explicitly
selected reviewed contract can make `coverage_passed` true, and only after those
proofs and the unchanged minimum count pass. A third demotion, wrong input or
baseline hash, altered model, mismatched target, different status/reason/interval,
or missing/duplicate actual API evidence fails closed.

This is acceptance policy only. Neither the review module, fixture nor baseline
IDs are inputs to fitting, price projection or runtime classification. It does
not change coverage, containment, arbitrage, price accuracy, timing thresholds,
or the thirteen independent correctness gates.

Run the oracle regressions from `atx-vol/bench` with:

```text
python -m unittest test_spy_acceptance test_spy_identification_review
```

To select the reviewed contract on an existing native audit, retain the usual
input/audit-directory/output arguments and add:

```text
--accuracy-contract raw-spread-price-v1 --identification-contract reviewed-feasible-floor-v1
```

Historical strict verification receipts are retained. A reviewed verification
is a new artifact with its explicit contract and strict failure diagnostic.

Validation: all 57 oracle tests pass, including ten review-contract tests. The
unchanged integrated native artifact passes all thirteen correctness gates under
the explicit reviewed contract; its default strict invocation still exits 2,
failing only identification coverage for these exact two rows. Evidence is
`calendar/identification-all-oracle-tests.txt`,
`calendar/verification-reviewed-identification-v3.json` and
`calendar/verification-default-strict-identification-v3.json` under the r7 cache
run. The earlier strict receipt in the native audit directory is untouched.
Neither verification is a latency qualification.
