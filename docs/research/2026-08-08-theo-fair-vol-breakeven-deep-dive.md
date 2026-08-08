# Theo Vol, Fair Vol, and Breakeven Vol: How Options Firms Manufacture Edge — and a Blueprint for Harvesting It

*Research deep-dive, 2026-08-08. Scope: American equity options (single names + index ETFs). Sources: academic literature, practitioner writing (Moontower/Abdelmessih, Sinclair, Natenberg, Derman, Sepp), vendor documentation (Vola Dynamics, ORATS, Orc), and firm-public material (SIG, Optiver, Citadel Securities, Jane Street, Akuna). Full reference list at the end.*

---

## 1. Executive summary

- **"Theo" / "theo vol" / "fair vol" / "fair value" are the same object seen from different desks**: the firm's own private estimate of what an option is worth right now. Market makers quote around it (`bid = theo − edge`, `ask = theo + edge`); vol-arb desks hold positions against it. Edge, per trade, is defined as traded price minus theo.
- **Two coexisting theo philosophies.** (a) *Market-relative theo* (the MM view): fair value is empirically discovered where deep-pocketed players transact; models are interpolation machinery that propagate fair value from liquid anchors to illiquid strikes. (b) *Forecast theo* (the vol-arb view, Sinclair): fair vol is your forecast of future realized vol plus a risk premium; edge exists where implied deviates from it. Real prop-MM stacks are (a) with (b)-style overlays.
- **The theo stack is layered**: clean inputs (forwards, borrow, dividends, de-Americanized IVs) → arbitrage-free surface fit → intraday dynamics (sticky rules / SSR / vol paths) → fair-vol overlays (RV forecasts, event variance, cross-sectional relative value, inventory/flow adjustments).
- **Breakeven vol (BEV) is the rigorous bridge between theo vol and realized P&L**: the σ at which buying the option and delta-hedging to expiry earns exactly zero. It is computable ex post per option per date by historical replay, which makes it a **supervised learning label**. This is not a new idea in the index space — Dupire (2006, "Fair Skew: Break-Even Volatility Surface") and Hull, Li & Qiao (2022, *Financial Analysts Journal*, "Option Pricing via Breakeven Volatility", ~400k SPX options) are the canonical references — but **no public work applies it at scale to American single-name options with modern ML**. That's the gap this document's system design targets.
- The proposed system: replay-engine label factory (American-pricer-aware) → gradient-boosted / NN model predicting `σ_BE` conditional on option and market state → edge signal `σ_mkt − σ̂_BE` → portfolio construction with vega/gamma budgeting and event/tail controls → daily-close delta-hedged execution. Every stage has a mature reference implementation in the literature; the hard parts are label correctness (dividends, early exercise, borrow), leakage-proof validation (to-expiry labels overlap massively), and separating true mispricing from the variance risk premium you may or may not want to carry.

---

## 2. The theo concept: vocabulary and philosophy

### 2.1 What a desk means by "theo"

Theo (theoretical value) is the model value of an option computed from the firm's own inputs: fitted vol ("theo vol"), forward, borrow, dividend forecast, rate, and time — pushed through a pricing model (Black-Scholes for European, binomial/BAW/Andersen-Lake for American). Everything on an options desk is organized around it (Natenberg 2014, ch. 5; Baird 1992):

- **Quoting**: `bid = theo − edge`, `ask = theo + edge`. Edge width is a function of hedge cost, inventory, vega/gamma risk of the contract, uncertainty in the fit, and event proximity.
- **Edge accounting**: every fill is booked as `edge = |traded price − theo|` (signed by side). The MM business model is: capture edge on flow, hedge the residual risk, survive to the long run. "If you can net a 1% edge, you are a casino" (Abdelmessih).
- **Risk and P&L attribution**: greeks are computed off theo, and P&L explain decomposes realized P&L into theo-change components (delta, gamma, vega, theta, carry) plus edge captured.

Desks *think in vol space*, not price space. Institutional delta-neutral structures are quoted in vol terms and traded "tied up" with a stock delta exchange; the option price is just the projection of (surface, forward, carry) through the model. Vol space is where structure lives: a 1-vol error is comparable across strikes and tenors, a $0.05 error is not.

### 2.2 Two philosophies of fair

**Market-relative fair (the MM view).** Kris Abdelmessih (ex-SIG floor MM, Moontower) is the best public articulation: *"any price that is transparently and liquidly trading is called fair value."* A market that is "choice"/"pick'em" — deep-pocketed players both sides at one price — defines fair. The model's job is not to *know* fair value but to **propagate it**: fit the liquid anchors (ATM near-dated, penny-wide strikes) and interpolate/extrapolate to everything illiquid, no-arbitrage constraints keeping the propagation honest. SIG's training institutionalizes this: mock trading drills where trainees quote around theo sheets, collapse positions strike-by-strike via put-call parity, and get graded on edge captured per trade, not directional opinion.

**Forecast fair (the vol-arb view).** Sinclair (*Volatility Trading*, *Positional Option Trading*): fair vol is your forecast of subsequent realized vol, plus the risk premium you demand for the risk profile of holding the position. Edge = implied minus forecast. This view holds positions rather than flipping them, and its natural P&L is the delta-hedged carry of implied-vs-realized.

