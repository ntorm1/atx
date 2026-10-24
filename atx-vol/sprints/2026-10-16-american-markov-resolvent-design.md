# Positive variance-resolvent transition proposal

Design submitted for parent and independent review before implementation. The
parent and independent mathematical reviewer approved the design before code.
Implementation and validation receipts are in the companion resolvent sprint
note. Frozen base
`5564fc51d6b27255979b1a79fe30e4252dbf5294`; lane
`feat/vol-markov-resolvent-20261016`, leased pool-34. Existing explicit diffusion
remains the default. No vendor labels are inputs to this work.

## Mathematical model

On the existing fixed interval grid X, retain the zero-drift birth-death
generator Q. Interior jump rates are

```
exit = sigma(X)^2 * X^2 / (lower_gap * upper_gap)
q_down = exit * upper_gap / (lower_gap + upper_gap)
q_up   = exit * lower_gap / (lower_gap + upper_gap)
Q_ii   = -(q_down + q_up)
```

Both boundaries are absorbing. In exact arithmetic Q1=0 and QX=0. For a
represented nonnegative variance-clock step h, choose

```
R_h = (I - h Q)^(-1)
    = integral_0^infinity exp(-s/h) exp(s Q) ds/h, when h > 0,
R_0 = I.
```

The integral establishes positivity, unit row mass, and preservation of X.
Every strike shares that same transition. Calendar endpoints, deterministic
growth/discount, cash shifts, and the endpoint Snell maximum remain unchanged.
Only the transition kernel is different. Calendar and variance maximum steps
still bind; explicit CFL subdivision does not apply to this opt-in method.

