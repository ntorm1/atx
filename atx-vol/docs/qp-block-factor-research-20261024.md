# Sequential double block-factor experiment: complete-fit evidence

Research source: `020e5be8c3508aa3726edfb924233dc1d7f3519e`, isolated branch
`research/vol-r7-qp-block-factor-20261024`. Fresh independent source review approves
the experiment, not production promotion. Local main remains `96cd56db`.
All relative evidence paths below use
`C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/`.

**The prototype passes every SPY correctness gate but has no qualified speedup
or subsecond result.** Both candidate timing sets fail the host-activity gate.
The full original control set passes that gate. Every run remains recorded.

## Why change the factorization

The rejected FP32 experiment needed 191 correction solves and 26 double fallback
directions. It did not justify promotion. The next experiment changes the double
factor algorithm, keeping the same matrix, objective, constraints, iteration
budget, stopping conditions and final certificates.

The exact 12,866-variable SPY Newton capture contains 35 factors and 70 RHS solves,
56,010 lower-triangle matrix entries, and 240,475 strict-L entries after Eigen AMD.
Only 1,502 columns belong to fundamental supernodes of width at least eight, but
they account for 83.4118% of the rank-one update-pair work estimate. This motivates
blocked dense arithmetic within the sparse factor; it is not itself a speedup.

The standalone double replay used faer 0.24.4 with no default features or Rayon,
explicit `Par::Seq`, and zero dynamic regularization. Its complete own-AMD solver
times were 248.4772/226.7069/264.2582 ms against original Eigen
339.7586/354.6424/374.6198 ms. One candidate run was contended. Same-order block
replay excluded about 9 ms of external Eigen ordering and lacked sibling readings
on all three runs. These measurements justified a full-QP experiment, not a
qualified component claim. All factors/directions and missing readings remain
in `qp-double-block-pinned-v1/`.

## Ownership, arithmetic and fallback

An explicit research-only CMake archive path enables the private C bridge.
Ordinary builds have no Rust dependency. The static library was built with
Rust 1.94.0 / LLVM 21.1.8, release ThinLTO, one codegen unit and
`RUSTFLAGS=-C target-cpu=x86-64-v3`. Cargo.lock pins the dependency closure.
This Windows/AVX2 experiment is not a portable production backend.

The Rust owner copies and checks the CSC indices, computes its own AMD/symbolic
factor, and owns factor values, answer and scratch. All setup is inside the cold
constructor clock. Factorization uses binary64 throughout and requires finite
positive pivots. Starting any factor attempt invalidates its predecessor. Solves
publish output only after checking every answer entry is finite. Borrowed C arrays
have explicit length, alignment, lifetime and nonaliasing contracts; C++ owns the
opaque handle through a custom `unique_ptr` deleter. Rust panics cannot cross
the ABI. Allocation exhaustion can abort the research process.

A refused block factor or solve invokes original Eigen on the same current
double matrix. Its symbolic analysis is lazy and its cost remains charged.
Solve-triggered fallback is in `linear_solve_ns`, not `factor_ns`; total work is
inclusive. Profile fields separately count block creation, factors, directions
and refusals. Padded block storage is not reported as strict-L nonzeros.
Original-direction capture refuses an enabled alternative backend so a changed
answer cannot silently become an original-control fixture.

Production ISA admission/build packaging, dependency/license review and complete
integrated consumer gates remain required before promotion. Successful-path
evidence does not replace those requirements.

Follow-up: call-local C++ injected-refusal controls pass after a runtime RED,
covering creation/factor/predictor/corrector failures, repeated matrices,
one-iteration rollback and null profiling. The combined run passes 36 with one
optional external raw-box skip; full normalized SPY is supplied and preserves
the prototype fingerprint. Owning PCH-off checks pass.

The frozen follow-up source is `b5f7bd6bff2b083edf1ed5d775f1c9fc6b8ec16d`,
freshly approved by the calendar reviewer. A newly archived development binary
`628fe1e344872a6c67e0d2cb3782f1a484f8bc8892ba92d3be3e9ec130cbae37` passes
all 13 independent actual-API gates and preserves prediction SHA256
`560da64113e2727373aaa5a08e9a095a10634216d149b569789a4b8d1cba39f0`.
Verification: `qp-faer-refusal-audit-v1/verification-final-v1.json`, SHA256
`0b6d09fe2e7a4c0bc3b9429b20229b088d38e199ccce210358d47d957924e75a`.
The native receipt remains in its `verification-v1.json/` subdirectory because
the run command treated that output argument as a directory. This closes the
fallback-control gap only; no replacement timing or deployment claim follows.

## Complete numerical evidence

Six Rust controls pass: non-diagonal solutions, non-self-inverse permutation,
malformed indices, owned input lifetime, failed/nonpositive/nonfinite factor
invalidation and untouched output on refusal. The C++ owning projection/publication
run passes 35 tests and skips one external raw-box fixture. Its normalized full-SPY
test is supplied and passes. PCH-off production/tool/test checks pass, including
the explicitly enabled bridge include path. The first test launch accidentally
used the fitting executable and ran zero tests; that log remains `v1` and is not
counted as validation. The correct quote-slice run is `qp-faer-full-projection-v2`.