**Synthesis.** A production MM theo is (a) at the core with (b) as an overlay: the surface is fit to the market, then *leaned* — tilted toward the firm's fair-vol forecast, cross-sectional relative value, and inventory. The lean size expresses conviction; the fit expresses respect for the market. Firms that lean too hard get run over by flow they don't understand; firms that never lean are pure spread-collectors with no opinion and get adversely selected by those who do.

---

## 3. Layer 1 — Input cleaning: the unglamorous 80%

Most "vol edge" claimed by naive backtests is actually input error. The single most repeated practitioner warning: **errors in carry masquerade as skew**.

### 3.1 Mids, microprice, quote intervals

- Raw NBBO mid is noisy and manipulable. Better: size-weighted microprice (weights bid by ask size, pulling toward the side that trades first; Stoikov 2017).
- Wide markets make mids nearly meaningless. Serious fitters treat quotes as **intervals** (fit inside the bid/ask corridor) rather than points.
- Stale quotes are pickoff bait and fit poison: detect via no-update-after-underlying-tick, crossed/locked markets, one-sided fading; down-weight or drop. Commercial fitters advertise "graduated defense" — error bars widen automatically as data degrades (Vola Dynamics).

### 3.2 Implied carry: rates, borrow, dividends

Professionals never assume the rate — they **imply it from put-call parity**. Regress `C − P` on `K` across strikes: slope gives the discount factor, intercept the forward. Deviations decompose into (rate − borrow) and dividend expectations, which are underdetermined from one expiry: desks triangulate across expiries (dividends are expiry-dated cash events; borrow is a rate) to separate them.

- **Hard-to-borrow names**: negative rebate makes the synthetic trade below fair forward; borrow behaves like a synthetic dividend. Option-implied borrow is extractable from put-call pairs (Muravyev et al.). Put IVs on HTB names are partly *fair borrow compensation, not vol* — a fitter (or a breakeven replay) that ignores borrow will flag them as rich forever and bleed shorting them.
- **Dividends**: forecast from history + announcements; model as escrowed, piecewise-lognormal, or blended. Deep ITM American calls are exercised the close before ex-div when extrinsic < dividend — this is carry machinery, and getting it wrong corrupts both IVs and hedge deltas.

### 3.3 De-Americanization

Single-name US equity options are American. To build a surface you convert American prices → implied vols via an American engine (binomial CRR with discrete dividends is the OptionMetrics standard; BAW/Ju-Zhong approximations; the modern answer is Andersen-Lake-Offengenden 2016 spectral collocation, ~100k prices/sec/core), fit the surface in de-Americanized (pseudo-European) vol space, and re-Americanize to publish theos.

Known failure modes (Burkovska et al. 2016): errors concentrate exactly where early exercise matters — deep ITM puts, high dividends, high rates, long tenors — and dividends amplify them. The design consequence for everything downstream: **quote and fit in de-Americanized IV space, but generate hedge deltas, exercise decisions, and premium root-finds with the American pricer.**

### 3.4 Vol time (the event calendar)

Desks don't run calendar time; they run a **variance-day clock**: weekends/holidays ≈ 0 weight, earnings/CPI/FOMC days > 1 (CPI days run ≈ 2× baseline realized; earnings day for a typical single name can be >50% of a front expiry's total variance). This explains the "weekend effect" in IV (Friday→Monday ATM IV must rise to fit undecayed prices under naive calendar theta) and is the substrate for event-variance stripping in §6.3.

---

## 4. Layer 2 — Surface fitting

The fitted surface *is* theo vol. Requirements: arbitrage-free (butterfly = no negative density; calendar = non-crossing total variance), robust to garbage quotes, stable tick to tick, and fast enough to refit the whole universe continuously.

| Family | Members | Notes |
|---|---|---|
| Parametric per-slice | SVI (Gatheral; arb-free version Gatheral-Jacquier 2014), Orc **Wing model** (vr/sr/pc/cc + cutoffs; the legacy MM-desk standard), polynomial-in-delta | ~5 params/expiry; fast, interpretable, wings controlled |
| Parametric whole-surface | SSVI, **eSSVI** (Hendriks-Martini 2017; global no-arb: Mingone 2022) | Cross-expiry consistency by construction |
| Stochastic-model-implied | SABR (equities: mostly a smile interpolator), Heston, rough Bergomi | Used for dynamics/greeks more than marks |
| Non-parametric | Fengler 2009 arb-constrained splines on call prices, kernel smoothers, GPs, deep smoothing (Ackerer et al. 2020: NN corrector + no-arb penalties) | Flexible; needs explicit constraint machinery |

Practitioner details that matter more than family choice:

- **Weighting**: WLS with weights ∝ 1/spread² and/or vega² (vega converts price error ↔ vol error); ATM near-dated dominates. Fit to quote intervals, not mids.
- **Error bars as minimum edge**: Vola Dynamics (fitter vendor to "the world's best market makers") derives per-point error bars from bid-ask spreads and positions them as the *natural minimum quoting edge* — a clean formalization of how fit uncertainty maps to quote width.
- **W-shaped smiles**: modern earnings/0DTE-era single names (NVDA, TSLA) exhibit bimodal-density W smiles around events that plain SVI cannot fit — one reason top firms run proprietary curve families with per-expiry automatic model selection.
- **Robustness**: Bayesian priors toward the previous fit, parameter freezing hierarchies in fast markets (freeze skew, refit level), trader-overridable marks. The fitter must survive VIX > 80 and GameStop without retuning.

