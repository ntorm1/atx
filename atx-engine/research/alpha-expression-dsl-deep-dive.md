# Equity Quant Alpha-Expression DSLs and the Compiler/VM Architecture to Evaluate Them

**Purpose**: Ground the design of `atx-engine` — a deterministic, point-in-time-correct, high-performance vectorized backtesting engine evaluating alpha expressions over a panel of US equities (date × instrument), implemented in C++20.

**Legend for claim confidence**
- ✅ **VERIFIED** — confirmed against a primary or strong secondary source (URL inline).
- ⚠️ **UNCERTAIN / CONFLICTING** — partial evidence, secondary source, or inference not fully nailed down.
- 📘 **STANDARD DOMAIN KNOWLEDGE** — well-established compiler/quant engineering fact, not separately cited.

> Method note: the canonical `101 Formulaic Alphas` PDF (arXiv:1601.00991) would not decode cleanly through the fetch tool (binary PDF stream). Operator *semantics* below are therefore cross-checked against (a) the arXiv abstract page, (b) widely-used open-source reference implementations of the paper (notably `yli188/WorldQuant_alpha101_code`), and (c) secondary summaries. Where a definition comes from a reference implementation rather than the paper text itself, it is tagged accordingly. This matters because the reference implementations encode the *operational* semantics an engine must replicate.

---

## 1. WorldQuant Alpha-Expression Language (canonical reference)

### 1.1 The `101 Formulaic Alphas` paper

