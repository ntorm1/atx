# W1-E6 / E-13 independent source review

Reviewed root commits `b17356ad53bc0b3c9e08ffc409b3edfa7bf8e7d9`,
`a53dbb6324c4e9432b37fd4982d581de1d553c8f`, and
`dce7312cfa6ed0d37fc59ec29aa3a32b6f939034` in
`combine/combined_source.hpp` and its owning combined-source fixture TU.

Source approval: no remaining blocker in the reviewed scope. Actual weighted
product exponent scaling preserves zero-weight and reciprocal-scale cases;
fixed-order compensated reductions precede finite date-local standardization.
Missing opinions are neutral in the fixed-gross recipe, entirely missing rows
remain unavailable, and constant finite cross-sections are zero. Explicit
`LegacyPerCellV1` retains its old blend arithmetic.

The first review identified that documented distinct constituent pointers were
not enforced, allowing one evaluation to invalidate an earlier borrowed span.
The final commit rejects null/duplicate source identities before evaluation,
using the standard total ordering for pointers; its allocating constructor is
no longer `noexcept`. It adds a duplicate-source death fixture and a direct
weighted-range underflow refusal fixture. Those close the source findings.

This is an independent read-only source review. No compilation or runtime,
performance, scale, or tradeable-alpha qualification was performed here.
