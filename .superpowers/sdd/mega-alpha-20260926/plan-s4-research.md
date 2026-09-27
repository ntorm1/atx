## 4. Research brief (numbers and formulas the tasks use; citations in Appendix B)

### 4.A Cost-aware aim (Gârleanu–Pedersen 2013)
- Optimal policy under quadratic cost Λ = λΣ: partial adjustment x_t = (1−θ)x_{t−1} + θ·aim_t; aim = Markowitz with each
  signal k scaled by 1/(1 + φ_k·a/γ), φ_k = signal mean-reversion rate. With discounting ≈ 0: a/γ = (1−θ)/θ, so
  **g_k = θ / (θ + φ_k(1−θ))** for an AR(1) signal, and for any signal
  **g_k = Σ_{j≥0} θ(1−θ)^j ρ_k(j)** where ρ_k(j) is the lag-j autocorrelation of the standardized signal (second moments only).
- GP's own calibration: θ ≈ 3-4.4%/day at $1bn; signals with half-lives 2.4 / 206 / 700 days get aim weights ≈ 0.15 / 0.93 / 0.98.
  The dynamic rule beat the best static partial-adjustment rule by ≈ 20% net Sharpe because it down-weights the fast signal.
- Turnover→decay is regime-dependent: diffusive (rolling price windows) τ ≈ √(2φ) ⇒ g = 1/(1+(τ/τ0)²), τ0 ≈ 0.25-0.32;
  jump-refresh (filings) τ = p√(2(1−c)) ⇒ g = 1/(1+τ/τ0), τ0 ≈ 0.10-0.17. **Hence measure ρ̄_k(j) directly** (T31) rather than map τ.
- Sanity: θ = 0.03-0.05 on a composite with φ ≈ 0.003-0.004 predicts ≈ 1.0-1.4%/day turnover — matches the observed 1.3%.
- Square-root impact (S2) is not quadratic; GP is a first-order guide. Partial adjustment + a small dust band is the practical rule
  (NMV 2019: banding beats less-frequent rebalancing; buy/hold spread 10%/20% for mid-turnover signals).

### 4.B Multi-speed: one integrated aim, per-name rate
- θ_i ≈ √(γ σ_i² ADV_i / 0.2) with γ = RRA/NAV (JKMP calibration Λ_i = 0.2/ADV_i: 0.1% impact at 1% ADV). In dollar-free form:
  **θ_i = clip( √( RRA · σ_i² · ADV_i / (0.2 · NAV) ), 0.01, 0.15 )**, RRA = 10, NAV = 1e9, σ_i daily, ADV_i dollars.
  Check: σ 1.5%/day, ADV $50m → 2.3%/day; $200m → 4.6%; $1bn → 10%.
- Fast signal weight in name i becomes θ_i/(θ_i+φ_k): reversal gets 0.5 in a $1bn-ADV name, 0.13 in a $20m one. Separate sleeve
  order books forgo netting (DeMiguel et al. 2020: with costs, characteristics worth holding rise from 6 to 15 because trades cancel).

### 4.C Breadth (what survives)
- Add (low priority, v5.1): `opex_at` operating leverage (Novy-Marx 2011; JKP mega 2010-24 +0.24%/mo t 1.8; large 2010-24 +0.39 t 2.9).
- Already held: net equity issuance (`net_payout`, `issuance_*`), `rd_me`, `noa`, `cfoa`.
- Reject: 1-month industry momentum (post-2000 t ≈ 0.6; TRAIN 2020-22 +1.79%/mo t 2.5 is an outlier that would flatter it),
  earnings persistence, O/Z/KZ scores, net debt issuance, SUE in mega caps. Defer: Hou 2007 lead-lag (weekly, mid-cap followers,
  ≈ 58% post-publication decay), BAC (1,260-day ρ window; projected out by price-risk-v1; corr factor +0.18%/mo t 1.3 in mega caps),
  CHS distress (needs 6 extras; raw spread 3× larger in the smallest quintile).

### 4.D Delisting returns
- Shumway 1997 / Shumway-Warther 1999: performance-related delistings average −30% (NYSE/AMEX) and −55% (Nasdaq); "reason
  unavailable" is treated as performance-related. OSAP convention: −35% NYSE/AMEX, −55% Nasdaq, cap −100%, compounded into the final return.
- Rule for this NAV: classify each terminal date from filings (8-K Item 2.01 = M&A; Item 3.01 / Form 25 / Item 1.03 = performance;
  unknown = performance). M&A: last close stands (η = 0). Performance/unknown: η = −0.30 NYSE/AMEX, −0.55 Nasdaq, −0.35 venue unknown.
  Stress: −1.0 on longs. Shorts must not be the main beneficiary (report long vs short write-off P&L).

### 4.E Small-sample gates and trial accounting
- Lo 2002: SE(ŜR_annual) ≈ √((1+SR²/2)/T) ≈ 0.58-0.71 at T = 3 years. P(pass "net ≥ 1" | true SR) ≈ 27% at 0.64, 37% at 0.8, 50% at 1.0.
- Paired: Var(ŜR_a − ŜR_b) ≈ 2(1−ρ)/T ⇒ SE ≈ 0.26 (ρ .9), 0.18 (.95), 0.12 (.98). Gate on ΔSR vs a frozen reference cell.
- Deflated Sharpe (Bailey & López de Prado 2014): DSR = Φ[(ŜR − SR₀)√(T−1) / √(1 − γ₃ŜR + (γ₄−1)ŜR²/4)],
  SR₀ = √V[ŜR_n]·[(1−γ)Φ⁻¹(1−1/N) + γΦ⁻¹(1−1/(Ne))], γ = 0.5772. Under the null the expected max annual SR over 3 years is
  0.69 (N=5), 0.91 (N=10), 1.10 (N=20). Harvey-Liu-Zhu: t > 3 for non-literature signals; one-sided tests for literature-signed themes.

