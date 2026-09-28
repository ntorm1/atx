# v6 literature review: lifting the mega-alpha book from net SR ~0.74 to >= 1.0 at $1bn

Scope: cited web literature review mapped onto library v5.1 (38 candidates, 9 themes), composition `ew-theme-v1`, construction `aim-partial-v5` (theta .05, dust .1), neutralization `price-risk-v1`, S2 costs at $1bn. Source of book numbers: `docs/plans/2026-09-27-mega-alpha-scorecard.md` sections 1-6. Read-only research; nothing was run. Evidence grades: **A** = several top-journal studies, replicated, incl. post-2010 and/or cost-aware; **B** = solid but single-study, pre-2010, or mixed recent evidence; **C** = thin, conflicting or practitioner-only. Numbers marked *[est]* are my synthesis from the cited magnitudes plus a post-publication haircut. They are priors, roughly +/-50%, not measurements.

---

## 0. Framing: what the arithmetic requires

* Reference cell `v5-ew-t.05-d.1-fixed`: gross SR 1.177 and vol 3.41%/yr give gross mu = 4.01%/yr. Net mu is 2.53%/yr, so the drag is 1.48%/yr (0.43 SR). This matches the brief's framing: 13 bps per $ x 4.4% NAV/day x 252 = 1.44%/yr. **To first order, trading cost is the entire gross-to-net gap.** S1 (flat 6 bps) nets .933, so the modeled impact premium over a flat 6 bps costs 0.19 SR.
* Reaching net SR 1.0 at unchanged vol requires G - C >= 3.41%/yr, i.e. +0.88%/yr of net alpha. Illustrative paths:
  * cut cost 60% with gross flat;
  * cut cost 35% (-0.51%/yr) *and* raise gross 10% (+0.40%/yr), i.e. gross SR ~1.29;
  * cut cost 25% and raise gross 15%.

  No single lever in the literature reliably delivers +0.26 SR. **Three or four stacked levers are needed.**
* **Statistical power.** Over 3 years, SE(SR) is about sqrt((1+SR^2/2)/3), roughly 0.63 (Lo 2002). An unpaired test of 0.74 vs 1.0 needs ~60 years of data. The scorecard's *paired* dSR SEs (.011-.14) are the only usable TRAIN evidence. Hence: take alpha-side choices from literature priors; estimate only cost-side quantities (turnover, signal decay, capacity, covariance) from the data, because these are estimated precisely from 750 days. Expected returns are not (Merton 1980 logic; DeMiguel-Garlappi-Uppal 2009; Kan-Zhou 2007).
* **Base-rate caution.** After spreads, post-publication decay and modern-era effects, the average published anomaly nets ~4 bps/month and good combinations ~20 bps/month (Chen-Velikov 2023). Borrow fees take the average long-short anomaly from +0.14%/month to -0.01%/month (Muravyev-Pearson-Pollet 2025). A net SR >= 1.0 at $1bn from public fundamentals, price and volume is at the upper edge of what the literature supports. The best cost-aware published result, Portfolio-ML with net SR ~1.38 (Jensen-Kelly-Malamud-Pedersen 2026), uses ~150 characteristics and a 50-year panel.

---

## 1. Executive summary: levers ranked

Score = mid(expected dSR_net at $1bn) x evidence weight (A=1, B+=.8, B=.7, B-=.55, C=.4) / effort (L=1, M=2, H=3). **Gains overlap and are not additive**: levers 1 and 2 both attack the same turnover. A realistic stacked outcome is +0.12 to +0.30 SR *[est]*, i.e. net 0.86-1.04.

| # | Lever (concrete action) | dSR_net *[est]* | Evidence | Effort | Score | Key citations |
|---|---|---|---|---|---|---|
| 1 | **Shrink or redesign the fast sleeves.** `ind_adj_rev_5`, `iv_rv_spread`, `seasonality_same_month`, `low_max`, `low_ivol` and `si_change` carry 31.5% of weight but ~64% of the standalone-turnover proxy (section 2a). Cut them to at most 1/3 of current weight, or rebuild slower versions: turnover-conditioned reversal, multi-lag seasonality, long-window shorting flow, 63-252d risk windows. Reallocate to slow themes. | +0.03 to +0.12 | B+ (theory A; TRAIN aim-v1 test contra, -0.126 +/- 0.112, but TRAIN is a high-VIX window that flatters reversal) | L | .056 | Garleanu-Pedersen 2013; Novy-Marx-Velikov 2016; Frazzini-Israel-Moskowitz 2018; Nagel 2012; Medhat-Schmeling 2022; Qian-Sorensen-Hua 2007 |
| 2 | **Cost-scaled no-trade band plus liquidity-penalized targets**, replacing rank-desired x uniform theta. Target_i = neutralized signal_i / (1 + kappa x c_i), with c_i ~ half-spread_i + sigma_i x sqrt(trade_i/ADV_i). Trade only when \|target - current\| > b x c_i (a buy/hold band, not the fixed dust .1/N). Keep one book-level theta. | +0.06 to +0.15 | A- | M | .047 | Novy-Marx-Velikov 2016, 2019; FIM 2018; DeMiguel et al. 2020; JKMP 2026; Constantinides 1986 |
| 3 | **Upgrade member definitions within themes**: residual momentum; cash-based operating profitability; composite value incl. intangibles; 5-year composite issuance / XFIN; Heston-Sadka multi-lag seasonality; BAC/SMAX low-risk (survives vol neutralization); long-horizon FINRA shorting flow; EAR-centred earnings momentum. | +0.05 to +0.15 (mostly gross) | B | L-M | .047 | Blitz-Huij-Martens 2011; Ball et al. 2016; Eisfeldt-Kim-Papanikolaou 2022; Daniel-Titman 2006; Heston-Sadka 2008; Asness et al. 2020; Wang-Yan-Zheng 2020; Brandt et al. 2008 |
| 4 | **Re-weight themes on priors and risk, not strict 1/9.** The single-signal `options_implied` theme has 11.1% weight. Merge it with short_interest into a "short-sale-cost / overpricing" cluster. Down-weight filing-lagged earnings surprise. Theme weights ∝ prior SR on an LW-shrunk covariance, 50/50 with inverse-vol, capped to [0.5, 2]x EW. | +0.02 to +0.08 | B | L | .035 | DeMiguel-Garlappi-Uppal 2009; Kan-Zhou 2007; Ledoit-Wolf 2004; JKP 2023; Muravyev-Pearson-Pollet 2025; Martineau 2022 |
| 5 | **Resolve the low_risk vs `price-risk-v1` overlap.** The neutralization projects the book off beta252 and vol63, which removes most of what `low_beta`, `low_ivol` and `low_max` load on while they still generate turnover. Replace with BAC (betting against correlation) and scaled-MAX, or fold the theme into quality. | +0.02 to +0.06 | B+ | L | .030 | Novy-Marx 2014; Novy-Marx-Medhat 2025; Asness-Frazzini-Gormsen-Pedersen 2020; Liu-Stambaugh-Yuan 2018; Bali et al. 2017 |
| 6 | Momentum sleeve risk management: volatility-scale the momentum sleeve and de-risk in crash states (post-bear rebounds). | +0.00 to +0.03 (tail benefit, e.g. 9 Nov 2020) | A- | L | .014 | Barroso-Santa-Clara 2015; Daniel-Moskowitz 2016 |
| 7 | Slow factor-momentum tilt across themes (trailing 12-month theme return sign, heavy shrinkage). | 0 to +0.05 | B- | L | .014 | Ehsani-Linnainmaa 2022; Gupta-Kelly 2019; contra Asness et al. 2017; Ilmanen et al. 2021 |
| 8 | New low-turnover families: q5 expected growth, SI x (1 - IO) constraint interaction, 13F breadth change, earnings seasonality, cash-flow duration. | +0.03 to +0.10 (diversification) | C+/B- (post-2010 thin; 13F weak) | M-H | .012 | Hou-Mo-Xue-Zhang 2021; Asquith-Pathak-Ritter 2005; Chen-Hong-Stein 2002; Chang et al. 2017; Weber 2018 |
| R | **Realism, must do.** Name-level borrow fees on the short leg. The SI and IV-spread signals are largely borrow-fee proxies. If `swap-fin-v1` charges a flat rate, modeled net SR is overstated. | likely -0.05 to -0.15 in *modeled* SR; protects against a false positive | A | M | n/a | Muravyev-Pearson-Pollet 2025 (JF); Muravyev-Pearson-Pollet 2025 (JFE); Drechsler-Drechsler 2014; Engelberg-Reed-Ringgenberg 2018 |
| X | **Do not do:** ML stacking or IC-optimized weights fitted on 750 days; valuation-spread theme timing; dropping slow fundamentals because of weak TRAIN t (e.g. `opbe` -0.16, `roa` -0.03). | expected <= 0 OOS | A (against) | n/a | n/a | DeMiguel et al. 2009; Kan-Zhou 2007; Avramov-Cheng-Metzker 2023; Asness et al. 2017; own v3 (TRAIN 1.81 -> VAL -1.27) |

**Suggested stack order**, each step accepted only on a paired dSR with sign matching the prior: 1 + 5 (cheap, turnover-side) → 4 → 2 → 3 → 6. Spend the remaining VAL trial only on the final stacked candidate.

---

## 2. Scorecard diagnostics the literature speaks to directly

**2a. Turnover is concentrated in a minority of weight.** The proxy below is w_ew51 x standalone tau (daily one-way). It ignores cross-signal netting (DeMiguel et al. 2020) and the theta filter, which attenuates fast components more than slow ones. So the fast share of *book* trading is lower than shown, but those are also the sleeves where alpha capture is lowest (g_k .36-.56).

