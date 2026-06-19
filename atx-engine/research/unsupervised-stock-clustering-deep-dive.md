# Unsupervised Clustering of Equities from Market Data (a Data-Driven Alternative to GICS)

**Purpose**: Ground the design of an `atx-engine` clustering module that partitions a panel of US equities into *data-driven cohorts* — "covariance-based sectors" — built from rolling histories of returns, correlations, and (aspirationally) implied volatility and volume, rather than from static industry/sector labels (GICS). Documents how systematic quant shops (Winton/CFM, Oxford-Man researchers) actually do this, with the central object (the empirical correlation matrix), the denoising step (Random Matrix Theory), the clustering algorithms (hierarchical, spectral, signed-graph), validation/stability, the downstream applications (statistical arbitrage, portfolio risk), replicable pseudocode, data structures, parameter tables, and references.

**Legend for claim confidence**
- ✅ **VERIFIED** — confirmed against a primary or strong secondary source via the deep-research pass (3-vote adversarial verification, 3-0 unless noted; URL inline).
- ⚠️ **UNCERTAIN / CONTESTED / UNDER-EVIDENCED** — partial evidence, single-study self-assessment, refuted under verification, or not covered by surviving claims (inference, flagged as such).
- 📘 **STANDARD DOMAIN KNOWLEDGE** — well-established ML/quant/numerical fact; not separately cited by the research pass but standard practice.

> **Method note**: This doc is the synthesis of a 5-angle deep-research pass (22 sources fetched, 107 claims extracted, 25 adversarially verified, 20 confirmed, 5 killed). **Two scoping truths you must keep in mind:**
> 1. **The verified evidence is overwhelmingly about *return* correlations.** No surviving claim documents implied-volatility or raw-volume *magnitude* features being used as production clustering inputs (one study uses volume-adjacent *co-trading* networks). The IV/volume parts of the original question are therefore marked 📘/⚠️ as extension, not established practice.
> 2. **Feasibility ≠ profitability.** That clustering is a workable, dynamic GICS alternative is *uncontested and strongly sourced*. Claims that it *beats* GICS on returns (">10% annualized, Sharpe > 1") were **refuted** under adversarial verification (§9). Treat any such figure as an un-replicated single-study backtest.

---

## 1. The core idea: let the price data draw the sectors

