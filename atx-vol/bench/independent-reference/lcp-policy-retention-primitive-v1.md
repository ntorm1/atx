# Admissible-row policy retention: primitive evidence

This component implements the prospective plan `e8060935`, approved by the
parent and independently by acceptance. It has **not** run the four saved
systems or any PDE. Fresh exact-source review and a changed-selector observer
remain required before the capped candidate experiment.

`lcp_policy_retention.py` loads the hash-pinned original correction source into
a private module. The experimental mode recompiles only its correction
function after replacing exactly one reviewed assignment with a call to a
per-invocation selector. The source bytes, original/default functions, linear
solve and correction arithmetic, residual admission, resource pair reservation
and refusal snapshots are unchanged. Default calls use the original compiled
function and return the original schema and bits.

The selector retains the just-used policy only when the current original row
test passes. It reselects a failing row with the original comparison. A retained
row participates in every coupled solve and global residual test. Experimental
answers/refusals carry a distinct mode and logical policy counts. The maximum
experimental budget is256 linear calls, while the original n+2 bound remains
visible. The selector reserves32*n bytes, conservatively covering its Boolean
buffers and floating minimum/absolute temporaries; it is not a process-memory
bound. Allocation failure publishes no values and does not fabricate an
unobserved original residual. If allocation fails before selector creation,
policy work is explicitly unavailable rather than an invented zero count.

The small runtime RED ran9 tests against refusal stubs. The final13 controls
pass, including:

- An independent33-ulp positive obstacle gap with a slightly larger positive
  continuation residual: the strict selector chooses exercise, while the
  approved rule preserves the actual previous continuation selection.
- Four actual three-row coupled LCPs checked against independently enumerated
  policy solutions. A dedicated neighbor-motion case requires two updates and
  releases a row that was initially obstacle-admissible.
- Exact-threshold retention and mandatory reselection for negative continuation
  or obstacle gaps beyond the unchanged tolerance.
- A deliberately wrong constant selector, labeled as scripted, that cannot
  bypass global admission: actual solves refuse at n+2 or the128-update cap.
  This is an admission/budget control, not evidence about the retention rule.
- Partial pair budgets, original nonfinite-solve refusal, source/hash/assignment
  guards, private buffer ownership, per-call reset and controlled allocation
  failures. No incomplete selection is counted as completed.

Combined commands from `atx-vol/bench/independent-reference`:

```text
python -m unittest -v test_lcp_policy_retention test_lcp_observe test_lcp_correction test_pde_reference.ObstacleTests
python -O -m unittest -v test_lcp_policy_retention test_lcp_observe
```

All44 combined controls pass; all29 optimized controls pass. Python syntax and
diff hygiene pass. Evidence and exact source hashes are in
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/repair-iv/lcp-policy-retention-primitive-v1/receipt.json`.

The selector test establishes the intended local decision, and small coupled
tests guard basic correctness. They do not establish convergence on the four
large systems, a general finite-precision theorem, a better reference grid,
Greek accuracy or production suitability. The existing observer's policy
evidence is defined by the original strict selector and must not be silently
reused for this modified mode.
