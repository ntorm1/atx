# Quant research platform design and automated alpha discovery (US equity L/S, small team, state of knowledge Sept 2026)

Scope note. These notes extend, and do not repeat, `code-review-v7.md` (measured bottlenecks A1-A8, B1-B9, C1-C7) and
`literature-v7.md` S5 (DSR, PBO/CSCV, CPCV, HLZ t > 3, CGS 3.4/3.8, BRAIN thresholds, Assaying Anomalies report card).
Evidence tags used below: **[PR]** peer-reviewed; **[PP]** preprint / working paper; **[DOC]** official documentation;
**[VEN]** vendor or author self-claim without independent replication; **[PRAC]** practitioner blog / repo.
Verification tags: **(text)** = I read the paper text or doc page; **(snippet)** = search-result snippet or a
machine summary only, treat numbers as unverified.
Method caveat: the web-search budget for the session ran out before the last batch (kdb+, Two Sigma/Jane Street,
multi-fidelity search, synthetic-data tests, incremental risk-model updates). Those items are listed under Gaps.

## 1. Architecture of formulaic-alpha research systems (what makes them fast)

### Takeaway
Every fast public system uses the same five ideas: a declarative expression language compiled to a DAG, de-duplication
of shared sub-expressions across the whole alpha batch, a cache keyed at the sub-expression (node) level rather than the
alpha level, fixed-width columnar binary storage that can be sliced by date offset, and one expression definition that
runs in both batch and streaming mode. Published speed-ups are large (Qlib 20x from caching on a 14-factor task; KunQuant
about 170x over pandas; DolphinDB median 15.5x over pandas), but all are measured against slow Python baselines, mostly in
float32, and none is measured under a bit-reproducibility requirement.