---

## 5. Layer 3 — Surface dynamics: theo between fits

A fitted surface is stale within seconds. Firms make theo *ride the market* without refitting:

- **Coordinates**: store the surface in normalized coordinates (delta or standardized moneyness `ln(K/F)/σ√τ`, variance time), so theos automatically follow the ticking forward.
- **Sticky rules**: sticky-strike (IV(K) fixed as spot moves) vs sticky-delta (IV(moneyness) fixed) vs sticky-local-vol. US equities are predominantly sticky-strike-ish; regime drifts toward sticky-delta in low-vol rallies. The Orc wing model bakes dynamics into parameters (`vcr`, `scr`, `ssr` — "skew swimmingness rate": 0 = sticky delta, 100 = sticky strike).
- **SSR (skew-stickiness ratio, Bergomi)**: `SSR = β(dATMvol/dlnS)/skew`; = 1 sticky-strike, 0 sticky-delta, → 2 in the short-maturity local-vol limit; empirically ≈ 1.2–1.6 for SPX at 1M–1Y. SSR drives **smart deltas** — the model-consistent delta including the smile's response to spot. Getting the vol path wrong flips book deltas: Abdelmessih reports a vol-path parameter change swinging his book's delta by $20mm.
- **Factor structure**: daily surface moves compress to ~3 PCA modes — level, skew, curvature — with AR(1)-ish coefficients (Cont & da Fonseca 2002). Intraday surface risk is expressed as level/skew/curvature betas to spot.
- **Cadence**: full refits on seconds-to-minutes timers or on triggers (spot move > x·σ, large prints, quote-flow anomalies); between refits, theo updates via the dynamics rule.

---

## 6. Layer 4 — Fair-vol overlays: where firms deviate from the market

This is the alpha layer: the components of theo that are *yours*, not the market's.

### 6.1 Measuring realized vol correctly

The fair-vol pipeline starts with a clean RV estimate — jump-robust, event-aware, overnight-split:

- **Daily-bar estimators**: close-to-close is unbiased but wildly inefficient (±30% relative error on 20-day windows). Range estimators: Parkinson (~5×), Garman-Klass (~7×), Rogers-Satchell (drift-free), **Yang-Zhang** (drift-free + overnight-aware, the practitioner default, ~8× efficiency). All biased down on gappy names.
- **Intraday RV**: sum of squared 5-minute returns is the canonical bias/variance compromise (Liu-Patton-Sheppard 2015: very hard to beat among ~400 estimators). Noise-robust variants for finer sampling: two-scale RV, realized kernels (Barndorff-Nielsen et al. 2008), pre-averaging.
- **Decompositions that carry signal**: overnight vs intraday split (overnight is one fat noisy observation; earnings land there); jump vs continuous via bipower variation; **signed semivariance** RS⁺/RS⁻ — negative semivariance drives future vol (Patton-Sheppard 2015).

### 6.2 Forecasting realized vol

- **HAR-RV (Corsi 2009)** is the universal benchmark: `RV_{t+1} = β₀ + β_d·RV_t + β_w·RV_{t−5:t} + β_m·RV_{t−22:t}` (log spec preferred). 1-day-ahead index R² ≈ 0.5–0.75; decays with horizon; single names lower.
- **Corrections that reliably beat plain HAR**: HARQ (Bollerslev-Patton-Quaedvlieg 2016 — shrink toward the mean when RV is noisily measured, via realized quarticity), SHAR (signed semivariances), leverage terms, and **cross-sectional pooling with a global vol factor** (Bollerslev et al. 2018 "Risk Everywhere") — pooling beats per-name estimation for noisy single names, a key production insight.
- **Rough vol**: log-RV behaves like fBM with Hurst H ≈ 0.1 (Gatheral-Jaisson-Rosenbaum); the fractional-kernel forecast matches or beats HAR with essentially one parameter. Mechanism contested; stylized fact strong.
- **ML**: with only RV lags as features, trees/NNs beat the HAR lineage modestly, gains growing with horizon (Christensen et al. 2023). The real ML edge comes from *feature breadth* (IV level and slope, option-implied moments, LOB/flow, news, peer vol) and *nonlinear interaction of IV with RV state* — and from extreme days (Rahimikia-Poon). For point forecasts from RV history alone, tuned-HAR ≈ ML.
- **IV as a predictor**: current IV is the strongest single predictor of future RV at monthly horizons — but biased high by the risk premium. Encompassing regressions blending model RV forecasts with IV are the standard desk fair-vol core.

### 6.3 Event variance (earnings, FOMC, CPI)

Scheduled events are deterministic-date jumps; total implied variance is additive (Dubinsky-Johannes):

```
σ_IV² · T = σ_diffusive² · T_var-days + Σ_events σ_event,j²
```

