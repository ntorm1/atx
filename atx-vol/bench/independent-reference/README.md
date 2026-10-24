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

## Long-expiry refinement follow-up

The second frozen run refines the two difficult groups `g044` and `g047` (14 puts)
to 32,769 spatial nodes and 4,096 time steps, with 49,153 nodes in the wider-domain
control. All six grids per group complete. Maximum empirical indicators fall to
$0.0000251924 and $0.0000230051. They remain too large to establish the tighter
comparison's $0.000005 reference allowance for every contract.

The fine PDE values differ from QuantLib highPrecision by less than $0.000002
on these 14 puts, and from the refined ATX values by approximately $0.00000545
through $0.0000179. This independent-method agreement is useful evidence, but
does not establish a rigorous error interval or resolve the whole corpus. In
particular, `g044` at spot 70 has an empirical indicator of $0.000000209 while
its difference from QuantLib is $0.000000654. The indicator is not an upper bound
on inter-method disagreement. The earlier unresolved labels remain unchanged.

Receipts are in `independent-pde-long-vol-refine-v2/` under the same evidence
root, pinned to clean source `7f93b987535d5ad548e249b3149a9a35e0c3ef74` and the
recorded individual Python-file hashes. The source includes the strict boolean
and string input guards. Each group retains all prices, policy iterations,
complementarity residuals and time/space/domain refinements. Roughly 152 seconds
per group is offline reference-generation cost, not a production latency claim.
