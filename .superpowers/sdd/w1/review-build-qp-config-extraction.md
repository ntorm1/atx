# Independent source review: QP and configuration extraction

Reviewed `008fcdb95e4054a9d5c2a2afdfdd22edc2b74c84` and
`7ee0b04bdaf11e7927c882ddf7a3bb49f184a6fa` from git objects, plus relevant
read-only caller/CMake source in pool-2. Source review: **approved, no blocker**.
No configuration, compilation, tests, numerical run or cache mutation performed.

The QP implementation class is exactly the original 1,334-line class after
restoring its declaration name and its static default-validation construction.
Independent normalized-body SHA256:
`980c9a9319fad83d1d6d90a4cb93c690ef31000d5553bb08cafeb57edf68a939`.
This agrees with `pool-2/build-equity/w1-qp-private-move.json`; the comparison
was recomputed, not inferred from that receipt. Error strings and arithmetic
remain unchanged. Public configuration, problem, certificate and result
declarations are also text-identical.

The six out-of-line wrappers retain overload selection, const qualification,
default warm-start argument and configuration. Their private temporary contains
only a copied `QpConfig`; borrowed problem, schedule and warm-start inputs are
used within the synchronous call, and results own their vectors. There is no
new persistent state, allocation for a pimpl, or exposed implementation type.
The implementation class has anonymous-namespace linkage; public methods have
one compiled definition. The new source is registered once in the existing
engine target with its existing options. Specialized KKT/factor-ADMM tests retain
their direct algorithm includes. Existing shared-library export policy is
unchanged.

Each extracted configuration declaration block exactly matches its predecessor:

| New header | SHA256 of moved declaration block |
|---|---|
| eval/cpcv_config.hpp | 04c55a4e8c1bcc802dc84e097bb3c74be12b83fba02d09e914dab7007a50663a |
| eval/pbo_config.hpp | c52e94bc62ea24740bb835f068f9c1b24a6b86aeffef361a1821b0d116106595 |
| factory/ic_screen_config.hpp | 0eade4dbd344b23d09a79729d6de6c30fa3e5c025b7dfef7a641c8f1e870278e |

These headers directly include their required small standard/type headers.
Algorithm headers include their owning configuration header, so old include
paths remain valid without duplicate enum/struct definitions. CPCV/PBO naming
functions remain constexpr; the active IC recipe factory remains declared once
and defined in its existing source. Application defaults, member layout and
parser implementation do not change. Actual algorithm consumers inspected in
discover/combine/mine retain algorithm includes.

Runtime/link qualification and numerical parity remain pending the parent's
single combined build. Text identity does not alone establish bitwise parity
after moving code between compiler translation units. This approval covers only
these two extraction commits, not subsequent risk behavior changes or new L1/R3
features. The QP public header still includes augmented-form/factor/constraint
types; this is a bounded parse-cost reduction, not a claim that all Eigen or
other transitive dependencies have been removed.