The probabilistic interpretation is exponential *latent variance duration* with
mean h during each deterministic calendar step. Exercise remains restricted to
the compiled deterministic layers. Calendar time, carry, discount, and dividend
dates are not randomized. This is a finite Bermudan Markov approximation, not a
direct implementation of Carr's continuously exercisable random-maturity
contract. Carr randomizes the contract's maturity and allows exercise before
that random maturity; the distinction matters for discount and exercise.
[Carr, 1998](https://engineering.nyu.edu/sites/default/files/2019-03/Carr-randomization-and-the-american-put.pdf)

At a fixed finite generator, R_h=I+hQ+h^2 Q^2+O(h^3), whereas the exact transition
exp(hQ) has second-order term h^2 Q^2/2. Repeated resolvents are a first-order
time approximation to that generator's semigroup. This does not certify an
American price convergence rate under grid, payoff, cash, and exercise errors.
No extrapolation or signed transition mixture is proposed.

## Cancellation-resistant positive factor action

Use scaled nonnegative rates a_i=h*q_down_i and c_i=h*q_up_i. Ordinary positive
Thomas elimination has

```
d_i = 1 + c_i + a_i * (1 - u_(i-1))
u_i = c_i / d_i
y_i = rhs_i / d_i + (a_i / d_i) * y_(i-1)
v_i = y_i + u_i * v_(i+1).
```

Computing 1-u by subtraction loses information when u rounds near one. Further
normalization also avoids large intermediate y products on extreme stock
scales. Store two weights instead:

```
t_i     = a_i * beta_(i-1)
s_i     = 1 + t_i
d_i     = s_i + c_i
alpha_i = 1 / s_i
beta_i  = s_i / d_i
z_i     = lerp(z_(i-1), rhs_i, alpha_i)  // left-to-right
v_i     = lerp(v_(i+1), z_i, beta_i)    // right-to-left
```

This is the same real-arithmetic solve after defining z_i=y_i/(1-u_i).
Absorbing boundaries have alpha=beta=1. Weights and intermediate factors must
be finite, with alpha,beta in (0,1]. There is no clipping or posthoc probability
renormalization. Numerical range failure or excess action residual refuses the
model. A fixed dimensionless 1e-12 action-on-1/X residual ceiling is enforced;
it is not caller-relaxable. This is an observed floating-point action residual,
not a rigorous error bound.

The stock probe uses the original finite nonnegative state values directly and
normalizes each residual by max(X_i, spot). Bounded convex lerps avoid overflow
for these inputs. Independent code review found that global scaling can hide
material drift when small states become positive subnormals: for grid
{0, 1e-100, 1e223}, sigma=1e7, h=1e308, the upward rate underflows and the
represented stock action loses about 8.99% at spot. A scaled probe had rounded
that loss away. The direct probe refuses this case and permits an extreme
zero-variance identity that preserves its original states exactly.

The transpose applies these same factors in reverse composition. First traverse
left-to-right, distributing accumulated mass via beta to the current state and
1-beta to the next state. Then traverse right-to-left, distributing via alpha
to the current state and 1-alpha to the previous state. Both sweeps use positive
operations and caller-owned buffers. This is an O(N) action and transpose,
without materializing N squared transition coefficients. Independent small-grid
dense solves and transpose duality checks will detect orientation mistakes.

QuantLib's own implicit Euler implementation solves a linear system, and its
tridiagonal operator uses a Thomas solve. Those are implementation references;
the normalized stochastic factorization above is derived for this library's
zero-drift generator and deterministic dual-clock semantics.
[QuantLib implicit Euler](https://github.com/lballabio/QuantLib/blob/master/ql/methods/finitedifferences/schemes/impliciteulerscheme.cpp),
[QuantLib tridiagonal solve](https://github.com/lballabio/QuantLib/blob/master/ql/methods/finitedifferences/operators/triplebandlinearop.cpp)

## Proposed API and budgets

- `AmericanMarkovDiffusionMethod::{ExplicitEuler, VarianceResolvent}` in model
  config, defaulting to ExplicitEuler, with resolved model provenance.
- Immutable shared `AmericanMarkovResolventFactor` storage, containing the two
  weights. Repeated exact represented h values share factors within an interval
  only. Const factor spans retain the same copy/move ownership guarantees as the
  shared explicit rows.
- An explicit transition discriminator, plus `source_state_count()`,
  `target_state_count()`, and `rollback_work()`. Explicit-row access is empty for
  resolvent transitions; consumers must dispatch explicitly.
- A checked raw transition action and transpose for caller-prepared spans,
  excluding discount/exercise, with invalid dimensions and overlap refused.
  Scalar and batch pricing share internal strided factor kernels; no allocations
  occur in successful prepared calls.
- Preserve logical `stored_rows = sum source-state counts` and its current hard
  cap. Add `transition_work` and `max_transition_work`, counting N state visits
  for explicit/cash transitions and 2N for resolvent transitions. Proposed default
  8,000,000 and hard cap 16,000,000, separately from the existing row cap. The
  default permits every formerly valid explicit configuration, including callers
  who raised the old row budget to its hard cap. The existing default row cap of
  1,000,000 still limits work to at most 1,000,000 explicit or 2,000,000 resolvent
  visits unless the caller explicitly raises it.
- Fitting charges the weighted `rollback_work()` for every quote/expiry prefix,
  so a two-pass solve cannot appear as zero work because explicit rows are absent.
  Physical factor elements/blocks are separate diagnostics. Factor construction
  and fixed-size operator probes remain bounded by logical model state counts.
- PWL construction explicitly refuses a resolvent model before any rollback.
  No alternate process is substituted. No automatic method selection, policy
  default change, or CLI default change belongs to this experiment.

## Postimplementation validation plan

Implement first, then add meaningful checks. Independently reconstruct small
dense generator inverses; test nonnegative basis images, constants, means,
transpose duality, total forward mass, absorbing boundaries, local volatility,
paused variance, negative rates, cash-grid collapse, and copy/move lifetimes.
Check exact resource boundaries and caller buffer/overlap failures. Preserve
explicit-mode operator and scalar-price fingerprints from the storage receipts.

Use synthetic time/grid refinements against independent Black-Scholes European,
deterministic stopping, and continuous American FD references. Audit coarse-step
bias explicitly; mathematical positivity alone does not imply adequate pricing
accuracy. Record scalar/batch agreement and allocation counts. Timing samples
are diagnostics until run on a quiet pinned host. No label-calibration or
world-fastest claim follows from this implementation.
