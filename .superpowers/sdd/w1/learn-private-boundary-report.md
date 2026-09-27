# Narrow learning implementation boundary

Source-only mechanical follow-up to the reviewed L1 source. Most cold fitting
and feature-building code was already in the two existing CPPs. This change
moves only the remaining non-template `detail::resolve_raw_fields`,
`detail::forward_return` and `predict_at` bodies into those CPPs. The three
bodies contain 43 lines including braces. Small row/maturity accessors remain
inline. Stale header-only comments are corrected.

All declarations, namespaces, value types, layout, defaults and arithmetic are
preserved. `detail::fit_standardization`, `build_design`, `fit_coeff` and
`oof_ic_series` declarations are untouched; GBT/common latent helpers are not
edited. No CMake change or extra source file is needed. Existing CPPs already
include every moved implementation dependency directly.

`learn-private-body-identity.json` records the exact LF-normalized body hashes
against source base `17917fd6`. The actual resulting CPPs contain each original
body exactly once. `git diff --check` passed. No build, new test, numerical run
or performance measurement was performed. Existing feature-matrix, linear,
GBT prediction and L1 dataset fixtures own runtime coverage on integration.

This is a small boundary improvement, not a demonstrated broad compilation
speedup. Larger include fanout remains through FeatureMatrix -> AlphaStore ->
AlphaStreams and through linear/latent/learned model type dependencies. Removing
those requires a separately reviewed type/API split rather than relabeling this
43-line move as a complete solution.