Full SPY projection: one block symbolic analysis, 35 block factors, 70 block
directions, zero refusals/fallbacks, 36 projection sweeps. Its original feasibility
and final certificates pass. Reordering changes final double values, so bit
identity with the original solver is not asserted.

Both development and ThinLTO executables pass all 13 independent actual-API gates:

- 33/33 positive-time expiries and 12,866/12,866 eligible rows served.
- 12,851 inside original raw quotes: 99.88341365%; the same 15 proven floor conflicts.
- 12,850 identified IVs and 16 typed PriceOnly results, with unchanged row statuses,
  reasons, IV intervals and conventions.
- Maximum scalar cold Reference residual $0.0005012966093573823/share, 50.12966%
  of its original-spread budget; zero failures and zero roundoff-only passes.
- Actual queried shape, parity, calendar, floor semantics, prediction values and
  metadata all pass, including the explicit reviewed-floor identification contract.

Against the original finalizer-cache predictions, 12,580 price strings and 12,568
IV strings differ. Maximum absolute differences are respectively
`2.6934980027620847e-10` USD and `1.2415661831965963e-9` IV. Every status and
classification field compared is unchanged. The independent economic checks,
not these small differences alone, admit the result.

| Artifact | SHA256 |
|---|---|
| Rust archive | `a288aa8a4de2f931a93971c0c90029f3f84ac62d07334399f1a54c470cb1e82c` |
| Development executable | `b07089de6cd87701a556030b64b8e2c1fad6af0b1749c60edb522e56b56c7c17` |
| ThinLTO executable | `60b876279eac97d393cab46e856d5c7d31dd8da767c6f1d267adeb6e6d81a73a` |
| Both prediction files | `560da64113e2727373aaa5a08e9a095a10634216d149b569789a4b8d1cba39f0` |
| ThinLTO native audit | `c90e8edcb713c98a7b5af20337b1033928ed247670ff65894202ea2300e05e89` |

Sources/archive are in `qp-faer-bridge-source-v1/`. API receipts are in
`qp-faer-full-audit-v1/` and `qp-faer-lto-audit-v1/`. Binary copies use their full
hash under `binaries/`. The audit explicitly reports the experimental backend.

## Complete cold clocks, including failed host gates

All agent builds/native work were paused. Every trial is a fresh process, one
worker, P-core 6, fast-math off. The sibling threshold remains 5%.

| Set | Cold-fit seconds in order | Median | Sibling busy percentages | Host gate |
|---|---|---:|---|---|
| Candidate v1 | 1.1301671 / 1.3739241 / 1.1918033 | 1.1918033 | 0.7519 / 22.4719 / 3.7975 | Fail |
| Original control | 1.3226988 / 1.4265373 / 1.4471122 | 1.4265373 | 0 / 4.3103 / 0 | Pass |
| Candidate repeat v2 | 1.6014335 / 1.2003743 / 1.1195960 | 1.2003743 | 44.6602 / 8 / 6.1728 | Fail |

The original control uses archived binary `6afb2de0` and its clean source
`0662116e`; candidate runs use clean `020e5be8`. All prediction files are
identical within their own algorithm. No failed trial is omitted and no
cross-set speedup is claimed. Repeating was prompted by the first host failure;
the repeat also fails and is not replaced by selected individual runs.

Manifests and hashes:

- `qp-faer-lto-clock-v1/manifest.json`:
  `a63501781946a0504ccfe32c610857ddb2e7655f9ccd72c715f05fb6d3c1e7e0`.
- `qp-faer-original-control-v1/manifest.json`:
  `b518828a9dd50e545824846723d1ab666b2c690ae33b4692508592c1884cea86`.
- `qp-faer-lto-clock-v2/manifest.json`:
  `47eebc72439a43cd343a0272c2245c0b6cb30c8d23743e8dafa339b472f510b6`.

The joined `qp-faer-lto-audit-v1/qualification-clock-v1.json` passes all correctness
checks and fails the three timing conditions: 100 fresh processes, idle sibling
throughout, and every cold fit below one second. The goal remains open.

## Next measured work

The first candidate trial attributes 367 ms to calendar work, 257 ms to family
construction and 438 ms to final-price certification. Within the last stage,
496 cold seed proofs cost 87 ms, 700 Screen/Fast candidates cost 130 ms, and
12,105 economic family attempts cost 104 ms. These nested scopes must not be
added to their parent again. They are attribution for this run, not latency claims.

Next, replay exact cold seed requests to assess safe batching of Reference prices,
including packing, scalar patches and full unchanged monetary/floor proofs.
Retain the block experiment separately until its wrapper failure path, deployment
requirements and reproducible full-fit performance earn integration. A subsequent
quiet timing attempt should investigate host placement/activity explicitly; it
must not silently weaken the gate or discard difficult trials.

Primary motivation and API contracts:
[faer sparse LDLT](https://docs.rs/faer/0.24.4/faer/sparse/linalg/cholesky/index.html),
[Karsavuran et al. on serial sparse Cholesky](https://arxiv.org/abs/2409.13090), and
[Eigen sparse interfaces](https://eigen.tuxfamily.org/dox/group__CholmodSupport__Module.html).
The replay source/README lives on the isolated research branch named above.
No published paper establishes this library's measured performance.
