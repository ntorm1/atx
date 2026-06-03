# atx-core `linalg` build-out — design

**Date:** 2026-06-02
**Module:** `atx::core::linalg` (L7)
**Status:** approved design, pre-implementation

## Goal

Turn the current thin `linalg` module (Eigen aliases + span bridges + OLS/ridge/WLS)
into a complete, correct, fast linear-algebra layer for the backtesting engine.
Linear algebra underpins risk models, factor decomposition, and solvers, so the
surface must be curated, `Result`-checked, and performant.

Eigen already provides raw decompositions and solvers. The value-add here is a
curated layer that:

- returns owned results through `Result<T>` with explicit failure paths (shape,
  singularity, non-positive-definiteness, non-convergence) — never exceptions
  except `std::bad_alloc`;
- selects the right Eigen algorithm per operation (no one-size-fits-all);
- keeps the canonical scalar `double` and canonical inputs `MatX`/`VecX`,
  consistent with `regression.hpp`; callers bridge external buffers with the
  existing `as_matrix` / `as_vector` span maps;
- avoids redundant copies in hot paths (`const&` in, NRVO/move out).

## Scope

In scope (chosen by the user):

1. **Decomposition + solve core** — `solve`, `inverse`, `pseudo_inverse`,
   `determinant`, `rank`, `condition_number`, plus owned-result decomposition
   wrappers (`cholesky`, `qr`, `svd`, `symmetric_eig`).
2. **PCA / factor decomposition** — principal components, explained variance and
   ratios, top-k selection, projection.
3. **PD-matrix hygiene** — `is_symmetric`, `is_positive_definite`, `nearest_pd`
   (Higham), `regularize` (diagonal jitter).

Explicitly out of scope (deferred; better placed in the stats layer):
covariance/correlation estimators, Ledoit–Wolf shrinkage, EWMA covariance.

Unchanged: `linalg.hpp` (typed bridge) and `regression.hpp` (already correct).

## Architecture

Split by concern — one job per header, each independently testable, each kept
within the agent profile's focused-file preference. Dependency order:

```
linalg.hpp        (existing: aliases Vec*/Mat*/VecX/MatX, span maps as_vector/as_matrix)
   |
   +-- decompose.hpp   (cholesky, qr, svd, symmetric_eig -> owned Result structs)
   |        |
   |        +-- spd.hpp   (is_symmetric, is_positive_definite, nearest_pd, regularize)
   |        +-- pca.hpp   (pca, transform, top_k)
   |
   +-- solve.hpp      (solve, inverse, pseudo_inverse, determinant, rank, condition_number)
   |
   +-- regression.hpp (existing: ols, ridge, wls)
```

`decompose.hpp` is the substrate; `spd.hpp` and `pca.hpp` build on its
`symmetric_eig`. `solve.hpp` depends only on `linalg.hpp` + Eigen directly.

All in namespace `atx::core::linalg`. New headers live under
`atx-core/include/atx/core/linalg/`.

## Module specifications

### `solve.hpp` — solvers and matrix queries

| Function | Signature | Algorithm / behavior |
|----------|-----------|----------------------|
| `solve` | `Result<VecX>(const MatX& A, const VecX& b)` | Square general system. `PartialPivLU`; reject non-square / size mismatch (`InvalidArgument`) and near-singular via reciprocal condition estimate (`Internal`). |
| `solve_spd` | `Result<VecX>(const MatX& A, const VecX& b)` | SPD fast path via `LLT`; `Internal` if factorization fails (not PD). |
| `inverse` | `Result<MatX>(const MatX& A)` | `PartialPivLU::inverse` guarded by invertibility check. |
| `pseudo_inverse` | `Result<MatX>(const MatX& A)` | Moore–Penrose via thin `BDCSVD`, tolerance `max(rows,cols)·eps·σ_max`; handles rectangular / rank-deficient. |
| `determinant` | `Result<f64>(const MatX& A)` | `PartialPivLU::determinant`; square-only. |
| `rank` | `Result<i64>(const MatX& A)` | `ColPivHouseholderQR::rank` with default threshold. |
| `condition_number` | `Result<f64>(const MatX& A)` | σ_max/σ_min from `BDCSVD`; a singular matrix yields `Ok(+inf)` (see failure policy). |

Failure policy: bad shape → `InvalidArgument`; numerically singular / non-PD →
`Internal` with a descriptive message. `condition_number` of a singular matrix
returns `Ok(+inf)` (a valid, informative answer), not an error.

### `decompose.hpp` — owned decomposition results

Result structs (owned matrices, `[[nodiscard]]`):

```cpp
struct CholeskyResult { MatX L; };                 // A = L Lᵀ, lower-triangular
struct QrResult       { MatX Q; MatX R; };         // A = Q R, thin
struct SvdResult      { MatX U; VecX singular; MatX V; }; // A = U Σ Vᵀ, thin, σ desc
struct EigResult      { VecX values; MatX vectors; };    // symmetric, values ASC
```

| Function | Signature | Algorithm |
|----------|-----------|-----------|
| `cholesky` | `Result<CholeskyResult>(const MatX& A)` | `LLT`; `Internal` if not SPD. Returns lower factor `L`. |
| `qr` | `Result<QrResult>(const MatX& A)` | `HouseholderQR`, thin Q (first `cols` columns) and square R. |
| `svd` | `Result<SvdResult>(const MatX& A)` | `BDCSVD` with `ComputeThinU \| ComputeThinV`. Singular values already descending. |
| `symmetric_eig` | `Result<EigResult>(const MatX& A)` | `SelfAdjointEigenSolver` (real spectrum, ascending). Requires symmetric input (checked, `InvalidArgument`). |

`symmetric_eig` is the shared substrate for `spd` and `pca`.

