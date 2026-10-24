# Feasible quote-midpoint target review

The original raw-midpoint prototype serves every SPY price but directs many
deep ITM observations to their lower model floor. Its first certified solution
was already close to the quadratic optimum: primal residual `4.94e-11`, dual
residual `2.20e-5`, and mean complementarity `3.49e-10`. Extra solver iterations
would not address an objective whose midpoint is below the model floor.

Of 840 frozen-baseline identified rows that became PriceOnly, all were FloorVol
at the projected target. Five hundred original midpoints were already at or
below the floor; 838 quoted asks had more than $0.001 above it. No status was
overridden. Re-running the existing economic candidate on those unchanged
floor prices identified none of the 840.

The new private target selector explicitly distinguishes `OriginalMid` from
`FeasibleBoxMid`. The latter sets each target to

`0.5 * max(effective_bid, intrinsic_floor, terminal_European_floor) + 0.5 * effective_ask`.

Selected call floors are already part of the effective lower bound. Effective
boxes preserve original duplicate intersections and the separately enabled,
proved-floor-only ask recovery. No other bid or ask changes. In particular,
choosing a target cannot widen an inconsistent box: an empty floor/quote interval
returns a typed refusal. Original quotes, raw midpoint targets and raw spread
budgets remain untouched. The public objective is named `JointFeasibleMid`;
its report string is `joint_feasible_mid`. It cannot be mistaken for the earlier
raw-midpoint or legacy curve objectives.

On the independently normalized raw input fixture, the private comparison is:

| Target | Served expiries/rows | Raw-box departures | Identified | PriceOnly | Baseline demotions | Gains |
| --- | --- | --- | --- | --- | --- | --- |
| Original raw midpoint | 33 / 12,866 | 15 | 11,767 | 1,099 | 840 | 424 |
| Feasible quote midpoint | 33 / 12,866 | 15 | 12,850 | 16 | 2 | 669 |

The feasible-midpoint projection took 36 iterations. The comparison checks the
exact original row-ID universe and input hashes. Baseline IDs are used only by
the offline comparator, never by target construction, projection or fitting.
The native smoke checks every final row through `query_quote_slice`; it does
not replace the independent complete API/cold-price acceptance audit. Its
development elapsed time is not a qualified fitting latency.

The remaining two transitions are real fitted-price changes, not evidence that
the baseline was invalid. Both calls are in expiry 21568 and have original asks
below their proved terminal European floor. Independent cold Reference prices
at the baseline IVs reproduce the old targets within $3e-12. Search identifies
the old target, while the new target lies inside its deterministic zero-volatility
band and therefore clamps to the reporting floor 0.005. The typed fitter correctly
publishes PriceOnly for the new target.

| Row ID | Strike | Raw bid / ask | Old target | New target | Zero-vol value |
| --- | --- | --- | --- | --- | --- |
| 27418697 | 120 | 651.530029296875 / 655.5 | 655.9104557785998 | 655.9104556897006 | 655.9104550052318 |
| 27419115 | 230 | 554 / 558 | 558.005390252233 | 558.005390232761 | 558.0053894733147 |

For both contracts, spot is 773, variance time is 2.257885694503784,
effective rate is 0.0515890046954155 and effective yield is
0.005931800012831142. The analytic exercise supremum is at maturity; the only
stationary times, 6.57494 and 20.82434 years, lie outside the exercise interval.
Cold Reference at sigma 0.005 agrees with that supremum to $1.2e-13. The Search
band is `1e-9 * 773 + 1e-12 = 7.73001e-7`. Old targets are just above this band;
new targets are inside it. The baseline IVs are 0.035424785275844375 and
0.13667756557476937 respectively. No artificial premium or point IV was inserted
to preserve an old label.

The strict zero-demotion diagnostic remains **failed by two rows**. Any acceptance
of those genuine PriceOnly transitions requires a separately reviewed, narrowly
pinned audit record containing old/new targets, contract identity, raw ask
conflict, analytic/cold floors and classifications. It must retain the strict
diagnostic, the minimum identified count, and every other coverage, containment,
arbitrage and cold-price gate. The runtime algorithm has no exception list.

Evidence is under `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/calendar/`:
`feasible-mid-diagnostic-v1.json`, `feasible-mid-comparison-v1.json`,
`feasible-mid-full-v1.xml`, `two-floor-classification.json`, and
`two-floor-classification.xml`. The normalized raw fixture SHA-256 is
`364abcecd0fb7af1e05915ed2d1e2e7aeac58c040d26bbf3ff417d800d3d9133`.
This candidate remains experimental pending the full independent 13-gate audit,
archive-policy enforcement and fresh one-core release measurement.
