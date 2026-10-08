# Exact pinned consumer bundles

`golden_export.export` first public-validates the immutable golden generation
and admits the exact independently pinned consumer role. It writes an isolated
pending bundle, runs complete native cell replay, rechecks the golden source,
then publishes by rename under an exclusive writer lock. Existing bundles are
immutable. `validate_export` repeats the native public validators and every
field/date/security value comparison. `validate_bundle` is only offline shape
QA and does not establish consumer runtime acceptance.

The role axes, source identity, close/raw-close/volume conventions, date/id
ordering, present/member masks, score bounds, prior-63-session ADV recipe and
existing seal are preserved. Role membership is independently reconstructed
from prior raw-price/share-volume observations with the exact price >5,
USD ADV >5,000,000 and securityID tie rules. It is separately audited against
the panel's all_common population; the two populations are not conflated.
Historical-vintage/common-stock verification remains false where the role
declares false. No consumer source/mask/benchmark changes are authorized.

Select 1..64 point-in-time fields explicitly. Every field is little-endian f64,
date-major with the exact pinned role shape and canonical NaN for missing or
absent names. The manifest binds native units/definition/clock/staleness,
source generation identity, axes/role recipe, full file SHA/extent and explicit
exclusions for all unselected full-panel fields. Full registry breadth remains
in the panel; <=64 is a bundle boundary, not a feature selection heuristic.

The root-owned real `atx-impl` load and smoke test remains
PENDING_ROOT_ATX_IMPL_LOAD_SMOKE. No offline dtype/shape/native replay test is
consumer acceptance and no actual bundle has been published in this delivery.
