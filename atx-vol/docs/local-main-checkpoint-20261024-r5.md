# Local main backtest coverage and Greek-reference checkpoint, 2026-10-24

This user-requested checkpoint advances local main from `a9f8b36f` with the
reviewed backtest coverage/persistence component and second independent Greek
study. Combined source is `a92654f7d42b144294e440b1682d790784ff0983`.
Component sources `a5c8fe79`, `3fb49fff` and `05099d9a` were freshly approved
before integration. Only append-only ledger entries conflicted; both histories
were retained. All fifteen coverage code/test files match reviewed source exactly.

Backtest output records how many option positions have a supported price and
how many of those lack requested Greeks. Counts describe each recorded snapshot,
including partial-price and carried positions. They are not block-summed flows
or a claim that aggregate risk is finite. Unknown legacy coverage stays an empty
pair, never fabricated zeroes. Known and unknown nonempty histories cannot be
appended together silently.

Both counts survive archive and BacktestDb save, read and append through owned
optional F64 columns. The existing 27-column archive registry, schema hash and
unknown-coverage golden bytes are unchanged. Malformed shapes, noninteger counts,
invalid cross-counts and reserved-name collisions are rejected. Python exposes
owned arrays. Frozen TSV/CSV columns still omit these optional observations.

Compatibility changes are explicit: the native `BacktestResult` aggregate grows
from 53 to 55 fields, so C++ consumers must rebuild. The research
`encode_backtest_section` function now returns `Result<RaSectionData>`; all three
production callers propagate its error. No fitting or pricing default changes.

Combined validation and exact evidence hashes are recorded in
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/integrated-coverage-receipt-v1.json`.
The native checks pass 122 backtest, 49 archive/RunDir and 26 BacktestDb tests.
The archive tests include the unchanged legacy golden, actual save/read/append
and mapped checksum-valid malformed coverage controls. The latter exercise
mapped spans and the shared validator, not a forged BacktestDb manifest.
Eleven component owning translation units compile without PCH.

The full available Python suite passes 805 tests, with 245 environment skips
and one existing strict expected failure. Both isolated partitions exit zero;
the imported native module is byte-identical to this integration build. Optional
third-party Windows PyArrow loader diagnostics remain in the complete log.
The receipt SHA-256 is
`fb80348552cb09676725367a09d7904ff1377d5da848434256080971300321f9`.
An initial database command selected the wrong executable and ran zero tests;
that vacuous attempt is retained and rejected. The owning storage executable
then ran all 26 required database controls successfully.

The umbrella suite retains 7 passes and 3 existing failures. All 14 assertions
match the preceding checkpoint exactly: stale Tier-B inventory and twelve
source-path literals. The earlier checkpoint also reproduced these against
clean prior-main source. The historical BEV numeric pin remains open. Neither
is masked or described as a green release gate.

The integrated independent-reference controls pass all 23 tests. Both archived
studies reproduce every reference and candidate decision, 2,304 comparisons
each. The new study retains all 144 requested solves: 138 computed and six
refused; it resolves Delta for 10/12 and Gamma for 7/12 contracts under its
unchanged empirical rule. Its comparison has 1,386 passes, 54 failures, 312
candidate refusals and 552 unresolved references. See
`independent-greek-pilot-v2-20261024.md`. This is evidence tooling, not a new
production Greek default or complete Greek qualification.

The candidate-proof batch implementation, block/vector fitting alternatives,
cash Gaussian work and prospective LCP correction remain separate research.
The complete SPY economic budget, coverage and arbitrage requirements are
unchanged. This checkpoint establishes no new latency, subsecond qualification
or worldwide performance claim. No remote push is part of this checkpoint.
