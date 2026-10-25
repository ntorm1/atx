# Prospective one-step reuse of a joint projection factor

This is a separate experiment after the negative centrality result at `9965771e`.
No quasi-Newton implementation or SPY result exists at this plan's freeze.
Ordinary defaults, all economic contracts and every final certificate remain
unchanged. The centrality option stays OFF for this experiment.

## Why change the method

Centrality saved six of 35 factorizations but increased solves from 70 to 124.
Adding more direction solves is therefore unsupported by the measured workload.
[Gondzio and Sobral, equations 11-12 and Algorithm 1](https://optimization-online.org/wp-content/uploads/2018/06/6677.pdf)
describe applying a Broyden inverse update with one existing-factor solve and
vector operations, without a separate solve to construct the update. Their
restart discussion motivates limiting reuse. This plan adapts the idea to
ATX's inequality equations; it does not inherit a convergence theorem.

The primary paper is archived at `quasi-newton-research-v1/gondzio-sobral-qnipm.pdf`,
SHA256 `98b60e9a86b922fc394fa2b37cb0a0dd22cfa4b5a411a01d8c4ba00615aa5498`.
Pages 5-7 and 10 were rendered and inspected. The adaptation below is explicitly
bounded to one stored secant and one skipped factorization at a time.

## ATX equations and the isolated primitive

In variable order `w=(x,s,z)`, use the unperturbed residual

```
F(w) = [ x-target-A^T z ; A x-b-s ; s*z ]
J(w) = [ I,0,-A^T ; A,-I,0 ; 0,Z,S ].
```

ATX's existing direction arguments correspond to RHS `v=(-rd,-rp,-rc)`.
The last genuine Newton factor defines `H0=J(w0)^-1`. After its accepted update,
store the actual representable step `d=w1-w0` and residual change `y=F(w1)-F(w0)`.
Use the algebraically equivalent linear forms for the first two blocks:
`y_dual=dx-A^T dz`, `y_primal=A dx-ds`; the complementarity block is
`z0*ds+s0*dz+ds*dz`. All vector dimensions and finite intermediates are checked.

For each RHS of the following iteration, the proposed inverse action is

```
rho   = dot(y,y)
alpha = dot(y,v)/rho
q     = v-alpha*y
r     = H0*q + alpha*d.
```

There is exactly one call to the existing-factor solve. No dense inverse,
new factor, or additional `H0*y` solve is constructed. A private standalone
primitive accepts disjoint borrowed input/output/scratch spans and a synchronous
base-solve callback. It allocates nothing and publishes the complete output only
after every intermediate and output is finite. Shape errors, zero/invalid secant,
nonpositive/nonfinite `rho`, overflow, or callback refusal leave output unchanged.
Work resets per call and counts every actual base-solve attempt. Callback
exceptions propagate from this primitive; the optional solver integration must
contain allocation failure explicitly.

The primitive tests compare against an independently assembled dense inverse
rank-one update, including a coupled non-unit full KKT matrix. Tests must check
the inverse secant equation, general RHS/linearity, preservation of the two
unchanged linear equation blocks, signs, unequal dimensions, zero/underflow/
overflow, callback refusal and whole-output rollback. Runtime RED precedes
implementation. A successful primitive is not a successful QP experiment.

## Bounded whole-QP adaptation

Iteration zero uses the original fresh factor and predictor/corrector. Only a
successful fresh-Newton update may create one secant. The next iteration may
reuse that factor for its original affine and corrected RHS via the inverse
action above. After that one reused iteration, discard the secant and require
a fresh factor. No chain of accumulated updates, tunable history, extra centrality
directions, warm state across fits, or data-dependent wall-clock budget is added.

The baseline slack and dual vectors must remain owned and immutable while their
factor is reused; using current vectors in that inverse would be incorrect.
Reserve all state, secant, modified-RHS and candidate storage once before the
iteration loop, with checked dimensions/byte arithmetic and a declared payload
ceiling before implementation. Count reservations and every copy/vector pass.
Allocation failure disables optional reuse and preserves the original algorithm.

Before accepting a reused direction, independently check both unchanged linear
blocks against its requested RHS. The exact numerical admission/error rule and
storage layout must be frozen in a wiring supplement before whole-QP code; this
first plan authorizes only the isolated inverse-action primitive. A nonfinite or
refused reused affine/corrector direction must refactor at the same iterate and
recompute the entire original predictor/corrector pair, at most once. Charge the
discarded work. The 100-iteration bound, original positivity update and `1e-10`
primal stopping rule remain unchanged. Final publication still requires all
original slice/box/calendar checks. No optimality claim is introduced.

## Execution gates

Fresh exact-source review precedes actual-factor controls and whole-SPY runs.
Freeze the wiring supplement and its runtime-failing controls first. Then retain
the complete original and reuse projection outcomes on the same 33-expiry,
12,866-row input, including every factor, solve, secant rejection, fallback,
constraint and certificate. All setup and destruction belong to inclusive work.

Any promising complete fit must pass all thirteen scalar actual-API gates under
the unchanged raw-spread price and identification contracts. Changed floor
targets need separate exact cold evidence and review; the current centrality
audit stays false and the existing fixture is not widened. Only a fully admitted
variant may enter a separately declared ThinLTO, one-worker quiet-host comparison.
The 100-process release gate and the complete subsecond objective remain open.
