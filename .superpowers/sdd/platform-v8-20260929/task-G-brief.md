# Brief: task G

Plan: docs/plans/2026-09-29-atx-impl-v8-sprint-plan.md (sections 3, 4 and 6.3 bind every task). Rules: .superpowers/sdd/platform-v8-20260929/lane-rules.md

### Task G-1..G-3: zero-trial diagnostics

**Files:** create `atx-impl/tools/book_diagnostics.py`, `atx-impl/tools/test_book_diagnostics.py`

Declared in `v8-prereg.md` before any read: no diagnostic gates, selects or re-weights anything. Output is one file,
`diagnostics-v8.json`, plus a section in the scorecard.

| id | diagnostic | input | answers |
|---|---|---|---|
| G-1a | `ic_theta`, `f_theta`, marginal IC per member | cards, K6 | which members contribute at the traded horizon |
| G-1b | turnover attribution: planned turnover of each theme's own aim | NAV with `--emit-holdings` | whether the four fast members carry 75% of turnover |
| G-1c | netting ratio: combined-score turnover over weight-averaged sleeve turnover | same | how much trades cancel |
| G-2a | variance split of the accepted book: factor, industry, specific | `risk --book-weights` | whether name-level risk sizing can matter |
| G-2b | IC by volatility tercile and by ADV tercile; IC in the top 1,000 by size | cards | which alpha scaling holds; where capacity is |
| G-2c | holding over ADV distribution (p50, p95, max) | holdings | whether R-5 binds |
| G-3a | borrow stress `S2-FEE`: fee by decile of short interest over institutional ownership, schedule 25, 25, 25, 25, 25, 25, 30, 50, 150, 570 bps | `si_shares`, `shares_out`, `inst_own_share` | how much of net Sharpe is a flat-fee artefact |
| G-3b | low-risk members' IC before and after the beta and volatility projection | cards | whether the theme survives its own neutraliser |
| G-3c | signal-lag sensitivity: book net Sharpe with the combined signal delayed 1, 2, 3 sessions | NAV | cost of slower execution |
| G-3d | cluster map: hierarchical clusters of sleeve return correlations against the theme labels | cards | whether the 10 themes are 10 bets |

- [ ] **Step 1:** tests on the fixture for each function (planted fast member shows the highest turnover share; a planted
  fee schedule reproduces a hand-computed drag). **Step 2:** implement. **Step 3:** root runs on cell B0c and commits
  `diagnostics-v8.json`.

`S2-FEE` is a descriptive scenario like S2-KO. S2 stays primary.

---

