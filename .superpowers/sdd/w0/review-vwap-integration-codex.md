# Independent D0 VWAP integration review

Source review of frozen production `deb939d25965fed0dd04f4493a8f91156a337e76`.
**Initial changes requested; see the fix-only review below. Runtime qualification pending.** No production edits,
configure, compilation or test execution were performed by this reviewer.

## Required repairs

1. **High: raw_close can be indexed before geometry validation.**
   `alpha/datafields.hpp` chooses a span from raw_close then indexes it in the
   dollar-volume/VWAP loops. For a 1×2 panel with valid two-cell OHLCV columns
   but a one-cell raw_close column, the old path left the extra raw column alone
   until Panel::create rejected it; V2 now reads beyond its end. Validate checked
   dates*instruments, name/data cardinality, every column length and nonempty mask
   length before indexing/allocation. Preserve valid V1 bytes. Postimplementation
   malformed-geometry cases must return Err, including a short raw_close.

2. **Context policy forwarding is missing.**
   `data/context.cpp`'s augmented `DataContext::price_panel()` path calls the new
   strict price_to_panel defaults. `DataContext::create` offers no VwapRule or
   ClosePriceBasis parameter, so ordinary known-raw OHLCV datasets cannot request
   explicit Raw and old users cannot select V1 through this API. Forward explicit
   policy/basis through context creation and move state while retaining default
   Unknown rejection. Cover default rejection, known Raw and V1 reproduction.

3. **Separate adjacent repair authorized by root: capacity resume identity.**
   This predates deb939d2. `compute_discover_fingerprint` and persisted
   `build_config_json` omit min_price/min_adv_usd/adv_window. Active-capacity runs
   with changed masks/ADV windows can share a fingerprint and resume stale scores.
   Bind all three under the same effective augmentation condition as VwapRule
   (`min_adv_usd > 0 || min_price > 0`); prove each active mutation changes identity
   and unused settings leave unaugmented identity unchanged. Keep this in a
   separately attributed commit; no general resume configuration audit is requested.

## Inspected behavior and remaining evidence

- Default raw proxy requires raw_close or explicit Raw close basis, refuses
  unknown policy enum values and overwrites supplied stale VWAP. Valid cells use
  finite positive raw price and volume, assigning raw price directly to avoid
  overflow/underflow in the algebraically cancellable product/division.
- Explicit V1 keeps supplied VWAP or the old (high+low+close)/3 arithmetic.
  Source inspection found no valid-input arithmetic/order difference in the
  preserved WQ101 recipe; exact byte equivalence still requires runtime evidence.
- real_panel declares its unadjusted Databento adapter input Raw and exempts V2
  VWAP from later TRI scaling. Legacy V1 follows its old candle scaling. Metadata
  distinguishes raw versus adjusted VWAP. Adjusted OHLC and mixed-basis expressions
  are explicitly still unsafe for unrestricted cross-sectional use; this patch
  does not claim every field's level is future-invariant or close the later lint.
- stage_panel forwards the policy, refreshes augmented field basis and binds the
  recipe's rule/basis. Mining forwards it for all roles and binds the report through
  manifest SHA. Discovery binds VwapRule only when its capacity adapter executes,
  matching the conditional numerical path; item 3 completes that capacity identity.
- The small enum/name parser header supplies its own standard includes. Header
  include hygiene and dependent production TUs still need scoped PCH-off evidence.
- Frozen N128 benchmark input digest did not previously exist. The required
  fixture proof must compare all names/order/masks/f64 bits at 2520×128 against
  an independent pre-D0 recipe oracle. Calling current datafields(V1) on both
  sides is insufficient. No benchmark timings may use a changed numerical fixture.

## Warm qualification scheduling audit

Read-only cached-graph audit of pool5's Debug equity-dev found groups
alpha;eval;combine, PCH ON, isolated deps/equity-dev and 54/54 alpha test objects
present. No data group/executable is configured. The original test PCH owner is
alpha; neither project PCH payload includes the D0 headers.

Cached Ninja dependencies identify nine affected existing alpha TUs:
alpha_datafields_test, alpha_widened_conformance_test, fusion_test, iv_fields_test,
liquidity_fields_test, multi_family_smoke_test, streaming_engine_test,
subtree_cache_test and vm_cs_pool_test. Five engine objects depend on D0 headers:
adapt_panel, context, real_panel, universe and history_panel. Four additional
engine objects may need D12-to-b185 catch-up: replay, orthogonalize,
signal_combiner and cross_section_ic. New test files add their actual compile work.

Root approved using pool5 for warm alpha-only qualification after the final D0
freeze/review, retaining group order and old PCH layout. Data and impl qualify in
their owning trees. This is a graph estimate, not a measured build speedup. No
configure, dependency regeneration or compiler was launched for this audit.

## Fix-only review and qualification candidate

Reviewed the frozen production followups and postimplementation tests through
`d40441ad352aab84ead6faecb129da72a56c0496`. **Ready for compilation qualification;
no remaining source/test blocker. This is not a runtime approval.**

- `c19e8a0a9182e8b450fa29480e3c93fd4dccd485` closes context forwarding: explicit
  rule/basis are stored, transferred by both move operations, and passed into
  lazy augmented lowering. Raw lowering with no ADV windows is unchanged.
- `a90b428a7c45f8823b8dcf4d24be76f88e982f95` closes the pre-read geometry issue:
  checked dates*instruments, matching name/data cardinality, all column lengths
  and any nonempty mask are validated before spans or element reads. Valid V1
  arithmetic is unchanged.
- Separately attributed `95828c6e5da1d249b46a2032a25ca805a71462d5` closes the
  adjacent capacity identity hole: both persisted JSON and fingerprint bind
  min_price, min_adv_usd and adv_window under the identical active branch.
- `6103ab692d9bc4bd5d63f2cef9c45dc2b6c94b1f` preserves empty valid V1 field order
  by distinguishing an available zero-length legacy VWAP column from no column.
- Tests at d40441ad cover stale VWAP replacement; finite-positive raw cells and
  invalid/masked cells; strict unknown-basis rejection and explicit alternatives;
  raw-price raggedness, cardinality, mask and product overflow; instrument-specific
  future factor perturbation and rank invariance; context moves before caching;
  versioned field basis, persisted recipes and active/off capacity identity.
  Synthetic raw declarations were inspected against their unadjusted generators.
- The new N128 fixture oracle independently copies the b185d056 RNG, base field
  generation and legacy derivation. Its oldest-to-newest ADV sum, NaN creation,
  field order and universe masks match the frozen old source. It does not call
  current datafields on the expected side. It compares every f64 bit, field name,
  mask cell and digest at 2520 x 128. Actual equivalence remains a runtime gate.

The exact six-commit import sequence for pool5 is deb939d2, c19e8a0a, a90b428a,
95828c6e, 6103ab69, d40441ad. It contains no CMake, preset or PCH ownership changes,
so the existing alpha;eval;combine group order and warm original PCH remain usable.
Data/impl suites qualify in their owning trees; pool5 owns alpha qualification.
The owner supplied one PCH-off real_panel.cpp compile log at
pool4/build-equity-hygiene/w0-vwap-real-panel-hygiene.log, whose recorded Ninja
command uses -j 1 and compiles that one object. This reviewer has not executed a
compiler or claimed broader hygiene/suite evidence at this source-review stage.
