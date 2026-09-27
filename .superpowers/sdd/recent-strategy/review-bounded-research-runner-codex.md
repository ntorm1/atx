# Independent bounded research-runner review

Reviewed root source `257ffd3b53477a0c2609cb365d7421daa239ea53` and correction
`11afd431eb21e1fce612947dd405b21dc3f1f8bb`, `scripts/run_bounded_research.py`.
Verdict: approved for the documented native/threaded research-command scope.
No commands, builds, data runs or limit tests were rerun by this reviewer.

The first source had two concrete gaps: direct-parent exit stopped monitoring
already observed descendants, and a transient process exit reset an entire RSS
sample to zero. The correction retains `(pid, creation_time)` Process identities
through parent exit, discovers descendants of retained live owners, catches RSS
lookup races per process, and terminates/waits only the retained owned set.
It also refuses a below-floor launch and records ownership identities. A limit
outcome remains failed even if the direct parent exited zero.

Inspected and independently verified executable and stdout/stderr SHA256 bindings
in the following root-pool receipts:

- `build-equity/strategy-runner-smoke/receipt.json`: source257ffd3b,
  completed/native0, 0.265s, sampled peak6,455,296bytes.
- `build-equity/strategy-runner-time-limit/receipt.json`: source257ffd3b,
  time-limit/native15, 1.109s for a1s bound.
- `build-equity/strategy-runner-child-limit/receipt.json`: source11afd431,
  time-limit/direct-parent-native0, 2.203s for a2s bound; three retained PID/start
  identities. Parent source owner reports all three observed processes gone.

The smoke and single-process deadline receipts precede the correction; the
descendant receipt exercises the final source. The owner disclosed that an initial
verification assertion expected exactly two processes, whereas the same recorded
attempt observed three; changing that assertion to at least two did not change or
rerun production.

This is a sampled operational limit, not a process sandbox or hard allocation cap.
Commands must not detach children between samples. RSS/time can exceed a threshold
between250ms samples and during cleanup; recorded peaks are sampled. Access/control
errors are failures, not successful completion. The helper bounds its new receipt
directory, passes argv without a shell and hashes explicitly supplied small bindings;
it does not inspect or confine arbitrary child output arguments. Actual research
callers still own their output and artifact contracts. No alpha evidence or timing
comparison follows from these guard checks.