| theme | weight | sum(w x tau) | share of proxy |
|---|---|---|---|
| reversal_seasonality | 11.1% | .01527 | **29.5%** |
| options_implied | 11.1% | .00998 | **19.3%** |
| low_risk | 11.1% | .00662 | 12.8% |
| short_interest | 11.1% | .00600 | 11.6% |
| price_momentum | 11.1% | .00440 | 8.5% |
| earnings_momentum | 11.1% | .00320 | 6.2% |
| value | 11.1% | .00228 | 4.4% |
| profitability_quality | 11.1% | .00216 | 4.2% |
| investment_issuance | 11.1% | .00190 | 3.7% |

The six fast members (rev .181, iv .090, seas .094, low_max .089, low_ivol .075, si_change .092) hold 31.5% of weight and 64% of the proxy. The three slowest themes hold 33% of weight and 12% of the proxy.

Break-even arithmetic for a standalone sleeve: annual cost ≈ tau x 252 x 13 bps. That is 5.9%/yr for `ind_adj_rev_5` and ~2.5-3.1%/yr for the other fast members, against 0.5-0.65%/yr for fundamental members. At an assumed sleeve vol of 5-8%/yr *[est]*, rev needs a standalone gross SR of ~0.75-1.2 to break even. Its TRAIN SR is ≈ t/sqrt(3) = 0.69. The long-run literature for liquid-universe reversal after realistic costs is ~0 (Novy-Marx-Velikov 2016; FIM 2018: reversal is the most cost-constrained style).

**2b. Double smoothing starves fast signals.** Every member passes through `decay_linear(.,21)` (mean lag ~7 sessions), then the book adjusts at theta .05 (mean lag ~19 sessions). A 5-day reversal whose predictability is concentrated in the first days to weeks (Da-Liu-Schaumburg 2014; Medhat-Schmeling 2022: reversal is indistinguishable from zero after two months) is traded mostly after its alpha has decayed. The construction's own aim gain g_k = .358 says this. Garleanu-Pedersen: with one trading rate, a fast-decaying signal should receive small aim weight. Qian-Sorensen-Hua 2007: smoothing length should be signal-specific. Slow signals gain nothing from 21-day smoothing, and value with a current price loses timeliness (Asness-Frazzini 2013).

**2c. `price-risk-v1` neutralizes beta252, vol63 and ladv63.** `low_beta`, `low_ivol` and `low_max` are primarily beta and volatility sorts: Bali et al. 2017 show MAX drives the beta anomaly, and Liu-Stambaugh-Yuan 2018 show beta works through IVOL. After linear projection off beta and vol, what remains is second-order content plus turnover. Their TRAIN HAC t are all negative (-0.46 to -1.90).

**2d. About 22% of weight is in themes whose alpha is largely a short-sale-cost proxy.** Implied-volatility spread and skew predictability falls by at least two-thirds when high-fee stocks are excluded (Muravyev-Pearson-Pollet, JFE 2025). Short-interest-constrained stocks underperform 215 bps/month equal-weighted but only an insignificant 39 bps value-weighted (Asquith-Pathak-Ritter 2005). Across 162 anomalies the long-short return is +0.14%/month before borrow fees and -0.01% after (Muravyev-Pearson-Pollet, JF 2025). The warehouse's `iv_atm_21d` is a vendor clean IV with "vintage unproven" and "often null" caveats (`prepare_research_fields.py`), which adds weight to shrinking that theme.

**2e. Earnings-surprise members are filing-lagged by construction.** `sue`, `droe` and `chtax` enter at the 10-Q/10-K XBRL acceptance clock ("filing-clock lag"), typically weeks after the earnings release. PEAD in non-microcaps has been ~0 since ~2006 (Martineau 2022). What remains is front-loaded around the announcement and in microcaps (Subrahmanyam, as reported by UCLA Anderson Review). `ear` is correctly timed off the vendor `earnFlag` reaction day and is the right carrier of the theme (Brandt et al. 2008).

**2f. TRAIN (2020-2022) is a high-VIX window.** Reversal profits scale with VIX (Nagel 2012) and intra-industry reversals are stronger after declines and in volatile times (Hameed-Mian 2015). The negative TRAIN verdict on aim-v1, which down-weighted fast sleeves, is therefore biased in their favour relative to calmer regimes such as 2023-24.

---

## 3. Per-theme review

Each subsection gives the canonical papers, long-run performance and decay, the best known construction, interactions, a flag on the DSL, and the proposed improved definition in words.

### 3.1 earnings_momentum (`chtax`, `droe`, `ear`, `sue`)

* **Canon:**
  * Bernard-Thomas (1989); Chan-Jegadeesh-Lakonishok (1996).
  * Livnat-Mendenhall (2006).
  * Brandt-Kishore-Santa-Clara-Venkatachalam (2008): EAR 6.3%/yr abnormal, 0.7% above SUE; EAR and SUE largely independent, about 12.5%/yr combined; no EAR reversal after 3 quarters.
  * Thomas-Zhang (2011): seasonally differenced tax expense.
  * Novy-Marx (2015): earnings surprises subsume price momentum in cross-sectional regressions and remove momentum crashes.
* **Decay:**
  * Chordia-Subrahmanyam-Tong (2014): attenuation in the high-liquidity era.
  * Martineau (2022): PEAD non-existent for large stocks since 2006.
  * Subrahmanyam's replication: drift t 2.18 with all stocks vs 1.43 ex-microcaps.

  Expected post-2010 gross SR in a liquid universe *[est]*: SUE (filing-lagged) 0-0.2; EAR 0.2-0.5.
* **Best construction.** Measure the surprise at the announcement: 3-day [-1, +1] abnormal return, industry-adjusted rather than market-adjusted. Hold in event time, about 60 sessions with linear decay from the event, rather than a daily `decay_linear(21)`. Combine z(EAR), z(SUE), z(dROE) and z(tax surprise).
* **Interactions.** Overlaps price momentum (Novy-Marx 2015) and profitability trend (Akbas-Jiang-Koch 2017: the profit trend predicts next-quarter surprises).
* **DSL flags.**
  * `sue`, `droe` and `chtax` arrive weeks late; expect near-zero alpha in non-microcaps.
  * `ear` is well timed. Industry-adjusting it (FF49 mean excess return) reduces sector-news noise.
* **Proposal.** Make EAR the theme core (half the theme weight). Merge SUE, dROE and chtax into one filing-time "fundamental surprise" composite. Add Akbas-Jiang-Koch profitability trend (a low-turnover member: slope of the last 8 quarters of ROA, scaled). Cut the theme to ~0.5-0.75x EW unless paired TRAIN supports it.

### 3.2 investment_issuance (`asset_growth`, `noa`; issuance members rejected as redundant with `net_payout`)

* **Canon:**
  * Cooper-Gulen-Schill (2008): top-minus-bottom asset-growth decile >20%/yr, 1968-2003; retains power in large caps, but Fama-French (2008) find it weaker there.
  * Hirshleifer-Hou-Teoh-Zhang (2004): NOA.
  * Pontiff-Woodgate (2008): post-1970 share issuance is more significant than size, B/M or momentum.
  * Daniel-Titman (2006): composite issuance over **5 years**.
  * Bradshaw-Richardson-Sloan (2006): net external financing (XFIN) is the strongest financing measure.
  * Hou-Xue-Zhang (2015) I/A; Fama-French (2015) CMA.
* **Decay.** McLean-Pontiff (2016) average post-publication decay of 58% applies. Investment is replicable in large caps under Hou-Xue-Zhang (2020) (65% of 452 anomalies fail overall, frictions worst). *[est]* post-2010 gross SR 0.3-0.5; issuance and XFIN are the most robust in large caps.
* **Interactions.** Investment, issuance and payout share q-theory content and a short-duration exposure (Gormsen-Lazarus 2023). Issuance and net payout are near-duplicates, which is why the admission step rejected them.
* **DSL flags.**
  * `issuance_vendor` uses 1 year. Daniel-Titman's composite issuance is 5 years and is slower (lower turnover) and more robust.
  * `net_payout` sits in the value theme although it is economically an issuance/payout signal. Under equal theme weights this moves issuance weight into value.
  * `asset_growth` is the weakest large-cap member.
* **Proposal.**
  * Composite issuance CI = log(ME_t / ME_{t-5y}) - log gross return over (t-5y, t); low CI is good.
  * XFIN = (sale of stock - purchase of stock - cash dividends + debt issued - debt retired +/- change in current debt) / average assets; low is good.
  * Keep NOA. Replace or average `asset_growth` with HXZ I/A (change in gross PP&E + change in inventory, over lagged assets).
  * Move `net_payout` into this theme, or average it with CI/XFIN as one "external financing" member.

### 3.3 low_risk (`low_beta`, `low_ivol`, `low_max`, `lowvol_ind`)

* **Canon:**
  * Frazzini-Pedersen (2014) BAB. Beta = 5-year correlation of overlapping 3-day returns x 1-year daily vol ratio, shrunk 0.6/0.4 toward 1.
  * Ang-Hodrick-Xing-Zhang (2006) IVOL; Bali-Cakici-Whitelaw (2011) MAX.
  * Asness-Frazzini-Pedersen (2014, FAJ): low risk without industry bets.
  * Asness-Frazzini-Gormsen-Pedersen (2020): BAC (betting against correlation) and SMAX.
