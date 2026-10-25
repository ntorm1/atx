# Bounded working-set projection component

This is the default-OFF implementation of the reviewed Stage B plan. It is an
experiment in the existing feasible-price projection, with every original free
variable, bound, row and final certificate retained. It is not a new financial
model or a least-squares optimality claim. The canonical SPY candidate remains
unexecuted until fresh exact-source review.

`ATX_VOL_JOINT_WORKING_SET_RESEARCH` changes the linked private projection's
Compiled constraint mode. Full mode explicitly bypasses the wrapper. Centrality
and external research factor backends are incompatible with the enabled option.
The Stage A census CLI refuses the working-set build because its original
single-system matrix-profile interpretation would be misleading.

The wrapper first scans the unchanged clamped target against all original rows.
Already-feasible data proceeds to the existing certificates without allocating
the optional working set. Otherwise it retains every bound plus every initially
violated row, in original order. Each reduced solve restarts at the original
target and uses the original infeasible-start solver and per-solve budget.
After a completed reduced solve, it scans every original row at both the raw
iterate and the actual publication clamp, adding every newly violated omitted
row. The published point must pass the full scan and the original slice,
non-dividend and all-pair calendar certificates before caller nodes change.

There are at most three reduced solves and one original full fallback, each
with the caller's unchanged budget. A reduced numerical failure, remaining
violation with no new row, exhausted round limit, full-scan nonfiniteness or
certificate refusal invokes that fallback. It reinitializes from the original
target. Optional allocation failure is contained; the original full fallback's
allocation and error behavior remains unchanged. All partial node arrays are
private until certification. The 14,000-variable and 60,000-row optional limits
do not restrict the original solver: larger inputs use it directly.

## Work ownership

Each projection has four fixed solve slots. Iteration, factor-call and direction
counts include every attempted reduced and full solve, including work before an
allocation exception. Factor counts mean actual factorize calls; a nonfinite
assembled Hessian refused before factorization is not counted as a factor call.
Per-slot graph storage is explicit; optional profile matrix/factor fields are
maxima across attempted systems. Original full row counts and arities are kept.
The total profile and existing fitting Calendar bucket include setup, row copies,
scans, all failed work, certificates and destruction. A null profile adds no
clock reads.

The no-allocation work aggregator runs once at the projection boundary. It uses
the caller's existing ChainScope: the global raw solve belongs to the first
chain, while other joint invocations belong to their actual scope. Every call
adds to aggregate totals. Four-slot detail is retained only for the first call;
`slot_detail_invocations_omitted` explicitly counts later omissions. Synthetic
test invocations and refusals are also summed, so a later injected call cannot
hide behind an ordinary first call. No total is copied into every expiry.

`workspace_bytes` is requested optional wrapper storage, including its fixed
objects, selected mask, reduced rows and three copied variable vectors. It is
not peak process memory: it excludes allocator metadata and the original
solver's vectors, sparse factors and certificate allocations. Existing stage
clocks include all those allocations and their destruction.

## Controls and evidence limits

The Full-dispatch stub produced four meaningful runtime failures out of six
controls. After implementation those initial six controls passed. A bounded
tiny-fixture authoring search found a natural omitted-row case immediately:
26 selected rows grew to35 of38 after nine omitted violations; two reduced
solves passed independent dense-KKT feasibility and explicit full inequalities
without a full fallback. That fixed input is now a regression, rather than a
repeated search or a SPY-derived threshold.

The private default-null test controls only force conservative refusal or lower
resource/round caps. They cannot supply prices, modify constraints, certify
success, skip a full scan or remove incurred work. The lowered one-round case
uses the natural omitted-row violation. Other injected scan/no-progress/
certificate cases are explicitly synthetic. Optional allocation coverage arms
the existing fail-once allocator immediately before a real container allocation;
it is distinct from synthetic numerical refusal. No seam is exposed by the
public API or normal benchmark CLI.

`working_set_audit.py` independently checks work caps, paired full-scan
denominators, monotonic row additions, fallback placement, first-call/aggregate
joins and explicit omitted detail. It never solves a system and is not a price
acceptance oracle. The separate actual-API economic gates remain mandatory.
Changed valid fitted prices/IVs are permitted; old solution bits and historical
floor-target pins are not promotion gates. No timing or SPY speed claim is made
by the tiny component controls.

The final component gates pass:16 new native controls,15 existing projection
controls with the experiment ON and the same15 with it OFF, two enabled CLI
checks plus one generated-input OFF provenance/work check, seven work-reader
controls and the same seven under Python optimization. Two actual native work
records (budget1 failure/fallback and budget100 natural row generation) also
pass the independent reader. Five owning translation units compile with PCH
disabled. The external raw-SPY fixture test was deliberately excluded; it is
the next reviewed experiment, not a skipped result counted as a pass.

The enabled executable, focused tests, owning object and cache are archived
before switching the warm build back to OFF. Component evidence is retained in
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/acceptance/joint-working-set-v1`.
Integration onto the current main's retained post-fit audit producer will be
reviewed separately, then the single canonical SPY candidate can supply its own
post-fit13 financial checks and eight same-instance marker joins. No old CSV
identity is substituted for that evidence.
