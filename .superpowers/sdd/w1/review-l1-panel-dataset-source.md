# Independent W1-L1 source review

Reviewer: pool5 / w0_gate_audit. Read-only frozen pool3 production:

- `603e7e9cf070f7302f06bab6ea3a5d66a648a0c7`: streamed dataset and actual adapters.
- `e672bcd8dd58955c370f29a8f7962a1cc27904d5`: absent-close mask and identity clarification.
- `6cf937f886ad23e394d3d431afe925c9fcba8970`: early endpoint/PCA admission and IEEE format.
- `342563f6da47527d6adaef7a9e475cda2b11f71e`: five postimplementation owning cases.
- `3d1998929a2a7f25eb5a9e266a5bb42ac79d27a0`: lane report and future-PCA assertion.

Verdict: APPROVE for focused compilation qualification. No remaining confirmed
source blocker in this bounded review. No C++ build, runtime rerun, numerical
experiment or market-data access was performed. The author report correctly
leaves all five fixtures uncompiled/unrun and does not claim the large RSS gate.

## Production contracts inspected

The builder retains independent presence and decision-membership masks, including
member rows with absent source values. It ranks finite present values among the
current member cohort, uses average ties, and stores a separate missing indicator.
Nonmember rows cannot influence ranks. Rank rounding is explicit f32; return entry
and endpoint prices and labels remain f64. D6 non-close features already stored as
f32 may have collapsed ties; original-f64 feature parity is not claimed.

Volatility normalization uses only adjacent observed daily returns ending strictly
before each feature date, with an explicit sample-count minimum and floor. Missing
prices do not bridge a gap. Entry/endpoint must be finite positive and the endpoint
must precede the exclusive maturity boundary. Date demeaning uses only finite
member labels and refuses a cohort smaller than two. Unavailable labels stay NaN;
there is no missing-return zero imputation or invented terminal evidence.

The original Panel adapter defect was independently found by root: a finite value
behind an absent source mask could enter volatility and labels. `e672bcd8` closes
it by mapping absent closes to NaN before the generic source callback returns.
The actual Panel-versus-D6 fixture deliberately supplies absent backing price 777
and checks every label and mask, so it exercises the real adapter composition.

The manifest binds axes, feature order, declared source/recipe, transformation,
holding horizons, execution delay, volatility recipe, maturity and budgets. Blocks
bind exact bytes, checked geometry, clocks and masks. Publication occurs only after
all blocks via an exclusive manifest publication step. Reader admission checks
captured extent before mapping, then SHA/header/geometry/value invariants before
exposing spans. Shared mapping byte/handle limits survive reader copies and block
lifetimes; mapping release precedes budget credit return. The bounded D6 adapter
checks source manifest/axes and accounts for its bounded source mapping cache.

Plain Panel/AlphaStore objects cannot authenticate caller-supplied source axes or
historical availability. The public contract and report now state that these are
caller assertions; the produced payload itself is hash-bound. D6 identity binds
its parent manifest, preserving that parent's precision and vintage limitations.
No source identity is falsely upgraded into independent proof of causal data.

The selected-window materializer retains complete session ordinals, emits member
rows with separate presence, and carries endpoint horizons as delay plus holding
horizon. It refuses future feature rows and hides labels that are not mature at
as-of under the existing inclusive endpoint convention. Its separate budget
covers selected-window matrices/row metadata and mapping allowance; it does not
implicitly materialize a large complete dataset. The checked linear consumer
requires DateV2 and exact endpoint metadata, rejects future-fitted supplied PCA,
and forwards existing fold-local augmentation, purge/embargo and CPCV admission.
The early recipe/PCA checks in `6cf937f8` precede materialization. The legacy raw
feature path only acquires its previously missing horizon metadata; it retains
its raw values and existing invalid-row behavior.

## Reviewed postimplementation checks and limits

The five cases include an independent rank/volatility/label oracle with 40% feature
missingness; protected-prefix future mutation and truncation; endpoint equality;
shared mapping lifetime/budget and changed extent/hash refusal; no publication on
callback failure; strict membership-clock equality; actual Panel/D6 producer
parity; and a tiny checked DateV2 fit with horizon/PCA/budget refusals. The added
future-PCA assertion directly exercises the final source guard. Required direct
`<array>` inclusion is present in the frozen fixture source.

The 3000 x 1750 x 500 case is only a metadata/storage-formula admission check, not
an allocated artifact or measured process RSS. The fit adapter is bounded window
materialization, not whole-dataset streaming training. Dataset/window identity is
returned alongside the model; the old model-only serializer does not automatically
retain it. Exposure residualization, full streaming training, serializer migration,
empirical label quality, large RSS and tradeable alpha remain separate gates.