* **Critiques and interactions:**
  * Novy-Marx (2014): defensive equity is explained by size, profitability and valuation.
  * Novy-Marx-Velikov (2022): BAB performance comes from non-standard, effectively equal-weighted construction and a biased beta estimator.
  * Liu-Stambaugh-Yuan (2018): the beta anomaly is the IVOL effect among overpriced stocks.
  * Stambaugh-Yu-Yuan (2015): IVOL is negative among overpriced stocks and positive among underpriced.
  * Huang et al. (2010): the IVOL effect disappears after controlling for prior-month return.
  * Bali-Brown-Murray-Tang (2017): lottery demand (MAX) explains the beta anomaly.
  * Novy-Marx-Medhat (2025): profitability prices defensive equity.

  *[est]* incremental gross SR after beta/vol neutralization and a profitability theme: 0-0.3.
* **Decay and regime.** Long-run BAB SR is high in equal-weighted form but much weaker value-weighted. Low vol is duration-like: it lagged when rates rose in 2022 relative to value but beat the market, and it lagged in the 2020-21 rebounds and the 2023-24 mega-cap rally (section 7).
* **DSL flags.**
  * **Neutralization overlap (section 2c).**
  * `low_beta` uses a 250-day correlation where FP use 5 years (noisier, tau .048).
  * `low_ivol` and `low_max` use 21-day windows (tau .075/.089) and are contaminated by last-month reversal (Huang et al. 2010).
  * `low_max` is MAX1; the literature favours the average of the 5 largest daily returns.
* **Proposal.** Replace the theme with members that survive vol/beta neutralization:
  * **BAC**: -rank of the 5-year correlation with the market, computed within vol quintiles.
  * **SMAX**: -(mean of 5 largest daily returns over 21d) / (252d vol).
  * Keep `lowvol_ind` only if it survives neutralization.
  * Use 126-252 day windows.

  Alternatively fold the theme into profitability_quality and redistribute the 1/9.

### 3.4 options_implied (`iv_rv_spread`)

* **Canon:** Bali-Hovakimian (2009): realized-minus-implied vol spread negatively predicts returns (volatility-risk proxy); call-put IV spread positively (jump risk). Related: Cremers-Weinbaum (2010), Xing-Zhang-Zhao (2010), An-Ang-Bali-Cakici (2014).
* **Key recent evidence.** Muravyev-Pearson-Pollet (JFE 2025): IV spread and skew predict returns *because they proxy for stock borrow fees*. Excluding high-fee stocks cuts predictability by at least two-thirds. Fees are a limit to arbitrage, not a harvestable premium.
* *[est]* gross SR 0.3-0.7 historically; net of borrow fees and turnover (tau .090) at $1bn it is near zero.
* **DSL flags.** It is a single member with the largest single weight in the book (11.1%) and the second-largest turnover share (19%). The vendor IV field has unproven vintage. It is highly redundant with SI and high-fee shorts.
* **Proposal.** Cap at <= 3-4% of book weight, or merge with short_interest into one "short-sale-cost / overpricing" theme at 1/9. Smooth IV-RV over 21-63 days. Never size it without name-level borrow fees in the cost model.

### 3.5 price_momentum (`high_52w`, `ind_mom_12_1`, `mom_12_1`, `within_ind_mom`)

* **Canon:**
  * Jegadeesh-Titman (1993); Grundy-Martin (2001).
  * Moskowitz-Grinblatt (1999): industry momentum explains much of stock momentum.
  * George-Hwang (2004): 52-week high.
  * Asness-Moskowitz-Pedersen (2013): value and momentum everywhere, negatively correlated.
  * Blitz-Huij-Martens (2011): residual momentum has risk-adjusted profits ~2x total-return momentum and is more consistent. Blitz-Hanauer-Vidojevic (2020): idiosyncratic momentum gives comparable returns at half the volatility and is distinct from momentum.
  * Da-Gurun-Warachka (2014): frog-in-the-pan; continuous-information momentum 5.94% vs -2.07% for discrete.
  * Novy-Marx (2015): fundamental momentum.
  * Ehsani-Linnainmaa (2022): stock momentum is aggregated factor momentum.
