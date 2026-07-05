# Sprint 1 progress ledger — Activate Crowding Defense

One line per clean review (ROADMAP §141). Newest last.

| Unit | Commit  | Deliverable                                                              | Review |
|------|---------|--------------------------------------------------------------------------|--------|
| S1-0 | 36a990e | `--dead-alpha-lib-dir` config field + CLI parse arm                      | —      |
| S1-1 | 15637db | thread accumulating library into `build_risk_model` (3 optimize sites)   | —      |
| S1-2 | 7a1dfc8 | twice-run + dead-id order-invariance proofs for the S1-1 wire            | —      |
| S1-3 | 1abc93f | byte-identity guard for opened-but-empty dead-alpha library              | —      |

**Review (clean):** SHIP. 19/19 optimize suite green byte-identical; acceptance grep confirms zero `nullptr` in any `build_risk_model(` call in `stage_optimize.cpp`. Coverage gap (opened-lib / zero-admitted byte-identity) closed by S1-3.
