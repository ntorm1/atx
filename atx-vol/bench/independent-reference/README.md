# Independent American-price reference qualification

This offline NumPy/SciPy tool solves the continuous-carry Black-Scholes obstacle
problem with finite differences and policy iteration. It has no ATX or QuantLib
pricing dependency and changes no shipping API or existing golden prices.
It is a qualification experiment, not a new fast pricing route.

The legacy test oracle solves a Crank-Nicolson linear system and then clamps the
result to payoff. That splitting is an approximation: even a two-row example
can violate the continuation residual after clipping. Its header's generic
second-order / six-to-seven-digit assertion is not a contract-specific convergence
receipt. Do not use that assertion to resolve the public comparators' tighter
price differences. Existing historical regressions retain their original meaning.

The new discrete solver checks `min(Mv-b,v-payoff)=0` after joint policy iteration,
requires a strictly dominant M-matrix, and refuses nonconvergence. This follows
the independently described [Reisinger-Witte policy iteration method](https://arxiv.org/abs/1012.4976).
Central log-price differences use a graded time mesh, with two initial implicit
steps followed by Crank-Nicolson. Far boundaries use deterministic exercise
envelopes. Neither the finite domain nor the mesh is assumed exact.

Each selected model gets six separate runs: base, time refinement, space
refinement, further time refinement, further space refinement, and a wider
domain at unchanged nominal spatial resolution. All prices, refusals, discrete
residuals, work counts, parameters, input/source hashes and dependency versions
are retained. The four-times-last-difference indicator is **empirical**, not an
interval bound. Agreement or a small indicator alone cannot prove accuracy.
Independent European closed forms, exercise bounds and comparison to both ALO
implementations are additional checks. The tool does not turn missing references
or unsupported models into passing cases.

No Greek accuracy follows from a price check. American gamma can oscillate under
Crank-Nicolson even with initial damping; a later Greek reference should compare
strongly damped time methods and independently refine space/time near exercise.
[In 't Hout's study](https://arxiv.org/html/2401.13361v1) motivates that separate
work. Discrete cash jumps likewise require a separate model and event treatment.

Run from this directory, using the already installed NumPy/SciPy environment:

```powershell
python -m unittest test_pde_reference
python run_pde_reference.py --corpus ../public-american/corpus-v1.json --output C:/atx-cache/pde-reference-new
```

An optional `--groups ID1 ID2` selection is recorded explicitly alongside both
the corpus and selected denominators. Do not present a selected run as full
coverage. These clocks measure reference-generation cost only, and must not run
during coordinated quiet performance windows.

## Initial five-model pilot (development evidence)

Five prerequisite tests pass after an initial failing implementation: the exact
obstacle counterexample, matrix/input refusals, European convergence, and an
American exercise/premium check. The first pilot includes 35 puts in groups
`g044`, `g047`, `zero_rate_closed_form`, `double_boundary_diagnostic` and
`negative_rate_european` from the frozen diagnostic corpus. All six refinement
runs per group complete. The finest ordinary grid is 8,193 spatial nodes and
2,048 time steps; the widened-domain grid has 12,289 nodes.

The two European groups have maximum actual closed-form errors of $0.00000382
and $0.00000419. The difficult three-year, 80%-volatility groups still have
empirical indicators up to $0.000365 and $0.000368: these runs **do not resolve**
the $0.0001 comparison's reference disagreement. All seven double-boundary puts
return finite PDE prices, but that is exploratory reference coverage, not a new
shipping capability or an accuracy guarantee. These results justify further
independent refinement; they do not justify relabeling unresolved cases.

Receipts: `C:/atx-cache/quote-benchmark/spy-qualified-r7-20261023/independent-pde-pilot-v1/`
contains `started.json`, `receipt.json` and all five group reports. Each report
retains every mesh, residual and refusal. The source was uncommitted during this
pilot and is pinned by file hashes in the receipt. Test logs are
`independent-pde-red.log` and `independent-pde-green-v3.log` under the same r7
root. This pilot is not a public pricing-speed benchmark.