✅ `101 Formulaic Alphas` is by **Zura Kakushadze** (with Geoffrey Lauprete and Igor Tulchinsky of WorldQuant cited in the work), arXiv:1601.00991, published in *Wilmott Magazine* (2016, vol. 84, pp. 72–80). It presents **101 real-life quantitative trading alphas** with **explicit formulas given as computer-code-like pseudocode**. (https://arxiv.org/abs/1601.00991)

✅ Stated properties: average holding period **~0.6 to 6.4 days**; average pairwise correlation **~15.9%** ("low"); returns correlate strongly with volatility but have no significant dependence on turnover. (https://arxiv.org/abs/1601.00991)

📘 The 101 Alphas are the *de facto* public reference for the WorldQuant "Fast Expression" / Alpha Expression style: a terse functional language mixing **cross-sectional** operators (act across all instruments at one date) and **time-series** operators (act backward in time per instrument), plus ordinary arithmetic and conditionals.

### 1.2 Operator / function vocabulary (Alpha101)

The paper defines a notation section (Appendix A) and uses the following vocabulary. Definitions below are corroborated by the `yli188/WorldQuant_alpha101_code` reference implementation, which is the most-cited faithful port. (https://github.com/yli188/WorldQuant_alpha101_code/blob/master/101Alpha_code_1.py)

**Cross-sectional (operate across instruments at a fixed date):**
- ✅ `rank(x)` — cross-sectional rank, returned as a **percentile in [0,1]**. Reference impl: `df.rank(pct=True)`. (yli188 repo)
- ✅ `scale(x, a=1)` — rescale so that `sum(abs(x)) = a`. Reference impl: `df.mul(a).div(np.abs(df).sum())`. (yli188 repo; secondary: studylib summary)
- ⚠️ `indneutralize(x, g)` — cross-sectionally **demean `x` within each group `g`** (subindustry / industry / sector). The paper defines it as cross-sectional demeaning by group; the `yli188` reference impl leaves it as a placeholder (no industry data shipped), so the *exact* WorldQuant variant (pure demean vs. regression-residual) is not nailed from code. Both interpretations appear in practice. (Definition: arXiv abstract/summary; placeholder status: yli188 repo)

**Time-series (operate backward in time, per instrument), generically `ts_{O}(x, d)`:**
✅ The paper defines `ts_{O}(x, d)` = operator `O` applied over the trailing `d` days; non-integer `d` is floored. (secondary summary of Appendix A)
- ✅ `delay(x, d)` — value of `x` from `d` days ago. Impl: `df.shift(d)`.
- ✅ `delta(x, d)` — `x_today − x_{d days ago}`. Impl: `df.diff(d)`.
- ✅ `ts_sum(x, d)` / `sum(x, d)` — trailing sum. Impl: `df.rolling(d).sum()`.
- ✅ `ts_mean` / `sma(x, d)` — trailing mean. Impl: `df.rolling(d).mean()`.
- ✅ `stddev(x, d)` — trailing sample std. Impl: `df.rolling(d).std()`.
- ✅ `ts_min(x, d)` / `ts_max(x, d)` — trailing min/max. Impl: `df.rolling(d).min()/.max()`.
- ✅ `ts_argmin(x, d)` / `ts_argmax(x, d)` — **which day in the window** the min/max occurred, **+1** (1-based). Impl: `df.rolling(d).apply(np.argmin)+1` / `np.argmax`. (yli188 repo)
- ✅ `ts_rank(x, d)` — rank of today's value within the trailing-`d` window. Impl: `rolling(d).apply(lambda w: rankdata(w)[-1])`. (yli188 repo)
- ✅ `correlation(x, y, d)` — trailing Pearson correlation. Impl: `x.rolling(d).corr(y)`.
- ✅ `covariance(x, y, d)` — trailing covariance. Impl: `x.rolling(d).cov(y)`.
- ✅ `product(x, d)` — trailing product. Impl: `rolling(d).apply(np.prod)`.
- ✅ `decay_linear(x, d)` — weighted MA over trailing `d` days, weights `d, d−1, …, 1` rescaled to sum to 1. Impl: weights `(arange(d)+1)/ (d*(d+1)/2)` dotted over the window. (yli188 repo; secondary summary)

**Element-wise arithmetic / math:**
- ✅ `+ − * /`, `abs(x)`, `sign(x)`, `log(x)`, `signedpower(x, a)` = `sign(x)·|x|^a` (paper uses `signedpower`; some impls use plain `x^a`), `min(x,y)`, `max(x,y)`. (secondary summary; signedpower definition is the paper's, `x^a` appears in some ports as a simplification — ⚠️ minor discrepancy)

**Conditional / logical:**
- 📘 Ternary `condition ? a : b` and comparison/logical operators (`<`, `>`, `==`, `&&`, `||`) appear throughout the formulas; implemented via `np.where` in ports.

**Data fields used (raw inputs):**
- ✅ `open, high, low, close, volume, vwap, returns, cap` (market cap), `adv{d}` (average daily dollar volume over `d` days, e.g. `adv20`), and `IndClass.{sector, industry, subindustry}` classification fields for `indneutralize`. (paper notation section; corroborated by reference ports)

### 1.3 The type/shape model (broadcast semantics)

📘 The implicit shape system across Alpha101 / WorldQuant has three shapes:
- **Scalar** — a constant (e.g. the `d` in `ts_mean(x, d)`, or a literal `0.5`).
- **CrossSection vector** — one value per instrument at a single date (output of `rank`, `scale`, `indneutralize`).
- **Panel matrix** — a `date × instrument` 2-D block (raw fields; output of `delta`, `ts_*`, arithmetic).

📘 Broadcast rules (as implemented by the pandas DataFrame model these ports use): element-wise ops broadcast a scalar over a panel/vector; time-series ops consume a panel and return a panel (each column rolled independently); cross-sectional ops consume a panel and return a vector *per row* (each date ranked independently). This row-wise (cross-sectional) vs column-wise (time-series) duality is the central type-system fact for a VM design.

### 1.4 WorldQuant BRAIN / WebSim "Fast Expression" operator surface

⚠️ The live BRAIN platform's operator set is a **superset** of Alpha101 and is documented behind the platform (official docs are gated). Secondary/community documentation lists these categories and names (semantics paraphrased, treat as indicative not authoritative):
- **Cross-sectional:** `rank`, `zscore`, `scale`, `normalize`, `quantile`, `winsorize`, `reverse`.
- **Time-series (`ts_*`):** `ts_rank`, `ts_zscore`, `ts_mean`, `ts_std_dev`, `ts_delta`, `ts_arg_min`, `ts_arg_max`, `ts_corr`, `ts_regression`, `ts_decay_linear`, `ts_decay_exp_window`, `ts_entropy`, `ts_moment`, `ts_backfill`. Common lookback windows: 5, 22, 66, 120, 240 days.
- **Group operators:** `group_neutralize`, `group_rank`, `group_zscore` (operate within sector/industry/region buckets).
- **Vector/transform:** `vec_avg`, `vec_sum`, `winsorize`.
(https://deepwiki.com/xiegengcai/world-quant-brain/3.3-advanced-alpha-generation)

⚠️ `ts_mean`-style "returns `x − ts_mean(x, d)`" phrasings appear in some community notes; the canonical `ts_mean` is just the trailing mean. NaN handling: BRAIN's `ts_*` generally ignore NaNs in the window (community docs). Do not treat platform-specific NaN behavior as authoritative — design your own explicit `min_periods` policy.

---

## 2. Microsoft Qlib Expression Engine (open-source, readable — the best architectural template)

Qlib is the cleanest open-source analogue of what `atx-engine` needs: a string DSL over a `date × instrument` panel with rolling (lookahead-safe) operators and a caching layer.

### 2.1 The string DSL

✅ Features are strings like `"Ref($close,1)"`, `"Mean($close,5)"`, `"($high-$low)/$open"`, `"Corr($close,$volume,10)"`. `$field` denotes a raw column (`$close`, `$volume`); `$$field` (PFeature) denotes point-in-time fundamental data. (https://qlib.readthedocs.io/en/latest/component/data.html)

### 2.2 Parsing — `eval(parse_field(field))`

✅ Qlib does **not** hand-write a parser. The `ExpressionProvider.get_expression` path: a `parse_field` utility rewrites `$close` → `Feature("close")` (regex substitution of the `$` sigil), then Python `eval()` evaluates the rewritten string in a namespace that imports all operator classes from `qlib/data/ops.py` (`from .ops import Operators`). So `"Mean($close,5)"` becomes `Mean(Feature("close"), 5)`. NameErrors are caught to report "invalid operator/variable". (https://github.com/microsoft/qlib/blob/main/qlib/data/data.py)

✅ The tree is also buildable via **operator overloading**: the `Expression` base class defines `__add__`, `__sub__`, `__mul__`, `__truediv__`, `__gt__`, `__and__`, etc., each returning the corresponding op node (e.g. `__add__` returns `Add(self, other)`). So `($close - $open)/$open` composes nested op instances = the compute DAG. (https://github.com/microsoft/qlib/blob/main/qlib/data/base.py)

> ⚠️ For a C++ engine, `eval()` is not available and not desirable (it's a security/perf liability). The *lesson* to take is the tree shape (`Op(args...)` with `Feature("close")` leaves), not the parsing mechanism — we will hand-roll a Pratt parser (§4).

### 2.3 Operator class hierarchy (`qlib/data/ops.py`)

✅ Base classes (https://github.com/microsoft/qlib/blob/main/qlib/data/ops.py):
- `ExpressionOps` (abstract base, in `base.py`)
- `ElemOperator` → `NpElemOperator` (wraps a numpy ufunc via `getattr(np, func)`)
- `PairOperator` → `NpPairOperator` (binary numpy ops, handles Expression-or-scalar operands)
- `Rolling` (windowed; window=0 ⇒ `expanding`, else `rolling`)

✅ Concrete operators:
- **Element-wise:** `Abs`, `Sign`, `Log`, `Not`, plus `Mask`/`ChangeInstrument`.
- **Pairwise:** `Power`, `Add`, `Sub`, `Mul`, `Div`, `Greater` (= np.maximum), `Less` (= np.minimum), `Gt`, `Ge`, `Lt`, `Le`, `Eq`, `Ne`, `And`, `Or`.
- **Ternary:** `If` (= `np.where(cond, left, right)`).
- **Rolling / time-series (all backward-looking):** `Ref` (shift/delay; `series.shift(N)`, `N>0` = past, `N<0` = future — guarded against), `Mean`, `Sum`, `Std`, `Var`, `Skew`, `Kurt`, `Max`, `Min`, `IdxMax` (`argmax()+1`), `IdxMin`, `Quantile`, `Med`, `Mad`, `Rank`, `Count`, `Delta`, `Slope`, `Rsquare`, `Resi` (rolling linear-regression slope / R² / residual, Cython-accelerated), `WMA`, `EMA`, `Corr`, `Cov`.

### 2.4 Lazy evaluation + caching (the throughput lever)

✅ `Expression.load(instrument, start, end, freq)` is lazy and memoized: a `cache_key = (str(self), instrument, start_index, end_index, ...)` indexes an in-memory cache `H["f"]`, so an identical subexpression computed for the same instrument/range is reused. (base.py)

✅ Two persistent cache tiers (https://qlib.readthedocs.io/en/latest/component/data.html):
- **`ExpressionCache` / `DiskExpressionCache`** — caches a single computed expression's series to disk, keyed by a `_uri`. Override `_uri` and `_expression` to customize.
- **`DatasetCache` / `DiskDatasetCache`** — caches a whole dataset (instrument pool × field list × time range × freq). Metadata files (`.meta`, `.index`) track calendar alignment and drive invalidation.

✅ The expression engine is described as using "multiple kernels for speed" — i.e. the rolling ops dispatch to optimized (Cython/numpy) kernels rather than Python loops. (data.rst)

📘 **Takeaway for atx-engine:** Qlib validates the core architecture — *(string DSL → op tree of element/pair/rolling nodes) + (per-(expr,instrument,range) memoization) + (vectorized numpy/Cython kernels)*. Our improvement is to push memoization to the **subexpression-DAG** level across *all* alphas in a batch, and to use a columnar VM instead of per-node pandas calls.

---

## 3. Other Reference Systems

### 3.1 Zipline `Pipeline` — a compositional compute DAG (not a string DSL)

✅ Pipeline expresses computation as a **graph of `Term` objects** over 2-D `(date, asset)` blocks. Class hierarchy (https://zipline.ml4trading.io/_modules/zipline/pipeline/term.html ; source: `quantopian/zipline/pipeline/term.py`):
- `Term` — base for anything in the compute graph; **memoized by constructor args** (identical args ⇒ same instance, so the DAG auto-dedups). Attributes: `dtype`, `missing_value`, `ndim` (default 2), `domain`, `window_safe` (default False).
- `LoadableTerm` — data loaded by a `PipelineLoader` (e.g. `BoundColumn`); `windowed=False`, `inputs=()`, `dependencies={self.mask: 0}`.
- `ComputableTerm` — computed from `inputs`; parent of `Factor` (numeric), `Filter` (boolean), `Classifier` (categorical/int). Attributes: `inputs` (tuple of input Terms), `outputs`, `window_length`, `mask`.

✅ **Dependency DAG + windowing:** `ComputableTerm.dependencies` computes `extra_input_rows = max(0, window_length - 1)` per input — i.e. how many trailing rows each input must supply. The mask needs 0 extra rows. If `term.windowed` is truthy, its `compute_from_windows` is called with `AdjustedArray` windows; otherwise baseline `compute`. (term.py)

✅ **Topological execution:** the pipeline graph is iterated in **topological order**; `LoadableTerm`s are dispatched to loaders, `ComputableTerm`s are executed by passing them their (windowed) inputs. (search-confirmed; design issue #2265, API docs)

✅ **`window_safe` guard:** a non-`window_safe` term cannot be used as a windowed input (`_validate` enforces it). This is a *correctness rail*: e.g. a raw price level is not safe to feed into a trailing window after adjustments, whereas a return is. (term.py)

📘 **Takeaway:** Zipline is the architectural model for the *graph/DAG layer* — memoize Terms by structure (free CSE), track per-input window depth so each node knows its required lookback, and execute in topological order. `Factor`/`Filter`/`Classifier` map cleanly onto our shape system (float panel / bool mask / int group-labels).

### 3.2 alphalens / Alpha101 numpy-pandas reference implementations

✅ `yli188/WorldQuant_alpha101_code`, `STHSF/alpha101`, `Harvey-Sun/World_Quant_Alphas` are faithful pandas ports of the 101 alphas; they define the helper functions quoted in §1.2 and are the practical ground-truth for operator semantics. (https://github.com/yli188/WorldQuant_alpha101_code)
📘 `alphalens` (Quantopian) is the standard *evaluation* library (IC, quantile returns, turnover) — relevant later for scoring alphas, not for the expression VM itself.

### 3.3 gplearn / genetic-programming alpha mining

✅ `gplearn` extends scikit-learn for symbolic regression via GP. Program representation: **S-expression-like, stored as flat Python lists** of functions + terminals; printed as a LISP-style flattened tree. Function set is configurable (`make_function`); built-ins include arithmetic, comparison, trig, and transformers. Genetic ops: tournament selection, subtree crossover, point mutation (a function node may only be replaced by another function of the **same arity**). Fitness evaluation is the dominant cost. (https://gplearn.readthedocs.io/en/stable/intro.html ; https://gplearn.readthedocs.io/en/stable/_modules/gplearn/genetic.html)

📘 **Relevance to atx-engine:** GP miners generate *millions* of candidate expression trees that share enormous subexpression overlap (every program references `rank(close)`, `ts_mean(volume,20)`, etc.). This is precisely the workload that makes **subexpression-DAG dedup + intermediate-column caching** (§5.3) the dominant performance lever — arity-typed function sets map 1:1 onto our opcode arities.
⚠️ `alphagen`, `AlphaForge`, `AutoAlpha` are research systems in the same family (RL/GP alpha search emitting expression trees evaluated over panels); specifics were not separately verified here — treat as "same architecture, different search strategy."

### 3.4 Financial query DSLs (kdb+/q, Deltix QQL, OneTick OTQ)

⚠️ Not independently fetched in this pass. 📘 Domain knowledge: kdb+/q is a vector/array language where time-series and (via `xgroup`/`by`) cross-sectional aggregations are first-class and columnar; OneTick OTQ and Deltix QQL are event/tick-stream query languages with windowed operators. They confirm the columnar + windowed-operator paradigm but are less directly applicable than Qlib/Zipline for a panel-of-equities alpha VM. Flagged as **not verified** — do not cite specifics.

---

## 4. Compiler / Parser Engineering for Expression Languages

### 4.1 Lexer / tokenizer

📘 A hand-written tokenizer is sufficient and fastest: scan to produce tokens for identifiers (`close`, `ts_mean`), numeric literals, `$`-sigil fields (if adopting Qlib syntax), operators (`+ - * / < > == && || ? :`), parens, comma. Track source spans for error messages. No lexer generator needed for a grammar this small.

### 4.2 Parsing operator-precedence expressions — Pratt vs precedence-climbing vs shunting-yard

✅ **Pratt parsing (top-down operator precedence, TDOP)** and **precedence climbing** are essentially the **same algorithm**; precedence climbing is a special case of Pratt. Both are top-down and build parent nodes before children (unlike shunting-yard). (http://www.oilshell.org/blog//2016/11/01.html ; https://www.oilshell.org/blog/2017/03/30.html)

✅ **Shunting-yard** (Dijkstra) uses an explicit operator stack to emit flat RPN; it's great for producing postfix/RPN but produces a flat sequence rather than directly building an AST, and is awkward for mixing function calls, prefix/postfix, and right-associativity. (https://en.wikipedia.org/wiki/Operator-precedence_parser ; oilshell)

✅ **Pratt is the cleanest for a grammar with infix operators + function calls + prefix unary + grouping.** In Crafting Interpreters' formulation, each token type maps to a `ParseRule { prefix_fn, infix_fn, precedence }`; the driver `parsePrecedence(prec)` calls the prefix parselet, then loops consuming infix operators while `prec <= rule.precedence`. `binary()` recurses with `precedence+1` for left-associativity (use `precedence` for right-assoc). It "gracefully handles prefix, postfix, infix, mixfix — any -fix you got," and adding an operator is just one table row. (https://craftinginterpreters.com/compiling-expressions.html)

📘 Canonical references: **Vaughan Pratt, "Top Down Operator Precedence," POPL 1973**; Douglas Crockford's "Top Down Operator Precedence" essay; **Bob Nystrom, *Crafting Interpreters*** ("Compiling Expressions" = Pratt parser; "A Virtual Machine" / "A Bytecode VM" = stack VM; "Optimization" = computed-goto dispatch).

### 4.3 AST design & desugaring

📘 AST = `Expr` variant: `Literal(double)`, `Field(name)`, `Unary(op, child)`, `Binary(op, lhs, rhs)`, `Call(fn, args[])`, `Ternary(cond, a, b)`. Desugar at parse/compile time: `a - b` → `Binary(SUB)`; `x ? a : b` → a `select` op; `signedpower(x,a)` → `sign(x)*pow(abs(x),a)` (or keep as a primitive). Fold the field sigil into `Field` leaves. Function arity is checked here against an operator registry (name → arity, shape signature, lookahead flag).

---

## 5. Execution: Virtual Machine / Evaluation Architecture (CRITICAL — the hot path)

### 5.1 Tree-walking vs bytecode VM vs JIT

✅ A **bytecode VM** beats a **tree-walking interpreter**: tree-walking pays recursive AST traversal + virtual-dispatch (Visitor) overhead per node; a bytecode VM is a flat dispatch loop reading instructions, with the instruction pointer kept as a raw pointer for speed. Crafting Interpreters chooses a **stack-based** VM for simplicity (register VMs are "quite a bit harder to write a compiler for"). (https://craftinginterpreters.com/a-virtual-machine.html)

✅ **Stack vs register bytecode:** register VMs execute ~**46% fewer instructions** (fewer dispatches) at the cost of ~**26% larger bytecode** and a harder compiler; stack VMs are simpler and smaller. Threaded dispatch (computed-goto) helps stack VMs more because they dispatch more often. (https://dl.acm.org/doi/abs/10.1145/1328195.1328197 ; https://arxiv.org/pdf/1611.00467)

✅ **Dispatch techniques:** `switch` (portable, simplest), **computed-goto / direct threading** (one `goto *table[op]` per instruction — reduces branch mispredictions vs a single switch), token/inline threading, tail-call dispatch. (search-confirmed; Crafting Interpreters "Optimization" chapter covers computed-goto.)

> 📘 **Crucial reframing for *this* workload:** the stack-vs-register and dispatch debates are about *scalar* VMs where dispatch cost dominates. In a **vectorized** VM each opcode processes a whole column of N values, so per-instruction dispatch cost is amortized over N and becomes negligible. The big lever is therefore **vectorization**, not dispatch micro-optimization. (See 5.2.)

### 5.2 Vectorized / columnar interpretation (the key idea)

✅ **Vectorized execution** (Vectorwise/X100, DuckDB): the query is still *interpreted*, but each primitive processes a **batch ("vector") of values** in one call — DuckDB's `STANDARD_VECTOR_SIZE = 2048` tuples. This amortizes interpretation/dispatch over the batch, keeps the working set in **CPU cache**, exposes **SIMD**, and yields predictable, branch-friendly loops. (https://duckdb.org/why_duckdb ; https://medium.com/@connect.hashblock/vectorized-execution-in-duckdb-55679d6874f6)

✅ **Compiled (data-centric) execution** (HyPer/LLVM): generates a custom program per query, fusing operators into one tight loop, eliminating interpretation entirely — best single-thread throughput for simple/repeated queries, but pays **compilation latency** and infrastructure complexity. Kersten et al., *"Everything You Always Wanted to Know About Compiled and Vectorized Queries But Were Afraid to Ask"* (VLDB 2018): **neither dominates universally**; vectorization wins for complex/exploratory workloads and avoids compile latency; compilation wins for simple, hot, predictable queries. (https://www.vldb.org/pvldb/vol11/p2209-kersten.pdf)

✅ **Morsel-driven parallelism** (HyPer/DuckDB): split data into small **morsels** (~10K rows / aligned to vector size) dispatched dynamically to worker threads from a shared queue — composes naturally with vectorized operators and gives good load balance. (https://www.greybeam.ai/blog/duckdb-internals-part-1 ; search-confirmed)

📘 For an alpha VM the natural "vector" is a **cross-section** (all instruments at one date) or a **per-instrument time-series column**, depending on op kind. Time-series ops vectorize down a column (per instrument); cross-sectional ops vectorize across a row (per date). The panel is processed in column-major float64 with NaN as the missing sentinel — exactly numpy/Arrow's model. (Apache Arrow compute kernels operate on contiguous columnar arrays with a separate validity bitmap — same principle.)

### 5.3 Common subexpression elimination + DAG dedup + caching (massive throughput lever)

✅ **CSE**: detect identical subexpressions and compute once. The classic method (Sethi–Ullman) does a bottom-up traversal labeling subtrees and **builds a DAG where shared subexpressions become shared interior nodes**; an interior node reachable by >1 parent *is* a common subexpression. (https://en.wikipedia.org/wiki/Common_subexpression_elimination ; flylib Algorithms for Compiler Design)

✅ Cost: once a shared result is materialized it must be **kept alive until its last consumer**, which can raise peak memory — so dedup needs a liveness/refcount-driven free schedule. (CSE Wikipedia/Grokipedia)

📘 **Why this is the #1 lever here:** evaluating 10^5–10^9 alphas (GP/RL mining) over the same panel means colossal overlap — `rank($close)`, `ts_mean($volume,20)`, `delta($close,5)` recur in thousands of expressions. Strategy:
1. **Hash-cons** all expressions into one global DAG: each node keyed by `(opcode, child-node-ids, params)`; structurally identical nodes collapse to one (this is exactly how Zipline's Term memoization gives free CSE — §3.1).
2. Evaluate the DAG in **topological order**, materializing each node's column **once** into a cache slot keyed by node-id.
3. Reference-count consumers; **free** an intermediate column once its last consumer has run (bounded memory).
This converts "evaluate N alphas" into "evaluate the union DAG of their unique subexpressions," often a 10–100× reduction in compute.

### 5.4 Constant folding & strength reduction

📘 At compile time: fold constant subtrees (`2*3` → `6`, `log(1)` → `0`); strength-reduce (`x/const` → `x*(1/const)`; `pow(x,2)` → `x*x`; `decay_linear` weight vector precomputed once). Hoist loop-invariant scalars out of vector loops. These are cheap and standard.

### 5.5 JIT tradeoffs (LLVM ORC / asmjit / Cranelift)

✅ **LLVM ORC** is the modern LLVM JIT API (Kaleidoscope tutorial, LLDB expression evaluator); heavyweight dependency, best codegen, slow compile. **asmjit** is a lightweight C++ x86/ARM assembler/JIT (you emit instructions yourself; fast compile, no optimizer). **Cranelift** (Wasmtime) is a fast, lower-optimization codegen backend. (https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-3-llvm/ ; https://llvm.org/docs/ORCv2.html)

📘 **Recommendation (balanced):** For a *first* implementation, a **vectorized bytecode interpreter wins on simplicity-to-throughput ratio**. JIT only pays off when per-element scalar work between vectorized ops dominates *and* the same expression is reused enough to amortize compile time — which the vectorized model already largely defeats by amortizing dispatch over N. Defer JIT (and prefer asmjit/Cranelift over LLVM if pursued, to avoid the LLVM build/dep cost) until profiling proves the interpreter's dispatch + materialization overhead is the bottleneck. ⚠️ This is an engineering judgment, not a benchmarked claim for this exact workload.

---

## 6. Correctness Concerns Specific to Alpha Expressions

### 6.1 Lookahead safety (no future leakage)

📘 The invariant: a value at `(date t, instrument i)` may depend only on data at dates `≤ t`. Enforcement, layered:
- **Operator level:** every time-series op is *defined* backward — `delay(x,d)`/`Ref` shifts **forward in index = backward in time** (`shift(+N)` pulls past values to the present row; `shift(−N)` would pull the future and must be forbidden or flagged). ✅ Qlib's `Ref` explicitly documents `N<0` = future and engines must guard it. Rolling ops use trailing windows `[t−d+1, t]` only. (Qlib ops.py; §2.3)
- **VM level:** compute the **required lookback** per node (Zipline's `extra_input_rows = window_length − 1`, summed up the DAG) and only ever index into already-computed history. A `window_safe`-style flag (Zipline) prevents feeding a non-causal/level term into a window. (§3.1, term.py)
- **Point-in-time correctness:** fundamentals must be keyed by *availability date* (when the market knew it), not report date — separate concern from operator causality but part of the same no-leakage discipline.

### 6.2 NaN / missing-data propagation

✅ **`min_periods`** governs windowed NaN: a rolling op returns NaN until the window has `≥ min_periods` valid observations; for integer windows pandas defaults `min_periods = window`. Choose this explicitly per op (e.g. require full window for `stddev`, allow partial for `ts_mean`). (https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.rolling.html)
📘 Cross-sectional ops (`rank`, `zscore`, `indneutralize`) must exclude NaN instruments from the cross-section (rank only the valid set; demean by valid group members) and **propagate NaN out** for excluded instruments — never impute silently.
📘 `ts_corr`/`ts_cov` need pairwise-valid handling: a window with any NaN either yields NaN or uses only complete pairs — pick one policy and apply it uniformly.

### 6.3 Universe membership

📘 Each date has a **valid universe mask** (which instruments are tradable/listed that day). Cross-sectional ops operate only over the masked set; `indneutralize` demeans within (group ∩ universe). Delisted/not-yet-listed instruments are NaN and excluded — this is both a correctness and a lookahead concern (don't rank against instruments that didn't exist yet).

### 6.4 Determinism

📘 Reproducibility requirements:
- **Tie-breaking in rank:** ✅ pandas default is `method='average'` (ties share the mean rank); options are `average/min/max/first/dense`. (https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.rank.html) For a deterministic engine, pick one method and a **stable secondary key** (e.g. instrument id) so ties resolve identically every run — `average` is the Alpha101 default (`rank(pct=True)`), but `ordinal`/`first` with a stable id gives bit-reproducibility.
- **Reductions:** floating-point sums are non-associative; fix a **deterministic reduction order** (e.g. always left-to-right over a canonical instrument ordering, or Kahan/pairwise summation) so parallel evaluation reproduces serial results.
- **NaN ordering:** define where NaNs sort (always last) consistently.

### 6.5 Cross-sectional neutralization mechanics

⚠️ Two variants, both in use:
- **Demean-by-group (`indneutralize` as defined in Alpha101):** subtract the group mean within each (sub)industry/sector at each date. ✅ (paper definition / summary)
- **Regression-residual neutralization:** at each date regress the raw signal on group dummies (and optionally log-cap), keep the residual. This is the standard "size/sector-neutral" factor construction. (https://goldinlocks.github.io/Fama-MacBeth-regression/ ; cross-sectional factor practice)
📘 Demean-by-group is the special case of the regression where regressors are only group indicators. Implement the general residualizer; expose `indneutralize` as the demean special case.

---

## 7. Performance & Scale

📘 / ✅ Levers, consolidated:
- **Columnar batch + SIMD:** float64 column-major panel, contiguous per-field arrays, 32/64-byte alignment for AVX. Vectorized ops over 2048-ish-element chunks (DuckDB's vector size) keep data in L1/L2 and auto-vectorize. ✅ (DuckDB model)
- **Subexpression sharing / DAG dedup + caching:** the dominant lever for mass evaluation (§5.3) — Qlib and Zipline both cache/memoize; we push it to a global hash-consed DAG. ✅
- **Parallelism:** morsel/date-partitioned threads sharing the dedup cache; time-series ops parallelize over instruments (columns), cross-sectional ops over dates (rows). ✅ (morsel-driven)
- **Memory layout:** one contiguous `double[]` per field per panel; a parallel **validity bitmap** (Arrow-style) or NaN sentinel for missing; intermediate node columns allocated from a pool and freed by refcount when their last consumer completes.

---

## 8. Operator Vocabulary Table (WorldQuant ∪ Qlib ∪ Alpha101 union)

Shape signatures: **S** = scalar, **V** = cross-section vector (per-date, indexed by instrument), **P** = panel (date × instrument). "LA-safe" = lookahead-safe (backward-looking only). Element-wise and cross-sectional ops are inherently causal (use only current date); time-series ops are causal *by construction* (trailing window).

| Operator | Arity | Shape sig (in → out) | Semantics | LA-safe? |
|---|---|---|---|---|
| `+ - * /` | 2 | P,P→P / P,S→P | element-wise arithmetic, scalar broadcast | yes |
| `abs` | 1 | P→P | absolute value | yes |
| `sign` | 1 | P→P | −1/0/+1 | yes |
| `log` | 1 | P→P | natural log | yes |
| `power(x,a)` / `signedpower(x,a)` | 2 | P,S→P | `x^a` / `sign(x)·|x|^a` | yes |
| `min(x,y)` / `max(x,y)` (Qlib `Less`/`Greater`) | 2 | P,P→P | element-wise min/max | yes |
| `<,>,<=,>=,==,!=` | 2 | P,P→P(bool) | comparison → mask | yes |
| `&&,||,!` (And/Or/Not) | 1–2 | P→P(bool) | logical on masks | yes |
| `cond ? a : b` (If/where) | 3 | P(bool),P,P→P | ternary select | yes |
| `rank(x)` | 1 | P→P (per-row V) | cross-sectional percentile rank in [0,1] | yes |
| `zscore(x)` | 1 | P→P | cross-sectional standardize (per date) | yes |
| `scale(x,a=1)` | 2 | P,S→P | rescale so Σ|x|=a per date | yes |
| `normalize/winsorize/quantile` | 1–2 | P→P | cross-sectional transforms | yes |
| `indneutralize(x,g)` / `group_neutralize` | 2 | P,group→P | demean (or regress-out) within group per date | yes |
| `group_rank/group_zscore` | 2 | P,group→P | rank/zscore within group per date | yes |
| `delay(x,d)` (Qlib `Ref`) | 2 | P,S→P | value d days ago (`shift(+d)`) | yes |
| `delta(x,d)` | 2 | P,S→P | `x − delay(x,d)` | yes |
| `ts_sum/sum(x,d)` | 2 | P,S→P | trailing-d sum | yes |
| `ts_mean/sma(x,d)` (Qlib `Mean`) | 2 | P,S→P | trailing-d mean | yes |
| `stddev(x,d)` (Qlib `Std`/`Var`) | 2 | P,S→P | trailing-d std / var | yes |
| `ts_min/ts_max(x,d)` | 2 | P,S→P | trailing-d min / max | yes |
| `ts_argmin/ts_argmax(x,d)` (Qlib `IdxMin/IdxMax`) | 2 | P,S→P | 1-based day-of-min/max in window | yes |
| `ts_rank(x,d)` (Qlib `Rank`) | 2 | P,S→P | rank of today within trailing-d window | yes |
| `correlation(x,y,d)` (Qlib `Corr`) | 3 | P,P,S→P | trailing-d Pearson corr | yes |
| `covariance(x,y,d)` (Qlib `Cov`) | 3 | P,P,S→P | trailing-d covariance | yes |
| `product(x,d)` | 2 | P,S→P | trailing-d product | yes |
| `decay_linear(x,d)` (Qlib `WMA`) | 2 | P,S→P | linear-weighted (d..1) trailing MA, weights sum 1 | yes |
| `ts_zscore/ts_std_dev/ts_decay_exp` (BRAIN) | 2 | P,S→P | trailing standardize / exp-decay | yes |
| `slope/rsquare/resid(x,d)` (Qlib) | 2 | P,S→P | trailing rolling-regression stats | yes |
| `ema(x,d)` (Qlib `EMA`) | 2 | P,S→P | exponential moving average | yes |
| `skew/kurt/med/mad/count(x,d)` (Qlib) | 2 | P,S→P | trailing higher moments / counts | yes |

⚠️ `indneutralize` semantics (demean vs regression-residual) and exact NaN policy per op are engine choices — the table gives the common interpretation.

---

## 9. Concrete Recommendation for the C++20 `atx-engine` Implementation

### 9.1 Parser — hand-rolled **Pratt parser**
- Hand-written lexer → tokens (idents, numbers, `$`fields if adopting sigil syntax, operators, parens, comma, `?`, `:`).
- **Pratt/TDOP parser** with a `ParseRule { prefix_fn, infix_fn, BindingPower }` table (Crafting Interpreters formulation). Left-assoc infix recurses at `bp+1`, right-assoc at `bp`. Prefix parselets: number, field, `(` grouping, unary `-`/`!`, function-call (`name(` → parse comma args until `)`). This cleanly handles infix operators **and** function calls **and** prefix unary in one mechanism, and new operators are one table row. ✅ (§4.2)
- Emit an `Expr` AST; run **constant folding + desugaring + arity/shape typecheck** against an operator registry (name → arity, shape signature, lookahead flag).

### 9.2 IR — global **hash-consed expression DAG** (free CSE)
- Compile every alpha's AST into nodes in a **single shared DAG**, hash-consed on `(opcode, ordered child node-ids, params)`. Structurally identical subexpressions across all alphas collapse to one node (this is Zipline's Term-memoization idea generalized; §3.1, §5.3). This *is* the CSE pass.
- Annotate each node with: output **shape** (Scalar / CrossSection-V / Panel-P), **dtype** (f64 / bool-mask / int-group), and **required lookback** (max trailing window depth, propagated up the DAG à la Zipline's `extra_input_rows`).

### 9.3 VM — **vectorized bytecode interpreter over columnar Frames**
- Linearize the DAG in **topological order** into a flat bytecode program; each instruction = `(opcode, src-slot-ids, dst-slot-id, params)`. Slots are column buffers in a pool.
- **Each opcode operates on a whole column at once** (a cross-section for CS ops, a per-instrument series for TS ops) — dispatch cost amortized over N (§5.2). Inner kernels are tight, SIMD-friendly loops over contiguous `double*` + validity bitmap.
- Dispatch: start with a `switch`; upgrade to **computed-goto** only if profiling shows dispatch matters (it usually won't once vectorized). Keep the instruction pointer as a raw pointer. ✅ (§5.1)
- **Materialize-once + refcount-free:** each DAG node's result column is computed once into its slot and reused by all consumers; free the slot back to the pool when its consumer refcount hits zero (bounds peak memory; §5.3).
- **Parallelism:** morsel/date- or instrument-partitioned worker threads sharing the immutable cache of already-computed node columns (§7). TS ops parallelize over instruments (columns); CS ops over dates (rows).

### 9.4 Type / shape system
- Three shapes: **Scalar (S)**, **CrossSection vector (V)** = one value per instrument at a date, **Panel (P)** = date × instrument. Plus dtype tags: f64, bool-mask, int-group-label (Zipline's Factor/Filter/Classifier split).
- Broadcast rules enforced at compile time: element-wise = P∘P→P / P∘S→P; time-series = P→P (per-column trailing window); cross-sectional = P→P where each row is reduced/ranked independently (logically P→V-per-row). Shape mismatches are compile errors.

### 9.5 Lookahead-safety & determinism enforcement (build them into the VM)
- **Causality by construction:** time-series kernels only read indices `[t−d+1 .. t]`; `delay`/`Ref` only shifts past→present; a negative shift is a **compile error**. A `window_safe` flag (Zipline) forbids feeding non-causal terms into windows. (§6.1)
- **Universe mask + NaN:** every date carries a validity mask; CS ops operate over the valid set only and emit NaN elsewhere; per-op `min_periods` policy for TS ops. (§6.2–6.3)
- **Deterministic rank:** fix tie-break method (`average` to match Alpha101, *or* `ordinal` keyed by a stable instrument id for bit-reproducibility) and NaN-sort-last. (§6.4)
- **Deterministic reductions:** canonical instrument ordering + fixed (e.g. pairwise/Kahan) summation so multithreaded == single-thread results. (§6.4)

### 9.6 Opcode / instruction-set sketch
```
; data / immediates
LOAD_FIELD   dst, field_id            ; raw panel column ($close ...)
CONST        dst, imm                 ; scalar literal

; element-wise (P,P->P or P,S->P)
ADD/SUB/MUL/DIV dst, a, b
ABS/SIGN/LOG/NEG dst, a
POW/SPOW     dst, a, b                ; pow / signedpower
MINP/MAXP    dst, a, b                ; element-wise min/max (Greater/Less)
CMP_LT/GT/LE/GE/EQ/NE dst, a, b       ; -> bool mask
AND/OR/NOT   dst, a, b
SELECT       dst, cond, a, b          ; ternary / If

; cross-sectional (per date row)
CS_RANK      dst, a                   ; percentile rank
CS_ZSCORE    dst, a
CS_SCALE     dst, a, k
CS_DEMEAN_G  dst, a, group            ; indneutralize (demean)
CS_NEUT_G    dst, a, group[, cap]     ; regression-residual neutralize
CS_RANK_G/ZS_G dst, a, group

; time-series (trailing window d, per instrument column)
TS_DELAY     dst, a, d                ; Ref
TS_DELTA     dst, a, d
TS_SUM/MEAN/STD/VAR  dst, a, d
TS_MIN/MAX   dst, a, d
TS_ARGMIN/ARGMAX dst, a, d            ; 1-based
TS_RANK      dst, a, d
TS_CORR/COV  dst, a, b, d
TS_PROD      dst, a, d
TS_DECAY_LIN dst, a, d                ; precomputed weight vector
TS_EMA/WMA/SKEW/KURT/MED/MAD/SLOPE/RSQ/RESID dst, a, d

; meta
STORE_ALPHA  alpha_id, src            ; emit a finished alpha column
FREE         slot                     ; refcount-driven release
```

---

## 10. Refuted / Do-Not-Overclaim

- ⚠️ **Exact `101 Formulaic Alphas` paper text** for each operator's edge-case semantics (NaN policy, `floor(d)`, `signedpower` vs `x^a`) was **not** read verbatim — the PDF didn't decode through the tool. Definitions here come from the abstract + faithful open-source ports + secondary summaries. Verify against the actual PDF before encoding edge cases.
- ⚠️ **`indneutralize` is demean-by-group OR regression-residual** — both are used; the paper's precise variant was not confirmed from primary text. Implement the general residualizer.
- ⚠️ **WorldQuant BRAIN's full operator list and per-op NaN/return semantics** come from community/secondary docs (official docs are gated). Treat the BRAIN operator names as indicative, not a guaranteed spec.
- ⚠️ **Qlib uses `eval(parse_field(...))`** — confirmed from `data.py`/`base.py` via fetch; the exact regex in `parse_field` was inferred (lives in `qlib/utils`), not quoted line-for-line.
- ⚠️ **JIT-vs-vectorized recommendation** for *this specific* alpha workload is an engineering judgment extrapolated from the DB-query literature (Kersten et al.), not a benchmark of alpha evaluation. The DB result ("neither dominates universally") is verified; the "interpreter-first" call is reasoned, not measured.
- ⚠️ **kdb+/q, Deltix QQL, OneTick OTQ specifics** were not independently fetched — only general domain framing is offered; do not cite specifics.
- ⚠️ **`alphagen` / `AlphaForge` / `AutoAlpha`** internals not verified beyond "GP/RL expression-tree miners in the same family."

---

## 11. Reference Index (URLs)

**WorldQuant / Alpha101**
- arXiv:1601.00991 abstract — https://arxiv.org/abs/1601.00991
- arXiv:1601.00991 PDF — https://arxiv.org/pdf/1601.00991
- Reference impl (yli188) — https://github.com/yli188/WorldQuant_alpha101_code/blob/master/101Alpha_code_1.py
- Reference impl (STHSF/alpha101) — https://github.com/STHSF/alpha101
- Reference impl (Harvey-Sun) — https://github.com/Harvey-Sun/World_Quant_Alphas
- BRAIN operators (community/DeepWiki) — https://deepwiki.com/xiegengcai/world-quant-brain/3.3-advanced-alpha-generation

**Microsoft Qlib**
- ops.py — https://github.com/microsoft/qlib/blob/main/qlib/data/ops.py
- base.py — https://github.com/microsoft/qlib/blob/main/qlib/data/base.py
- data.py — https://github.com/microsoft/qlib/blob/main/qlib/data/data.py
- Data layer docs — https://qlib.readthedocs.io/en/latest/component/data.html
- DeepWiki overview — https://deepwiki.com/microsoft/qlib

**Zipline Pipeline**
- term.py module docs — https://zipline.ml4trading.io/_modules/zipline/pipeline/term.html
- term.py source — https://github.com/quantopian/zipline/blob/master/zipline/pipeline/term.py
- factor.py docs — https://zipline.ml4trading.io/_modules/zipline/pipeline/factors/factor.html
- Pipeline domains design — https://github.com/quantopian/zipline/issues/2265

**gplearn / GP**
- Intro to GP — https://gplearn.readthedocs.io/en/stable/intro.html
- genetic.py module — https://gplearn.readthedocs.io/en/stable/_modules/gplearn/genetic.html

**Parsing**
- Pratt == precedence climbing — http://www.oilshell.org/blog//2016/11/01.html
- Precedence climbing widely used — https://www.oilshell.org/blog/2017/03/30.html
- Operator-precedence parser (Wikipedia) — https://en.wikipedia.org/wiki/Operator-precedence_parser
- Crafting Interpreters — Compiling Expressions (Pratt) — https://craftinginterpreters.com/compiling-expressions.html
- Pratt TDOP explainer — https://www.maxgcoding.com/pratt-parsing-tdop

**VM / vectorized execution**
- Crafting Interpreters — A Virtual Machine — https://craftinginterpreters.com/a-virtual-machine.html
- DuckDB execution format (vectors) — https://duckdb.org/docs/stable/internals/vector
- Why DuckDB — https://duckdb.org/why_duckdb
- DuckDB internals (morsel/vectorized) — https://www.greybeam.ai/blog/duckdb-internals-part-1
- Kersten et al., Compiled vs Vectorized (VLDB 2018) — https://www.vldb.org/pvldb/vol11/p2209-kersten.pdf
- Vectorized execution explainer — https://medium.com/@connect.hashblock/vectorized-execution-in-duckdb-55679d6874f6
- Stack vs register VM (Shi et al., TACO) — https://dl.acm.org/doi/abs/10.1145/1328195.1328197
- Stack vs register survey — https://arxiv.org/pdf/1611.00467

**CSE / DAG**
- Common subexpression elimination (Wikipedia) — https://en.wikipedia.org/wiki/Common_subexpression_elimination
- Eliminating global common subexpressions — https://flylib.com/books/en/1.424.1.72/1/

**JIT**
- LLVM ORCv2 — https://llvm.org/docs/ORCv2.html
- Adventures in JIT compilation (LLVM) — https://eli.thegreenplace.net/2017/adventures-in-jit-compilation-part-3-llvm/

**Correctness**
- pandas rolling (min_periods) — https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.rolling.html
- pandas rank (tie-breaking) — https://pandas.pydata.org/docs/reference/api/pandas.DataFrame.rank.html
- Fama–MacBeth / cross-sectional residualization — https://goldinlocks.github.io/Fama-MacBeth-regression/