### Cited Findings
**Qlib (Microsoft)**
- Storage is a flat-file database: one binary file per instrument per attribute, in a "compact fixed-width format so that
  indexing by bytes becomes possible"; the first 4 bytes hold the start time index so all series align on a shared
  calendar; data are appended in time order, and adding or removing an attribute touches only that file. **[PP] (text)**
  — [Yang et al. 2020, Qlib paper](https://arxiv.org/abs/2009.11189)
- Expression engine: each expression is parsed into a syntax tree and "all computed results of nodes" are stored in an
  in-memory LRU cache, so repeated sub-expressions are computed once. On top sit two disk caches: ExpressionCache (same
  layout as raw data, saves computed expressions) and DatasetCache (saves the combined, index-aligned dataset). Both are
  time-indexed, so a cache entry can serve a query with a different date range. **[PP][DOC] (text)**
  — [Qlib paper](https://arxiv.org/abs/2009.11189); [Qlib data-layer docs](https://qlib.readthedocs.io/en/latest/component/data.html)
- Measured benchmark (task: build a dataset of 14 factors from daily OHLCV, 800 stocks per day with a pool that changes
  daily, 2007-01-01 to 2020-01-01). Total seconds on 1 CPU: HDF5 184.4 +/- 3.7; MySQL 365.3 +/- 7.5; MongoDB 253.6 +/- 6.7;
  InfluxDB 368.2 +/- 3.6; Qlib no cache 147.0 +/- 8.8; Qlib expression cache only 47.6 +/- 1.0; Qlib expression + dataset
  cache 7.4 +/- 0.3. On 64 CPUs: no cache 8.8 +/- 0.6; expression cache 4.2 +/- 0.2. Storage: HDF5 287 MB, Qlib raw 303 MB,
  +expression cache 802 MB, +dataset cache 1,000 MB. The in-memory node cache alone saves about 24% of expression time;
  the expression cache saves 80.4% when nothing misses. **[PP] (text; authors' own benchmark)**
  — [Qlib paper, Table 1](https://arxiv.org/abs/2009.11189)
- Point-in-time database for fundamentals: per-feature files with columns (date = publication date, period, value,
  _next), where `_next` is the byte index of the next revision of the same period, plus an index file per feature; a
  query returns the latest value published on or before the as-of date. Docs state it is designed for quarterly/annual
  report data and that performance "can be improved". **[DOC] (machine summary of the docs page)**
  — [Qlib PIT docs](https://qlib.readthedocs.io/en/latest/advanced/PIT.html)
- Workflow: `qrun` executes a whole pipeline (data, model, backtest, recording) from one YAML file; the Recorder /
  ExpManager API mirrors MLflow and uses MLflow as its default backend. **[DOC] (snippet)**
  — [Qlib workflow docs](https://qlib.readthedocs.io/en/latest/component/workflow.html); [Qlib recorder docs](https://qlib.readthedocs.io/en/latest/component/recorder.html)

**Expression compilers**
- KunQuant compiles factor expressions to C++: builds an IR graph, applies common-subexpression elimination across
  factors, fuses adjacent element-wise operators into one loop (no temporary buffers), emits AVX2/AVX-512, runs
  multi-threaded, supports both time-major and stock-major layouts, float32 and float64, and a streaming mode.
  Author benchmarks: Alpha101 on 64 stocks x 260 days, pandas 6.138 s vs 0.083 s single thread and 0.027 s on 4 threads
  (i7-7700HQ, float32), quoted as about 170x; Alpha101 on 1,024 stocks x 2,600 days in 1.04 s on a 32-thread 14900KF
  (float32). **[VEN] (machine summary of the README; numbers not independently replicated)**
  — [KunQuant](https://github.com/Menooker/KunQuant)
- DolphinDB ships the 101 alphas as a module; on one year of simulated daily data, for the 69 alphas compared it
  reports a median 15.5x speed-up over pandas, with 27.5% of alphas more than 100x faster, attributed to incremental
  window functions. The same factor code is reused in streaming through a parser that builds a pipeline of
  reactive-state and cross-sectional engines. 32 of the 101 alphas were excluded from the comparison, and the page
  gives no correctness comparison. **[VEN] (machine summary of vendor tutorial)**
  — [DolphinDB WQ101 tutorial](https://docs.dolphindb.com/en/Tutorials/wq101alpha.html); [reactive state engine](https://docs.dolphindb.com/en/Tutorials/reactive_state_engine.html)
- AlphaEvolve prunes redundant operations before evaluation and fingerprints the pruned alpha; if the fingerprint is in
  the cache the stored fitness is reused instead of re-evaluating. **[PR] (text)**
  — [Cui et al. 2021, SIGMOD](https://arxiv.org/abs/2103.16196)

**Zipline / Alphalens / vectorbt / dataframe stacks**
- Zipline's Pipeline engine computes a dependency graph of terms and evaluates it by date chunk (`compute_chunk`), with
  windowed terms receiving lookback windows through AdjustedArray. **[DOC] (snippet)**
  — [Zipline engine source](https://zipline.ml4trading.io/_modules/zipline/pipeline/engine.html)
- Alphalens standard tear sheet: rank IC and its distribution, mean return by factor quantile, quantile turnover,
  factor rank autocorrelation, and breakdowns by sector. **[DOC] (snippet)**
  — [Alphalens](https://github.com/quantopian/alphalens); [API](https://alphalens.ml4trading.io/api-reference.html)
- vectorbt packs many parameter configurations into one NumPy array and simulates them together with Numba (and, in the
  PRO version, Rust), with chunking, caching and thread/process parallelism; the README claims "up to 100x" over
  event-driven frameworks and about 1000x over backtrader in a pairs-trading example. The performance page I fetched
  contains no benchmark numbers. **[VEN] (snippet)**
  — [vectorbt README](https://github.com/polakowo/vectorbt); [vectorbt PRO performance](https://vectorbt.pro/features/performance/)
- Polars uses lazy evaluation with a query planner (predicate/projection push-down, elimination of redundant scans);
  blog benchmarks put Polars and DuckDB at roughly 10x pandas on join/filter/group workloads over Parquet. These are
  generic analytics benchmarks, not rolling cross-sectional factor workloads. **[DOC][PRAC] (snippet)**
  — [Polars comparison page](https://docs.pola.rs/user-guide/misc/comparison/); [codecentric benchmark](https://www.codecentric.de/en/knowledge-hub/blog/duckdb-vs-dataframe-libraries)
- DuckDB has a native ASOF JOIN for "value of a varying property at a specific point in time", and exchanges data with
  Polars/Arrow without copying. **[DOC] (snippet)**
  — [DuckDB AsOf joins](https://duckdb.org/2023/09/15/asof-joins-fuzzy-temporal-lookups)
- ArcticDB (Man Group, open source; commercial arrangement with Bloomberg) versions every write, which gives
  point-in-time reads of any earlier dataset state, cheap daily appends and audit of research inputs. **[VEN][DOC] (snippet)**
  — [ArcticDB repo](https://github.com/man-group/ArcticDB); [time travel](https://arcticdb.io/blog/time-travel-in-arcticdb/); [Man Group page](https://www.man.com/arcticdb-dataframe-database)

**WorldQuant BRAIN / WebSim as publicly described**
- Simulation settings exposed to the user: region, universe (e.g. TOP3000), delay 0/1, decay (linear smoothing, raises
  holding period and cuts turnover), neutralization (market / sector / industry / subindustry), truncation (max weight
  per name), pasteurization, NaN handling, unit check. Fitness = Sharpe x sqrt(|returns| / max(turnover, 0.125)).
  **[PRAC] (machine summary of a community repo; official docs are behind a login)**
  — [WorldQuant-alpha-trading repo](https://github.com/alexisdpc/WorldQuant-alpha-trading)
- A 2026 preprint that ran agents on BRAIN describes the benchmark as US TOP3000, delay 1, 2019-2023 in-sample window,
  with gates on Fitness, Sharpe, turnover, weight concentration, sub-universe Sharpe and self-correlation;
  self-correlation is computed by the platform against previously submitted alphas with 0.70 as the ordinary limit,
  and a submission above 0.70 can still pass through a Sharpe-improvement exception. **[PP] (text)**
  — [AgonAlpha, arXiv 2608.11250](https://arxiv.org/abs/2608.11250)
- Self-correlation is measured on daily PnL changes over a 2-year window. **[PRAC] (snippet)**
  — [wq-alpha-research](https://github.com/QuantML-Research/wq-alpha-research)
- The 101 formulaic alphas paper is the public reference for the operator vocabulary and formula style. **[PR]
  (cited from prior knowledge of the paper; not re-read in this session)**
  — [Kakushadze 2016, 101 Formulaic Alphas](https://arxiv.org/abs/1601.00991)

### Inferences
- Sub-universe rule recovered from data. The limits printed in AgonAlpha's Table 3 (1.09, 0.76, 1.00, 1.51 for
  full-universe Sharpe 2.52, 1.76, 2.32, 3.48) are reproduced to two decimals by
  `limit = 0.75 x sqrt(1000/3000) x Sharpe_full`. So the test is: Sharpe on the TOP1000 subset must be at least 0.75 x
  sqrt(N_sub/N_full) x the full-universe Sharpe. This is a cheap, parameter-free robustness gate the platform can copy.
- The platform's cache is keyed per alpha (DSL hash + field payload hashes). The public designs cache per node. A
  Merkle-style key `hash(op, params, child keys)` with leaves keyed by field payload hash would make the cache shared
  across alphas and library versions, and would let a changed outer operator reuse all inner nodes. Qlib (LRU node
  cache), KunQuant (CSE) and AlphaEvolve (fingerprint cache) are three independent precedents.
- Memory is the binding constraint locally, so the node cache must be bounded: one f64 node panel for 1,155 dates x
  5,627 names is about 52 MB, so a 10-entry LRU costs about 0.5 GiB. Evaluate in topological order and free a node
  when its last consumer has run; persist only nodes whose reference count across the batch is 2 or more.
- Canonicalise before hashing (commutative operand order, constant folding, `rank(rank(x)) -> rank(x)`, sign
  normalisation), otherwise mined candidates that differ only in form defeat the cache and inflate the trial count.
- Date-blocked evaluation with a lookback halo (the fix already proposed as A6) is the same design as Zipline's
  `compute_chunk`. It also enables incremental daily update: append one date block and recompute only the block.
- Published speed-ups will not transfer one-for-one: they are against pandas, mostly float32, and free to reorder
  floating-point reductions. With f64 and a fixed reduction order, AVX2 gives at most 4 lanes and fused multiply-add
  must stay off. The gain to expect locally is from removing repeated work (CSE, node cache, hashing once), not from
  SIMD.
- Operator fusion (one loop for a chain of element-wise ops) is compatible with bit-exactness as long as the
  per-element operation order is unchanged; it removes temporaries, which helps the 1.5 GiB limit directly.
- A fixed-width file per field (the current layout) is already the Qlib layout transposed. Memory-mapping the file
  and reading only the date range needed is the missing step.

### Gaps
- kdb+/q practice (splayed/partitioned, memory-mapped column files): the one documentation page I fetched covered only
  object serialization, and the search budget ran out. No citable finding.
- Official BRAIN/WebSim documentation is behind a login; operator count and exact thresholds come from community
  repos, from a 2026 preprint, and from S5 of the v7 literature review.
- I found no public measurement of expression-DAG caching or CSE under a byte-reproducibility constraint, and no
  public benchmark on a US panel of about 5,600 names.
- Tulchinsky et al. "Finding Alphas" (2nd ed., 2019): only the publisher's table of contents was reachable; the
  second edition adds chapters on alpha correlation, controlling biases and machine learning/automated search
  ([Wiley](https://www.wiley.com/en-us/Finding+Alphas%3A+A+Quantitative+Approach+to+Building+Trading+Strategies%2C+2nd+Edition-p-9781119571216)). No page-level claims are cited.

## 2. Backtest speed: vectorised screens, exact replays, batched sweeps, warm starts

### Takeaway
The published numbers say an optimiser-based daily backtest of a few thousand names is a minutes-scale job on one
workstation, and that warm-starting plus factorization reuse cuts QP time by roughly 3-7x. The design that follows is
two-tier: a vectorised weights-times-returns screen with a linear cost term for every candidate, and the exact replay
only for finalists, with construction variants run in lockstep over shared signal and risk inputs.

### Cited Findings
- Single-period convex trading problem with 1,500 assets and a 50-factor risk model solves in under 0.5 s on one
  thread (conservative estimate at 1 Gflop/s). One year of daily backtest (about 250 solves) takes "a few minutes or
  less". A 32-core machine runs 5 years x 64 parameter choices (80,000 solves) in under 10 minutes. Time scales
  linearly in assets and quadratically in factors; 4,500 assets with 100 factors is about 12x slower. **[PR] (text;
  the figures are the authors' estimates, not a measured benchmark)**
  — [Boyd et al. 2017, Multi-Period Trading via Convex Optimization](https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf)
- The same monograph recommends checking simulator trustworthiness by re-running with randomly perturbed parameters
  (for example 10% each day): if results move a lot under reasonable perturbation the simulation should not be
  trusted. **[PR] (text)**
  — [Boyd et al. 2017](https://web.stanford.edu/~boyd/papers/pdf/cvx_portfolio.pdf)
- OSQP: factorization caching gives a 2.6x to 4x time improvement; on the portfolio problem warm start gives a 5.8x to
  7x reduction in time and 2.9x to 3.6x fewer iterations. **[PR] (text)** One example improved from 200.6 ms to 32.2 ms
  (6.2x). **(snippet)**
  — [Stellato et al. 2020, OSQP](https://arxiv.org/abs/1711.08013)
- vectorbt's approach to parameter sweeps is to broadcast all configurations into array columns and simulate them in
  one compiled pass. **[VEN] (snippet)**
  — [vectorbt README](https://github.com/polakowo/vectorbt)
- AlphaEval argues that a backtest-free battery (IC-based predictive power, rank stability, perturbation robustness,
  diversity by eigenvalue entropy, LLM-scored logic) can rank alpha sets, and reports more than 25% less evaluation
  time than backtest-based evaluation. **[PP] (machine summary; not verified against the PDF)**
  — [AlphaEval, arXiv 2508.13174](https://arxiv.org/abs/2508.13174)
- A budget allocator that runs a halving tournament over candidates (16 + 8 + 4 + 2 + 1 = 31 evaluations) used 61%
  fewer simulations than carrying all 16 candidates through five passes (80). **[PP] (text)**
  — [AgonAlpha](https://arxiv.org/abs/2608.11250); method origin [Li et al. 2018, Hyperband](https://arxiv.org/abs/1603.06560)
- The evolutionary-computation survey warns that a cheap proxy can steer search to a "proxy optimum", prune good
  candidates early, or send expensive backtests to false positives, and asks for uncertainty-aware surrogates.
  **[PP] (text)**
  — [Yu et al. 2026, arXiv 2608.01789](https://arxiv.org/abs/2608.01789)

### Inferences
- Tier 1 (screen), per candidate signal: standardise, neutralise, form dollar-neutral weights W (dates x names), then
  `pnl_t = sum_i W[t-1,i] * r[t,i] - c_i * |W[t,i] - W[t-1,i]|` with a per-name linear cost. This is two passes over
  one panel, well under a second in compiled code, and can share the IC pass's loaded returns. Report Sharpe, turnover,
  BRAIN-style fitness, and margin. Tier 2 (exact replay) only for candidates that pass tier 1 and the marginal test in
  section 4.
- Validate the tier-1 proxy once: rank-correlate tier-1 net Sharpe with exact-replay net Sharpe over the existing
  library. Only use tier 1 as a gate if the rank correlation is high and no replay-winner is rejected by the screen.
  This answers the surrogate-bias warning directly.
- The platform's NAV replay (26-70 s per construction cell) is not slow by the published yardstick; the saving is in
  not running it for most candidates and in running construction grids in lockstep. Signal composition, exposures and
  the risk basis are identical across a theta/dust/cap grid, so they should be computed once per decision date and
  only the per-variant state (holdings, cash, fills) should fork.
- If the book moves to a QP (the deferred qp-factor-v1 trial), reuse yesterday's primal/dual solution and keep the KKT
  factorization when only the linear term changes. With OSQP-class solvers the published 3-7x applies. Determinism
  requires a fixed iteration cap, fixed tolerance and single-threaded linear algebra.
- Successive halving fits the alpha search: score all candidates on a cheap fidelity (IC on a date sub-sample or the
  TOP1000 subset), keep half, repeat at higher fidelity. Every evaluation at every rung still counts as a trial.

### Gaps
- No published throughput numbers for lockstep construction sweeps or for incremental factor-risk-model updates
  (rolling EWMA covariance, rank-one updates). Search budget exhausted before this was covered.
- No independent benchmark of vectorbt; all figures are vendor claims.
- No public measurement of how well a linear-cost vectorised screen ranks strategies relative to an exact replay with
  borrow, caps and participation limits. The validation above has to be done in-house.

## 3. Research workflow and experiment management

### Takeaway
Public descriptions agree on an assembly line with separately measured stations, a declarative spec per experiment,
versioned point-in-time inputs, and a standard automatic report per signal. They also agree that the backtest must not
be the research tool: the count of iterations is itself the main source of false discoveries.

### Cited Findings
- Lopez de Prado: "It takes almost as much effort to produce one true investment strategy as to produce a hundred";
  the recommended organisation is an assembly line where "tasks ... are clearly divided into subtasks" and "quality is
  independently measured and monitored for each subtask". **[PR] (text)**
  — [Lopez de Prado 2018, JPM](https://jpm.pm-research.com/content/44/6/120.abstract); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3104816)
- Same paper: repeating a backtest until it looks good leads to false discovery even if each run is walk-forward
  out-of-sample; "it typically takes about 20 such iterations to discover a (false) investment strategy" at the 5%
  level; "feature importance is a research tool, and backtesting is not". **[PR] (text)**
  — [Lopez de Prado 2018](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3104816)
- Sculley et al.: glue code and "pipeline jungles" grow as new signals are added incrementally; experimental code
  paths left as conditional branches in production code make it "difficult or impossible to test all possible
  interactions". **[PR] (snippet)**
  — [Sculley et al. 2015, NeurIPS](https://papers.neurips.cc/paper/5656-hidden-technical-debt-in-machine-learning-systems.pdf)
- Qlib: a whole experiment is a YAML file run by `qrun`; the recorder logs parameters, metrics and artifacts with an
  MLflow-like API. **[DOC] (snippet)**
  — [Qlib workflow](https://qlib.readthedocs.io/en/latest/component/workflow.html); [Qlib recorder](https://qlib.readthedocs.io/en/latest/component/recorder.html)
- Novy-Marx and Velikov provide a single script (`test_signal.m`) that runs the full protocol and writes a standard
  report for a new signal by changing only the signal input. **[PP][DOC] (snippet)**
  — [Assaying Anomalies toolkit](https://github.com/velikov-mihail/AssayingAnomalies); [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4338007)
- An audit of 30 LLM-based trading studies found 25 give some recoverable information on point-in-time discipline or
  split basis and 26 on execution timing, but only 21 clearly describe held-out evaluation, 18 release artifacts, and
  14 treat cost or turnover at a level that can be stress-tested. **[PP] (text)**
  — [Yao, Zheng and Li 2026, arXiv 2606.08285](https://arxiv.org/abs/2606.08285)
- The 2026 evolutionary-computation survey sets a minimum reporting list for an automated discovery system: candidate
  counts, LLM calls, stopping rules, rejected candidates, all evaluation filters, seeds, splits, cost assumptions and
  neutralisation protocol; it states that reproducibility "must include the search process itself". **[PP] (text)**
  — [Yu et al. 2026](https://arxiv.org/abs/2608.01789)
- AgonAlpha's unit of search is a frozen artifact (hypothesis, executable expression, platform evidence, rationale,
  review status) and its reviewer runs in a fresh context with authority to re-execute and veto. The scheduler never
  generates or judges; it only allocates the next evaluation. **[PP] (text)**
  — [AgonAlpha](https://arxiv.org/abs/2608.11250)
- Two Sigma's public material covers tools (Flint time-series library for Spark, BeakerX notebooks), not its research
  process. **[VEN] (snippet)**
  — [Two Sigma Flint](https://www.twosigma.com/articles/introducing-flint-a-time-series-library-for-apache-spark/)
- Isichenko's book has dedicated sections titled "Simulation vs production", "Simulation and overfitting" and
  "Research and simulation efficiency". **(snippet; book text not accessed)**
  — [Wiley](https://www.wiley.com/en-us/Quantitative+Portfolio+Management:+The+Art+and+Science+of+Statistical+Arbitrage-p-9781119821328)

### Inferences
- The platform already has the hard parts that the literature asks for and most published systems lack: a
  hash-chained trial ledger, pinned inputs, byte-reproducible receipts. What is missing is (a) the spec as the only
  input (no per-version generator script), and (b) the alpha registry as a first-class store.
- Alpha registry record: canonical DSL, DSL hash, node-hash set, fields read with payload hashes, hypothesis text and
  citation, origin class (literature prior / human idea / parameter search / machine mined), prereg hash, first trial
  id, all trial ids, report-card hash, status (candidate, admitted, retired), decay half-life. Origin class decides
  which hurdle in section 6 applies.
- Automatic report card contents (extends R5.6 without repeating it): tier-1 net Sharpe and fitness; sub-universe
  Sharpe ratio against the 0.75 x sqrt(N_sub/N_full) limit; maximum PnL correlation to admitted alphas on daily PnL
  changes over a 2-year window; residual IC and spanning alpha t-stat versus the book (section 4); cumulative trial
  count for the alpha's family and the hurdle it was held to.
- Pre-registration is cheap to enforce mechanically: the driver refuses to run a candidate whose hypothesis/prereg
  stub hash is not in the ledger before the first evaluation timestamp.
- "Research debt" in this codebase is concrete: 9 hand-copied ladder scripts, a Python re-implementation of the C++
  neutraliser, and unconnected legacy mining code. The Sculley failure mode is exactly the per-version generator.

### Gaps
- No reliable public description of how Two Sigma, Jane Street, AQR or WorldQuant organise internal alpha pipelines
  beyond marketing material and the sources above. I did not find a citable Jane Street talk on research
  infrastructure.
- No measured evidence that experiment-tracking tools (MLflow and similar) reduce false discoveries; the benefit is
  argued, not measured.
- I did not find a public feature-store design with point-in-time correctness that is specific to equity
  cross-sections other than Qlib PIT and ArcticDB versioning.

## 4. Marginal-contribution scoring without a full backtest

### Takeaway
A candidate's value to the book is its appraisal ratio after regressing its PnL (or its signal) on the book, and that
needs only the candidate's daily PnL series from the tier-1 screen plus stored book series. The inclusion rule is
closed-form: add the candidate only if its Sharpe exceeds its correlation with the book times the book's Sharpe.

### Cited Findings
- "It is optimal to include an asset i in a portfolio if and only if S_i >= rho_{i,p} x S_p", where rho is the
  correlation of the asset with the portfolio. The portfolio Sharpe is a risk-weighted combination of component
  Sharpes each divided by its correlation with the portfolio. **[PR] (text, Propositions 2 and 3)**
  — [Benhamou and Guez 2018, Incremental Sharpe](https://arxiv.org/abs/1807.09864)
- For an optimal combination of a benchmark and an active portfolio, SR_P = sqrt(SR_B^2 + IR^2). **(snippet; textbook
  Treynor-Black result)**
  — [AnalystPrep CFA note](https://analystprep.com/study-notes/cfa-level-2/calculate-and-interpret-the-information-ratio-ex-post-and-ex-ante-and-contrast-it-to-the-sharpe-ratio/)
- With a positive average pairwise correlation among signals, the diversification gain of an equal-weight
  combination is bounded by 1/sqrt(average correlation). **[PP] (snippet only; PDF download failed)**
  — [Hentschel 2026, The Limits of Diversification](https://www.ludgerhentschel.com/PDFs/Hentschel%20'25.pdf)
- With many alphas and short history the sample covariance matrix is singular, so weights come from a regression over
  principal components; unconstrained regression gives under-diversified weights skewed against turnover, which
  bounds on the weights correct. Source code is included. **[PR] (text, abstract)**
  — [Kakushadze 2015, Combining Alphas via Bounded Regression, Risks 3(4)](https://arxiv.org/abs/1501.05381)
- Optimal weights for N alphas can be computed in O(N) operations without principal components, matrix inversion or
  iteration. **[PR] (text, abstract)**
  — [Kakushadze and Yu 2017, How to Combine a Billion Alphas, J. Asset Management](https://arxiv.org/abs/1603.05937)
- AlphaGen uses the marginal criterion as the search reward: the reward for a new formula is the IC of the linear
  combination model after the formula is added to the pool. The mined pool has combined test IC 0.0725 on CSI300; the
  paper notes that filtering by mutual IC does not solve the synergy problem. Pool sizes beyond a few dozen are
  limited by the quadratic cost of pairwise correlations. **[PR] (text)**
  — [Yu et al. 2023, KDD](https://arxiv.org/abs/2306.12964)
- Redundancy cut-offs in use: BRAIN 0.70 on PnL correlation with a Sharpe-improvement exception; RD-Agent(Q) drops a
  new factor if its maximum IC-correlation with existing factors is >= 0.99; AlphaEvolve cites 15% PnL correlation as
  the hedge-fund standard for "weakly correlated". **[PP][PR] (text)**
  — [AgonAlpha](https://arxiv.org/abs/2608.11250); [RD-Agent(Q)](https://arxiv.org/abs/2505.15155); [AlphaEvolve](https://arxiv.org/abs/2103.16196)
- The survey warns that low historical correlation is unreliable in stress and that large pools that look diverse by
  structure become redundant in portfolios ("correlation red sea"). **[PP] (text)**
  — [Yu et al. 2026](https://arxiv.org/abs/2608.01789)

### Inferences
- Derivation (follows from the two formulas above). Let S_b be book Sharpe, S_c candidate Sharpe, rho their PnL
  correlation. The candidate's appraisal ratio against the book is `AR = (S_c - rho x S_b) / sqrt(1 - rho^2)`, and the
  best attainable combined Sharpe is `sqrt(S_b^2 + AR^2)`. Examples with S_b = 1.2: a candidate with S_c = 0.6 and
  rho = 0.3 has AR = 0.25 and lifts the book to 1.226 (+2%); the same candidate at rho = 0.5 has AR = 0 and adds
  nothing; at rho = 0.0 it lifts the book to 1.342 (+12%). Correlation matters more than standalone Sharpe.
- Three estimators in rising cost, all far cheaper than a replay:
  1. Signal space: cross-sectionally regress the candidate signal on the combined book signal each date, then compute
     the IC of the residual. Needs only the cached candidate panel and the cached composite.
  2. PnL space: regress tier-1 daily PnL of the candidate on book daily PnL; report intercept t-stat (HAC) and AR.
     This is the spanning test already named in R5.5(iii).
  3. Refit: add the candidate to the admission fit and read the change in combined IC, the AlphaGen criterion.
     Incremental if the Gram matrix of member signals/PnLs is stored and only one row and column is added.
- The estimated marginal gain is itself selected on, so it inherits the multiple-testing problem: the hurdle in
  section 6 should be applied to the residual/spanning t-stat, not to the standalone t-stat.
- Apply the PnL-correlation test to net tier-1 PnL on daily changes, and also within the worst-decile book days, to
  address the stress-correlation warning.

### Gaps
- No published study measures how well residual IC or spanning alpha predicts the realised change in net book Sharpe
  after costs and constraints in an exact replay. In-house calibration on the existing library is required.
- Hentschel's bound is quoted from a search snippet only.
- A primary academic source for the squared-Sharpe decomposition (Treynor-Black; Gibbons-Ross-Shanken;
  Barillas-Shanken) was not retrieved because the search budget ran out; the formula is standard.

## 5. Automated alpha mining: methods, evidence, critiques, regularisers

### Takeaway
Reported results come overwhelmingly from China A-share indices, price-volume inputs only, short test windows and
authors' own baselines; when a later paper re-runs an earlier method the IC is usually much lower. On US large caps the
reported ICs of mined price-volume alphas are about 0.005-0.017, and the one backtest-versus-live comparison shows
Sharpe roughly halving. The defensible use for a small US team is machine-assisted hypothesis generation and search over
a constrained grammar, with the evaluation, trial accounting and admission rules kept outside the generator.

### Cited Findings
**Genetic programming and evolutionary search**
- gplearn controls bloat with a parsimony coefficient that penalises program length in selection; in "auto" mode the
  coefficient is Cov(length, fitness)/Var(length), recomputed each generation. Fitness for SymbolicTransformer is
  Pearson or Spearman correlation with the target. **[DOC] (snippet)**
  — [gplearn docs](https://gplearn.readthedocs.io/en/stable/intro.html)
- AutoAlpha: hierarchical evolutionary search over the 101-alphas operator set with a PCA-based quality-diversity
  step to push search away from explored regions, warm starts and replacement; backtested on the Chinese market only.
  **[PP] (text, abstract)**
  — [Zhang et al. 2020, arXiv 2002.08245](https://arxiv.org/abs/2002.08245)
- AlphaEvolve: evolves alphas with scalar, vector and matrix operands (AutoML-Zero style) plus sector-relation
  operators; data are 5 years of NASDAQ prices (2013-2017, 1,220 days, 1,026 stocks after filters); claims high Sharpe
  and low correlation versus GP and neural baselines. One market, one five-year window. **[PR] (text)**
  — [Cui et al. 2021, SIGMOD](https://arxiv.org/abs/2103.16196)

**Reinforcement learning and generative models**
- AlphaGen: 21 operators, 6 price-volume features, a fixed constant set, windows of 10-50 days, formulas capped at 20
  tokens, PPO with invalid-action masking, reward = combined-pool IC. Train 2009-2018, validation 2019, test 2020-2021,
  label 20-day return. Test IC / RankIC: CSI300 0.0725 / 0.0806, CSI500 0.0438 / 0.0727, against GP-with-filter 0.0183
  and 0.0117. The trading test is a top-k/drop-n long-only simulation on CSI300 for 2020-2021. **[PR] (periods, token
  cap and headline ICs read in the text; operator list and baseline rows from a machine summary)**
  — [Yu et al. 2023, KDD](https://arxiv.org/abs/2306.12964)
- AlphaForge: a generator network trained against a surrogate fitness predictor, plus a combination model that
  re-selects and re-weights factors every day from recent IC/ICIR. Walk-forward test 2018-2022 with annual retraining,
  label `Ref(VWAP,-21)/Ref(VWAP,-1)-1`, 5 seeds. IC: CSI300 4.40% (s.d. 0.56), CSI500 2.84% (0.58). In the same table the
  AlphaGen-style RL baseline scores 2.09% and 1.91%, and GP 1.29% and 0.37%. Best pool size was 10; larger pools did
  worse. The paper also claims real-money results. Example formulas printed in the paper are deeply nested, with
  repeated inverse and log transforms of volume volatility. **[PR] (text)**
  — [Shi et al. 2025, AAAI](https://arxiv.org/abs/2406.18394)
- QuantFactor REINFORCE: replaces PPO with REINFORCE plus a greedy baseline and an information-ratio reward shaping
  term; the abstract claims a 3.83% gain in correlation with returns over the latest methods. **[PP] (text, abstract)**
  — [Zhao et al. 2024, arXiv 2409.05144](https://arxiv.org/abs/2409.05144)
- AlphaQCM: distributional RL (learned Q function and variance) for the same search problem; claims it outperforms
  competitors "particularly when dealing with large datasets comprising numerous stocks". **[PR] (snippet)**
  — [Zhu and Zhu 2025, ICML](https://proceedings.mlr.press/v267/zhu25ag.html)

**LLM-driven agents**
- Alpha-GPT: human-in-the-loop system that turns a researcher's idea into formulas and then runs algorithmic search;
  the EMNLP 2025 demo version claims a top-10 rank among more than 41,000 teams in the WorldQuant IQC 2024 and an IC
  rise from 0.58% to 2.23% after the interactive process. **[PR demo track][VEN] (snippet)**
  — [Wang et al., arXiv 2308.00016](https://arxiv.org/abs/2308.00016); [EMNLP 2025 demo](https://aclanthology.org/2025.emnlp-demos.14/)
- QuantAgent: two-loop self-improving agent with a knowledge base; claims only that it uncovers "viable financial
  signals". **[PP] (text, abstract)**
  — [Wang et al. 2024, arXiv 2402.03755](https://arxiv.org/abs/2402.03755)
- AlphaAgent: three regularisers inside the generation loop: originality (size of the largest common subtree between
  the candidate's AST and any alpha in the library), hypothesis-factor alignment (LLM-scored), and complexity (symbol
  length, parameter count, log of feature count). OHLCV inputs only. Train 2015-2019, validation 2020, test
  2021-01 to 2025-01. CSI500: IC 0.0212, annual excess return 11.0%, IR 1.49, max drawdown 9.4%. S&P 500: IC 0.0056,
  return 8.74%, IR 1.05, max drawdown 9.1%. Costs: CSI500 5 bp buy / 15 bp sell; S&P 500 5 bp on sells only. Hit ratio
  0.29 versus 0.16 without the regularisers. LLM: GPT-3.5-turbo. **[PR] (machine summary of the arXiv HTML; headline
  returns and IRs match the abstract)**
  — [Tang et al. 2025, KDD](https://arxiv.org/abs/2502.16789)
- RD-Agent(Q): research/development/feedback loop with a bandit scheduler choosing between factor and model work; new
  factors with maximum IC-correlation >= 0.99 to existing ones are dropped; cost "under $10" per run. Main test CSI300,
  train 2008-2014, validation 2015-2016, test 2017-01 to 2020-08: best configuration IC 0.0532, annual return 14.21%,
  IR 1.74, max drawdown 7.4%, against Alpha158 IC 0.0341, return 5.70%, IR 0.85. Second test with train 2008-2021,
  validation 2022-2023, test 2024 to June 2025: CSI500 IC 0.0288, IR 2.17; NASDAQ 100 IC 0.0162-0.0172, IR 1.33-1.77,
  against Alpha158 NASDAQ 100 IC 0.0040, IR 0.03. The LLM sees schema-level information only, not raw data or split
  dates. **[PR] (text)**
  — [Li et al. 2025, NeurIPS Datasets and Benchmarks](https://arxiv.org/abs/2505.15155)
- LLM + MCTS: each tree node is a formula, expansion is an LLM refinement guided by multi-dimensional backtest
  feedback, and a frequent-subtree-avoidance rule tells the LLM not to reuse the most common subtrees of already
  effective alphas. CSI300 and CSI1000, train 2011-2020, test 2021-01 to 2024-11. LLM methods were given 1,000-3,000
  search iterations; non-LLM methods up to 600,000. Reported LLM cost ranges from $7.5 to $74.4 depending on the model.
  **[PR] (text)**
  — [Shi, Duan and Li 2026, AAAI](https://arxiv.org/abs/2505.11122)
- Chain-of-Alpha: generation chain plus optimisation chain using backtest feedback; A-share benchmarks only. The
  arXiv record carries an administrator note that one version was removed for a licence-rights problem. **[PP] (text,
  abstract)**
  — [Cao 2025, arXiv 2508.06312](https://arxiv.org/abs/2508.06312)
- AgonAlpha on BRAIN (US TOP3000, delay 1, 2019-2023): across five users and six model back-ends, 17 of 60
  submissions were graded SPECTACULAR; reported Fitness up to 9.50 and Sharpe up to 3.48; the featured alphas have
  turnover 5.1-9.6% and self-correlation below 0.70. All of these are in-sample platform metrics: the paper says the
  alphas "entered BRAIN's OS tracking stage" and I found no out-of-sample performance figures in the text. Its own
  per-node notes flag regime dependence (annual Fitness ranging 0.91 to 6.87 for one node; one year contributing about
  41% of in-sample PnL for another). **[PP] (text)**
  — [Ye et al. 2026, arXiv 2608.11250](https://arxiv.org/abs/2608.11250)

**Independent evaluations and critiques**
- AlphaBench (ICLR 2026): LLMs generate syntactically valid factors reliably, but as zero-shot judges of factor
  quality "the performance of almost all LLMs ... is surprisingly poor": no model scores above 0.40 overall on
  ranking and scoring, accuracy on signal classification and pairwise selection is "generally close to random
  guessing", and chain-of-thought gives no consistent benefit. In search, evolutionary capacity saturates at about 20
  candidates per round. Prompt caching hit 85% in the cost case study. **[PR] (text)**
  — [Luo et al. 2026, AlphaBench](https://www.cs.cityu.edu.hk/~cliu644/HomePage/doc/AlphaBench/AlphaBench_PDF.pdf); [OpenReview](https://openreview.net/forum?id=d97Q8r7ZKZ)
- Backtest versus live: AlphaCrafter splits data into train 2016-2022, validation 2023, backtest 2024-01 to 2026-02
  and a paper-traded live window 2026-03-02 to 2026-06-12 through a brokerage API. For CSI300 it reports backtest
  Sharpe about 1.53 and live return 5.70% with Sharpe about 0.70; it states that deep-learning baselines such as LSTM
  show "pronounced degradation in live trading". The live window is about 3.5 months, so the live Sharpe has a very
  wide error band. **[PP] (text for the design; headline numbers from a snippet because the table did not extract
  cleanly)**
  — [AlphaCrafter, arXiv 2605.05580](https://arxiv.org/abs/2605.05580)
- A 2026 survey of agentic trading tabulates live benchmarks: "Most LLMs lost money in live evaluation" (DeepFund);
  "strong static benchmark scores do not predict live trading performance" (LiveTradeBench); "backtest gains fall
  after model cutoffs" (FinLake-Bench). **[PP] (text)**
  — [Agentic Quantitative Trading survey, arXiv 2608.31041](https://arxiv.org/abs/2608.31041)
- LLM look-ahead: a one-standard-deviation rise in a look-ahead-bias proxy raises the marginal effect of the LLM
  forecast on next-day return by 0.077%, about 37% of the baseline effect of 0.197%. Separately, GPT-4o recalls S&P 500
  closing levels inside its training window with under 1% error, and masking or instructions do not prevent recall.
  **[PP] (snippet)**
  — [Gao, Jiang and Yan, arXiv 2512.23847](https://arxiv.org/abs/2512.23847); [Look-Ahead-Bench, arXiv 2601.13770](https://arxiv.org/abs/2601.13770)
- Collective overfitting: "repeated optimization against the same market, frequency, split, operator library, and
  cost assumptions can create collective overfitting"; most methods still optimise raw IC; "increasing autonomy mainly
  increases the speed at which systems fit historical noise" unless fitness controls for repeated testing; an LLM
  rationale creates "narrative overfitting"; memory archives that store lucky results steer later search to the same
  failures. **[PP] (text)**
  — [Yu et al. 2026, arXiv 2608.01789](https://arxiv.org/abs/2608.01789)
- AlphaEval's cross-family comparison (A-shares 2010-2024 and S&P 500 2010-2020): GP and RL families score highest on
  perturbation robustness and rank stability; generative and LLM families score highest on predictive power and
  interpretability but lower on robustness. **[PP] (machine summary; not verified against the PDF)**
  — [AlphaEval, arXiv 2508.13174](https://arxiv.org/abs/2508.13174)
- Decay of static libraries: AlphaAgent reports Alpha158 and GP factor ICs falling from 0.02-0.04 toward zero over
  2019-2024 on its data, while its own stay near 0.02. The comparison is made by the method's authors. **[PR][VEN]
  (machine summary)**
  — [AlphaAgent](https://arxiv.org/abs/2502.16789)

### Inferences
- Cross-paper re-evaluation is the closest thing to independent replication, and it is unflattering. AlphaGen reports
  CSI300 IC 0.0725 for itself; AlphaForge re-runs an AlphaGen-style RL baseline at 0.0209 (different label and a
  walk-forward 2018-2022 window). An AutoAlpha baseline scores 0.0334 in RD-Agent's CSI300 table, and GP scores
  0.0129 in AlphaForge's. Headline ICs depend heavily on label, window and universe; they are not portable estimates.
- Evidence relevant to a US book is thin and points to small effects. US results I could verify: AlphaEvolve
  (NASDAQ 2013-2017), AlphaAgent (S&P 500, IC 0.0056), RD-Agent(Q) (NASDAQ 100, IC about 0.017, an 18-month test),
  AgonAlpha (BRAIN TOP3000, in-sample only). None covers a 5,600-name universe with borrow costs and short
  constraints. None reports deflated Sharpe, PBO or a trial count.
- Mined alphas are price-volume, short-horizon and high-turnover by construction of the search space (windows up to
  about 50 days, OHLCV inputs). Most papers charge 5-15 bp or nothing. For a book with realistic US costs the fitness
  must be net of the linear cost model, otherwise the search converges on untradable reversal variants.
- Regularisers with some published support, in order of how directly the evidence bears on overfitting:
  1. Hard complexity cap (AlphaGen 20 tokens; AlphaAgent symbol/parameter/feature penalties; gplearn parsimony).
  2. Marginal rather than standalone fitness (AlphaGen reward; BRAIN self-correlation gate).
  3. Structural novelty versus the library (AlphaAgent AST common-subtree; MCTS frequent-subtree avoidance). The survey
     warns that syntactic novelty does not imply economic novelty, so pair it with a PnL-correlation test.
  4. Hypothesis first, formula second (AlphaAgent alignment score; AgonAlpha frozen artifact). Useful as a prior and
     as documentation. It is not evidence, because an LLM can rationalise any formula.
  5. Turnover term in the objective (BRAIN fitness).
  6. Small pools: AlphaForge found 10 factors best; AlphaGen says a few dozen suffice.
  7. Walk-forward retraining with a validation year between train and test (AlphaForge, RD-Agent second split).
- Use the LLM as a proposer only. AlphaBench shows LLMs cannot rank factors without a backtest, so any step where the
  model scores, filters or "reviews" candidates from their text alone is noise. Scoring must come from the engine.
- LLM contamination control for this platform: never show the model dates, tickers, returns or holdout results; give
  it the operator list, field names with descriptions, and train-window report cards only. Treat any alpha family the
  model may know from the literature as a literature prior, not as a discovery.
- The platform's DSL (about 90 operators including group operators, fundamentals, short-interest and filing fields)
  is a larger search space than the 21-operator / 6-feature space in AlphaGen, so the number of distinct expressible
  formulas, and with it the false-discovery exposure, is much larger. Constrain the grammar per family (typed
  templates with a few free slots) rather than searching free-form trees.
- Minimum viable miner: template grammar with typed slots; canonicalise and hash; drop duplicates by node hash;
  successive halving on tier-1 net fitness; marginal test against the book; every evaluation appended to the trial
  ledger with family id; finalists go to the exact replay; nothing touches the holdout.

### Gaps
- No independent, peer-reviewed replication of any mining method on US equities with realistic costs, a long test
  window and a disclosed trial count. This is the largest gap in the literature.
- FAMA (the neural-symbolic factor mining agent, ACL Findings 2024) was not retrieved; it appears only as a baseline
  in AlphaEval.
- Numbers for AlphaQCM, QuantFactor REINFORCE beyond the abstract, and LLM+MCTS test-set metrics were not extracted.
- Not reviewed, listed for follow-up only: AlphaSAGE / GFlowNets ([2509.25055](https://arxiv.org/abs/2509.25055)),
  AlphaPROBE ([2602.11917](https://arxiv.org/abs/2602.11917)), QuantaAlpha ([2602.07085](https://arxiv.org/abs/2602.07085)),
  FactorMiner ([2602.14670](https://arxiv.org/abs/2602.14670)), reinforcement fine-tuning for alpha discovery
  ([2605.15412](https://arxiv.org/abs/2605.15412)), AlphaDiverse ([2609.29014](https://arxiv.org/abs/2609.29014)).
- No published synthetic-data test protocol for mining pipelines (planted signal recovery, pure-noise panels) was
  found before the search budget ran out. The procedure in section 6 is my proposal, not a cited method.
- AlphaCrafter's table did not extract cleanly; its backtest/live figures should be re-read from the PDF before
  being quoted in the final report.

## 6. Multiple-testing control for mined alphas

### Takeaway
The literature disagrees on how many published findings are false (estimates run from under 10% to over 45%), but it
agrees on two things that matter here: signals selected at |t| > 2 lose about half their return out of sample whether
they came from peer review or from mining, and in US non-micro-cap stocks after 2005 the median published anomaly
earned about 7 bp per month. Hurdles should therefore scale with the number of trials in the search, be applied to the
marginal (spanning) statistic, and be paired with an explicit shrinkage of expected performance.

### Cited Findings
- Chordia, Goyal and Saretto generate more than 2 million strategies from real data and derive multiple-testing
  thresholds of t = 3.8 (time-series) and 3.4 (cross-sectional regressions); without adjustment about 45% of
  rejections would be false. **[PR] (snippet)**
  — [Chordia, Goyal and Saretto 2020, RFS](https://academic.oup.com/rfs/article/33/5/2134/5739455)
- Harvey, Liu and Zhu catalogue at least 316 factors and argue a new factor needs t > 3.0, using Bonferroni, Holm and
  Benjamini-Hochberg-Yekutieli adjustments. **[PR] (snippet)**
  — [Harvey, Liu and Zhu 2016, RFS](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2249314)
- Harvey and Liu 2020 state that 3.0 was never meant as a universal rule; the hurdle should be calibrated to the data
  set by double bootstrap and to the prior fraction of true strategies p0. Their example: with p0 = 10% on their data,
  a 5% Type I error rate needs t = 2.4, at which 18% of strategies survive. A higher p0 allows a lower hurdle. They
  also calibrate a hurdle to a chosen ratio of misses to false discoveries, so the two error costs can differ.
  **[PR] (text)**
  — [Harvey and Liu 2020, JF](https://arxiv.org/abs/2006.04269)
- Yan and Zheng build more than 18,000 fundamental signals and find by bootstrap that many remain significant after
  accounting for data mining; predictability is stronger after high-sentiment periods and where limits to arbitrage
  are greater. **[PR] (snippet)**
  — [Yan and Zheng 2017, RFS](https://academic.oup.com/rfs/article-abstract/30/4/1382/2908895)
- Chen, Lopez-Lira and Zimmermann mine 29,000 accounting ratios. Sorting on in-sample performance each June
  (1994-2020), the most extreme bin has in-sample return -59 bp per month (average t -4.2) and out-of-sample -47 bp, a
  20% decay; the opposite extreme bin decays 32%. Value-weighted decay is about 60%. Out-of-sample predictability is
  "much weaker post-2004, though it still exists". Conditional on in-sample t > 2, peer-reviewed and data-mined
  predictors both retain about 50% post-sample. Predictors with risk-based or equilibrium explanations do not do
  better; only research agnostic about the explanation shows signs of outperformance. **[PP] (text; v7 dated
  December 2025)**
  — [Chen, Lopez-Lira and Zimmermann, arXiv 2212.10317](https://arxiv.org/abs/2212.10317)
- Chen's "easy bound": at least one in five randomly chosen accounting ratios has |t| > 2, against one in twenty
  under the null, so the false discovery rate among |t| > 2 findings is at most 25%; a tighter bound gives 9%. He
  attributes the 45%+ estimates in HLZ and CGS to interpretation and calibration choices. **[PP] (text; v10 dated
  November 2025; the text thanks an editor and referee, journal not confirmed here)**
  — [Chen, arXiv 2206.15365](https://arxiv.org/abs/2206.15365)
- Chen (Management Science): raised hurdles are weakly identified because results below current hurdles are
  unobserved; empirical-Bayes shrinkage and the FDR among observed findings are well identified. **[PR] (snippet)**
  — [Chen, Do t-Statistic Hurdles Need to Be Raised?](https://pubsonline.informs.org/doi/10.1287/mnsc.2023.03083)
- Chen and Welch: across about 200 published long-short anomalies, the median return is 48 bp per month through 2005
  in all stocks, 19 bp after 2005, 26 bp through 2005 in the top-90%-of-capitalisation universe (about 3,000 stocks),
  and 7 bp after 2005 in that universe (median CAPM alpha 9 bp, median t 0.64). Exceptions were "a handful of
  profitability and financing signals". **[PP] (text; draft July 2026, targeted at FAJ)**
  — [Chen and Welch 2026, arXiv 2607.06502](https://arxiv.org/abs/2607.06502)
- McLean and Pontiff: across 97 predictors, returns are 26% lower out of sample and 58% lower after publication; the
  26% is an upper bound on the data-mining effect. **[PR] (snippet)**
  — [McLean and Pontiff 2016, JF](https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365)
- Jensen, Kelly and Pedersen: with a Bayesian hierarchical model most factors replicate, cluster into 13 themes, and
  hold out of sample in 93 countries; a large number of observed factors strengthens the evidence. **[PR] (snippet)**
  — [Jensen, Kelly and Pedersen 2023, JF](https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13249)
- Deflated Sharpe: the expected maximum Sharpe over N independent trials under the null is approximately
  `sqrt(V) x [(1 - g) x Zinv(1 - 1/N) + g x Zinv(1 - 1/(N e))]`, with V the variance of trial Sharpes and g the
  Euler-Mascheroni constant; it rises with N and with V. **[PR] (formula from a snippet; the statement that it rises
  with N and V read in the text of the 2018 paper)**
  — [Bailey and Lopez de Prado 2014](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551); restated in [Lopez de Prado 2018](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3104816)
- Reusable holdout: repeated adaptive queries to a holdout degrade its validity; Thresholdout uses
  differential-privacy ideas to allow many adaptive queries with quantified error. **[PR] (snippet)**
  — [Dwork et al. 2015, Science](https://www.science.org/doi/10.1126/science.aaa9375)
- Rademacher Anti-Serum (Paleologo 2025, chapter 8): uses the empirical Rademacher complexity of the whole set of
  tested strategies to give a uniform finite-sample lower bound on true performance (empirical Sharpe or IC minus
  penalty terms), without a holdout. **(snippet of a secondary source only; the book and the primary derivation were
  not accessed)**
  — [Balaena Quant note](https://medium.com/balaena-quant-insights/rademacher-anti-serum-ras-08cef83f7102); [book, Wiley](https://www.wiley.com/en-us/The+Elements+of+Quantitative+Investing-p-00421496)

### Inferences
- Hurdle as a function of search size (my computation from the cited DSR formula and from Bonferroni at 5%
  two-sided; trials assumed independent, so use effective N from clustering; the Sharpe columns assume the null
  standard error of an annualised Sharpe is 1/sqrt(years)):

  | N trials | E[max z] under null | null max annual Sharpe, 3-year sample | same, 10-year sample | Bonferroni t |
  |---|---|---|---|---|
  | 10 | 1.58 | 0.91 | 0.50 | 2.81 |
  | 100 | 2.53 | 1.46 | 0.80 | 3.48 |
  | 1,000 | 3.26 | 1.88 | 1.03 | 4.06 |
  | 10,000 | 3.86 | 2.23 | 1.22 | 4.57 |
  | 100,000 | 4.39 | 2.54 | 1.39 | 5.03 |

  On the platform's 3-year TRAIN panel, a mined alpha that is the best of 1,000 effective trials needs a standalone
  annual Sharpe well above 1.9 just to beat the expected maximum of pure noise. This is the strongest argument for
  granting the longer (2010-2022) panel to mining before anything else: a 10-year sample nearly halves every hurdle.
- Three-class admission hurdle, keyed to the registry's origin class:
  1. Literature prior with a canonical definition: sign veto plus t >= 2 on the spanning statistic; count one trial.
  2. Human idea with a bounded parameter grid: count every grid cell; Bonferroni or BY-FDR within the family.
  3. Machine mined: N is the effective number of candidates evaluated at any fidelity in that search campaign; require
     the spanning t to exceed the Bonferroni value for that N (3.5 to 4.6 for 10^2 to 10^4), or BY-FDR <= 10% across the
     campaign. CGS's 3.4 is the floor, not the target, once N exceeds about 100.
- Apply the hurdle to the marginal statistic (residual IC t or spanning-alpha t), because that is the quantity selected
  on at admission.
- Shrink expected performance regardless of significance: plan on about 50% of in-sample return for a signal selected
  at |t| > 2 (CLZ; McLean-Pontiff 58% after publication), and more for value-weighted or liquid-universe
  implementation (about 60% decay in CLZ value-weighted sorts). For capacity and sizing use the shrunk figure.
- Base rates differ by data family. Chen's one-in-five rate is for accounting ratios in all stocks over long samples;
  Chen-Welch shows the same published signals earn almost nothing in the top-3,000 universe after 2005. For a
  5,600-name US universe, price-volume mined signals in liquid names should be assumed to have a low prior p0, which
  by Harvey-Liu's calibration logic pushes the hurdle up, not down.
- Accounting for the search inside the DSR: (a) N = ONC cluster count over all candidate PnL series evaluated in the
  campaign, including those rejected at tier 1 and at every halving rung; (b) V = variance of Sharpe across cluster
  representatives, which is large for a wide search and raises the expected maximum; (c) campaigns accumulate: the
  ledger's N for a family never resets; (d) report the DSR with search-N next to the DSR with construction-N.
- Trial budget. Fix the campaign budget before it starts (for example 500 effective trials per quarter) and write it
  into the prereg. With the table above the team can see the price of each extra order of magnitude: about +0.6 in
  required t and about +0.35 in required 3-year annual Sharpe.
- Holdout management. Keep three tiers: TRAIN (search, unlimited but counted), VALIDATION (one look per finalist,
  answered as pass/fail through a Thresholdout-style noisy threshold, with a fixed query budget written in the
  ledger), and the sealed 2025+ holdout (never queried by the search loop). Never feed validation outcomes back to an
  LLM prompt or a generator's memory.
- Pipeline self-test (proposal): run the full mining loop on (i) the real fields with returns permuted across dates,
  and (ii) real data with one planted synthetic signal of known IC. The admission rules should admit about zero
  alphas in (i) and recover the planted signal in (ii). The admitted count in (i) is a direct estimate of the
  pipeline's false-discovery rate at the chosen hurdle.

### Gaps
- No published hurdle calibrated for machine-mined formulaic alphas on daily US data; CGS and Yan-Zheng use monthly
  portfolio sorts. The Bonferroni/DSR table is a conservative stand-in.
- The disagreement between Harvey-Liu/CGS (FDR above 45%) and Chen/JKP (FDR below 25%) is unresolved in the
  literature; both sides are peer-reviewed. The notes report both.
- The exact RAS formula and its constants were not verified from a primary source.
- Chen-Welch 2026 and CLZ (v7) are working papers, not yet peer-reviewed.

## 7. Practical guidance for a small team (what to automate first, what to keep manual, loop under a few minutes)

### Takeaway
Automate evaluation, accounting and reporting before automating idea generation: the evidence says generation is cheap
and unreliable while evaluation discipline is what separates real from false discoveries. A loop of a few minutes per
idea is reachable on one workstation by removing repeated work, not by adding compute.

### Cited Findings
- Generation is cheap: a full LLM-driven search costs under $10 (RD-Agent(Q)) or $7.5-$74.4 (LLM + MCTS); LLMs produce
  valid formulas reliably. **[PR] (text)**
  — [RD-Agent(Q)](https://arxiv.org/abs/2505.15155); [Shi, Duan and Li 2026](https://arxiv.org/abs/2505.11122); [AlphaBench](https://www.cs.cityu.edu.hk/~cliu644/HomePage/doc/AlphaBench/AlphaBench_PDF.pdf)
- Judging is where models fail: zero-shot factor ranking is close to random. **[PR] (text)**
  — [AlphaBench](https://www.cs.cityu.edu.hk/~cliu644/HomePage/doc/AlphaBench/AlphaBench_PDF.pdf)
- "Human oversight should define the scientific and risk-management boundaries within which the search is conducted"
  (which mechanisms are plausible, which exposures are unacceptable, how significance is assessed, what evidence is
  required before production); agents then search inside those boundaries. **[PP] (text)**
  — [Yu et al. 2026](https://arxiv.org/abs/2608.01789)
- Separating proposer from verifier, with the verifier re-executing evidence in a fresh context and holding veto
  authority, is the central design choice of AgonAlpha. **[PP] (text)**
  — [AgonAlpha](https://arxiv.org/abs/2608.11250)
- Caching is the largest measured lever in a public system: 147.0 s to 7.4 s (about 20x) on one CPU in Qlib's
  benchmark. **[PP] (text)**
  — [Qlib paper](https://arxiv.org/abs/2009.11189)
- About 20 backtest iterations are enough to find a false strategy at the 5% level. **[PR] (text)**
  — [Lopez de Prado 2018](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3104816)

### Inferences
Order of work (each item names the section it rests on):
1. One spec in, one report out. Finish the driver so that adding an alpha is a delta in a JSON spec and the output is
   the report card plus a ledger line. No generator script per library version. (Sections 1 and 3.)
2. Node-level content-addressed cache with canonicalisation and cross-alpha CSE; hash each payload once; Release
   build with an identity canary. Target: a DSL-only idea costs one or two new node evaluations. (Section 1.)
3. Tier-1 vectorised screen with linear costs, validated once against the exact replay. (Section 2.)
4. Marginal test against the book from cached series: residual IC, spanning t, appraisal ratio, max PnL correlation.
   (Section 4.)
5. Trial accounting by origin class and campaign, with the hurdle looked up from effective N. (Section 6.)
6. Date-blocked evaluation so the 2010-2022 panel fits in 1.5 GiB. It roughly halves every hurdle in the section 6
   table, which is worth more than any search algorithm. (Sections 1 and 6.)
7. Only then a constrained-grammar miner with successive halving; LLM as proposer of hypotheses and templates, never
   as judge. (Section 5.)

Keep manual: the hypothesis and its economic rationale; the choice of fields and operator templates opened to search;
the campaign trial budget; admission of any machine-mined alpha to the book; every holdout query; retirement
decisions.

Loop-time budget for one DSL-only idea on the current hardware, after items 1-4 (targets, not measurements): spec
validation and cache lookup under 5 s; new node evaluation 1-5 s; IC and decay curve about 5 s; tier-1 screen about
2 s; marginal test about 2 s; report card about 5 s. Total well under one minute, with the exact replay (about 30 s
per cell today) run only for finalists. A batch of 48 candidates fits the 180 s bound if shared nodes are computed
once and candidates are evaluated by date block on several cores within the memory limit.

Process rules that cost nothing: count every evaluation; never reuse validation results as search feedback; record
rejected candidates; fix the campaign budget in advance; run the permuted-returns self-test whenever the admission
rules or the generator change.

### Gaps
- The loop-time budget is an estimate built from the v7 review's measured stage timers and the published caching
  results; it has not been measured on this platform.
- No public case study of a small team (fewer than five people) running a formulaic-alpha factory with disclosed
  trial counts and live results was found.
- Whether LLM-proposed hypotheses have a higher prior of being true than grammar-sampled formulas is untested in the
  literature I could reach; AlphaAgent's hit-ratio gain (0.29 versus 0.16) is an in-sample measure by its authors.