✅ Systematic quant firms build **"covariance-based sectors"** by applying statistical clustering directly to the covariance/correlation matrix of returns, as a deliberate, dynamic, data-driven alternative to GICS. Winton (a major systematic manager) states it verbatim: *"we apply statistical clustering methods to the covariance matrix of returns… Pairs with a large positive correlation can be thought of as lying close to one another, while those with a large negative correlation are far apart. We then have a measure of distance between any pair of stocks."* Winton frames the output as responding *"to market data more dynamically and systematically than GICS."* (https://www.winton.com/research/systematic-methods-for-classifying-equities)

✅ The motivation is general, not firm-specific. The *Journal of Portfolio Management* tutorial on peer grouping (Mehta, Thompson, Lee & Lee, 2025) independently states that *"traditional heuristics such as industry codes, static style boxes, or return correlations offer only coarse and rigid notions of peer groups,"* and that metric learning, graph methods, and LLMs *"make it possible to build adaptive neighborhoods… that align more closely with actual risk, liquidity, and thematic exposures."* (https://www.pm-research.com/content/iijpormgmt/52/2/150)

**Why this beats GICS as an *input* (📘 + ✅ design rationale):**
- **Dynamic** — clusters re-estimate on a rolling window and track regime shifts; GICS is revised infrequently and lags. ✅ Co-trading-network clusters *"capture the time evolution of the dependency among stocks"* beyond *"static GICS sectors."* (https://arxiv.org/pdf/2302.09382)
- **Objective & unlabelled** — ✅ *"Since the clusters computed this way use only price data, they are objective, unlabelled and can be updated frequently. Subsequent interpretation is not essential to the method."* (Winton) The labels (`Cluster 7`) carry no human-assigned meaning; you attach interpretation *after*, if at all.
- **Captures idiosyncratic co-movement** GICS misses — two names in different GICS sectors that consistently trade together land in the same data cluster.

> **Framing caution (⚠️)**: "objective" is Winton's *design-level* framing (it follows mechanically from using only price data); it is **not** a claim that the clusters are uniquely correct. The number of clusters is explicitly *arbitrary* (§5.1), and the membership depends on every preprocessing choice (window, residualization, denoising, linkage).

---

## 2. Feature engineering from rolling windows

The clustering input is a per-stock feature representation derived from a **rolling lookback window**. The verified production pattern is correlation-of-returns; the multi-feature vector (returns + IV + volume) is a documented *aspiration* of the broader literature but **under-evidenced** as standing practice.

### 2.1 Returns — and the case for *residual* returns (✅)

✅ The strongest-attested practice does **not** cluster raw returns. It clusters the correlation matrix of **market-residual (CAPM-residual) returns** over a rolling window (the replication uses the **last 60 days**), so clusters form on *idiosyncratic* co-movement rather than on the common market factor that otherwise dominates and would collapse everything into one blob:

```
residual return:   ε_i,t = r_i,t − β_i · r_mkt,t
   where β_i estimated by rolling OLS of r_i on r_mkt over the same window
```

Cartea, Cucuringu & Jin, *"Correlation Matrix Clustering for Statistical Arbitrage Portfolios"* (Oxford-Man, ICAIF '23): *"we use various clustering methods to partition the correlation matrix of market residual returns of stocks into clusters."* The resulting portfolios are *"neutral to the market and cannot be fully explained by intra-industry mean-reversion effects."* (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4560455 ; replication https://arxiv.org/html/2406.10695v1 ; ACM https://dl.acm.org/doi/fullHtml/10.1145/3604237.3626894)

📘 The same logic generalizes: strip *any* known common structure first. Residualize against the market alone (CAPM), or against a fuller factor model (Fama-French, or your Barra/Axioma exposures) before correlating, so the cluster step explains the *residual* covariance — exactly the structure GICS/style factors don't already capture.

### 2.2 Implied volatility, volume, and multi-feature vectors (⚠️ under-evidenced — extension, not confirmed practice)

⚠️ **The research pass found no surviving primary claim** that implied-volatility level/skew or raw-volume magnitude are used as production clustering *features*. The original question's IV/volume dimension is therefore an **extension** you would be inventing, not copying. Two honest routes:

- 📘 **Feature-vector route**: build a per-stock vector `x_i = [ rolling-return-moments | IV-level | IV-skew | IV-term-slope | log-dollar-volume | turnover | realized-vol ]`, z-score each feature cross-sectionally, then cluster in Euclidean space (k-means/GMM/hierarchical on the vector). This is standard ML, but you are clustering *characteristics*, which is closer to a style/quintile bucketing than to the *co-movement* clustering the literature validates. Different object — name it as such.
- ✅ **Co-trading route (the one volume-adjacent thing that IS verified)**: Lu, Reinert & Cucuringu (Oxford / Alan Turing Inst., companion in *Quantitative Finance* 2024) build **dynamic networks from a co-trading similarity** (edges from time-proximity of concurrent cross-stock trades, derived from high-frequency LOB data, US equities 2017–2019) and run spectral clustering: *"By applying spectral clustering on co-trading networks, we uncover economically meaningful clusters of stocks."* (https://arxiv.org/pdf/2302.09382) This is the closest verified analogue to a "volume/flow-based" clustering — but it's HFT order-flow proximity, not daily share volume, and the "economically meaningful" tag is the authors' self-assessment on a 2017–2019 sample.

> **Recommendation (⚠️ inference):** Treat IV/volume as a *second, separate* clustering you can compare against and blend with the correlation clustering — not as extra columns bolted onto the return-correlation distance. Mixing a co-movement metric (correlation) with characteristic features (IV level) in one Euclidean distance has no validated precedent in the sources and silently weights one against the other.

### 2.3 The window (📘 + ✅ data point)

| Lookback | Used by | Note |
|---|---|---|
| 60 trading days | Cartea/Cucuringu/Jin residual-return stat-arb (✅) | short, reactive; pairs the rolling β estimate |
| ~250 days (1 year) | Winton S&P 500 daily-return clustering (✅) | the basis for their "12 groups" result |
| 2017–2019 HFT | Lu et al. co-trading (✅) | high-frequency, intraday network |

📘 The window is the central bias/variance knob: short → reactive but noisy and unstable cluster membership; long → stable but laggy and exposed to non-stationarity (corporate actions, regime shifts, universe churn). This directly interacts with the `N`-vs-`T` denoising problem (§4): a 60-day window on a 500-name universe is deep in the high-dimensional regime (`q = N/T ≈ 8`), so the raw correlation matrix is *mostly noise* and denoising is not optional.

---

## 3. The correlation matrix as the central object; correlation → distance

✅ Correlation is used **directly as a similarity** (`+1` = close, `−1` = far). To turn it into a proper metric, the standard transform (📘, Mantegna 1999) is:

```
d_ij = sqrt( 2 · (1 − ρ_ij) )        # Euclidean distance on the unit hypersphere
   ρ_ij = +1  → d = 0   (identical)
   ρ_ij =  0  → d = √2
   ρ_ij = −1  → d = 2   (maximally far)
```

✅ This distance feeds correlation-based **hierarchical** clustering of financial time series — a well-established line (Mantegna; "Correlation Based Hierarchical Clustering in Financial Time Series"). (https://www.researchgate.net/publication/239831442_Correlation_Based_Hierarchical_Clustering_in_Financial_Time_Series)

📘 **Two regimes, two representations** — this fork drives the algorithm choice in §5–§6:
- **Unsigned / distance regime**: collapse sign via `d_ij = √(2(1−ρ))`. Negative correlation → large distance. Compatible with *any* metric clusterer (hierarchical, k-means on an embedding, DBSCAN). This is the Winton/Mantegna route.
- **Signed-graph regime**: keep the sign. Use `ρ` (or residual-return `ρ`) **as the adjacency matrix** of a signed, weighted graph. Negative edges carry information (anti-correlated = should be *separated*), which the distance transform throws away. ✅ *"classical clustering is inapplicable"* here because of negative entries — you need signed-graph methods (§6).

---

## 4. Denoising the matrix (do this *before* clustering)

✅ **Random Matrix Theory (RMT) is the foundational toolset for cleaning large empirical correlation/covariance matrices**, needed because sample matrices are heavily noise-corrupted when `N` (assets) is comparable to `T` (observations) — the high-dimensional regime `q = N/T`. Bun, Bouchaud & Potters, *"Cleaning large correlation matrices: tools from random matrix theory"* (Physics Reports 666:1–109, 2017 — canonical review by CFM practitioners): *"This review covers recent results concerning the estimation of large covariance matrices using tools from Random Matrix Theory."* (https://arxiv.org/abs/1610.08104) Empirically, Laloux et al. found **~94% of the S&P 500 correlation eigenvalue spectrum is indistinguishable from a random matrix** — i.e., most of the raw matrix is noise.

### 4.1 Marchenko–Pastur: where signal ends and noise begins (✅)

✅ The **Marchenko–Pastur (MP) law** characterizes the eigenvalue spectrum of a pure-noise correlation matrix. Eigenvalues *inside* the MP bulk are noise; *outliers above* `λ₊` are signal. (https://arxiv.org/abs/1610.08104)

```
λ₊ = σ² · (1 + sqrt(N/T))²          # upper edge of the noise bulk
λ₋ = σ² · (1 − sqrt(N/T))²          # lower edge
```

✅ **Practical fit (de Prado, *Machine Learning for Asset Managers*, Ch.1, 2020):** *"One approach fits a Marchenko–Pastur PDF to the covariance matrix with the aim of finding the variance that minimizes the sum of the squared differences between the analytical PDF and the kernel density estimate of the observed eigenvalues."* This `σ²` fixes the `λ₊` cutoff. Verified against the reference implementation (`errPDFs` computes SSE, `findMaxEval` minimizes it for `σ²`, `eMax = λ₊`). (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3558728)

> ⚠️ **Two adjacent de Prado claims were REFUTED** (§9): the bare framing that *all* eigenvalues in `[0, λ₊]` are simply discarded as noise (1-2), and the specific enumeration of "Constant Residual Eigenvalue + Targeted Shrinkage" as Ch.1's two named techniques (0-3). Use the MP-fit cutoff as verified above; do not over-state the exact downstream cleaning recipe without re-reading the primary.

### 4.2 Beyond clipping — Rotationally Invariant Estimators & shrinkage (✅)

✅ RMT also yields **consistent Rotationally Invariant Estimators (RIE)** that clean a matrix with *no prior on its structure* by keeping the empirical eigenvectors fixed and **nonlinearly shrinking only the eigenvalues**:

```
Ξ = Σ_i ξ_i · u_i u_iᵀ      # keep sample eigenvectors {u_i}; replace eigenvalues λ_i → ξ_i
```

Bun-Bouchaud-Potters: build *"consistent Rotationally Invariant estimators (RIE) for large correlation matrices when there is no prior on the structure."* Corroborated by Ledoit-Wolf nonlinear shrinkage (*"an estimator is rotation-equivariant iff it has the same eigenvectors as the sample covariance matrix"*). **Open-source tooling exists**: `pyRMT` implements the BBP optimal-shrinkage RIE. (https://arxiv.org/abs/1610.08104 ; https://github.com/GGiecold/pyRMT)

### 4.3 Why denoising matters downstream (✅ for risk; ⚠️ for clustering)

✅ **For risk**: RMT cleaning *"result[s] in much improved out-of-sample risk of Markowitz optimal portfolios"* versus optimizing on the raw matrix — denoising cures the optimizer's over-allocation to noise-dominated low-variance eigenmodes (`pyRMT`; Laloux et al.; BBP). (https://github.com/GGiecold/pyRMT ; https://arxiv.org/abs/1610.08104)
- ⚠️ Genuine caveats: the best methods generally require `T > N`; and Bongiorno & Challet (arXiv:2112.07521) argue nonlinear shrinkage is *not the absolute optimum* under non-stationarity/heavy tails — though they concede cleaning still beats the raw matrix.

⚠️ **For clustering specifically**: whether denoising *before* clustering measurably changes cluster membership/stability is flagged as an **open question** — intuitively it should (cleaner correlations → more stable partitions), but no surviving claim quantifies it. Treat "denoise, then cluster" as well-motivated best practice, not a measured result.

---

## 5. Clustering algorithms

What the verified evidence actually attests: **hierarchical** (Winton, Mantegna), **spectral** (co-trading + signed-graph), and **signed-graph community detection** (SPONGE / Signed Laplacian). k-means / DBSCAN / affinity propagation are standard ML options but were **not** surfaced as confirmed practitioner methods — included below as 📘 with their fit/caveats.

### 5.1 Hierarchical clustering (✅ — the most-attested method)

✅ The clustering Winton uses is **hierarchical**: recursively nest clusters into a **dendrogram**; cut at a chosen height to get the partition. *"Clusters can themselves be grouped together into larger clusters, forming a hierarchy of nested groups."* The number of clusters is an **arbitrary granularity choice**: *"The number of sectors is arbitrary… we are free to choose the precise level of granularity."* For **S&P 500 daily returns over a 1-year rolling window, Winton found 12 groups** produced distinct sectors of comparable size, with higher-level "super sectors" visible up the dendrogram. (https://www.winton.com/research/systematic-methods-for-classifying-equities)

📘 Mechanics: agglomerative linkage on the `d_ij = √(2(1−ρ))` distance.
- **Linkage choice matters**: *single* linkage → the Mantegna Minimum Spanning Tree (chaining, sensitive to noise); *average/Ward* → more compact, comparable-size clusters (matches Winton's "comparable size" goal).
- **Strength**: no pre-commitment to `k`; the whole hierarchy is one object you can cut at multiple resolutions. **Best fit** when you want nested super-sectors/sub-sectors.

### 5.2 Spectral clustering (✅)

✅ Used on **co-trading networks** (Lu et al.) and as one of the five signed-graph methods (§6). Embed the graph via the eigenvectors of its Laplacian, then k-means in that low-dim eigenspace. Handles non-convex cluster shapes that k-means-on-raw-features cannot. (https://arxiv.org/pdf/2302.09382 ; https://arxiv.org/html/2406.10695v1)

### 5.3 k-means / GMM (📘 — standard, but ill-posed on raw signed correlation)

📘 k-means partitions a *feature space* by Euclidean distance. It is fine on a **denoised PCA/spectral embedding** or on a z-scored characteristic vector (§2.2), but it is **ill-posed directly on a signed correlation matrix** (negative correlations have no Euclidean-distance meaning, and `k` must be fixed up front). No surviving claim attests k-means as a *primary* practitioner method here; it appears only *inside* spectral/SPONGE as the final-step partitioner on the eigen-embedding (k-means++).

### 5.4 DBSCAN / density-based (📘 — niche fit)

📘 DBSCAN finds arbitrarily-shaped dense regions and labels sparse points as noise/outliers — attractive because it does **not** require `k` and can leave idiosyncratic names unclustered. But it needs a meaningful density (`ε`, `minPts`) in the distance space, which is hard to set on `√(2(1−ρ))` distances where everything is `O(√2)` apart. **Not surfaced** in the verified evidence; consider only as an outlier-detection pre-pass.

### 5.5 Affinity propagation (📘 — niche fit)

📘 Affinity propagation picks exemplars and auto-selects cluster count from a "preference" parameter — conceptually appealing (no `k`), but `O(N²)` memory/time per iteration is rough at `N` = 3k–5k, and it was **not** found in the verified practitioner evidence. Lower priority.

---

## 6. The signed-graph route (✅ — keeps the negative correlations)

✅ Represent the universe as an **undirected, signed, weighted graph** with the (residual-return) correlation matrix as the **adjacency matrix**, then partition with graph/community-detection algorithms. Cartea/Cucuringu/Jin and the replication evaluate **five algorithms** — 1 spectral + **2 Signed Laplacian variants** + **2 SPONGE variants** — and report **SPONGEsym (symmetric) performed best** in their backtest. (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4560455 ; https://arxiv.org/html/2406.10695v1 ; https://dl.acm.org/doi/fullHtml/10.1145/3604237.3626894)

✅ **SPONGE** (Signed Positive Over Negative Generalized Eigenproblem; Cucuringu et al. 2019, AISTATS): *"decomposes the adjacency matrix A into A⁺ and A⁻… followed by Laplacian matrix computation and k-means++ clustering in eigenspace."* (https://arxiv.org/pdf/2406.10695)

```
SPONGE (sketch):
  A⁺ = max(A, 0)          # positive-correlation edges
  A⁻ = max(−A, 0)         # negative-correlation edges (kept, not discarded)
  L⁺, L⁻ = regularized Laplacians of A⁺, A⁻
  solve generalized eigenproblem  (L⁺ + τ⁻ D⁻) v = λ (L⁻ + τ⁺ D⁺) v
  k-means++ on the bottom-k generalized eigenvectors → cluster labels
```

> ⚠️ **Two caveats on this block**: (1) *"SPONGEsym performed best"* and the **12.2% annualized / Sharpe 1.1** figure are the **authors' own un-replicated backtest** (it beat Fama-French sector clusters and buy-and-hold *in that study*). (2) The companion claim that mean-reverting stat-arb *within* clusters yields ">10% return, Sharpe > 1 statistically significant" was **REFUTED 0-3** (§9). Use the *method*; do not import the *numbers*.

📘 Why signed-graph over distance-collapse: anti-correlated names (`ρ < 0`) are *information* — they should be pushed into different clusters and are natural hedges. The `√(2(1−ρ))` distance just makes them "far," losing the explicit negative-edge structure that SPONGE/Signed-Laplacian exploit.

---

## 7. Downstream applications

### 7.1 Statistical arbitrage (✅ method; ⚠️ profitability)

✅ The flagship application: cluster the residual-return correlation matrix, then run **mean-reversion / pairs stat-arb *within* each discovered cluster** instead of within GICS industries. Portfolios come out **market-neutral** and **not explained by intra-industry mean-reversion**, i.e., the clustering adds something GICS grouping does not. (https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4560455)

⚠️ **Profitability is the contested layer.** The headline ">10% annualized, Sharpe > 1" claims were **refuted** (§9). Broader-literature stat-arb-from-clustering returns are often **modest (~36–41 bps/month, Sharpe ~0.2–0.3)**. Build the pipeline expecting a *grouping improvement*, not a *guaranteed alpha*.

### 7.2 Portfolio risk & Hierarchical Risk Parity (✅)

✅ **Risk modeling is the more robust payoff.** RMT-denoised correlation matrices **improve out-of-sample Markowitz risk** (§4.3). And clustering is the engine of **Hierarchical Risk Parity (HRP)** (de Prado 2016): cluster assets hierarchically, then allocate by recursive bisection down the dendrogram — sidestepping the unstable matrix inversion that plagues Markowitz, and explicitly using the cluster tree as the allocation structure. (https://en.wikipedia.org/wiki/Hierarchical_Risk_Parity ; https://arxiv.org/abs/1703.00485)

---

## 8. Validation & stability of clusters (⚠️ partially under-evidenced)

⚠️ **No surviving claim pins down a single standard validation metric**, and the appealing idea that **RMT/MP fixes the optimal cluster count was REFUTED 0-3** (§9) — you cannot read `k` off the eigenvalue spectrum or a "90%-variance eigenvectors" rule. `k` selection is genuinely open. Use 📘-standard tooling:

- 📘 **k selection**: silhouette score, gap statistic, eigengap heuristic (spectral). For hierarchical, inspect the dendrogram gap (Winton chose 12 by "distinct, comparable-size" eyeballing — an explicitly *arbitrary* call).
- 📘 **vs-GICS agreement**: Adjusted Rand Index / Normalized Mutual Information between your clusters and GICS — measures *how much* the data clustering departs from sectors (the whole point is that it should, somewhat).
- ✅ **Temporal / bootstrap stability**: re-estimate on rolling/bootstrapped windows and measure label churn. `clusterboot` (R `fpc`) is a reference implementation of bootstrap cluster stability (Jaccard recovery per cluster). (https://search.r-project.org/CRAN/refmans/fpc/html/clusterboot.html) **This is the metric that matters most in production** — a cluster you re-trade on must persist across re-estimations, or your "sector" is noise.

---

## 9. Honest performance caveats — what got REFUTED

⚠️ Five claims were killed under 3-vote adversarial verification. Carry these explicitly so the module isn't built on a myth:

| Refuted claim | Vote | Source |
|---|---|---|
| Clustering residual returns → ">10% annualized, Sharpe > 1, statistically significant" stat-arb | 1-2 ✗ | ACM 3604237.3626894 |
| Mean-reversion *within* clusters → ">10% annualized, Sharpe > 1 significant" | 0-3 ✗ | SSRN 4560455 |
| Optimal cluster count determinable from MP asymptotics / 90%-variance eigenvectors | 0-3 ✗ | arXiv 2406.10695 |
| de Prado Ch.1 treats *all* eigenvalues in `[0, λ₊]` as noise to remove (bare framing) | 1-2 ✗ | SSRN 3558728 |
| de Prado Ch.1 names "Constant Residual Eigenvalue + Targeted Shrinkage" as its two techniques | 0-3 ✗ | SSRN 3558728 |

⚠️ **LLM-based peer grouping** (the newest area) is the most volatile: shows hallucination/instability, and in at least one cited study LLM embeddings beat GICS but **lost to plain price-based clustering**; dynamic LLM clustering underperformed the full universe (~34–37% lower returns/Sharpe). Don't reach for LLM "neighborhoods" over the correlation method.

---

## 10. Proposed `atx-engine` clustering module (design sketch)

📘 Synthesis of the verified pipeline into an implementable shape. Pipeline order is the load-bearing decision: **residualize → correlate → denoise → cluster → validate-stability**.

```
            rolling window of daily returns  (N assets × T days, e.g. N≈3–5k, T∈{60,250})
                       │
   [1] residualize     ▼   ε = r − β·r_mkt   (rolling OLS β; optionally full factor model)
                       │
   [2] correlate       ▼   C = corr(ε)        (N×N empirical correlation)
                       │
   [3] denoise (RMT)   ▼   fit MP PDF → σ², λ₊ ; clip/RIE-shrink eigenvalues → C̃
                       │
   [4a] distance route ├──► D = √(2(1−C̃)) ─► hierarchical (Ward/avg) ─► cut dendrogram → labels
   [4b] signed route   └──► A = C̃ (signed) ─► SPONGEsym / Signed-Laplacian ─► k-means++ → labels
                       │
   [5] validate        ▼   silhouette/eigengap (pick k) ; clusterboot Jaccard (stability) ; ARI vs GICS
                       │
                       ▼   cluster_id per asset  →  feeds stat-arb grouping / HRP allocation / risk
```

**Data structures (📘):**

```
struct ClusterConfig {
    int     window_days;        // 60 (reactive) … 250 (stable)
    enum    Residualize { NONE, CAPM, FACTOR_MODEL };   // default CAPM
    enum    Denoise     { NONE, MP_CLIP, RIE_SHRINK };  // default MP_CLIP; RIE if T>N
    enum    Method      { HIER_WARD, HIER_AVG, SPECTRAL, SPONGE_SYM, SIGNED_LAPLACIAN };
    int     k;                  // arbitrary granularity; e.g. 12 for S&P 500 (Winton); else silhouette/eigengap
    double  bootstrap_jaccard_min;  // reject unstable clusters below this (e.g. 0.6)
};

struct ClusterResult {
    vector<uint16_t> cluster_id;       // N entries, label per asset (unlabelled / non-semantic)
    Matrix<float>    denoised_corr;    // C̃, reuse for HRP / risk
    // dendrogram (hierarchical) or eigen-embedding (spectral/SPONGE) retained for re-cut / diagnostics
    DendrogramOrEmbedding structure;
    float            silhouette;       // k-selection diagnostic
    vector<float>    cluster_jaccard;  // per-cluster bootstrap stability (clusterboot-style)
    float            ari_vs_gics;      // departure from sector labels
};
```

**Parameter defaults (✅ where cited, else 📘):**

| Param | Default | Basis |
|---|---|---|
| `window_days` | 60 (stat-arb) / 250 (sectoring) | ✅ Cartea et al. 60d ; ✅ Winton 1yr |
| residualize | CAPM residuals | ✅ Cartea/Cucuringu/Jin |
| denoise | MP-fit clip; RIE if `T>N` | ✅ de Prado MP-fit ; ✅ BBP/`pyRMT` RIE |
| method (sectoring) | hierarchical Ward/avg | ✅ Winton/Mantegna |
| method (signed/stat-arb) | SPONGEsym | ✅ Cartea et al. (best in *their* backtest) |
| `k` | 12 for S&P 500; else silhouette/eigengap | ✅ Winton (arbitrary) ; ⚠️ NOT from RMT |
| stability gate | `clusterboot` Jaccard ≥ ~0.6 | ✅ method ; 📘 threshold |

> ⚠️ **Build expectations**: ship this as a *better grouping primitive* (dynamic, idiosyncratic, denoised) feeding stat-arb selection / HRP / risk — **not** as a standalone alpha. The profitability deltas over GICS are unproven (§9). Carry the IV/volume features (§2.2) as a *separate, comparable* clustering, not as extra columns in the correlation distance.

---

## 11. Open questions (carried from the research pass)

1. **IV/volume features**: how do shops engineer clustering features from implied vol and volume (not just returns), and how are multi-feature vectors combined into one distance? *No surviving claim addresses this.*
2. **Validation/k-selection in production**: which stability/agreement metrics are standard (silhouette/gap for `k`, ARI vs GICS, bootstrap temporal stability)? The refuted RMT-`k` claim leaves this open.
3. **k-means / DBSCAN / affinity propagation placement**: where (if anywhere) do partition/density/exemplar methods beat hierarchical + signed-graph, especially in the signed regime where k-means is ill-posed?
4. **Denoise-then-cluster interaction**: does RMT cleaning *before* clustering measurably change membership/stability, and what's best practice in the realistic `N≈T` / `N>T` regime?

---

## References (verified sources)

**Practitioner / primary**
- Winton — *Systematic Methods for Classifying Equities* ("covariance-based sectors"). https://www.winton.com/research/systematic-methods-for-classifying-equities
- Cartea, Cucuringu & Jin — *Correlation Matrix Clustering for Statistical Arbitrage Portfolios* (Oxford-Man, ICAIF '23). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4560455 · https://dl.acm.org/doi/fullHtml/10.1145/3604237.3626894
- Korniejczuk & Slepaczuk — replication of the above (signed-graph, SPONGE). https://arxiv.org/html/2406.10695v1 · https://arxiv.org/pdf/2406.10695
- Lu, Reinert & Cucuringu — *Co-trading networks + spectral clustering* (Oxford/Turing; Quantitative Finance 2024). https://arxiv.org/pdf/2302.09382
- Mehta, Thompson, Lee & Lee — JPM tutorial on adaptive peer grouping (2025). https://www.pm-research.com/content/iijpormgmt/52/2/150

**Denoising / RMT**
- Bun, Bouchaud & Potters — *Cleaning large correlation matrices: tools from RMT* (Physics Reports 666, 2017). https://arxiv.org/abs/1610.08104
- López de Prado — *Machine Learning for Asset Managers*, Ch.1 (MP-fit denoising). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3558728
- `pyRMT` — optimal-shrinkage RIE reference implementation. https://github.com/GGiecold/pyRMT

**Clustering / risk / validation**
- Mantegna — *Correlation Based Hierarchical Clustering in Financial Time Series*. https://www.researchgate.net/publication/239831442_Correlation_Based_Hierarchical_Clustering_in_Financial_Time_Series
- López de Prado — *Hierarchical Risk Parity* (2016). https://arxiv.org/abs/1703.00485 · https://en.wikipedia.org/wiki/Hierarchical_Risk_Parity
- `fpc::clusterboot` — bootstrap cluster-stability reference. https://search.r-project.org/CRAN/refmans/fpc/html/clusterboot.html

---
*Generated from a deep-research pass: 5 angles · 22 sources fetched · 107 claims extracted · 25 adversarially verified (3-vote) · 20 confirmed / 5 refuted · 9 synthesized findings. Confidence tags reflect that pass; performance/profitability claims are explicitly contested (§9).*
