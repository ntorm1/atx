# Linear calendar certificate and publication walk

This component replaces repeated strike searches with monotone cursors and
removes each publication pair's allocated/sorted union vector. It changes no
prices, tolerance, support or outer expiry-pair selection. Base:
`e000b9dd2e3b82189c64c1685a9ed33d5b25f5d1`. The separate cash builder remains
frozen at `7a1b1299`; no cash reference or six-option outcome ran in this lane.

## Preserved checks

`check_american_calendar` already merged union knots, but made four binary-search
price queries per knot. It now evaluates both sides through a pair of monotone
cursors. It still checks **knots only**, including both overlap endpoints, and
returns the same first call/put failure with the same comparison and error text.

Publication previously collected, sorted and uniqued the union for every pair,
then called the served-price API at every knot and midpoint. Each API call also
searched support masks when present. Publication now captures borrowed node/mask
views once and uses the same private pair walk with midpoints enabled. Views
remain local; their immutable owners outlive the validation call.

Exact node hits return original prices and only that node's support. Interior
points use both endpoint masks and the original fraction/std::lerp expression.
A malformed mask or non-Success fit serves no side. Disjoint domains still
produce no overlapping pair. Nested, partial and touching domains retain their
closed intersection. All outer pair loops and focus rules are unchanged.

The midpoint expression remains `0.5*(left+right)`, including endpoint rounding
and overflow. Its allowance remains that of the **right union knot**. A
nonfinite midpoint is unavailable and cannot advance the cursor past the next
finite knot. The certificate comparison remains `near-far > allowance`;
publication remains `near-far-allowance > 0`. Finding order and caps are unchanged.
The implementation does not invoke calendar transitivity or adjacent-only checks.

## Work attribution

Two groups are appended to the existing optional `FitTimingCounters`:
`calendar_certificate` and `calendar_publication`. Each has these uint64 fields:

| Field | Meaning |
|---|---|
| attempted_pairs | Actual pair-walk entries, including disjoint/invalid pairs |
| overlapping_pairs | Pairs with a nonempty closed strike intersection |
| knots | Union-knot visits actually reached |
| midpoints | Midpoint visits, including an unavailable nonfinite midpoint |
| side_comparisons | Actual comparisons; publication requires both sides served |
| node_advances | Interpolation-cursor advances, excluding the merge stream |
| wall_ns | Inclusive pair setup/walk time when a recorder is armed |

Early certificate refusal retains only work actually reached. Publication's
initial view-vector setup is inside the original fit/validation lifetime but
outside the pair-only subclock. The legacy `calendar_pairs` field is untouched.
Without a current recorder there is no clock read or diagnostic write. The
private helper also accepts a direct optional work sink for focused controls.
The quote-benchmark serializer is intentionally left to the integration owner.

The subsequent integration adds both groups to each slice's timing JSON with
schema `atx-vol-calendar-walk-work-v1`, an explicit pair-only clock scope and
`exclusive_fit_bucket=false`. A two-expiry, 104-row native CLI control verifies
the actual knot/midpoint/side/cursor counts and that later retained-instance
proof does not contaminate the cold-fit counters. Its first execution failed
on the missing JSON group; the implemented report passes that control and all
eight existing post-fit controls. The combined affected native closure passes
101 cases. These small controls are separate from full-SPY qualification.

The frozen normalized raw input's strike topology has 33 slices/6,433 unique
nodes and 528 overlapping pairs. Independently enumerating the closed unions
gives 102,645 knots and 102,117 midpoints. A complete unmasked pass would make
205,290 certificate side comparisons and 409,524 publication side comparisons.
This is derived topology, **not observed native work or a timing result**.
Its fixture hash is
`364abcecd0fb7af1e05915ed2d1e2e7aeac58c040d26bbf3ff417d800d3d9133`.

The earlier guide's roughly100ms attribution was not an isolated measurement.
This component makes no speed claim; the all-pair counts and subclocks allow
the complete fit to measure the effect after source review.

## Validation and remaining gate

Two unavailable-helper controls failed at runtime before implementation. The
final focused closure passes **83 controls**, with three external-fixture
checks skipped explicitly. Eight new controls cover:

- Scalar price-bit parity and exact/interior support masks.
- Union order, midpoint allowance and bounded cursor advances.
- Nonfinite/out-of-domain/backward requests without stale-bracket corruption.
- One-ULP intervals, signed zero and overflowing midpoint behavior.
- Empty, unequal, nested, touching and disjoint domains against an independent
  legacy binary-search reference, in both knot-only and midpoint modes.
- Certificate allowance decisions and actual early-refusal work.
- All 528 synthetic expiry pairs and the original focused-pair count.
- Original served-API finding bits/order under masks, malformed masks and
  refused fits.

The existing non-neighbour nested-domain crossing, publication, joint projection
and raw-draft regression controls are included. An initial synthetic fixture
omitted its effective carry diagnostics and therefore produced two legitimate
non-dividend certificate findings; only the fixture metadata was corrected.
That failed output is retained separately from the two intended runtime REDs.

Four changed owning translation units pass PCH-off compilation. No full-SPY
candidate, changed-output acceptance audit, standalone latency run or broad
performance claim has been made. Fresh exact-source review precedes the first
own-output SPY check. Evidence lives under
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`, with the
component receipt in `calendar-linear-walk-v1/receipt.json`.
