### Task T32: NAV consumes terminal returns (C++)

**Lane:** C (same pool-5 after T33b, or pool-7) · **Model:** Opus 5.5 · **Depends on:** T33b schema · **Tokens:** NAVSIM · **Peak:** 0.3 GiB

**Files:** Modify `atx-impl/src/strategy_nav_replay.hpp:114-116` (`NavFinancingFields`-style loader → `NavTerminalFields`), `strategy_nav_replay.cpp:62, 1320-1340` (load), `:358-371` (`carry_absent`), `:1365-1378, 1421-1423` (scenario η), summary; test `strategy_nav_replay_test.cpp`.

**Interfaces:** Consumes `terminal_kind`/`terminal_return` from the fields manifest (optional; absent → today's behaviour). Produces: in S1/S2, `carry_absent` write-off uses `eta_i = terminal_return[i]` when `terminal_kind[i] != 0` and the terminal session ≤ write-off session, else the scenario η; S3 unchanged (K = 1 adverse); events CSV gains `kind` = `write-off-terminal-<mna|performance|unknown>`; summary adds `writeoff_by_kind {kind: {count, long_pnl, short_pnl}}` and a stress row `terminal-adverse-100` (η = −1 on longs) computed from the same run.

- [ ] **Step 1:** Loader mirrors `NavFinancingFields` (`:1320-1340`), validated against the manifest SHA; missing fields → `has_terminal = false`.
- [ ] **Step 2:** In `carry_absent`, replace `c.eta_long/eta_short` by a per-name lookup when available; short side uses the S3 convention (loss = +0.30 → for a short, η enters as the price move, so a −0.30 price move is a gain; keep the sign logic identical to S3's `assumed_missing_price_return(Unknown, side)`).
- [ ] **Step 3:** Fixtures: `TerminalReturn_ReprintAndShortSide` (a reprint after the terminal session is ignored; a short with a performance terminal gets the short haircut; a name never held produces no event) and `TerminalReturn_AbsentFields_BitIdentical` (no terminal fields → S2 outputs equal the pre-change SHA).
- [ ] **Step 4:** Report and commit: `feat(nav): per-name terminal returns in write-offs; write-off by kind; -100% stress (T32)`.

**Acceptance (root, T39):** reference cell re-run with fields-v7: S2 net SR change reported with its paired SE; `writeoff_by_kind` shows shorts are not the main beneficiary (|short_pnl| ≤ |long_pnl|) or the ledger explains why.

---

