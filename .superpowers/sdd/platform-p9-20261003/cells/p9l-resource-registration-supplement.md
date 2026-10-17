# P9-L resource registration supplement (R15-REG-BODY)

Before w attempt 2, this supplement supplies the exact complete JSON diff of registration commit
`f560f741`. That commit's body omitted the requested diff. No commit or spec is rewritten.
Each of the two files changes only by adding these entries at `change.set`:

```diff
--- scripts/specs/p9/templates/label-terminal.json
+++ scripts/specs/p9/templates/label-terminal.json
+      "ic.w_flags.--max-memory-mib": "3584",
+      "runner.phases.w.max_rss_mib": 3584,
--- scripts/specs/p9/templates/label-terminal-cal.json
+++ scripts/specs/p9/templates/label-terminal-cal.json
+      "ic.w_flags.--max-memory-mib": "3584",
+      "runner.phases.w.max_rss_mib": 3584,
```

Template SHA-256 `1d15513e74851e776a7e40d538f528690fbab04c4a92bb8afdcc240a805e3170`;
calibration recipe SHA-256 `1f00e19ec9dc3092ebe570b9645700910062881835dc9cd391623cff76c00062`.
The PRE-40 and plan restatements are committed at `7a49c825`; the saved plans at `89477326` verify the
unchanged input pins and the two w budgets. No w attempt 2 has launched. RAM waiting began at
2026-10-17T13:22:23Z, with a fixed deadline of 16:22:23Z; this paperwork does not reset it.