Desk procedure (Abdelmessih's worked recipe): guess the earnings straddle → strip that event variance from every expiry containing the event → iterate until the **ex-earnings term structure is smooth** (kinks ⇒ wrong guess). Implied daily move ≈ straddle/S ≈ 0.8·σ_event,daily (MAD factor). A 40-DTE option at IV 36 with ambient vol 24 is carrying an earnings day that is 1/40th of the days and ~57% of the variance.

Empirics: IV ramps into events and crushes after (30–60% front-IV crush typical for large caps); implied earnings moves slightly exceed realized on average (small earnings VRP, mostly eaten by costs); FOMC/CPI now explicitly priced in weeklies/0DTE (Wright NBER w28306). Fair-vol systems carry an **event-variance calendar** and interpolate term structure through variance time, never calendar time. Breakeven replay must treat event days separately or labels mix jump premium with diffusion vol.

### 6.4 Cross-sectional relative value

A single name's fair vol is disciplined by its neighbors:

- **Index vs constituents**: implied correlation (Cboe COR3M construction) is chronically rich vs realized → dispersion (short index vol / long single-name vol) is both a structural desk trade and a *consistency check on single-name theos* (Driessen-Maenhout-Vilkov 2009: the index VRP is largely a correlation risk premium).
- **ETF vs basket** triangles (SPY-SPX-ES, XLE vs components) propagate fair vol into less liquid names.
- **Peer/sector pooling**: same logic as pooled RV forecasting — a name's fair vol is shrunk toward sector/peer fair vol.

### 6.5 The variance risk premium: what "fair" must reckon with

- **Index**: implied systematically exceeds subsequent realized — Carr-Wu 2009: mean realized/implied variance ratio ≈ 0.6 for indices; IV > subsequent RV in ~85–90% of months; short index variance earns Sharpe ≈ 0.5–1.0 with brutal negative skew (Feb 2018, Mar 2020). Compensation for jump tail risk and vol-of-vol (Bollerslev-Todorov 2011).
- **Single names**: the average premium is thin to zero after costs (Carr-Wu; Bakshi-Kapadia companion) — the index premium is mostly *correlation* premium. Single-name vol edge is therefore mostly **cross-sectional and conditional**, not a static carry.
- **Design consequence**: `σ_mkt − σ̂_BE` mixes (i) genuine conditional mispricing and (ii) the risk premium. A model trained on realized outcomes learns the P-measure mean; selling every option above predicted breakeven is *deliberately harvesting the premium* — a real business (that's what the VRP is compensation for), but it must be sized like a short-tail-risk book, not like alpha. Neutralize signals to VIX beta and the average IV−RV spread if you want the residual alpha instead of the carry (feature-neutralization practice, à la Numerai FNC).

### 6.6 Inventory and flow

Not fair-vol in the forecast sense, but part of where quotes deviate from mid-theo:

- **Inventory skewing in greek space** (Stoikov-Sağlam 2009; Bergault-Guéant line): quotes shift against net book delta/vega/gamma, not per-contract inventory.
- **Flow toxicity**: wholesalers pay for benign retail flow; toxic institutional flow concentrates on lit exchanges. Markout-based flow classification, size-dependent edge, counterparty tiering. Public consensus (job posts, vendor design): firms' ML spend goes heavily into *microprice prediction and flow classification*, more than into replacing the arbitrage-free surface itself.

---

## 7. Breakeven vol: the mathematics

### 7.1 The fundamental P&L decomposition

Long one option priced/marked at implied vol σᵢ, delta-hedged continuously with the BS delta at σᵢ, while the path realizes instantaneous vol σᵣ(t):

```
dPnL_t = ½ · Γ(S_t, t; σᵢ) · S_t² · (σᵣ,t² − σᵢ²) dt

PnL(T) = ∫₀ᵀ e^{r(T−t)} · ½ Γ S² (σᵣ,t² − σᵢ²) dt
```

Derivation: Itô on V, subtract financing, eliminate theta via the BS PDE; the drift μ cancels — hedged P&L depends only on realized quadratic variation vs σᵢ², never on direction. The discrete-time version is the trader's daily identity:

```
PnL_day ≈ ½ Γ S² [ (ΔS/S)² − σᵢ² Δt ]     ("gamma gains minus theta rent")
```

with daily breakeven move `σᵢ·√Δt`.

**Dollar-gamma weighting / path dependence.** The realized vol that matters is the *dollar-gamma-weighted* average of σᵣ,t². ΓS² peaks near the strike with little time left, so two paths with identical unconditional RV give different hedged P&L depending on when/where vol was delivered. The 1/K²-weighted option portfolio (log contract) is the unique gamma-flat construction — that's why variance swaps are the path-independent version of this trade (Demeterfi-Derman-Kamal-Zou 1999). A single option's breakeven is the **gamma-localized** version.

**Robustness bound** (El Karoui-Jeanblanc-Shreve 1998): if the hedge vol dominates true vol pointwise and price is convex in S, the hedge super-replicates a.s. — the rigorous backbone of "long gamma at σᵢ can't lose if σᵣ ≥ σᵢ everywhere" (valid for σ(t,S) vols; can fail under general stochastic vol where convexity breaks).

**Smile-marking caveat** (Bergomi ch. 1): if the option is *remarked on a moving smile* rather than held at constant σ, daily P&L gains vega/vanna/volga terms and breakeven becomes a joint condition on realized variance, vol-of-vol, and spot-vol covariance. A single-scalar breakeven vol implicitly assumes hold-to-expiry with constant-vol marking — exactly the replay design below, so the concept is self-consistent, but interim MTM will be noisy.

### 7.2 Which vol do you hedge at? (Ahmad-Wilmott)

Three vols: actual σₐ, implied σᵢ (what you paid), hedge σ_h (delta input). Ahmad & Wilmott (2005):

- **Hedge at σₐ (your forecast, if right)**: total P&L is *locked at inception*: `PV = V(σₐ) − V(σᵢ)`, zero terminal variance — but the mark-to-market path is noisy (can be deeply underwater en route).
- **Hedge at σᵢ**: P&L accrues smoothly and monotonically (`½(σₐ²−σᵢ²)S²Γᵢ dt ≥ 0` pathwise when long-cheap), but the *total* is path-dependent.
- **General σ_h**: `PnL = V(σ_h) − V(σᵢ) + ∫ e^{−rt} ½ (σₐ² − σ_h²) S² Γ_h dt`.

Desks hedge at implied because daily P&L-explain is clean and risk marks at market. For a breakeven *label*, the hedge-vol choice is a free parameter of the experiment that changes the answer — fix it deliberately (§8.2).

### 7.3 Discrete hedging: the noise floor

- Hedging N times: replication error is asymptotically normal, std ∝ 1/√N. Derman-Kamal (1999): `std(PnL) ≈ √(π/4) · vega · σ / √N` for ATM — daily hedging of a 1-month option (N≈21) leaves P&L noise on the order of the vol bid-ask. Rigorous treatment: Bertsimas-Kogan-Lo 2000 ("temporal granularity"; halving error costs 4× rebalance frequency).
- Even at exactly σᵣ = σᵢ, a single replayed path's daily-hedged P&L ≠ 0: each day contributes a centered χ²(1) shock with variance ∝ (½ΓS²σ²Δt)². **Breakeven labels are inherently noisy; the model learns a conditional mean through that noise, but evaluation and sizing must respect the noise floor.**
- **Hedge timing defines the target**: close-to-close hedging measures close-to-close variance; the BEV must be computed under the hedging policy you will actually trade (Hull-Li-Qiao make this explicit). Daily-at-the-close is a legitimate, self-consistent choice.
- **Transaction costs**: Leland (1985) adjusted vol `σ̂² = σ²(1 ± √(2/π)·k/(σ√Δt))` (+ short, − long); band policies (Whalley-Wilmott O(k^{1/3}) no-trade bands; Zakamouline refinement) dominate time-based rebalancing per unit cost. Sepp (2011) gives closed forms for optimizing the Sharpe of a discretely-hedged, cost-paying vol strategy — directly reusable for the live hedging policy.
- **Better deltas**: minimum-variance delta ≠ BS delta under spot-vol correlation (Hull-White 2017; Sepp's "skew-beta" delta). Worth an ablation in the replay: BS-at-trial-vol vs min-var deltas.

### 7.4 Breakeven vol: definition, fixed point, properties

**Definition.** For option C (underlier, K, T, type) at time t₀ on realized path ω:

```
σ_BE(C, t₀, ω, H) = the σ such that:  −Price(σ) + Σ hedge cashflows(H) + payoff = 0
```

where `Price(σ)` is the model premium at entry and `H` is the hedging policy (frequency, delta model, exercise rule).

**The fixed-point subtlety.** Two conventions:

- **(a) Self-consistent**: hedge deltas computed at the trial σ itself. P&L is then (empirically, essentially always for vanillas) monotone decreasing in σ for a long position — premium and theta rent both rise with σ — so the root is unique and bisection/Brent converges. This answers: *"what could I have paid, behaving consistently with that price, and broken even?"* This is what public implementations do (e.g., Arthurim/breakeven_volatility) and the right default for labels.
- **(b) Exogenous hedge vol**: deltas at market IV (or a forecast); σ_BE is then just the premium level zeroing P&L given those flows. Matches desk practice; differs from (a) at second order in (σ_h − σ_BE), material for far-from-ATM strikes.

**Ill-conditioning at the wings**: for tiny-gamma options PnL(σ) is nearly flat in σ → the root is unstable. Restrict the label universe to |Δ| ∈ [~0.05, ~0.95] and/or weight training loss by vega.

**Realized skew.** The same path gives a *different* σ_BE per strike: a 90%-put's breakeven loads on vol realized while spot was near 90 — i.e., after down-moves. With negative spot-vol correlation, low strikes systematically break even at higher vols ⇒ **replayed breakeven surfaces exhibit skew**. This is Dupire's "Fair Skew" (2006): the historical-replay justification of how much of market skew is fair. Your model, fit across strikes, learns exactly this object conditionally.

**Variance-swap limit**: σ_BE of the 1/K² portfolio → flat-weighted realized variance; a single option's σ_BE is gamma-weighted realized vol on the path.

### 7.5 Prior art on the supervised idea

- **Hull, Li & Qiao (2022, FAJ 79(1), "Option Pricing via Breakeven Volatility")** — the canonical version of exactly the proposed idea, on SPX: compute σ_BE for ~400k options; fit a two-stage regression of σ_BE on option characteristics (moneyness, tenor, vol-state covariates); trade when market IV deviates from predicted BEV; framed as *nonparametric option pricing without specifying the underlying process*. Validates the concept end-to-end.
- **Adjacent cross-sectional literature**: Goyal-Saretto 2009 (RV−IV gap sorts earn ~1%/mo on straddles — a naive breakeven-gap signal); Bali-Beckmeyer-Moerke-Weigert 2023 (ML on 12M delta-hedged option observations, 265+ characteristics; nonlinear models win; IV itself the top feature); Zhan et al. 2022 (delta-hedged writing returns predictable from stock characteristics); Israelov-Nielsen (index options rich vs subsequent realized even in calm markets).
- **Gap**: no public work trains ML on **breakeven vol surfaces from American single-name replay** with dividends/borrow/exercise handled properly. Hull-Li-Qiao is index/European-style. The infrastructure cost of correct American replay *is the moat*.

---

## 8. The system: supervised breakeven vol at scale

### 8.1 Architecture

```mermaid
flowchart LR
  subgraph Label Factory (offline)
    A[OPRA/ORATS history +\ncorporate actions, divs, borrow,\nearnings calendar] --> B[Input cleaning:\nimplied carry, de-Americanized IVs]
    B --> C[Replay engine:\ndaily-close delta hedge to expiry,\nAmerican deltas + exercise rule]
    C --> D[Root-find σ_BE per option-date\nBrent on entry premium]
  end
  subgraph Model
    D --> E[Labels: ln σ_BE/σ_IV]
    B --> F[Features: option, underlier,\nsurface, events, cross-section]
    E --> G[GBM/NN + quantile heads]
    F --> G
  end
  subgraph Trading (live)
    G --> H[Edge: σ_mkt − σ̂_BE vs\nspread + uncertainty band]
    H --> I[Portfolio: vega/gamma buckets,\nevent & tail caps, name limits]
    I --> J[Execution + daily-close hedging,\nband policy, exercise mgmt]
    J --> K[P&L attribution:\nedge realization vs hedge noise vs costs]
  end
```

### 8.2 Label generation (the part that must be perfect)

For each (option, entry date t₀) in the universe:

1. **Entry state**: cleaned forward, implied borrow, dividend schedule, de-Americanized σ_IV from the fitted surface (not raw quotes).
2. **Replay**: from t₀ to expiry, at each close: recompute delta with the **American pricer** at the trial vol (discrete dividends, borrow-adjusted carry); trade the delta difference; accrue financing on the cash account at the realized rate; credit/debit dividends on the stock leg; apply the **optimal exercise rule** on the long side (exercise call before ex-div when extrinsic < dividend; put when carry on strike exceeds extrinsic). Without this, single-name labels are biased low by O(div yield).
3. **Root-find**: PnL(σ) is monotone in σ (self-consistent convention (a)) → Brent on σ ∈ [0.01, 3.0]. Each trial σ re-runs the replay (deltas change with σ). ~10–15 replays per label; a replay is ~250 American pricings max — trivially batchable on a vectorized American engine.
4. **Label hygiene**: drop |Δ| outside [0.05, 0.95]; record per-label metadata: number of earnings/FOMC days spanned, max |daily move|, gamma-weighted RV, hedge turnover. Compute **two label variants**: all-days, and event-days-excluded (replay that "collapses" scheduled event days by inserting the deterministic move into a separate bucket) — lets the model separate diffusion fair vol from event fair variance rather than smearing them.
5. **Short-side asymmetry**: assignment on short American legs is counterparty behavior, not model-determined; short-option breakeven ≠ −(long-option breakeven). Label from the long side; handle shorts in the portfolio layer with an assignment-risk haircut.

**Target transform**: predict `y = ln(σ_BE / σ_IV,t₀)` rather than raw σ_BE — approximately stationary, cross-sectionally comparable, and directly interpretable: E[y] < 0 is the variance risk premium; the *conditional deviation* of y from its unconditional mean is the alpha.

### 8.3 Features

| Block | Features |
|---|---|
| Option | moneyness (standardized `ln(K/F)/σ√τ`), delta, tenor, type; days-to-next-earnings |
| Underlier RV state | Yang-Zhang and 5-min RV at 5/21/63/252d; RS⁻/RS⁺ split; overnight share; jump variation; realized quarticity (measurement-noise proxy, HARQ-style) |
| Surface state | ATM IV level, slope (25Δ skew), curvature, term slope; IV−RV spread and its z-score; IV percentile vs own history |
| Events | implied earnings move (term-structure-stripped), historical earnings move distribution (median |move|, 8–12q), FOMC/CPI dummies in tenor window |
| Cross-section | sector/peer pooled RV and IV, index IV (VIX level + slope), implied correlation, name's beta to index vol |
| Carry/flow | implied borrow level and change, option volume/OI changes, put/call skew in flow |
| Regime | VIX term structure, credit spreads, market breadth — as *features*, never as split criteria |

### 8.4 Model and validation

- **Baseline first**: reproduce Hull-Li-Qiao's two-stage regression on your data; then LightGBM with monotonicity constraints where economics demand them; NN ensemble later if the cross-feature structure warrants. **Quantile heads** (predict q10/q50/q90 of y) — the spread is a per-option uncertainty estimate that flows into sizing.
- **The validation problem is the project's biggest silent killer.** To-expiry labels overlap massively in time: adjacent entry dates share almost the whole path; all strikes/expiries per underlier-date share one path. Mandatory: **purged walk-forward CV with embargo** (López de Prado) — purge any training label whose interval overlaps test intervals; cluster/aggregate by underlier-date (otherwise N is fictitious); report per-regime (calm/stress) and per-year, never pooled-only. Nested tuning (inner purged CV, outer walk-forward). Loss: QLIKE-style on vol ratios beats MSE under noisy labels (Patton 2011).
- **Neutralization decision**: neutralize predictions to VIX beta and unconditional IV−RV spread → residual = mispricing alpha; leave un-neutralized → signal = alpha + VRP carry. Run both books mentally; size them differently (carry book is short-tail).

### 8.5 From prediction to P&L

- **Edge**: `e = σ_mkt − σ̂_BE` per option, in vol points; convert to dollars via vega. Trade only where `|e| > spread-in-vol + k·(q90−q10)` — edge must clear both the toll and your own uncertainty.
- **Selection**: cross-sectional ranks, not absolute thresholds alone (removes residual regime level); long the cheapest decile / short the richest, delta-hedged, within liquidity screens (spread < x vols, OI/volume minimums).
- **Sizing**: per-position vega scaled by edge/uncertainty; portfolio caps on net vega, per-tenor gamma buckets, per-name and per-sector limits, explicit **event budget** (max short-vega through any single earnings), tail scenario limits (±20% spot × ±10 vol grid).
- **The short side is not symmetric**: short-vol positions harvest premium with χ²-left-tail (a single 10σ day can erase months). Historical replay labels embed only historical jumps; the model has never seen the jump that kills you. Cap short-side sizing by scenario loss, not by model uncertainty.
- **Execution reality**: single-name option spreads are wide; a taker needs `edge > spread`, an MM captures `spread + edge`. Resting orders inside the spread near theo captures some of both — but fills are adversely selected (you get filled when flow disagrees with you). Mid-fill backtests are fiction; model fills at mid ± α·(half-spread) with α ≥ 0.5 for takers.
- **Live hedging**: daily at the close to match the label definition; band policy (Whalley-Wilmott/Zakamouline widths, or Sepp's Sharpe-optimal frequency) to cut costs; min-var deltas as an ablation. Manage exercise/assignment actively around ex-div dates.
- **P&L attribution** closes the loop: decompose realized P&L per position into edge realization (`vega·(σ_BE,realized − σ_entry)`), discrete-hedge noise, costs, and event contributions. Attribution drift is the earliest model-decay alarm.

### 8.6 Failure modes checklist

1. **Carry errors → fake skew edge** (borrow/dividends wrong → puts on HTB names always "rich"). Test: implied-borrow time series sanity per name.
2. **Label leakage via overlap** → OOS Sharpe hallucination. Test: shuffled-entry-date placebo should destroy performance.
3. **Earnings smearing** → model sells every pre-earnings straddle. Use event-split labels.
4. **VRP masquerading as alpha** → book is one big short-vol carry. Test: neutralized vs raw signal performance.
5. **Discrete-hedge noise floor ignored** → oversized positions on noise-level edges. Test: compare claimed edge to Derman-Kamal noise band per position.
6. **Mid-fill fantasy** → all edge is inside the spread. Test: re-run backtest at bid/ask fills; strategy must survive.
7. **Wing labels** → ill-conditioned roots poison training. Delta-filter the universe.
8. **Regime overfit** → 2017-style calm-market model meets 2020. Per-regime reporting; time-decay weights; feature (not split) regimes.

---

## 9. Mapping to atx infrastructure

Most of the expensive substrate for this system already exists in this repo:

| System stage | Existing atx component |
|---|---|
| American pricing + Greeks at scale | Andersen-Lake engine, batch/laned Greeks (AVX2), `al_fast`/`al_bulk` preset ladder |
| De-Americanization + carry | American IV conversion pipeline, carry/de-Americanization fix (2026-07-05 spec) |
| Arb-free surface fitting | eSSVI fitter, no-arb integrity work, certified-band provenance, W-smile gaps noted in C-8 brief |
| Historical data | ORATS history loader, OPRA/databento ingestion, surface DB with fixed/prefix surfaces |
| Replay machinery | Backtest framework (waves A–E): daily-close delta-hedged strangle backtest is ~80% of the replay engine — needs the σ-trial loop + Brent wrapper and long-side exercise rule |
| Cross-route validation | Dispersion backtest, two-route parity — reusable as label cross-checks |

Shortest path to first labels: wrap the existing strangle-backtest hedging loop as `replay_pnl(option, t0, σ_trial)`, Brent it, and run over one year of SPY + a handful of liquid single names (mixed dividend/borrow profiles) to validate label distributions against the known stylized facts: E[ln(σ_BE/σ_IV)] < 0, breakeven skew present, earnings-window labels fat-tailed.

---

## 10. Reading list (prioritized)

**Start here**
1. Hull, Li, Qiao (2022) "Option Pricing via Breakeven Volatility", *FAJ* 79(1) — the published version of this exact system, SPX. SSRN 3938897.
2. Dupire (2006) "Fair Skew: Break-Even Volatility Surface", Bloomberg — the foundational replay/fair-skew reference.
3. Ahmad & Wilmott (2005) "Which Free Lunch Would You Like Today, Sir?", *Wilmott* — hedge-vol choice and P&L distributions.
4. Sinclair, *Volatility Trading* (2e 2013) and *Positional Option Trading* (2020) — the practitioner synthesis of forecast-theo trading.

**P&L math**
5. El Karoui, Jeanblanc, Shreve (1998) "Robustness of the Black and Scholes Formula", *Math. Finance* 8(2).
6. Demeterfi, Derman, Kamal, Zou (1999) "A Guide to Volatility and Variance Swaps", GS QSRN.
7. Derman & Kamal (1999) "When You Cannot Hedge Continuously", GS QSRN; Bertsimas, Kogan, Lo (2000) "When Is Time Continuous?", *JFE* 55(2).
8. Bergomi, *Stochastic Volatility Modeling* (2016), ch. 1 (hedged P&L), ch. 9 (SSR).
9. Leland (1985) *JF* 40; Whalley-Wilmott (1997) *Math. Finance*; Zakamouline (2006); Sepp (2011) SSRN 1865998; Hull & White (2017) "Optimal Delta Hedging", *JBF*.

**Surface construction**
10. Gatheral & Jacquier (2014) "Arbitrage-free SVI volatility surfaces", *QF* 14(1); Hendriks-Martini (2017) eSSVI; Mingone (2022).
11. Fengler (2009) "Arbitrage-free smoothing of the IVS", *QF* 9(4); Ackerer et al. (2020) "Deep Smoothing of the IVS".
12. Andersen, Lake, Offengenden (2016) "High-Performance American Option Pricing", *JCF* 20; Burkovska et al. (2016) de-Americanization, arXiv:1611.06181.
13. Cont & da Fonseca (2002) "Dynamics of Implied Volatility Surfaces", *QF* 2.

**Vol forecasting + VRP**
14. Corsi (2009) HAR-RV, *JFEC*; Bollerslev, Patton, Quaedvlieg (2016) HARQ, *J. Econometrics*; Patton & Sheppard (2015) SHAR, *REStat*; Bollerslev, Hood, Huss, Pedersen (2018) "Risk Everywhere", *RFS*.
15. Liu, Patton, Sheppard (2015) "Does anything beat 5-minute RV?", *J. Econometrics*; Barndorff-Nielsen et al. (2008) realized kernels, *Econometrica*; Yang & Zhang (2000), *J. Business*.
16. Gatheral, Jaisson, Rosenbaum (2018) "Volatility is rough", *QF* 18(6).
17. Carr & Wu (2009) "Variance Risk Premiums", *RFS* 22(3); Bakshi & Kapadia (2003), *RFS* 16(2); Bollerslev, Tauchen, Zhou (2009), *RFS*; Driessen, Maenhout, Vilkov (2009) correlation risk, *JF*.
18. Dubinsky & Johannes (+ Kaeck, Seeger 2019) "Option Pricing of Earnings Announcement Risks"; Wright (2020) "Event-Day Options", NBER w28306.

**ML for options**
19. Bali, Beckmeyer, Moerke, Weigert (2023) "Option Return Predictability with ML and Big Data", *RFS*; Goyal & Saretto (2009), *JFE* 94; Zhan, Han, Cao, Tong (2022), *Mgmt Science*.
20. Christensen, Siggaard, Veliyev (2023) "A ML Approach to Volatility Forecasting", *JFEC* 21(5); Patton (2011) QLIKE, *J. Econometrics*.
21. López de Prado (2018) *Advances in Financial Machine Learning* — purged CV, embargo, CPCV, deflated Sharpe.
22. Buehler, Gonon, Teichmann, Wood (2019) "Deep Hedging", *QF*; arXiv:2407.14736 (deep-vs-delta hedging as stat-arb).
23. Israelov & Nielsen (2015) "Covered Calls Uncovered", *FAJ* 71(6); "Still Not Cheap", *JPM*.

**Practitioner color**
24. Abdelmessih (Moontower): "Understanding Edge"; "Mock Trading Options With Market Makers"; "Lessons From Susquehanna"; "Implying the cost of carry in options"; "How an option trader extracts earnings from a vol term structure"; "Sticky vs floating strike" — moontower.ai / moontowermeta.com.
25. Natenberg, *Option Volatility and Pricing* (2e 2014); Baird, *Option Market Making* (1992).
26. Vola Dynamics (voladynamics.com — feature list as revealed MM best practice); ORATS blog (SMV smoothing, ex-earnings IV); Orc Wing Model parameter doc; Jacobson "Think Like a Market Maker" (volquant.medium.com); Stoikov & Sağlam (2009) "Option market making under inventory risk".
