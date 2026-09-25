# Fresh membership repair review

Reviewed `af40186d` relative to `bc5cc646` and the scope/report commit `39347e28`.
Review mode: static; implementation author and this reviewer are separate agents.

No blocking code findings. Runtime acceptance remains conditional on the implementation
agent's pending focused and full target results; this review did not run those tests.

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
