# Fresh membership repair review

Reviewed `af40186d` relative to `bc5cc646` and the scope/report commit `39347e28`.
Review mode: static plus independent runtime check; implementation author and this
reviewer are separate agents. Final status: APPROVE.

No blocking code findings. The owning focused/full logs passed and were independently
read; a separately executed mask spotcheck passed. Exact evidence is recorded below.

- The VM owns the mask and rejects invalid shape/binary values without mutation.
- Every one of the 16 CS opcodes uses the same masked valid-index construction. The
  fused evaluator leaves CS instructions on this dispatch; date-parallel work shares
  only immutable mask data and uses the existing private row scratch.
- Cache-aware subset/root evaluation disables reads and writes to the mask-blind
  subtree cache. Clearing eligibility restores the original unmasked cache path.
- Field loading retains the observation mask, preserving public price history for
  entering members. Rolling CS features receive historical per-date eligibility over
  the complete feature warmup, so pre-entry normalized values are not invented.
- The family evaluator accounts for the extra feature mask and ID scratch before
  allocation; valid context/baseline bounds determine mask length and feature rows.
- The physically reduced-universe oracle is independent of output-only masking; the
  input-perturbation, joiner warmup, constant-member V1 and cache isolation tests target
  distinct failure modes. They are appropriate post-implementation verification.

G0 integration consequence: all 13 cp21 cells must use this repair before measurement.
Frozen L7/L10/L9 use an empty new CS mask and do not require reruns solely for this change.

## Runtime closure: APPROVE

Production source remains `af40186d`, with no fix pass. Independently read the owning
agent's focused log (27/27 passed, 96.01 seconds) and whole alpha executable output
(706/706 passed, 33.190 seconds). Verified its alpha executable SHA256:
`90FCBC5C23C2F4A7A0749F8AAF3959B87ACE995905B439A8D89B0E82F1E2637B`.

Then independently ran the unchanged pool-5 binary, read only from pool-3 cwd:

```
C:/atx-wt/pool-5/build-equity/bin/atx-engine-alpha-tests.exe
  --gtest_filter=AlphaCrossSectionMembership.EveryCsOpcodeMatchesAnActuallyReducedUniverse
[==========] 1 test from 1 test suite ran. (14 ms total)
[  PASSED  ] 1 test.
exit=0
```

Receipt/stdout/stderr live under
`C:/atx-wt/g0-data/bc5cc646_20260925/logs/membership-independent.*`.
Owning focused/full logs are copied beside that receipt for durable evidence. No
pool-5 files, source, build cache or binary were written by this reviewer. Root and
implementation agent received the runtime approval; corrected G0 runs are released.
