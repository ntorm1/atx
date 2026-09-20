# Price failure recovery: independent controller review

One Codex source review on2026-09-20, independent of the fresh implementer.
Reviewed connection.py recovery seam, ticker_history_bulk.py failure handling
and logging, activation.py fallback ledger handling and four focused cases.

Verdict: **CLEAN; no Critical or Important findings.** Four focused cases passed
under a2GiB guard, reported native peak0.595GiB. Review did not rerun tests or
touch the warehouse. Accepted implementation SHA: `59dc04e7`.
The companion price-failure-recovery-report.md records the focused validation.

Recovery closes the raw connection without issuing SQL to the invalidated
connection, passes the recorded memory/thread budget at connect before WAL
recovery, and does not initialize/migrate or retry publication. Original
exceptions are re-raised. Failed attempts can be recorded in dataset and stage
ledgers after recovery; an unavailable reopen remains an explicit operator
recovery requirement without masking the initial exception. In-memory recovery
is refused because a new connection would lose that database's state. Staged
prices remain available for inspection, and the invalid logging formatter is
corrected. Source data, identity and publication semantics are unchanged.

The separate operator action closed the already failed attempt's two ledger
rows at16:59:42UTC, as a recorded recovery observation. This code change does not
claim to solve the1GB commit budget failure. A2GB query retry remains subject to
the same3GiB process cap and preflight headroom checks; full publication success
must be measured independently.
