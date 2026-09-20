# Core metric breadth fix 1

The production definitions are correct. The focused test used the wrong
trailing-flow window at `_QUARTERS[4]` (2020-03-31): it summed fixture quarters
0 through 3, while `ttm(...)` at that period must sum quarters 1 through 4.
The average balance inputs remain correct: `avg2` uses the current Q1 balance
and the Q1 balance four quarters earlier.

| Measure | Numerator | Correct denominator | Expected value |
| --- | ---: | ---: | ---: |
| DSO | `(20 + 24) / 2 * 365 = 8,030` | `110 + 120 + 130 + 140 = 500` | `16.06` |
| DIO | `(30 + 34) / 2 * 365 = 11,680` | `62 + 64 + 66 + 68 = 260` | `44.9230769` |
| DPO | `(15 + 19) / 2 * 365 = 6,205` | `62 + 64 + 66 + 68 = 260` | `23.8653846` |

`cash_conversion_cycle` remains tested as `DSO + DIO - DPO`. The correction
changes only the three test expectations and adds a comment fixing the relevant
TTM boundary. It does not change CSV definitions, source inputs, the derived
engine, or migration 0315.

Root already has the guarded project-interpreter command in
`core-metric-breadth-report.md`; run only the affected test after the current
writer and PIT work are stable. This repair performed no application or runtime
workload.