* **Risk.** Daniel-Moskowitz (2016): crashes occur in panic-state rebounds; a dynamic strategy ~doubles SR. Barroso-Santa-Clara (2015): vol scaling lifts SR from 0.53 to 0.97 and kurtosis falls from 18 to 2.7. Momentum fell ~5% in one day on 9 Nov 2020, the largest intraday drop on record (Bloomberg; MSCI).
* *[est]* post-2010 gross SR: raw 12-1 0.2-0.5 with fat left tail; residual 0.4-0.7.
* **DSL flags.** `mom_12_1` and `within_ind_mom` are raw-return versions. They carry time-varying factor and industry bets, which is exactly what residual momentum removes. The TRAIN evidence is actually decent (t 1.54, 1.83), so this is an upgrade, not a rescue.
* **Proposal.**
  * Residual momentum: regress daily returns over t-252..t-22 on market + FF49 industry (or the book's risk factors). Take the sum of residuals divided by residual stdev and rank. Replace `within_ind_mom` with it, and keep `mom_12_1` or average the two.
  * Optional frog-in-the-pan filter: ID = sign(PRET) x (%neg days - %pos days); tilt toward low-ID (continuous) winners.
  * Vol-scale the theme sleeve by its trailing 126d realized vol (lever 6).

### 3.6 profitability_quality (8 members)

* **Canon:**
  * Novy-Marx (2013): GP/A has roughly the power of B/M, is negatively correlated with value, and the two combine well.
  * Fama-French (2015) RMW; Hou-Xue-Zhang (2015) quarterly ROE.
  * Ball-Gerakos-Linnainmaa-Nikolaev (2016): **cash-based operating profitability subsumes accruals**, and adding CbOP raises SR more than accruals plus profitability together.
  * Asness-Frazzini-Pedersen (2019) QMJ: profitability, growth, safety, payout.
  * Piotroski (2000); Sloan (1996).
  * Green-Hand-Soliman (2011): accruals anomaly decayed to no longer reliably positive.
  * Hafzalla-Lundholm-Van Winkle (2011): percent accruals (scaled by \|earnings\|) are stronger.
  * Novy-Marx (2011): operating leverage.
* **Post-publication.** Novy-Marx-Medhat (2025 retrospective): profitability subsumes "quality", prices defensive equity, explains all abnormal performance of "alternative value" (incl. intangibles-adjusted) and half of value's post-2007 underperformance. Detzel-Novy-Marx-Velikov (2023): with costs, factor models using cash profitability dominate.

  *[est]* post-2010 gross SR 0.4-0.7. This is the most robust, lowest-turnover theme.
* **DSL flags.**
  * `accruals`, `cfoa` and `roa` are partially redundant with CbOP, and accruals decayed.
  * `fscore` is a 0-9 integer, so its rank is tie-heavy and has little resolution.
  * `opex_at`'s proxy (sale - oi) includes D&A. It correlates with GP/A through asset turnover, but TRAIN t 3.2 and a low tau justify keeping it.
* **Proposal.** Replace `accruals` + `cfoa` with **CbOP** = (revenue - COGS - SG&A excl. R&D - Δreceivables - Δinventory - Δprepaid + Δdeferred revenue + Δpayables + Δaccrued expenses) / total assets. A cash-flow-statement approximation is acceptable. Add two low-turnover members:
  * profitability trend (Akbas-Jiang-Koch 2017);
  * 5-year change in GP/A (QMJ growth).

### 3.7 reversal_seasonality (`ind_adj_rev_5`, `seasonality_same_month`)

* **Reversal canon:**
  * Da-Liu-Schaumburg (2014): reversal of non-fundamental moves is 4x stronger.
  * Hameed-Mian (2015): intra-industry reversals are larger, present in large and liquid stocks, and stronger after declines and in volatile times.
  * Blitz-Huij-Lansdorp-Verbeek (2013): residual reversal earns ~2x the risk-adjusted return of conventional reversal and is net-positive even in post-1990 large caps.
  * de Groot-Huij-Zhou (2012): 30-50 bps/week net only when restricted to large caps with smart construction.
  * Medhat-Schmeling (2022): **reversal exists only in low-turnover stocks; high-turnover stocks show short-term momentum** that survives costs and is strongest in large, liquid stocks.
  * Nagel (2012): reversal = liquidity-provision return, strongly increasing in VIX.
* **Cost evidence.** FIM (2018): reversal is the most capacity-constrained style. Novy-Marx-Velikov (2016): anomalies with one-sided monthly turnover above 50% rarely survive costs; rev at 18%/day is far beyond that.
* **Seasonality canon:**
  * Heston-Sadka (2008): same-calendar-month effect at annual lags up to 20 years, decile spread >60 bps/month.
  * Keloharju-Linnainmaa-Nyberg (2016): 13%/yr; seasonalities also exist in anomalies and are modestly correlated with each other.
  * KLN (2021): seasonal reversals, i.e. other-month returns reverse.
  * Post-publication evidence is mixed: strong internationally 1998-2017; some practitioners report US decay (C).
* *[est]* gross SR: reversal 0.5-1.0 (regime-dependent), net at $1bn <= 0.2; seasonality 0.3-0.6 gross, marginal net.
* **DSL flags.**
  * `ind_adj_rev_5` is unconditional, uses 5-day total (not residual) returns, and is double-smoothed (section 2b).
  * `seasonality_same_month` uses a single lag (one year), although the effect is spread across up to 20 annual lags. Its window (sessions 224-245, plus the ~7-session mean lag of `decay_linear`) is correctly aligned to next month's calendar month.
* **Proposal.**
  * **Reversal:** -(5-21 day residual return vs FF49 + market) x 1{share turnover in bottom two terciles}, i.e. Medhat-Schmeling conditioning. Cap at <= 1/3 of the current weight. Alternatively use it only as a trade-timing overlay that accelerates or delays slow-signal trades. That overlay is practitioner lore consistent with GP (C for the overlay itself).
  * **Seasonality:** SEAS = mean over y = 1..k (k = 5, or up to data) of the same-calendar-month return y years ago, minus the mean of other-month returns over the same years (KLN seasonal reversal). This keeps monthly turnover but improves the signal-to-noise per unit traded.

### 3.8 short_interest (`dtc`, `si_change`, `si_ratio`)

* **Canon:**
  * Asquith-Pathak-Ritter (2005): high SI with low IO means constrained stocks; -215 bps/month EW but an insignificant -39 bps VW.
  * Boehmer-Jones-Zhang (2008); Diether-Lee-Werner (2009).
  * Boehmer-Huszar-Jordan (2010): **the long leg, low-SI heavily traded stocks, earns larger abnormal returns than the heavily shorted short leg.**
  * Hong-Li-Ni-Scheinkman-Yan (2015): DTC = SIR / turnover beats SIR; long-short 1.2%/month.
  * Engelberg-Reed-Ringgenberg (2018): short-selling risk.
  * Drechsler-Drechsler (2014): shorting premium tied to fees.
  * Wang-Yan-Zheng (2020): FINRA daily shorting flows, 2010-2015. **Long-term shorting flows predict negative returns for about a year; abnormal short-term flows do not.**
  * Chen-Da-Huang (2022): short-selling efficiency.
* **Decay and regime.** Borrow fees largely consume the short leg (Muravyev-Pearson-Pollet 2025). The January 2021 squeezes were coordinated retail buying with options activity and lending constraints central (Allen et al. 2025; Pedersen 2022; Barber et al. 2022). This fits TRAIN HAC t of -0.56, -0.91 and -1.27.

  *[est]* gross SR 0.3-0.7; net of fees 0.1-0.4.
* **DSL flags.**
  * `si_change` cites Rapach-Ringgenberg-Zhou (2016), which is an *aggregate* market-timing result, not cross-sectional.
  * A 21-day change of semi-monthly SI is noisy (tau .092).
  * FINRA daily short volume (warehouse) is contaminated by market-maker shorting. The FINRA and OTC Markets notes put the average ratio at ~46% of off-exchange volume, so only long-window aggregates carry signal.
* **Proposal.**
  * (i) Keep DTC, computed with a 63-126 day average turnover denominator. Weight the long leg as much as the short leg.
  * (ii) Replace `si_change` with **long-horizon shorting flow**: -(126-252 day mean of FINRA short volume / total volume), industry-demeaned.
  * (iii) Add an **SI x low-IO constraint** member from 13F: SIR / max(IO, floor), which proxies lending-supply utilization (Asquith-Pathak-Ritter; Nagel 2005).
  * (iv) Squeeze guard: cap short positions in the top 1-2% SIR and top retail-attention names.

### 3.9 value (`cfp`, `fcfp`, `net_payout`, `rd_me`; `bm`, `ebit_ev`, `ep`, `sp` rejected as redundant with `cfp`)

* **Canon:**
  * Basu (1977); Rosenberg-Reid-Lanstein (1985); Fama-French (1992); Lakonishok-Shleifer-Vishny (1994).
  * Asness-Frazzini (2013): use *current* price; 305-378 bps/yr alpha against a model that already contains standard HML.
  * Israel-Laursen-Richardson (2021): criticisms of systematic value have little support; composite and intra-industry measures are better.
  * Eisfeldt-Kim-Papanikolaou (2022): capitalized-SG&A intangible value (HML^INT) earns substantially higher returns, incl. recent decades.
  * Arnott-Harvey-Kalesnik-Linnainmaa (2021): HML's 55% drawdown to mid-2020 is explained by intangibles and valuation-spread compression.
  * Chan-Lakonishok-Sougiannis (2001): R&D/ME.
  * Goncalves-Leonard (2023): fundamental-to-market ratio.
* **Decay.**
  * Fama-French (2021): the value premium is much lower in 1991-2019 but not statistically different.
  * Novy-Marx-Medhat (2025): profitability explains half of value's post-2007 underperformance and all of alt-value's edge.
  * Ehsani-Harvey-Li (2023): sector neutrality helps long-short investors, which supports FF12 `group_rank`.

  *[est]* post-2010 gross SR 0.1-0.4, highly regime-dependent: 2020 crash, 2021-22 rebound.
* **Interactions.** Value is negatively correlated with momentum (AMP 2013) and with profitability (Novy-Marx 2013), and shares a duration factor with profitability, investment, low-risk and payout (Gormsen-Lazarus 2023).
* **DSL flags.** The |rho| > .90 redundancy screen discards members that are useful for *averaging out measurement noise* (Israel et al. 2021; DeMiguel et al. 2020 on netting). `net_payout` is economically issuance (section 3.2). `ep` is used for all firms; Basu, FF and positive-earnings E/P conventions differ, so negative earnings need an indicator.
* **Proposal.** One composite member = mean of within-FF12 z-scores of: CFO/ME, E/P (with a negative-E flag), S/EV, EBIT/EV, and (BE + intangibles)/ME. Intangibles use perpetual inventory of R&D (15% depreciation) and 30% of SG&A (20% depreciation), per EKP / Peters-Taylor. All ratios use the current price. Keep `rd_me` and `fcfp` as separate members. Because profitability already sits in the book, expect the intangible adjustment to add less than the standalone EKP result (Novy-Marx-Medhat 2025).

---

## 4. Missing families (buildable from fundamentals, price/volume, shares outstanding, 13F, FINRA short volume)

Columns: definition in words; turnover class; *[est]* standalone gross SR (long-short, liquid US, post-2010); evidence grade; overlap with existing themes.

| family | definition | turnover | gross SR *[est]* | evid. | overlap / note | cite |
|---|---|---|---|---|---|---|
| Residual (idiosyncratic) momentum | 12-2 month sum of residuals vs market + industry, / residual vol | low-mid (monthly) | 0.4-0.7 | B+ | replaces raw momentum members | Blitz-Huij-Martens 2011; Blitz-Hanauer-Vidojevic 2020 |
| Cash-based operating profitability | section 3.6 | low | 0.4-0.7 | A- | subsumes accruals | Ball et al. 2016; Detzel et al. 2023 |
| Expected growth (q5 Eg) | cross-sectional forecast of 1-3y investment growth from Tobin's q, CFO/assets and dROE (rolling FM slopes); long high Eg | low | 0.4-0.7 (in-sample 0.84%/month, t 10.3, 1967-2018) | B | overlaps profitability and earnings momentum | Hou-Mo-Xue-Zhang 2021 |
| Composite issuance (5y) / XFIN | section 3.2 | very low | 0.3-0.5 | A- | net_payout | Daniel-Titman 2006; Bradshaw-Richardson-Sloan 2006; Pontiff-Woodgate 2008 |
| Long-horizon shorting flow | -(6-12 month mean FINRA short volume / volume) | low | 0.3-0.5 | B- (2010-15 only) | SI theme | Wang-Yan-Zheng 2020 |
| SI x low IO (13F) | SIR / IO, or SIR within low residual-IO names | low | 0.3-0.6 gross; fee-exposed | B | SI; needs borrow cost | Asquith-Pathak-Ritter 2005; Nagel 2005 |
| 13F breadth change | quarterly change in number of 13F holders / holders last quarter, 45-day filing lag | very low | 0.1-0.4 | C+ (1979-98 mutual fund data; post-2010 mixed; results reportedly differ for increases vs decreases) | IO | Chen-Hong-Stein 2002 |
| Institutional demand as contrarian | 4-quarter change in 13F IO; short heavy institutional buying of anomaly-short names | very low | 0.1-0.3 | C | anomaly shorts | Edelen-Ince-Kadlec 2016; Sias-Starks-Titman 2006 |
| Hedge-fund conviction/consensus (13F) | portfolio-weight conviction x number of HF holders | low | long-only SR 0.75 vs S&P 2004-19 (not L/S) | C | crowding risk | Angelini-Iqbal-Jivraj 2019; Cohen-Polk-Silli 2010 |
| Connected-stock reversal (13F) | return of stocks sharing active-fund owners, used cross-stock | mid | 0.2-0.5 | C (pre-2010) | reversal | Anton-Polk 2014 |
| Earnings seasonality | rank of this upcoming quarter's historical share of annual earnings (last 5 years); long "positive-seasonality" quarters into announcement | event, mid | 0.2-0.5 | B- | needs expected announcement date (vendor earnFlag history) | Chang-Hartzmark-Solomon-Soltes 2017 |
| Earnings announcement premium | long stocks expected to announce in the next month (from last year's date), short non-announcers | high (monthly rotation) | 0.4-0.8 gross; poor net at $1bn | B | use only as a timing tilt | Frazzini-Lamont 2007; Barber et al. 2013 |
| Cash-flow duration | Dechow-Sloan-Soliman implied duration from ROE and sales-growth mean-reversion forecasts; long short-duration | low | 0.3-0.6 (Weber: 1.10%/month L/S, concentrated in short-constrained stocks) | B | value/profitability/low-risk (Gormsen-Lazarus); 2020 pandemic duration risk (Dechow et al. 2021) | Dechow-Sloan-Soliman 2004; Weber 2018; Gormsen-Lazarus 2023 |
| Intangible-adjusted value | section 3.9 | low | 0.2-0.4 | B | explained by profitability (Novy-Marx-Medhat 2025) | Eisfeldt-Kim-Papanikolaou 2022; Arnott et al. 2021 |
| Profitability trend | slope of ROA over last 8 quarters | low | 0.2-0.4 | B | earnings momentum | Akbas-Jiang-Koch 2017 |
| Short-term momentum (high-turnover names) | prior-month return x 1{top turnover tercile} | high | 0.4-0.7 gross; survives costs in large caps per authors | B | fold into reversal theme | Medhat-Schmeling 2022 |
| BAC / SMAX | section 3.3 | low | 0.3-0.6 | B+ | replaces low_risk members | Asness-Frazzini-Gormsen-Pedersen 2020 |
| Distress (failure probability) | Campbell-Hilscher-Szilagyi logit of leverage, profitability, volatility, cash, price | low | 0.2-0.5, crashes in junk rallies (2009, 2020-21) | B- | quality, low-risk | Campbell-Hilscher-Szilagyi 2008 |

Prioritization for a $1bn book: CbOP, residual momentum and 5-year issuance/XFIN are low-effort upgrades. Expected growth and long-horizon shorting flow are the most promising genuinely new slow signals. The 13F signals are cheap to trade but weak post-2010. The announcement-premium family is best used as a trade-timing tilt, not a sleeve.

Diversification arithmetic: with average pairwise theme correlation ~0.1 and per-theme SR ~0.4, EW SR is 0.4 x sqrt(9/1.8) = 0.89 for 9 themes. At 12 themes it is 0.4 x sqrt(12/2.1) = 0.96, i.e. +8% gross from three new independent themes.

---

## 5. Combination methodology (~40 signals, ~750 days)

1. **Estimation error dominates alpha-side optimization.**
   * DeMiguel-Garlappi-Uppal (2009): sample mean-variance needs ~3000 months to beat 1/N with 25 assets and ~6000 with 50.
   * Kan-Zhou (2007): plug-in tangency weights are never optimal under estimation risk.
   * Squared-SR inflation ≈ N/T per period. For 38 signals and 36 months that is ~1.06 per month (≈12.7 annualized); for 9 themes it is ≈3.0 annualized. Both exceed the true SR^2 of ~1.4.
   * Conclusion: MVE or IC-optimized weights on TRAIN are noise-fitted. The v3 result (TRAIN 1.81 → VAL -1.27) is the textbook outcome.
2. **What is estimable and should be used.** Signal persistence, turnover and aim gain (already computed as g_k), cost per $, and covariance of sleeve returns with Ledoit-Wolf (2004) shrinkage. Means should come from priors: literature post-publication SR, JKP-style cluster shrinkage, and the ~50% out-of-sample haircut (McLean-Pontiff 2016; Falck-Rej-Thesmar 2022; Chen-Lopez-Lira-Zimmermann 2022, which finds about half of predictability persists whether peer-reviewed or data-mined, and theory does not help).
3. **Theme-level beats signal-level weighting** under this data constraint. JKP (2023) cluster 153 factors into 13 themes, estimate alphas hierarchically, and find most themes are significant parts of the tangency portfolio. Within a theme, **average rather than prune**: correlated members reduce measurement noise, and their trades net against each other (DeMiguel et al. 2020: with costs, the number of significant characteristics rises from 6 to 15 because trades cancel).
4. **Recommended recipe.**
   * w_theme ∝ Σ_LW^-1 (s_prior ∘ σ). This is MVE with *prior* means and a shrunk covariance, which is stable because nothing mean-like is estimated.
   * Blend 50/50 with inverse-vol and cap to [0.5, 2] x EW.
   * Multiply by the GP capture (1 + φ_k·a/γ)^-1, where φ_k is the signal's mean-reversion estimated from signal autocorrelation. This is precise.
   * Validate only by paired dSR (Memmel / LW bootstrap) with prior-consistent sign.
5. **IC-weighting** (Grinold-Kahn, Qian-Hua-Sorensen). Correct form: w ∝ Ω_IC^-1 IC, with IC shrunk toward the prior. With ~36 non-overlapping monthly ICs per signal, IC t-stats of 1-2 make raw IC weights mostly noise. Use at most 25-50% data weight.
6. **Garleanu-Pedersen dynamic trading** (JF 2013): "aim in front of the target, trade partially toward the aim"; slower-decaying signals get more aim weight. Extensions:
   * Collin-Dufresne-Daniel-Saglam (2020): trade faster in persistent, liquid states.
   * Brandt-Santa-Clara-Valkanov (2009): parametric portfolio policies with costs.
   * JKMP (2026): learning portfolio weights directly on a net-of-cost objective gives net SR ~1.38 where cost-agnostic Markowitz-ML (gross ~2.0) is not implementable.

   The current `aim-partial-v5` implements the "trade partially" half. The "aim by persistence" half (`ew-theme-aim-v1`) lost on TRAIN (-0.126 +/- 0.112), but TRAIN favours fast liquidity-provision signals (Nagel 2012) and the test is 1.1 SE. Re-test it on the lever-2 construction and with slower rebuilt fast sleeves, not as a multiplicative g_k on the old ones.
7. **ML / alpha stacking.**
   * Gu-Kelly-Xiu (2020) gains need decades of panel.
   * Avramov-Cheng-Metzker (2023): ML profitability is concentrated in microcaps, distressed stocks and high-volatility episodes, and deteriorates with realistic costs because of turnover and extreme positions.
   * Leung et al. (2021) and Green-Hand-Zhang (2017): post-2003 independent predictability in non-microcaps collapsed to two characteristics.

   Verdict: no nonlinear stacking on 750 days. If attempted, only a cost-aware objective on a long pre-2020 panel with a TRAIN-only hold-out.
8. **Turnover-aware smoothing.** Replace uniform `decay_linear(.,21)` with signal-specific smoothing chosen to maximize a lagged-IC-minus-turnover-cost criterion (Qian-Sorensen-Hua 2007; horizon IC vs lagged IC). Slow fundamentals need ~0-5 days: sample weekly or monthly and hold. Fast signals need either their own trading path or a much smaller weight.

**What gains the most net SR at N≈40, T≈750:** fixed prior-based theme weights plus cost-aware construction, with data used for cost and decay parameters only. Evidence: A (DeMiguel 2009/2020; Kan-Zhou; JKP; JKMP). ML and mean-optimization are expected to be negative.

---

## 6. Transaction-cost-aware construction at $1bn

**Cost models:**

* **Frazzini-Israel-Moskowitz** (2012/2018). $1.7 trillion of live institutional trades across 21 markets over 19 years. The key driver of impact is trade size as a fraction of daily volume, with a square-root plus linear specification. Realized costs are about an order of magnitude below prior academic estimates. International realized costs are ~16 bps. Break-even fund sizes are tens of $bn for value and momentum (UMD ~$52bn) and far smaller for short-term reversal.
* **Almgren-Thum-Hauptmann-Li** (2005). Temporary impact is ∝ (trade rate)^~0.6 and permanent impact is linear.
* **Kyle-Obizhaeva** (2016) invariance. Costs in bps scale with volatility and fall with trading activity W = σ·P·V roughly as W^(-1/3); impact rises with order size / volume.
* **Square-root law** (Toth et al. 2011).

**Anomaly-level results:**

* **Novy-Marx-Velikov** (2016):
  * anomalies with one-sided monthly turnover below 50% mostly survive when cost-mitigated; few above;
  * the buy/hold spread is the single most effective simple mitigation;
  * size, value and profitability have the most capacity.
* **Novy-Marx-Velikov** (2019, FAJ). Banding beats lower rebalancing frequency (similar cost cut, better signal exposure). Both beat restricting to cheap-to-trade names, which hurts gross the most.
* **DeMiguel et al.** (2020). Combining characteristics nets trades.
* **Patton-Weller** (2020). Live mutual funds realize low value premia and none of momentum after implementation costs.
* **Chen-Velikov** (2023). Net post-publication ~4 bps/month on average and ~20 bps/month for combinations.
* **Korajczyk-Sadka** (2004). Momentum capacity.

**Where our 13 bps comes from** *[est, illustrative]*: $44m/day traded over ~2000 names averages ~$22k per name per day.

* A $5m-ADV name: 0.44% of ADV, so sqrt impact ≈ 2.5% x 0.066 ≈ 17 bps plus a 10-20 bp half-spread, ~30 bps in total.
* A $200m-ADV name: ~2 bps impact plus ~1-2 bps spread.

Under near-equal rank weights the average per-$ cost is dominated by the illiquid tail and half-spreads, not by large-cap impact. The mitigations with the largest leverage therefore act on per-name cost heterogeneity.

**Mitigations ranked for a 4.4%/day book** *[est]*:

1. **Cost-penalized targets plus a cost-scaled buy/hold band** (lever 2). Maximize α'w - Σ_i c_i(Δw_i) - (γ/2) w'Σw, with c_i ∝ half-spread_i \|Δw_i\| + k σ_i \|Δw_i\|^1.5 / sqrt(ADV_i). Linear costs imply a no-trade region (Constantinides 1986) and concave impact implies partial trading (GP 2013), so combine a band with theta. Expect -25 to -45% cost for -3 to -8% gross, i.e. +0.06 to +0.15 SR. The earlier `per-name` theta test (-0.195) slowed illiquid names without shrinking their targets. That is not the GP/NMV mechanism.
2. **Fast-sleeve re-weighting or redesign** (lever 1): -20 to -35% turnover. Seen from the cost side this is DeMiguel's netting argument plus NMV's turnover taxonomy.
3. **Refresh cadence for slow members.** Sample fundamentals weekly or monthly, not daily re-rank with daily price denominators. Standalone tau of .015-.025/day for pure fundamentals (≈4-6x/yr) is several times academic annual-rebalance turnover, which points to rank-churn noise. Expect +0.01 to +0.04.
4. **Participation caps** (<= 1% ADV per name-day) and closing-auction or patient execution. FIM show that costs depend on participation and style. Backtest gains depend on the cost model, so treat as C.
5. **Soft liquidity floor** (penalty rather than exclusion): 0 to +0.05. NMV (2019) show that hard universe cuts lose more gross than they save.

**Capacity.** At $1bn, slow themes (value, profitability, investment, issuance, SI-DTC) are far below FIM/NMV break-evens. Reversal, IV-spread and seasonality are the capacity-bound sleeves.

---

## 7. Regime robustness 2020-2024 and what predicts persistence

| family | 2020 (COVID crash / rebound / 9 Nov) | 2021 (meme / growth → value) | 2022 (rates, value rally) | 2023-24 (mega-cap concentration) | sources (grade) |
|---|---|---|---|---|---|
| value | deep drawdown into mid-2020 (HML -55% from 2007); sharp rally from 9 Nov | rebound | strong | weak | Arnott et al. 2021; Blitz 2021 (B) |
| momentum | worked until a record one-day crash on 9 Nov | choppy | mixed | strong (AI leaders) | Bloomberg / MSCI 2020; Daniel-Moskowitz 2016 (B/C) |
| profitability / quality | defensive in the crash; financially stronger firms fell less | ok | ok vs market, hurt by growth-quality overlap | ok (mega-caps are profitable) | Ding et al. 2021; Novy-Marx-Medhat 2025 (B) |
| low risk | lagged rebounds | lagged | held up (duration-like, but short-duration factors outperformed) | lagged | Gormsen-Lazarus 2023; MSCI/industry (C) |
| short interest | mixed | **negative in Jan-2021 squeezes** | ok | mixed | Allen et al. 2025; Pedersen 2022; Barber et al. 2022 (B) |
| reversal | strong (high VIX) | ok | strong (high VIX) | weaker (low VIX) | Nagel 2012; Hameed-Mian 2015 (A for mechanism) |
| duration (value/profit/invest/payout) | long-duration winners | turning | short duration outperforms as rates rise | long-duration mega-caps lead | Gormsen-Lazarus 2023; Dechow et al. 2021 (B) |

Blitz (2021): in the 2018-2020 "quant crisis", nearly the only winning strategy was the largest, most expensive growth stocks. Mega-caps were 55% of Russell 1000 returns in 2023 and 48% in 2024 (Russell Investments). The book's TRAIN years (+0.2% / +3.9% / +3.4%) and the VAL behaviour are consistent with these patterns.

**Implication.** Value, profitability, investment and low-risk share a *duration* factor (Gormsen-Lazarus 2023), so the nine "independent" themes carry fewer independent bets than their count suggests. Adding duration-orthogonal families helps diversification: residual momentum, shorting flow, earnings/EAR, expected growth.

**What predicts out-of-sample persistence:**

1. **Publication and time decay.** Returns are ~26% lower out-of-sample and ~58% lower post-publication (McLean-Pontiff 2016). About 50% of in-sample performance survives (Falck-Rej-Thesmar 2022; Chen-Lopez-Lira-Zimmermann 2022). Decay is larger for liquid, cheap-to-arbitrage anomalies (McLean-Pontiff; Chordia-Subrahmanyam-Tong 2014) and where hedge-fund capital flowed in (Green-Hand-Soliman 2011 on accruals). A liquid-universe book faces the decayed version.
2. **Cluster-level replication.** JKP (2023): Bayesian US replication rate ~82-84%; clusters work out of sample in 93 countries, and evidence strengthens with the number of factors. Favour clusters with global support: value, momentum, profitability/quality, investment, low risk, seasonality, accruals, debt issuance. Avoid single-paper signals.
3. **Overfitting proxies.** Complexity and in-sample SR predict decay (Falck et al. 2022). Harvey-Liu-Zhu (2016) set a t > 3 hurdle. The v3 overfit is a local instance.
4. **Harvestability, not existence.** Alpha concentrated in high-fee or illiquid names persists precisely because it cannot be harvested (Muravyev-Pearson-Pollet 2025; Asquith-Pathak-Ritter 2005; Avramov et al. 2023).
5. **Factor momentum.** Evidence for persistence: the average factor earns 53 bps/month after a positive year vs 1 bp after a negative year (Ehsani-Linnainmaa 2022); time-series factor momentum has SR 0.84 (Gupta-Kelly 2019). It crashes when factor autocorrelation breaks (Nov 2020). Use it only as a mild, shrunk tilt (lever 7).
6. **Factor timing on valuation spreads.** Mostly disappointing out of sample in a multi-style book (Asness-Chandra-Ilmanen-Israel 2017; Ilmanen et al. 2021: modest predictability unlikely to beat frictions). Haddad-Kozak-Santosh (2020) report strong PC-based timing, but with estimation risk. Do not time themes on valuation.
7. **Known conditional regularities** worth encoding as risk rules rather than alpha: reversal ∝ VIX (Nagel 2012); momentum crashes in post-bear rebounds (Daniel-Moskowitz 2016); vol-managed momentum (Barroso-Santa-Clara 2015).

---

## 8. Bibliography (author, year, title, link)

**Costs, construction, capacity**
- Novy-Marx, R., Velikov, M. (2016). A Taxonomy of Anomalies and Their Trading Costs. RFS 29(1). https://academic.oup.com/rfs/article-abstract/29/1/104/1844518 ; NBER w20721 https://www.nber.org/papers/w20721
- Novy-Marx, R., Velikov, M. (2019). Comparing Cost-Mitigation Techniques. FAJ 75(1). https://www.tandfonline.com/doi/abs/10.1080/0015198X.2018.1547057
- Frazzini, A., Israel, R., Moskowitz, T. (2018). Trading Costs. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3229719
- Frazzini, A., Israel, R., Moskowitz, T. (2012/2015). Trading Costs of Asset Pricing Anomalies. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2294498
- Garleanu, N., Pedersen, L.H. (2013). Dynamic Trading with Predictable Returns and Transaction Costs. JF 68(6). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12080
- DeMiguel, V., Martin-Utrera, A., Nogales, F., Uppal, R. (2020). A Transaction-Cost Perspective on the Multitude of Firm Characteristics. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/2180/5821387
- Jensen, T.I., Kelly, B., Malamud, S., Pedersen, L.H. (2026). Machine Learning and the Implementable Efficient Frontier. RFS. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4187217
- Detzel, A., Novy-Marx, R., Velikov, M. (2023). Model Comparison with Transaction Costs. JF 78(3). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13225
- Chen, A., Velikov, M. (2023). Zeroing In on the Expected Returns of Anomalies. JFQA. https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/zeroing-in-on-the-expected-returns-of-anomalies/945133D5A3ECEEAF466AEE91551FD225
- Patton, A., Weller, B. (2020). What You See Is Not What You Get: The Costs of Trading Market Anomalies. JFE 137(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X20300453
- Kyle, A., Obizhaeva, A. (2016). Market Microstructure Invariance: Empirical Hypotheses. Econometrica 84(4). https://onlinelibrary.wiley.com/doi/abs/10.3982/ECTA10486
- Almgren, R., Thum, C., Hauptmann, E., Li, H. (2005). Direct Estimation of Equity Market Impact. Risk. https://www.researchgate.net/publication/228754794_Direct_Estimation_of_Equity_Market_Impact
- Toth, B. et al. (2011). Anomalous Price Impact and the Critical Nature of Liquidity. PRX 1. https://arxiv.org/abs/1105.1694
- Korajczyk, R., Sadka, R. (2004). Are Momentum Profits Robust to Trading Costs? JF 59(3). https://doi.org/10.1111/j.1540-6261.2004.00656.x
- Constantinides, G. (1986). Capital Market Equilibrium with Transaction Costs. JPE 94(4). https://doi.org/10.1086/261386
- Collin-Dufresne, P., Daniel, K., Saglam, M. (2020). Liquidity Regimes and Optimal Dynamic Asset Allocation. JFE 136(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X19302302
- Brandt, M., Santa-Clara, P., Valkanov, R. (2009). Parametric Portfolio Policies. RFS 22(9). https://academic.oup.com/rfs/article-abstract/22/9/3411/1572695
- Qian, E., Sorensen, E., Hua, R. (2007). Information Horizon, Portfolio Turnover, and Optimal Alpha Models. JPM 34(1). https://www.pm-research.com/content/iijpormgmt/34/1/27
- Muravyev, D., Pearson, N., Pollet, J. (2025). Anomalies and Their Short-Sale Costs. JF 80(6). https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501
- Muravyev, D., Pearson, N., Pollet, J. (2025). Why Does Options Market Information Predict Stock Returns? JFE. https://www.sciencedirect.com/science/article/pii/S0304405X25001618
- Drechsler, I., Drechsler, Q.F. (2014). The Shorting Premium and Asset Pricing Anomalies. NBER w20282. https://www.nber.org/papers/w20282
- Engelberg, J., Reed, A., Ringgenberg, M. (2018). Short-Selling Risk. JF 73. https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12601

**Replication, decay, combination, statistics**
- McLean, R.D., Pontiff, J. (2016). Does Academic Research Destroy Stock Return Predictability? JF 71(1). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365
- Jensen, T.I., Kelly, B., Pedersen, L.H. (2023). Is There a Replication Crisis in Finance? JF 78(5). https://onlinelibrary.wiley.com/doi/10.1111/jofi.13249 ; https://www.nber.org/papers/w28432
- Hou, K., Xue, C., Zhang, L. (2020). Replicating Anomalies. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/2019/5236964
- Chen, A., Zimmermann, T. (2022). Open Source Cross-Sectional Asset Pricing. CFR 11(2). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3604626 ; https://www.openassetpricing.com/
- Chen, A., Lopez-Lira, A., Zimmermann, T. (2022). Does Peer-Reviewed Research Help Predict Stock Returns? WP. https://arxiv.org/abs/2212.10317
- Falck, A., Rej, A., Thesmar, D. (2022). When Systematic Strategies Decay. Quantitative Finance 22. https://arxiv.org/abs/2105.01380
- Harvey, C., Liu, Y., Zhu, H. (2016). ...and the Cross-Section of Expected Returns. RFS 29(1). https://academic.oup.com/rfs/article/29/1/5/1843824
- Chordia, T., Subrahmanyam, A., Tong, Q. (2014). Have Capital Market Anomalies Attenuated in the Recent Era of High Liquidity and Trading Activity? JAE 58(1). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2029057
- Green, J., Hand, J., Zhang, X.F. (2017). The Characteristics that Provide Independent Information about Average U.S. Monthly Stock Returns. RFS 30(12). https://academic.oup.com/rfs/article-abstract/30/12/4389/3091648
- DeMiguel, V., Garlappi, L., Uppal, R. (2009). Optimal Versus Naive Diversification. RFS 22(5). https://academic.oup.com/rfs/article-abstract/22/5/1915/1592901
- Kan, R., Zhou, G. (2007). Optimal Portfolio Choice with Parameter Uncertainty. JFQA 42(3). https://www-2.rotman.utoronto.ca/~kan/papers/erisk8.pdf
- Ledoit, O., Wolf, M. (2004). Honey, I Shrunk the Sample Covariance Matrix. JPM 30(4). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=433840
- Kozak, S., Nagel, S., Santosh, S. (2020). Shrinking the Cross-Section. JFE 135(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301655
- Lewellen, J. (2015). The Cross-section of Expected Stock Returns. CFR 4(1). https://www.nowpublishers.com/article/Details/CFR-0024
- Gu, S., Kelly, B., Xiu, D. (2020). Empirical Asset Pricing via Machine Learning. RFS 33(5). https://doi.org/10.1093/rfs/hhaa009
- Avramov, D., Cheng, S., Metzker, L. (2023). Machine Learning vs. Economic Restrictions: Evidence from Stock Return Predictability. Mgmt Sci 69(5). https://pubsonline.informs.org/doi/abs/10.1287/mnsc.2022.4449
- Lo, A. (2002). The Statistics of Sharpe Ratios. FAJ 58(4). https://doi.org/10.2469/faj.v58.n4.2453
- Ehsani, S., Harvey, C., Li, F. (2023). Is Sector Neutrality in Factor Investing a Mistake? FAJ 79(3). https://www.tandfonline.com/doi/abs/10.1080/0015198X.2023.2196931

**Factor timing, factor momentum, regimes**
- Ehsani, S., Linnainmaa, J. (2022). Factor Momentum and the Momentum Factor. JF 77(3). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13131
- Gupta, T., Kelly, B. (2019). Factor Momentum Everywhere. JPM 45(3). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3300728
- Asness, C., Chandra, S., Ilmanen, A., Israel, R. (2017). Contrarian Factor Timing is Deceptively Difficult. JPM 43(5). https://jpm.pm-research.com/content/43/5/72
- Ilmanen, A., Israel, R., Moskowitz, T., Thapar, A., Lee, R. (2021). How Do Factor Premia Vary Over Time? A Century of Evidence. JOIM. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3400998
- Haddad, V., Kozak, S., Santosh, S. (2020). Factor Timing. RFS 33(5). https://academic.oup.com/rfs/article-abstract/33/5/1980/5753962
- Nagel, S. (2012). Evaporating Liquidity. RFS 25(7). https://academic.oup.com/rfs/article-abstract/25/7/2005/1602153
- Blitz, D. (2021). The Quant Crisis of 2018-2020: Cornered by Big Growth. JPM 47(6). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3774164
- Pedersen, L.H. (2022). Game On: Social Networks and Markets. JFE 146(3). https://www.sciencedirect.com/science/article/pii/S0304405X22000964
- Barber, B., Huang, X., Odean, T., Schwarz, C. (2022). Attention-Induced Trading and Returns: Evidence from Robinhood Users. JF 77(6). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13183
- Allen, F., Haas, M., Nowak, E., Pirovano, M., Tengulov, A. (2025). Squeezing Shorts Through Social Media Platforms. Mgmt Sci. https://pubsonline.informs.org/doi/10.1287/mnsc.2023.02887
- Dechow, P. et al. (2021). Implied Equity Duration: A Measure of Pandemic Shutdown Risk. JAR. https://onlinelibrary.wiley.com/doi/10.1111/1475-679X.12348
- Russell Investments (2024). The Magnificent Seven: Market Concentrations and Complications. https://russellinvestments.com/content/ri/us/en/insights/russell-research/2024/10/the-magnificent-seven-market-concentrations-and-complications-.html
- Bloomberg (2020-11-09). Momentum Trade Plunges the Most on Record in Rotation Frenzy. https://www.bloomberg.com/news/articles/2020-11-09/momentum-trade-plunges-the-most-on-record-in-rotation-frenzy
- MSCI (2020). Are Momentum's Wings Finally Starting to Melt? https://www.msci.com/research-and-insights/blog-post/are-momentum-wings-finally-starting-to-melt

**Theme papers: value / profitability / investment / duration**
- Asness, C., Frazzini, A. (2013). The Devil in HML's Details. JPM. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2054749
- Israel, R., Laursen, K., Richardson, S. (2021). Is (Systematic) Value Investing Dead? JPM 47(2). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3554267
- Eisfeldt, A., Kim, E., Papanikolaou, D. (2022). Intangible Value. CFR 11(2). https://www.nber.org/papers/w28056
- Arnott, R., Harvey, C., Kalesnik, V., Linnainmaa, J. (2021). Reports of Value's Death May Be Greatly Exaggerated. FAJ 77(1). https://www.tandfonline.com/doi/full/10.1080/0015198X.2020.1842704
- Fama, E., French, K. (2021). The Value Premium. RAPS 11(1). https://academic.oup.com/raps/article-abstract/11/1/105/6033665
- Goncalves, A., Leonard, G. (2023). The Fundamental-to-Market Ratio and the Value Premium Decline. JFE. https://www.sciencedirect.com/science/article/abs/pii/S0304405X22002276
- Novy-Marx, R. (2013). The Other Side of Value: The Gross Profitability Premium. JFE 108(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X13000044
- Novy-Marx, R., Medhat, M. (2025). Profitability Retrospective: What Have We Learned? NBER w33601. https://www.nber.org/papers/w33601
- Ball, R., Gerakos, J., Linnainmaa, J., Nikolaev, V. (2016). Accruals, Cash Flows, and Operating Profitability in the Cross Section of Stock Returns. JFE 121(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X16300307
- Asness, C., Frazzini, A., Pedersen, L.H. (2019). Quality Minus Junk. RAS 24(1). https://link.springer.com/article/10.1007/s11142-018-9470-2
- Fama, E., French, K. (2015). A Five-Factor Asset Pricing Model. JFE 116(1). https://doi.org/10.1016/j.jfineco.2014.10.010
- Hou, K., Xue, C., Zhang, L. (2015). Digesting Anomalies: An Investment Approach. RFS 28(3). https://doi.org/10.1093/rfs/hhu068
- Hou, K., Mo, H., Xue, C., Zhang, L. (2021). An Augmented q-Factor Model with Expected Growth. RoF 25(1). https://academic.oup.com/rof/article-abstract/25/1/1/5727769
- Green, J., Hand, J., Soliman, M. (2011). Going, Going, Gone? The Apparent Demise of the Accruals Anomaly. Mgmt Sci 57(5). https://pubsonline.informs.org/doi/abs/10.1287/mnsc.1110.1320
- Hafzalla, N., Lundholm, R., Van Winkle, E.M. (2011). Percent Accruals. TAR 86(1). https://www.ssrn.com/abstract=1558464
- Akbas, F., Jiang, C., Koch, P. (2017). The Trend in Firm Profitability and the Cross-Section of Stock Returns. TAR 92(5). https://searchworks-lb.stanford.edu/articles/bth__125259428
- Cooper, M., Gulen, H., Schill, M. (2008). Asset Growth and the Cross-Section of Stock Returns. JF 63(4). https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01370.x
- Hirshleifer, D., Hou, K., Teoh, S.H., Zhang, Y. (2004). Do Investors Overvalue Firms with Bloated Balance Sheets? JAE 38. https://doi.org/10.1016/j.jacceco.2004.10.002
- Pontiff, J., Woodgate, A. (2008). Share Issuance and Cross-sectional Returns. JF 63(2). https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01335.x
- Daniel, K., Titman, S. (2006). Market Reactions to Tangible and Intangible Information. JF 61(4). https://doi.org/10.1111/j.1540-6261.2006.00884.x
- Bradshaw, M., Richardson, S., Sloan, R. (2006). The Relation between Corporate Financing Activities, Analysts' Forecasts and Stock Returns. JAE 42. https://www.sciencedirect.com/science/article/abs/pii/S0165410106000371
- Dechow, P., Sloan, R., Soliman, M. (2004). Implied Equity Duration: A New Measure of Equity Risk. RAS 9. https://link.springer.com/article/10.1023/B:RAST.0000028186.44328.3f
- Weber, M. (2018). Cash Flow Duration and the Term Structure of Equity Returns. JFE 128(3). https://www.sciencedirect.com/science/article/abs/pii/S0304405X18300667
- Gormsen, N.J., Lazarus, E. (2023). Duration-Driven Returns. JF 78(3). https://onlinelibrary.wiley.com/doi/10.1111/jofi.13216
- Campbell, J., Hilscher, J., Szilagyi, J. (2008). In Search of Distress Risk. JF 63(6). https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1540-6261.2008.01416.x

**Theme papers: momentum / earnings / reversal / seasonality**
- Jegadeesh, N., Titman, S. (1993). Returns to Buying Winners and Selling Losers. JF 48(1). https://doi.org/10.1111/j.1540-6261.1993.tb04702.x
- Moskowitz, T., Grinblatt, M. (1999). Do Industries Explain Momentum? JF 54(4). https://onlinelibrary.wiley.com/doi/abs/10.1111/0022-1082.00146
- George, T., Hwang, C.-Y. (2004). The 52-Week High and Momentum Investing. JF 59(5). https://doi.org/10.1111/j.1540-6261.2004.00695.x
- Asness, C., Moskowitz, T., Pedersen, L.H. (2013). Value and Momentum Everywhere. JF 68(3). https://doi.org/10.1111/jofi.12021
- Blitz, D., Huij, J., Martens, M. (2011). Residual Momentum. JEF 18(3). https://www.sciencedirect.com/science/article/abs/pii/S0927539811000041
- Blitz, D., Hanauer, M., Vidojevic, M. (2020). The Idiosyncratic Momentum Anomaly. IREF 69. https://www.sciencedirect.com/science/article/abs/pii/S1059056020300927
- Da, Z., Gurun, U., Warachka, M. (2014). Frog in the Pan: Continuous Information and Momentum. RFS 27(7). https://academic.oup.com/rfs/article-abstract/27/7/2171/1578455
- Daniel, K., Moskowitz, T. (2016). Momentum Crashes. JFE 122(2). https://www.sciencedirect.com/science/article/pii/S0304405X16301490
- Barroso, P., Santa-Clara, P. (2015). Momentum Has Its Moments. JFE 116(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X14002566
- Novy-Marx, R. (2015). Fundamentally, Momentum is Fundamental Momentum. NBER w20984. https://www.nber.org/papers/w20984
- Brandt, M., Kishore, R., Santa-Clara, P., Venkatachalam, M. (2008). Earnings Announcements are Full of Surprises. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=909563
- Thomas, J., Zhang, F. (2011). Tax Expense Momentum. JAR 49. https://onlinelibrary.wiley.com/doi/abs/10.1111/j.1475-679X.2011.00409.x
- Martineau, C. (2022). Rest in Peace Post-Earnings Announcement Drift. CFR 11(3-4). https://www.nowpublishers.com/article/Details/CFR-0122
- UCLA Anderson Review (2025). Is Post-Earnings Announcement Drift a Thing? Again? (Subrahmanyam commentary). https://anderson-review.ucla.edu/is-post-earnings-announcement-drift-a-thing-again/
- Chang, T., Hartzmark, S., Solomon, D., Soltes, E. (2017). Being Surprised by the Unsurprising: Earnings Seasonality and Stock Returns. RFS 30(1). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460166
- Frazzini, A., Lamont, O. (2007). The Earnings Announcement Premium and Trading Volume. NBER w13090. https://www.nber.org/papers/w13090
- Barber, B., De George, E., Lehavy, R., Trueman, B. (2013). The Earnings Announcement Premium Around the Globe. JFE 108(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X12002188
- Da, Z., Liu, Q., Schaumburg, E. (2014). A Closer Look at the Short-Term Return Reversal. Mgmt Sci 60(3). https://www3.nd.edu/~zda/Reversal.pdf
- Hameed, A., Mian, G.M. (2015). Industries and Stock Return Reversals. JFQA 50(1-2). https://www.jstor.org/stable/43862244
- Blitz, D., Huij, J., Lansdorp, S., Verbeek, M. (2013). Short-Term Residual Reversal. JFM 16(3). https://www.sciencedirect.com/science/article/abs/pii/S1386418112000468
- de Groot, W., Huij, J., Zhou, W. (2012). Another Look at Trading Costs and Short-Term Reversal Profits. JBF 36(2). https://www.sciencedirect.com/science/article/abs/pii/S0378426611002263
- Medhat, M., Schmeling, M. (2022). Short-term Momentum. RFS 35(3). https://academic.oup.com/rfs/article-abstract/35/3/1480/6286969
- Heston, S., Sadka, R. (2008). Seasonality in the Cross-Section of Stock Returns. JFE 87(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X0700195X
- Keloharju, M., Linnainmaa, J., Nyberg, P. (2016). Return Seasonalities. JF 71(4). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12398
- Keloharju, M., Linnainmaa, J., Nyberg, P. (2021). Are Return Seasonalities Due to Risk or Mispricing? JFE. https://www.sciencedirect.com/science/article/abs/pii/S0304405X20301951

**Theme papers: low risk / options / short interest / ownership**
- Frazzini, A., Pedersen, L.H. (2014). Betting Against Beta. JFE 111(1). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2049939
- Novy-Marx, R., Velikov, M. (2022). Betting Against Betting Against Beta. JFE 143(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X21002051
- Asness, C., Frazzini, A., Gormsen, N.J., Pedersen, L.H. (2020). Betting Against Correlation. JFE 135(3). https://www.sciencedirect.com/science/article/abs/pii/S0304405X1930176X
- Novy-Marx, R. (2014). Understanding Defensive Equity. NBER w20591. https://www.nber.org/papers/w20591
- Liu, J., Stambaugh, R., Yuan, Y. (2018). Absolving Beta of Volatility's Effects. JFE 128(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X18300163
- Stambaugh, R., Yu, J., Yuan, Y. (2015). Arbitrage Asymmetry and the Idiosyncratic Volatility Puzzle. JF 70(5). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12286
- Bali, T., Brown, S., Murray, S., Tang, Y. (2017). A Lottery-Demand-Based Explanation of the Beta Anomaly. JFQA 52(6). https://www.cambridge.org/core/journals/journal-of-financial-and-quantitative-analysis/article/abs/lotterydemandbased-explanation-of-the-beta-anomaly/B5B9F0A65256E6E86B45D72AE0A256C4
- Ang, A., Hodrick, R., Xing, Y., Zhang, X. (2006). The Cross-Section of Volatility and Expected Returns. JF 61(1). https://doi.org/10.1111/j.1540-6261.2006.00836.x
- Bali, T., Cakici, N., Whitelaw, R. (2011). Maxing Out: Stocks as Lotteries. JFE 99(2). https://doi.org/10.1016/j.jfineco.2010.08.014
- Huang, W., Liu, Q., Rhee, S.G., Zhang, L. (2010). Return Reversals, Idiosyncratic Risk, and Expected Returns. RFS 23(1). https://academic.oup.com/rfs/article-abstract/23/1/147/1576727
- Bali, T., Hovakimian, A. (2009). Volatility Spreads and Expected Stock Returns. Mgmt Sci 55(11). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1443848
- Asquith, P., Pathak, P., Ritter, J. (2005). Short Interest, Institutional Ownership, and Stock Returns. JFE 78(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X05001170
- Boehmer, E., Huszar, Z., Jordan, B. (2010). The Good News in Short Interest. JFE 96(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X09002402
- Hong, H., Li, W., Ni, S., Scheinkman, J., Yan, P. (2015). Days to Cover and Stock Returns. NBER w21166. https://www.nber.org/papers/w21166
- Wang, X., Yan, X., Zheng, L. (2020). Shorting Flows, Public Disclosure, and Market Efficiency. JFE 135(1). https://www.sciencedirect.com/science/article/abs/pii/S0304405X19301436
- Chen, Y., Da, Z., Huang, D. (2022). Short Selling Efficiency. JFE 145(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X21003512
- FINRA. Daily Short Sale Volume Files (data description). https://www.finra.org/finra-data/browse-catalog/short-sale-volume-data/daily-short-sale-volume-files
- Nagel, S. (2005). Short Sales, Institutional Investors and the Cross-Section of Stock Returns. JFE 78(2). https://www.sciencedirect.com/science/article/abs/pii/S0304405X05000735
- Chen, J., Hong, H., Stein, J. (2002). Breadth of Ownership and Stock Returns. JFE 66(2-3). https://www.sciencedirect.com/science/article/abs/pii/S0304405X02002234
- Edelen, R., Ince, O., Kadlec, G. (2016). Institutional Investors and Stock Return Anomalies. JFE 119. https://www.sciencedirect.com/science/article/abs/pii/S0304405X16000039
- Sias, R., Starks, L., Titman, S. (2006). Changes in Institutional Ownership and Stock Returns: Assessment and Methodology. J. Business 79(6). https://www.jstor.org/stable/10.1086/508002
- Angelini, L., Iqbal, M., Jivraj, F. (2019). Systematic 13F Hedge Fund Alpha. WP. https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3459526
- Anton, M., Polk, C. (2014). Connected Stocks. JF 69(3). https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12149

*Items cited without a fetched link, as standard references: Basu (1977); Rosenberg-Reid-Lanstein (1985); Fama-French (1992, 2008); Lakonishok-Shleifer-Vishny (1994); Chan-Lakonishok-Sougiannis (2001); Sloan (1996); Piotroski (2000); Novy-Marx (2011, operating leverage); Grundy-Martin (2001); Bernard-Thomas (1989); Chan-Jegadeesh-Lakonishok (1996); Livnat-Mendenhall (2006); Cremers-Weinbaum (2010); Xing-Zhang-Zhao (2010); An-Ang-Bali-Cakici (2014); Boehmer-Jones-Zhang (2008); Diether-Lee-Werner (2009); Rapach-Ringgenberg-Zhou (2016); Cohen-Polk-Silli (2010); Peters-Taylor (2017); Ding-Levine-Lin-Xie (2021); Leung et al. (2021); Merton (1980). Verify the page references before external circulation.*