### `spd.hpp` — positive-definite hygiene

| Function | Signature | Behavior |
|----------|-----------|----------|
| `is_symmetric` | `bool(const MatX& A, f64 tol = default)` | Square + `‖A − Aᵀ‖∞ ≤ tol·‖A‖∞`. |
| `is_positive_definite` | `bool(const MatX& A)` | Symmetric and `LLT` succeeds (cheapest reliable test). |
| `nearest_pd` | `Result<MatX>(const MatX& A, f64 eps = default)` | Higham one-step: symmetrize, `symmetric_eig`, clamp eigenvalues to `≥ eps`, reassemble `V·diag(λ⁺)·Vᵀ`, re-symmetrize. `InvalidArgument` if non-square. |
| `regularize` | `Result<MatX>(const MatX& A, f64 jitter)` | `A + jitter·I`; `InvalidArgument` if non-square or `jitter < 0`. |

`nearest_pd` returns the closest PD matrix in Frobenius norm (eigenvalue-clamp
approximation, sufficient for de-noising estimated risk matrices).

### `pca.hpp` — principal component analysis

```cpp
struct PcaResult {
  VecX mean;                // per-feature column mean (length = n_features)
  MatX components;          // each column a unit eigenvector, variance DESC (n_features × k)
  VecX explained_variance;  // eigenvalues, DESC (length k)
  VecX explained_ratio;     // explained_variance / total, sums to ≤ 1
};
```

Input convention: `X` is `n_samples × n_features` (rows = observations), matching
the regression design-matrix convention.

| Function | Signature | Behavior |
|----------|-----------|----------|
| `pca` | `Result<PcaResult>(const MatX& X, i64 k = -1)` | Mean-center columns; covariance `C = Xcᵀ·Xc / (n−1)`; `symmetric_eig(C)`; reverse to descending; keep `k` components (`k ≤ 0` ⇒ all). `InvalidArgument` for `n_samples < 2` or `k > n_features`. |
| `transform` | `Result<MatX>(const PcaResult&, const MatX& X)` | Center by stored `mean`, project: `(X − mean)·components` → `n_samples × k`. Shape-checked. |

Numerics: covariance route uses `SelfAdjointEigenSolver` (fast when n_features is
modest, the common factor-model case). Eigen returns ascending eigenpairs; we
reverse columns/values once to present descending (largest variance first).

## Error handling

- Shape / precondition violations → `Err(InvalidArgument, "<fn>: <reason>")`.
- Numerical failure (non-PD factorization, non-convergence, singular solve) →
  `Err(Internal, "<fn>: <reason>")`.
- Only `std::bad_alloc` from Eigen propagates; all functions are `noexcept(false)`.
- Every fallible function is `[[nodiscard]]`.

## Performance

- Per-operation algorithm selection (table above): LU for general solve, LLT for
  SPD, BDCSVD for SVD/pseudo-inverse/condition, SelfAdjointEigenSolver for
  symmetric spectra.
- Inputs by `const&`; results moved out (NRVO). No materialized intermediates
  beyond what each Eigen decomposition requires.
- Thin SVD/QR (not full) to avoid computing unused basis columns.
- `nearest_pd` is one eigen-clamp pass, not an iterative Higham loop, unless a
  test demonstrates insufficiency.
- Hot single-system `solve` avoids forming an explicit inverse.

Baselines captured later from a Release build; the default preset is Debug, where
these numbers are not representative (documented in the README).

## Testing (TDD)

One test file per header, known-value anchors plus error paths. Anchors:

- **solve:** round-trip `A·solve(A,b) ≈ b`; `inverse` then `A·A⁻¹ ≈ I`;
  `determinant` of a known matrix; `condition_number` of a Hilbert matrix
  (large, known-ill-conditioned); `pseudo_inverse` of a rectangular matrix
  satisfies the Moore–Penrose identities; singular matrix → `Internal`.
- **decompose:** `cholesky` reassembles `L·Lᵀ ≈ A` on an SPD matrix, errors on
  indefinite; `qr` gives `Q·R ≈ A` and `Qᵀ·Q ≈ I`; `svd` reassembles
  `U·Σ·Vᵀ ≈ A` with descending σ; `symmetric_eig` reconstructs `A` and returns
  ascending eigenvalues; non-symmetric input errors.
- **spd:** `is_positive_definite` true/false on planted matrices; `nearest_pd`
  on a planted indefinite matrix yields a PD matrix close to the input and is a
  fixed point on an already-PD matrix; `regularize` shifts the spectrum by jitter.
- **pca:** planted 2-factor dataset recovers the known principal directions and
  variance ratios; `explained_ratio` sums to 1 with all components; `transform`
  round-trips a centered point; degenerate (`n_samples < 2`) errors.

Wire the four new test files into `atx-core/tests/CMakeLists.txt` (explicit list)
and add the headers to the umbrella `core.hpp` under the L7 group. Update the
README L7 table.

Build gate: `/W4 /permissive- /WX` clean; full suite green via the dev-shell
build incantation.

## Files

New:
- `atx-core/include/atx/core/linalg/solve.hpp`
- `atx-core/include/atx/core/linalg/decompose.hpp`
- `atx-core/include/atx/core/linalg/spd.hpp`
- `atx-core/include/atx/core/linalg/pca.hpp`
- `atx-core/tests/solve_test.cpp`
- `atx-core/tests/decompose_test.cpp`
- `atx-core/tests/spd_test.cpp`
- `atx-core/tests/pca_test.cpp`

Modified:
- `atx-core/include/atx/core/core.hpp` (L7 includes)
- `atx-core/tests/CMakeLists.txt` (4 test files)
- `atx-core/README.md` (L7 table)
