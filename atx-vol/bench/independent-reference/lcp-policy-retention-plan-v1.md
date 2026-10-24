# Prospective admissible-row policy retention experiment

This is an isolated offline experiment following the frozen four-stage outcome
`95b37a90` and saved-row diagnosis `dd4aad57`. No new solve has run under this
rule. The parent approved the exact prospective selection rule before this
plan, conditioned on unchanged residual admission and full coupled rechecking.
Fresh exact-source review is required before a candidate replay of the four
saved systems. No PDE or full144 experiment belongs to this component.

## Observed mechanism and primary context

The fixed observation used the original binary64 one-correction map. Its seven
final failing rows were locally admissible at state127: positive obstacle gaps
of4–223 ulps were smaller than tiny positive continuation residuals, all inside
the original1e-10 row test. The strict minimum selector chose exercise, clamping
those rows at state128 and making continuation fail. The local price changes
and coupled neighbors matter; a row cannot be removed from the system merely
because it was admissible at the preceding state. Complete policy/state vectors
never repeated in129 observations, so a global-cycle claim remains unsupported.

Huang, Forsyth and Labahn, *Inexact Arithmetic Considerations for Direct Control
and Penalty Methods: American Options under Jump Diffusion*, Sections5–6,
describe finite-precision oscillations despite exact-arithmetic convergence.
Their analysis motivates investigating policy selection; it does not prove this
retention rule. We do not adopt an iterate-difference stopping criterion or
change the original complementarity tolerance.
Primary URL: <https://cs.uwaterloo.ca/~glabahn/Papers/inexact.pdf>.

Reisinger and Witte's M-matrix policy-iteration analysis and feasibility/
complementarity checks supply the original method context. The altered
finite-precision selection has no claimed finite-convergence theorem.
Primary pinned preprint: <https://arxiv.org/pdf/1012.4976v4>.

## Exact rule

Let c=A*x-rhs and g=x-payoff, using the original binary64 multiply/subtraction
order. Let p be the Boolean policy actually used for the just-completed update,
where true selects the obstacle equation. Let tau be the original caller's
positive finite tolerance, fixed to1e-10 in the saved-stage candidate.

1. The initial selection is unchanged: p_new=(g<c).
2. After a completed update, compute the original row admission
   a_i = abs(min(c_i,g_i)) <= tau.
3. For a_i=true retain p_i. For a_i=false use the original comparison g_i<c_i.
   A changed neighbor can invalidate a retained row at the next inspect; it
   must then be reselected. No permanent active-row lock is introduced.
4. Solve the entire selected coupled system, perform exactly the existing one
   binary64 residual-correction solve, and inspect every original equation.
5. Only the unchanged global max(abs(min(c,g)))<=tau admits a result. Retaining
   a policy does not admit a price, skip a residual check, freeze a value or
   declare convergence. Nonfinite operands retain their original refusal.

Both the original n+2 algorithmic limit and the paired linear-call reservation
remain. The first candidate saved-stage experiment must stop at128 updates /
256 attempted linear calls per system, using the same original initial arrays.
An observation-cap refusal is inconclusive. Neither more iterations nor a
different tolerance is an available success mechanism.

## Isolated source and ownership

Add a new private `lcp_policy_retention.py` and focused test file. Preserve
`lcp_correction.py`, `pde_reference.py`, `lcp_observe.py` and all old archives
byte-for-byte. Load the pinned correction source into a private unregistered
module. Validate its canonical hash and require exactly one expected policy
assignment before replacing only that assignment with a call to a per-invocation
selector. Do not copy or reimplement the linear solves, residual arithmetic,
work reservation, update limit, admission or refusal snapshots.

The selector's preceding-policy buffer belongs to that invocation and is never
global or shared across threads. Its first call has no preceding policy. A
later call can occur only after the preceding update's original inspect.
Use owned Boolean copies and do not mutate input residual arrays.

The new control defaults to the unchanged reviewed one-correction map; only an
explicit experimental mode enables retention. Experimental results/refusals
carry a distinct mode, base-mode provenance and selector work counts. Default
results and arithmetic keep their old schema/bits. Unknown modes refuse before
numerical work. No production API dispatch is added.

Require at most16,383 rows and a caller-visible conservative selector-workspace
reservation of32*n bytes, plus bounded scalar metadata, before experimental
arithmetic. This covers the prior/selected/raw/admissible Boolean arrays and
binary64 minimum/absolute-value temporaries; it is not a Python process-memory
bound. The default unchanged path does not pay that reservation. Count initial
policy rows, admissible retained rows, retained overrides of the strict choice,
failing reselected rows and selection calls, including completed selection
before a later original numerical refusal.

## TDD and review gates

Use small independent synthetic systems and explicit selector controls first.
Runtime RED must demonstrate that the original strict selector undoes a row
whose gap and continuation already satisfy the original tolerance, using gaps
of the observed ulp scale. Include actual coupled small solves and independent
enumeration of their possible policy solutions; do not use a saved failed
16,383-row stage as a test fixture.

Required controls include: unchanged initial/default bits; retention at and
below the original threshold; mandatory reselection for negative continuation
or obstacle violations beyond it; neighbor-induced invalidation of a previously
admissible row; no acceptance from unchanged policy alone; all original global
inequalities rechecked; zero/partial pair budgets; n+2 and128/256 limits;
nonfinite and allocation/resource refusal; source-pin failure; per-call reset
and immutable inputs. A scripted inexact linear result may exercise a phase
control, but must be labeled and cannot substitute for actual coupled-solve
controls or evidence about the real four saved systems.

Freeze this primitive, focused source-bound receipts and exact SHA for peer
review. A later reviewed observer must distinguish actual retained selections
from raw minimum selections; the old observation reader cannot be silently
reused because it proves the old selector. Only after that source review may
the declared four-stage candidate run once under the fixed cap. Full PDE and
historical reference promotion remain separate gates.
