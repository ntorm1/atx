# P9 literature review: raising Sharpe, gross return and capacity of the mega-alpha book

**Date** 2026-10-02. **For** the P9 sprint plan (input to the PM's pre-registration; nothing here is a registration).
**Book reviewed** X-5 `theme-erc-v1` on TRAIN 2020-2023, S2 at $1bn: net Sharpe 1.7695, net 5.08%/yr, gross of cost
6.45%/yr, vol 2.87%, turnover .0268/day, max DD 2.06%, L 1.172 (all-rows gross .986), N 56, DSR .731 [house].
**Method.** I read the inputs named in the brief (status 7, v8x §14, v8y §6/§14, XCOMB, YCOMB, XSIG, YSIG, YDATA, XIMP,
review-x5, scorecard interim, the v8 literature review `reports/Equity long short alpha v8.md` = **[V8L]**) and the house
data docs (`atx-db/docs/datasets/indexes.md`, `compustat_std.md`, XDATA §1a). I checked about 45 citations by web
search on 2026-10-02 at abstract or publisher-page level. I ran no 3-vote harness, read no full texts beyond abstracts,
and opened no data. **Labels:** [web] = the venue and headline claim were checked this session; [mem] = cited from
memory, venue believed right; [V8L] = taken from the v8 review, not re-checked; **[verify]** = a number or detail
not checked; [est] = my own arithmetic or judgement (about ±50%); [house] = the house's ledger or status files.
Published magnitudes are gross, monthly, mostly pre-2015 and mostly the whole CRSP universe. None of them transfers
one-for-one to a daily, neutralised, top-1,850, $1bn book.

## 0. Summary

1. **At a book Sharpe of 1.77, one new signal adds very little.** The appraisal arithmetic (§3.0) gives +.045 for a
   standalone net Sharpe of .4 at zero correlation, and +.014 at correlation .1. ERC earned +.31 of its +.35 from
   lower volatility. The levers that act on the book's risk therefore dominate: hedging the unpriced part of each
   theme's covariance (Daniel-Mota-Rottke-Santos 2020 [21]) is the largest published Sharpe gain not yet tried here.
2. **Timing is the second channel with real evidence.** Anomaly returns are about 6x higher on earnings days [28] and
   are concentrated in the first month after an information release [29]. The book's theta .05 holds about 40% of a
   new score in its first 21 sessions [arith]. An information-clock trade rate is new content distinct from R-3 and
   R-4, but it is close to Y-5 (§1.5, §2.3).
3. **Most new families either need data or have faded since publication.** The options-based signals need data:
   put-call parity / implied borrow [50][51] and option volume [54]. Customer momentum faded [41], and fund-flow
   pressure largely vanishes once it is measured correctly (Wardlaw 2020 [45]). The best families on in-house data
   are Lazy Prices (7% landed) [39], hedge-fund crowding via Form ADV [46], skill-weighted 13F [61] and the Russell
   proxies (in house from 2018) [56].
4. **Significance is the binding constraint.** The house's paired SEs run from .023 to .20 [house]. The minimum
   detectable gain at 80% power is about 2.5 x SE, so .06-.50. The dSR > 0 acceptance rule carries a selection bias of
   up to about .3-.4 across the accepted lineage if every cell were null [arith], which is in line with the reviewer's
   +.15 out-of-sample estimate for X-5. On its own, the sealed 2015-2019 block can confirm only a cumulative gain of
   about .40-.45 or more (§5.3).
5. **Two items flag contamination and realism.** Candidates whose source papers used 2015-2019 data contaminate the
   OD-3 read for those members (§5.2). A fee-aware short side would *lower* modelled net Sharpe by up to about
   .06-.10 [51][house G-3a].

## 0.1 What exists, what was tried (not re-proposed)

| status | items |
|---|---|
| accepted | ic-shrink-v1, theme-resid-v1, theme-erc-v1 (X-5, +.349; vol +.311, gross alpha +.080, cost −.042) |
| rejected | R-3 GP aim / persistence gain (−.012), R-4 rank band (−.019), R-5 ADV hold cap (−.054), R-6 target-tracking spo-v3 (−.495), R-7 library v8.1 (−.066), R-8 ex-ante risk target (−.011), X-4 FF49 value (−.058), X-6 name inverse-vol (−.095), X-7 Kakushadze (−.161), v9-mine-c1 (0 of 110 above t 5.40) |
| pending (registered, unread) | Y-S 15 strings (incl. smile_slope, stio_trade, conn_rev, deal_target, so_wang_rev, mom_turn, fscore_hbm, iv_vol_of_vol); Y-3 norm-score; Y-2 theme-tsmom; Y-5 two-speed; X-10 L 2.0; Y-1 vol-target |
| [V8L] do-not-build (still binding unless new evidence) | multi-lag seasonality, liquidity/volume levels, spanned financing signals, theme timing (Y-2 overrode it by PM ruling), book vol-scaling for Sharpe, nonlinear transforms that tilt into hard-to-short names, HRP / min-variance sleeves, orthogonalisation as a combiner, mined price-volume |
| house diagnostics used below | G-2a variance: style .657, industry .182, specific .156, market .005; G-2b IC: high-vol tercile .0144 vs low .0089, top-1000 .0085 vs rest .0127; G-1c netting ratio .45; G-3c one-session delay −.012; G-3a flat-fee to fee-schedule −.064 (B0c); G-3d effective bets 4.9 |

## 1. Raising the Sharpe of a multi-signal L/S book (combination)

Ranked by evidence × applicability to a 4-year, 13-theme book. "Cost" means what the method trades against.

**1.1 Hedging unpriced common risk (Daniel-Mota-Rottke-Santos 2020 [21], web): the top untried lever.**
Characteristic-sorted portfolios carry priced characteristic risk *and* unpriced covariance risk. A hedge portfolio
holds the characteristic constant and is short the covariance loading. In their paper it raises the squared Sharpe of
the optimal factor combination from 1.31 to 2.29, about 1.14 → 1.51 (+32%). The weights come from past returns, so
they are real-time.
- Why it fits here: the book's gain so far came from risk (ERC), and 66% of its variance is style [house G-2a].
  theme-resid-v1 orthogonalises themes against each other and price-risk-v1 projects size, beta, vol and liquidity.
  Neither hedges a theme's loading on *its own* factor at a fixed characteristic.
- House form: per theme, w = rank(score) − h·rank(beta⊥). beta⊥ is the name's trailing beta on that theme's sleeve
  return (decisions ≤ d−2), orthogonalised to the score. h is the walk-forward regression coefficient.
- Cost: turnover rises as betas move; it fits about 13 hedge ratios walk-forward; it overlaps theme-resid.
- Expected effect: +.05 to +.15 [est]. ERC already removed part of the reducible variance, and DMRS is monthly,
  pre-cost and covers a few characteristics.

**1.2 Shrinkage and forecast combination: no further gain on 4 years.** 1/N is not beaten out of sample by 14 MV
variants; estimation needs about 3,000 months for 25 assets (DeMiguel-Garlappi-Uppal 2009 [1], mem). Bayes-Stein
shrinkage of means is the standard repair (Jorion 1986 [2], mem); blending 1/N with a sophisticated rule beats both
(Tu-Zhou 2011 [3], mem). ic-shrink-v1 already holds this content, and [V8L]'s blend weight on a fitted theme
portfolio is about .26 on 4 years. Expected effect 0 to +.03 [est]; defer to after OD-3.

**1.3 Signal timing.** Factor premia concentrate after a year of positive factor returns (Ehsani-Linnainmaa 2022 [4],
web; magnitude [verify]); this holds across many factors (Gupta-Kelly 2019 [6], web) and subsumes industry momentum
(Arnott-Kalesnik-Linnainmaa 2023 RFS [5], web). Valuation-based timing of factor PCs gives an optimal timing
portfolio Sharpe of .71 (Haddad-Kozak-Santosh 2020 [7], web; static benchmark [verify]). Against: [V8L]'s power
arithmetic (t .6 on 4 years), and Y-2 is registered. Verdict: no second timing cell before Y-2 reads; the one timing
form with distinct evidence is momentum-sleeve risk management (§1.6).

**1.4 Machine-learning stacking: do not build on 4 years.** Trees and NNs beat linear models through interactions
(Gu-Kelly-Xiu 2020 [8], web; NN value-weighted decile Sharpe about 1.35 vs about .6 for OLS-3 [verify]). The "virtue
of complexity" (Kelly-Malamud-Zhou 2024 [9], web) is disputed: Nagel 2025 [10] (web, WP) shows the random-feature
timing strategy reduces mechanically to volatility-timed momentum that it does not learn from the data. Deep-learning
profits come from microcaps, distressed names and high-volatility states and fall further after costs
(Avramov-Cheng-Metzker 2023 MS [11], web), the opposite of this universe. Defer to the backward extension, if ever.

**1.5 Characteristic-based weights and robust MV with costs.** Parametric policies (Brandt-Santa-Clara-Valkanov 2009
[12], mem) fit a few coefficients by utility; with costs the characteristics that matter rise from 6 to 15 because
trades net (DeMiguel et al. 2020 [13], [V8L]). On 13 themes and 4 years that is a fitted-weights problem (break-even
about 11.6 years [V8L]). Garleanu-Pedersen 2013 [14] (mem) has three parts: the aim (rejected, R-3), partial trading
(the house's theta .05) and per-signal decay in the aim (R-3 again; Y-5 holds the bucketed form). Distinct and
untried in that line: (a) a per-name, time-varying trade rate keyed to information arrival (§2.3), new in its clock,
not its algebra; (b) the learned implementable frontier (Jensen-Kelly-Malamud-Pedersen RFS 2026 [15], [V8L]: net
Sharpe 1.38 vs .94 for the tuned one-period optimiser at $10bn), which needs 40 years and two tuning layers: do not
build. R-6's −.495 keeps the optimiser path closed until the risk model's alpha alignment is solved.

**1.6 Volatility management.** The market timed by inverse variance earns alpha (Moreira-Muir 2017 [16], mem), but in
real time it fails for about half of 103 strategies (Cederburg et al. 2020 [17], [V8L]). A multifactor conditional
portfolio survives costs out of sample (DeMiguel-Martin-Utrera-Uppal 2024 JF [20], web), but it is a joint MV rule,
not per-factor scaling. Momentum is the robust exception: scaling WML by its 6-month realised vol lifts its Sharpe
from .53 to .97 (Barroso-Santa-Clara 2015 [18], web), and dynamic mean/variance weighting roughly doubles it
(Daniel-Moskowitz 2016 [19], mem). Y-1 tests the book level, where Sharpe changes only if vol is forecastable and
unrelated to return. A **momentum-sleeve-only** rule is distinct (Y-4 was left unused citing Cederburg, whose
exceptions include momentum). Book effect: price_momentum holds .057 of the weight after ERC at sleeve vol 5.7%
[house review-x5], so +.01 to +.03 [est].

**1.7 Hedging named common risks.**
- BAB-style beta neutralisation: done (price-risk-v1). BAB's own alpha largely reflects rank weighting toward
  microcaps and is cut more than 55% by costs (Novy-Marx-Velikov 2022 [23], [V8L]).
- Size and vol: projected.
- Industry: .18 of variance [house]. Full book-level neutralisation would cut vol by at most about 9% (sqrt(.82)),
  but industry momentum and peer_mom_1m *intend* industry bets. X-4's value-only FF49 failed (−.058) [81]. Prior −.05
  to +.05 [V8L].
- Not recommended separately; DMRS (1.1) is the principled version.

**1.8 Horizon-matched blending.** Turnover scales with sqrt(1 − ρ_forecast), and signals should be weighted by IC at
the holding horizon (Qian-Sorensen-Hua 2007 JPM [22], web).
- In this house that content is R-3 (persistence gains on weights, rejected) plus Y-5 (bucketed rates, pending).
- An "IC at theta horizon" weight estimator would be the same hypothesis as R-3. Do not register.

**1.9 Ensembles across formation windows.** The cited "Betting against betting against beta" [23] is about BAB
construction, not window ensembles (the brief's [verify] resolves to: wrong source). Rebalance timing luck exceeds
100 bps/yr for concentrated factor indices (Hoffstein-Sibears-Faber 2019 JII; Hoffstein-Faber-Braun 2020 [25], web),
and construction choices move factor Sharpe by more than its SE (Soebhag et al. 2024 [26], [V8L]). A daily
partial-adjustment book is already an exponentially weighted ensemble over formation dates; window ensembles cut
non-standard error, not expected return. No cell.

**Implication for the book.** One combination cell is worth a trial: theme-cov-hedge (1.1). Momentum-sleeve risk
management (1.6) is a cheap second. Everything else is held, deferred to OD-3 / the long panel, or already a house
rule.

## 2. Raising gross return per unit gross (concentration, conditioning, interaction)

The return-starved state is mostly a leverage choice: gross .99, vol 2.9%. X-10 and Y-1 address scale. Gross-per-gross
matters for Sharpe only when it does not raise risk proportionally, and for return only beyond the executable L cap
of 2.0.

**2.1 Tail concentration.**
- Anomaly profits concentrate in extreme portfolios, mostly the short leg, especially after high sentiment
  (Stambaugh-Yu-Yuan 2012 [80], mem). The return-characteristic relation is nonlinear (Freyberger-Neuhierl-Weber 2020
  [77], mem).
- House evidence: IC is 60% higher in the high-vol tercile and 33% lower in the top 1,000 [house G-2b]. Concentration
  buys gross per dollar by moving into less liquid, costlier-to-short names: capacity down, borrow up [51].
- Y-3 norm-score is the registered test. No second shape cell until it reads.

**2.2 Interactions.**
- *Value × profitability:* the two strategies correlate −.57, and a 50/50 mix has Sharpe .85 against .34 for the
  market (Novy-Marx 2013 [27], web). The book already holds both linearly. The conditional form ("profitable value")
  is `fscore_hbm` in Y-S. Nothing new.
- *Momentum × volatility or uncertainty:* momentum is stronger in high-uncertainty names (Zhang 2006 [82], mem). YSIG chose
  `mom_turn` as the one momentum interaction (PM7-36 rule: one interaction per mechanism).
- *Quality-conditioned reversal:* the house's industry-relative, ex-news reversal (`ind_adj_rev_5_nx`) showed the
  wrong sign at X-2. No new form.
- Verdict: interactions are exhausted at one per mechanism.

**2.3 Event-conditioned alpha: the information clock.**
- Anomaly returns are 50% higher on corporate news days and **6x higher on earnings days** (Engelberg-McLean-Pontiff
  2018 [28], web; among large caps about a quarter of the small-cap interaction [V8L]).
- Anomaly returns are **concentrated in the first month after the information release** and decay soon after; stale,
  June-style formation understates predictability (Bowles-Reed-Ringgenberg-Thornock 2024 JF [29], web).
- Arithmetic for this book: at theta .05 the book holds on average .40 of a new target change over its first 21
  sessions. At theta .129 (Y-5's fast rate) it holds .70 [arith].
- For a step change, the total traded quantity is unchanged; only participation per day rises. So the cost is
  impact convexity, not turnover volume.
- Proposed cell `info-clock-rate-v1`: theta_f = .12945 for a name while `ea_days_since` ≤ 21, else .05. Constants are
  from [29] (one month) and Y-5 (fast rate); no new constant.
- If a third of fundamental-theme alpha arrives in month 1, those themes' captured alpha rises about 14% [est], and
  book gross rises about 5% [est]. Net +.02 to +.06 [est].
- Restatement risk: the PM must rule that it is distinct from Y-5. Y-5 buckets themes; this cell buckets name-days
  by an event clock. Run it after Y-5.
- Pre-announcement acceleration alone is weak: slow scores barely move before an EA, so there is nothing to
  accelerate.
- *Index reconstitution:* the S&P 500 addition effect fell from 7.4% (1990s) to under 1% in the last decade
  (Greenwood-Sammon 2025 JF [57], web). Russell 1000/2000 boundary moves still have regression-discontinuity price
  effects (Chang-Hong-Liskovich 2015 RFS [56], web; post-2015 size [verify]). The house has Russell proxies with a
  rank-date clock from 2018-06 [house indexes.md]. Small and once a year: §3.
- *Option expiration:* stock prices cluster at strikes on expiration days (Ni-Pearson-Poteshman 2005 [83], mem), an intraday
  effect worth basis points. No daily-book value.

**2.4 Intraday and overnight decomposition.**
- Momentum accrues overnight; most other anomalies accrue intraday with negative overnight returns (Lou-Polk-Skouras
  2019 [30], [V8L]).
- Institutions trade mispricing early in the day and unwind before the close, so anomaly returns accrue during the
  day and are **attenuated at the end of the day** (Bogousslavsky 2021 JFE [31], web).
- Implication: close execution buys anomaly-consistent positions after the end-of-day attenuation, which supports the
  house's d+1 close fills. A move to open execution has no support. G-3c (−.012 per session of delay) bounds the
  gain from faster execution at about .01.
- `day_rev_freq` (Y-S) carries the tug-of-war signal. No cell.

**2.5 Alpha decay: which classes still pay after 2015.**
- Post-publication returns are 26% lower out of sample and 58% lower after publication (McLean-Pontiff 2016 [32], mem).
- 319 signals replicate in open source (Chen-Zimmermann 2022 CFR [33], mem).
- Most factors replicate and cluster into 13 themes (Jensen-Kelly-Pedersen 2023 JF [34], web).
- Outside microcaps, only 2 of 94 characteristics are independent predictors after 2003 (Green-Hand-Zhang 2017 RFS
  [37], web).
- Net post-2005 anomaly returns are about 4 bps/month (Chen-Velikov 2023 [35], [V8L]). The median anomaly in the top
  90% of cap earns 7 bps/month after 2005, with profitability and financing the exceptions (Chen-Welch 2026 WP [36],
  [V8L]).
- Ranking for this book [est, from those sources]: (1) profitability / quality, which earned more after publication
  [38]; (2) issuance / financing; (3) slow earnings momentum (12-month announcement returns, t 2.94 in the top 1,000,
  2010-2024 [79]); (4) value, weak 2007-2020 and recovered 2021-22 [verify]; (5) low risk, alive after projection but
  cost-sensitive [23]; (6) price momentum, residual part weak after 2000 [V8L]; (7) short interest, near zero after
  fees [51]; last, latest-surprise drift (gone for large caps [78]), seasonality lags 2-5 and trading frictions.
- New P9 signals should come from data classes with *no* published post-2005 decay test (text, options microstructure,
  holdings structure) or be slow fundamentals.

## 3. New signal families not in the 13 themes

**3.0 What a new member can add.** The best attainable gain is ΔSR = sqrt(SR_b² + AR²) − SR_b, with
AR = (SR_c − ρ·SR_b)/sqrt(1 − ρ²) [V8L, Benhamou-Guez].

| SR_c, ρ | SR_b 1.77 (TRAIN) | SR_b 1.2 (planning) |
|---|---|---|
| .4, 0 | +.045 | +.065 |
| .4, .1 | +.014 | +.033 |
| .6, .1 | +.050 | +.093 |
| .4, .2 | +.001 | +.011 |

These gains assume an optimal weight. At about 1/60 of the gross a member realises less [est]. So the bar for a
P9 signal is net SR_c ≥ .4 and ρ ≤ .1 to the *book*, and correlation matters more than standalone strength.

**3.1 Candidates.** Data codes: **H** = in house and bound to the engine; **W** = in the atx-db warehouse but not in
engine fields; **P** = partly landed; **N** = not in house. ρ is the expected correlation to the existing themes [est].

| # | family | citation (claim) | sign / hold | data | ρ to themes [est] | disposition |
|---|---|---|---|---|---|---|
| F1 | Filing-text change ("Lazy Prices") | Cohen-Malloy-Nguyen 2020 JF [39] web: shorting changers / buying non-changers earns up to 188 bps/mo alpha; value-weighted 34-58 bps/mo for up to 18 months [V8L]; sample 1995-2014, post-publication test unknown [verify] | long non-changers; 3-18 mo | P: atx-db TXT code on main, 7% of 10-K/10-Q landed; engine reader missing | ≤ .1 (filing_events partial) | **recommend** (slow, high capacity) once landing completes |
| F2 | Hedge-fund crowding (long crowded) | Brown-Howard-Lundblad 2022 RFS [46] web: across about 1,500 hedge funds 2004-2016, the most crowded long portfolios have the *highest* returns (tail-risk compensation) | long HF-crowded; quarterly | P: 13F manager holdings H (2013q2+); hedge-fund flag of filers N; Form ADV (public SEC bulk) gives it | .2 to momentum and inst_best_ideas | **recommend** after a Form ADV join; check vs `inst_best_ideas` |
| F3 | Skill-weighted holdings | Wermers-Yao-Zhao 2012 RFS [61] web: aggregating fund holdings by inferred skill predicts returns, not subsumed by momentum, value or earnings quality | long skill-weighted; quarter | H: 13F manager books; manager track records computable from holdings; fund-level returns N | .15 | **consider** (in house; manager-level, not fund-level; sample pre-2010 [verify]) |
| F4 | Put-call parity deviation / implied borrow | Cremers-Weinbaum 2010 JFQA [50] web: expensive-call minus expensive-put earns 51 bps/week, *declining* over the sample. Option-implied lending fees: anomalies are not exploitable after fees (Muravyev-Pearson-Pollet 2025 JF [51] web); fee-risk premium small [52]. Shorting premium: Drechsler-Drechsler [53] (NBER WP 2014, unpublished, web) | long cheap-to-short / expensive calls; 1-4 wk | N: needs strike-matched call/put IV or prices 2018+ (alpha panel has ATM IV, 15 tenors, slopes only; ORATS strikes file is post-seal) | .3 to short_interest | **data ask, high value**: the only route to borrow-fee information (short_interest has no candidate without it) and to a fee-aware cost model |
| F5 | Option / stock volume | Johnson-So 2012 JFE [54] web: low minus high O/S decile earns 1.47%/mo risk-adjusted; weekly O/S predicts next week; 1996-2010 | short high O/S; 1-4 wk | N: option volume per underlying-day (OPRA pulls in atx-vol are not a panel) | .2 (short_interest, sv_flow) | data ask; medium |
| F6 | IV changes, call vs put | An-Ang-Bali-Cakici 2014 JF [55] mem: rising call IV predicts high returns, rising put IV low | long ΔIV_call − ΔIV_put; 1 mo | N: separate call/put IV | .3 (iv_rv_spread, smile_slope) | bundle with F4's data; one hypothesis with F4 by PM rule |
| F7 | Option smirk | Xing-Zhang-Zhao 2010 JFQA [49] web: steepest smirks underperform by about 10.9%/yr (1996-2005), persistent 6 months | short steep smirk | H (vendor slope) | - | **already Y-S `smile_slope`**; a put-wing version is the same hypothesis (PM8-8 (5)) |
| F8 | Russell reconstitution | Chang-Hong-Liskovich 2015 RFS [56] web: R2000 additions rise, deletions fall around the June recon (discontinuity at rank 1,000); S&P effect gone [57] | long predicted R2000 adds / short R1000 adds, May-July | H/W: `indexes/` Russell proxy, rank-date clock, membership 2018-06+ (official lists unlicensed) | ~0 | **consider**: in house; small breadth; also usable as a do-no-harm trade filter |
| F9 | ETF flow pressure | Brown-Davies-Ringgenberg 2021 RoF [58] web: short high-flow / long low-flow ETFs earns 1-4%/mo; flows predict reversal in the *underlying* assets too | reversal of ETF-flow pressure; weeks | P: ETF shares outstanding on vendor lines [verify]; ETF holdings via N-PORT (16 of 27 quarters parsed) | .3 (reversal) | fast; later wave |
| F10 | Intangible-adjusted value | Eisfeldt-Kim-Papanikolaou 2022 CFR [47] web: HML with SG&A capitalised (perpetual inventory) beats HML over the full sample including recent decades; against: spanned by profitability (Novy-Marx-Medhat 2025 [38]) | long high (B+intangibles)/M; annual | W: SG&A (`xsga`, `xsgaq`) now in atx-db `compustat_std`; engine export lacks SG&A history | .7 (value) | **consider** as a value *member change* (one variant); XBRL from 2009 gives a 10-year inventory by 2019 |
| F11 | Cash-flow duration | Weber 2018 JFE [48] web: high-duration stocks earn 1.10%/mo less; Dechow-Sloan-Soliman 2004 [85] (mem) | long short duration; annual | H: fundamentals | .6 (value, low_risk) | low priority (largely value plus projected vol) |
| F12 | Revenue surprise (analyst-free) | Jegadeesh-Livnat 2006 JAE [63] mem: revenue surprise adds drift beyond SUE | long positive SURGE; 1-3 mo | W: `saleq` in compustat_std; export lacks the quarterly lag item | .4 (earnings_momentum) | low (PEAD absent in large caps after 2006 [78]); house analyst-free proxies (SUE, dROE, dTax, EAR-12m, ea_uvol) already cover the class |
| F13 | Customer-supplier momentum | Cohen-Frazzini 2008 JF [40] web: 1.55%/mo, t 5.4, 1981-2004; post-2004 smaller and insignificant (Pinchuk 2023 [41], web WP); much is momentum commonality [V8L Burt-Hrdlicka] | long suppliers of winning customers; 1 mo | N: links (Compustat segment customers). Proxy: BEA input-output industry links (Menzly-Ozbas 2010 JF [42], mem), public, not in house | .4 (peer_mom_1m, ind_mom) | **do not build** (decayed; proxy overlaps peer_mom_1m) |
| F14 | Mutual-fund flow pressure | Lou 2012 RFS [43]; Coval-Stafford 2007 JFE [44] (mem). **Wardlaw 2020 JF [45] web:** the standard measure mechanically contains the realised return; corrected, outflow pressure is negligible with no reversal | - | P: N-PORT | - | **do not build** |
| F15 | Retail order imbalance | Boehmer-Jones-Zhang-Zhang 2021 JF [59] web: net retail buying predicts about 10 bps/week; Barber et al. 2024 JF [60] web: the algorithm finds 35% of retail trades and mis-signs 28% | long net retail buying; 1 wk | N: trades with sub-penny prices (house has OHLCV, BBO-1m) | ~0 | do not buy for this |
| F16 | Activist 13D | Brav-Jiang-Partnoy-Thomas 2008 JF [64] mem: about 7% around the filing, no reversal; post-filing drift weak [verify] | long post-13D; months | H: SEC form types in `sec_filings` | ~0 | low (jump captured before the book can trade) |
| F17 | High-volume return premium | Gervais-Kaniel-Mingelgrin 2001 JF [62] mem | long volume shock | H | - | **prior-exposed** in the pv libraries (YSIG §8); excluded |
| F18 | Seasonality variants | [V8L] CZ-own: lags 2-5 −.65%/mo value-weighted; same-month theme seasonality is timing | - | H | - | excluded (do-not-build, Y-S already holds `season_y2_5`) |

**Implication.** Five families are worth P9 trials or data asks: **F1** Lazy Prices (finish the landing), **F2**
hedge-fund crowding (Form ADV join), **F4** put-call / implied borrow (data ask; also fixes cost realism), **F3**
skill-weighted 13F (in house) and **F8** Russell (in house). F10 is a value member change if the PM wants one more
value variant after X-4's rejection. The rest are excluded with a reason.

## 4. Capacity

**4.1 Cost models.**
- Live institutional costs (1.7 trillion USD of executions, 21 markets, 19 years) are an order of magnitude below
  earlier academic estimates (Frazzini-Israel-Moskowitz 2018 SSRN [65], web). Implied break-even sizes are tens to
  hundreds of $bn for value and momentum [verify].
- On effective-spread cost models, anomalies with turnover under about 50%/month survive, and buy/hold spreads are
  the most effective mitigation (Novy-Marx-Velikov 2016 RFS [66], mem).
- Banding and slower rebalancing hurt gross less than restricting the universe to cheap names (Novy-Marx-Velikov 2019
  FAJ [24], web). R-5's failure (−.054) fits this.
- Capacity elasticity is 1 to impact and 2 to signal-decay speed (Landier-Simon-Thesmar 2015 [67], [V8L]). Moving
  from a 1-day to a 5-day trade horizon raises momentum capacity from $65bn to $320bn (Ratcliffe-Miranda-Ang 2017
  [68], [V8L]).
- Hedge-fund alpha falls about 17 bps/yr per doubling of size after 2008 [V8L], and industry-level returns decrease
  with scale (Pástor-Stambaugh-Taylor 2015 [84], mem).

**4.2 Where the house stands.** The house already runs the main literature-supported cost tools: the GP
partial-adjustment trade rate, trade netting across themes (netting ratio .45 [house], the DeMiguel 2020 mechanism),
the square-root S2 impact model and daily capacity curves. The constructions that keep Sharpe while cutting
turnover were tried and rejected: R-4 (buy/hold spread), R-5 (ADV cap) and X-6 (inverse vol). ERC *raised* turnover
per unit gross by 16.5% and failed its registered criterion [house]. 4x net Sharpe is 1.655 against 1.770 at 1x.

**4.3 Capacity of slower sleeves.**
- Under square-root impact, the annual cost of a sleeve is about proportional to τ^1.5·sqrt(AUM). Halving a sleeve's
  turnover cuts its impact cost to about .35x [arith], and the AUM at which a sleeve's net alpha halves scales about
  as α²/τ³ [arith, from LST [67]].
- So the capacity ranking of P9 cells:
  - slow additions (F1 Lazy Prices, F10 intangible value, F2/F3 quarterly holdings) **raise** capacity;
  - theme-cov-hedge (beta drift) and info-clock-rate (front-loaded participation) **lower** it, about 5-15% more
    impact [est];
  - F4-F6 and F9 are weekly signals: fast-sleeve material under Y-5, capacity-negative.
- No P9 cell is proposed for capacity alone. Print the 2x / 4x / 8x curve and the cost per traded dollar on every
  cell (already house practice). If capacity becomes the binding priority, re-run R-9's theta frontier at $4-8bn.

## 5. Significance and deflation

**5.1 The frame.**
- New factors need t > 3.0 given hundreds tested (Harvey-Liu-Zhu 2016 RFS [69], mem).
- Thresholds should depend on the prior share of true ideas: about 2.4-2.6 for curated literature priors, 3.5-4.6 for
  mined [V8L] (Harvey-Liu 2020 JF [71], mem).
- From two million strategies, Chordia-Goyal-Saretto 2020 RFS [72] derive t thresholds of 3.8 and 3.4 ([V8L],
  snippet-level).
- DSR deflates the best Sharpe by E[max] over N trials with cross-trial variance V (Bailey-López de Prado 2014 JPM
  [70], mem). The house uses this with V from the construction lines.
- Correlated trials need an effective N by clustering (López de Prado-Lewis 2019 QF [74], mem [verify]). The house
  prints ONC beside DSR.

**5.2 What counts as a trial in a waved, pre-registered design.** The house rules already match the literature
[75][76]: every read of a TRAIN statistic for a distinct variant, every admission string (the gate selects on TRAIN)
and every campaign evaluation count, and voids keep their count. Three refinements from the literature:
- **(a) Winner's curse of the acceptance rule.** dSR > 0 is a one-sided test at α = .5. Under the null, an accepted
  cell's expected dSR is .80 x SE. Across the accepted lineage (R-2 .0945, X-2 .060, X-3 .120, X-5 .201) that sums to
  about .38 [arith] if all were null. Report the cumulative gain with conditional ("inference on winners")
  estimates (Andrews-Kitagawa-McCloskey 2024 QJE [73], web), not the raw sum.
- **(b) Publication contamination of the holdout.** A P9 member whose source paper's sample includes 2015-2019 has
  already been selected on the OD-3 window by the profession. Examples: Gerard-Jehl (1992-2024), Bowles et al.,
  Novy-Marx-Medhat. Record each member's source-sample end date at registration and print OD-3 with and without the
  contaminated members. Prefer sources whose sample ends before 2015: F1 (2014), F4-F5 (2005-2010), F8 (2012) are
  clean in this sense.
- **(c) Implicit trials.** Ideas drawn from this review cost nothing until read. A P9 cap of **≤ 6 construction cells
  and ≤ 10 admission strings** keeps E[max] growth under about 2% [arith from v8y §4's scale].

**5.3 Minimum detectable paired gain.** MDE (one-sided 5%, power .8) = 2.49 x SE. The house's measured paired SEs
[house] give: R-5 .023 → MDE .06; X-4 .037 → .09; X-2 .060 → .15; R-2 .095 → .24; X-3 .120 → .30; X-5 .201 → .50.
Signal waves (SE about .06-.12) cannot detect their literature-expected +.01 to +.05 (power about 5-15% [arith]);
combination rules (SE about .2) detect only gains of about .5. The longer history: **2015-2019 alone** (the sealed
one-shot read) scales SE by sqrt(4/5) = .89, so a cumulative book difference at SE .18-.20 has MDE about .40-.45;
**2015-2023 pooled** scales SE by .67, MDE about .30 [arith].

Two caveats bind the OD-3 read:
1. The fields start at different dates. FINRA short interest, FTD and the Russell proxies start in 2018; 13F in
   2013q2; options in 2012. So 2015-2017 tests a *different* book (short_interest and filing members partly absent).
   Print theme coverage by year (review-x5 condition 3).
2. Register predictions, not only dSR. For ERC the predictions are vol ratio .80-.87 and return ratio .90-1.0
   (review-x5); for each P9 cell, its mechanism's sign. A one-shot holdout answers pass or fail [76]. A model changed
   after it "is no longer an out-of-sample test" [75].

**Implication.** Spend P9 trials on few, large-effect, mechanism-predicting cells. Treat signal additions as a bundle
judged on the cumulative test. Keep the OD-3 read for the frozen Y-F0 plus P9 bundle once, with contamination
flags.

## 6. Candidate table

Priority: S = Sharpe, G = gross return, C = capacity, Sig = significance. "Gain" is book-level [est] unless a
published number is quoted. Lanes: P9COMB (rules), P9SIG (strings), P9DATA (fields and data), P9PRE
(pre-registration and statistics).

| cell | priority | expected gain (published → book, caveat) | data needed | engine capability | lane |
|---|---|---|---|---|---|
| **theme-cov-hedge-v1** (DMRS hedge per theme) | S | squared SR 1.31 → 2.29 [21] → **+.05 to +.15**; ERC already took part; monthly, pre-cost evidence | H (prices, sleeve returns) | **missing**: kernel `atx-engine/.../combine/char_cov_hedge.hpp` (rolling beta on own sleeve, walk-forward h); runner rule `strategy_ic_theme_hedge.{hpp,cpp}` beside `strategy_ic_theme_resid`; fitter spec only (PM8-12) | P9COMB |
| **info-clock-rate-v1** (theta_f for 21 sessions after an EA) | G, S | EA days 6x [28]; month-1 concentration [29] → **+.02 to +.06**; Y-5 restatement risk | H (`ea_days_since`) | **missing**: per-name rate vector in aim-partial-v5 (`strategy_target_replay`), generalising `book/two_speed.hpp` | P9COMB |
| **lazy_prices** (F1) | S, C | 34-58 bps/mo VW [39][V8L] → **+.03 to +.06** (ρ ≤ .1); post-publication unknown | P: complete 10-K/10-Q text landing and `text_features` export | **missing**: field builder `research_fields_text.py`; DSL exists | P9DATA → P9SIG |
| **hf_crowd** (F2) | S, G | crowded HF longs earn the most [46] → **+.02 to +.04**; 2004-2016 sample | P: Form ADV private-fund flag (public) joined to 13F filer CIK | **missing**: classification in `research_fields_mgr13f.py` (module exists for stio) | P9DATA → P9SIG |
| **pcp_spread / implied borrow** (F4, F6 one hypothesis) | S, Sig | 51 bps/wk declining [50]; fee-aware anomalies fade [51] → **+.02 to +.04 signal, and a realism correction of −.06 to −.10** | **N: data ask** (strike-level call/put, 2018+) | **missing**: field builder; `book/borrow_schedule.hpp` exists to take name-level fees | P9DATA (owner ask) |
| **gia_13f** (F3) | S | skill-weighted holdings not subsumed [61] → **+.01 to +.04** | H (13F manager books) | **missing**: holdings-return history per manager in the 13F builder | P9SIG |
| **mom-volman-v1** (momentum sleeve only, 126-session realised vol, target = running mean) | S | WML .53 → .97 [18]; DM doubles [19] → **+.01 to +.03** (momentum weight .057) | H | **partial**: theme mass schedule exists (`strategy_ic_theme_tsmom` `composition_schedule`); add a vol-schedule rule there; sequence after Y-2 | P9COMB |
| **russell_recon** (F8) | G | RD price effects [56]; S&P effect gone [57] → **+.00 to +.02**; also a trade filter | H/W: `indexes/` Russell proxy 2018-06+ | **missing**: field builder `research_fields_indexes.py`; DSL exists | P9SIG |
| **intangible value** (F10, member change of `bm`) | S | HML_INT > HML [47] vs spanned [38] → **0 to +.04** | W: `xsga` in compustat_std | **missing**: SG&A history in the fundamentals export plus a perpetual-inventory field | P9DATA |
| **os_ratio** (F5) | S | 1.47%/mo [54] → **0 to +.03**; weekly, capacity-negative | **N: data ask** (option volume) | missing builder | P9DATA (owner ask) |
| **etf_flow** (F9) | G | 1-4%/mo at ETF level [58] → **0 to +.03**; fast | P: ETF shares [verify], N-PORT | missing builder | later wave |
| OD-3 prediction set + contamination flags (§5.2b, §5.3) | Sig | none to Sharpe; protects the only clean test | H | `atx-impl/tools/dsr_total.py` exists; print-only additions | P9PRE |
| winner's-curse-adjusted cumulative print [73] | Sig | none; honest cumulative estimate (about −.2 to −.4 vs raw) | H | missing: conditional-inference helper beside `dsr_total.py` | P9PRE |
| not recommended | - | F11, F12, F13, F14, F15, F16, F17, F18; ML stacking [8]-[11]; PPP [12]; Talmud blend [3]; book industry neutralisation; window ensembles; horizon-IC weights (= R-3) | - | - | - |

**Suggested P9 order** (signals before rules, as house practice): (1) data asks to the owner now: F4/F5 options,
Form ADV, text landing; (2) P9SIG wave: gia_13f, russell_recon, plus lazy_prices and hf_crowd if their data lands
(≤ 10 strings); (3) theme-cov-hedge-v1; (4) info-clock-rate-v1, after Y-5 reads; (5) mom-volman-v1, after Y-2 reads.
Total ≤ 3 construction cells plus 1 wave.

## 7. Caveats and confidence (read before acting)

- **Magnitudes do not transfer.** Every published number is a different object: monthly, decile, CRSP-wide,
  usually pre-2015, often gross. Every book-level gain here is my estimate. The §3.0 arithmetic bounds what any single
  member can add at SR 1.77.
- **Only partly verified.** About half the citations were checked at abstract level ([web]); none were read in full.
  Numbers marked [verify] need the paper: Gu-Kelly-Xiu Sharpe, the Haddad-Kozak-Santosh static benchmark,
  Ehsani-Linnainmaa magnitudes, Chang-Hong-Liskovich post-2015 size, Frazzini-Israel-Moskowitz break-even sizes, Lazy
  Prices post-publication, the 13D drift, the López de Prado-Lewis venue.
- **Citation corrections.** Arnott et al. "Factor momentum" is RFS 2023 (author list varies by version);
  Drechsler-Drechsler is an unpublished NBER WP; the brief's "Novy-Marx-Velikov 2022" is about BAB construction, not
  window ensembles; "Factor momentum everywhere" is Gupta-Kelly 2019 JPM, not Arnott et al.
- **DMRS is the largest claim.** Its translation to a 13-theme, already-ERC'd, daily book is untested anywhere. The
  hedge ratio is fitted (walk-forward) and adds parameters that the DSR does not penalise.
- **Restatement risk.** info-clock-rate shares algebra with Y-5. mom-volman shares the schedule mechanism with Y-2 and
  the Moreira-Muir logic with Y-1. The PM rules on distinctness before any read (E-45 / PM7-14).
- **Realism cuts the other way.** A fee-aware short side (F4 data) is expected to *lower* the reported net Sharpe [51].
  This is a feature for significance, not a loss of alpha.
- **Time sensitivity.** Several sources are 2024-2026 working papers or new publications ([10], [36], [15]). Their
  claims may change.

## 8. Open questions

1. Does Bowles et al. [29] give a month-1 share of anomaly returns for large caps? That number sets info-clock-rate's
   prior. A full-text read is needed.
2. Is ETF shares-outstanding history present on the vendor's ETF lines in TickerHistory3 (XDATA says "every vendor
   line incl. ETFs")? That decides F9's data class.
3. Can a strike-level option history for 2018-2023 be bought (ORATS SMV strikes or OPRA end-of-day)? Cost decides F4,
   F5 and F6 together.
4. Lazy Prices: is there any post-2018 out-of-sample test in large caps? A replication search is needed (one public
   S&P 100 replication exists on GitHub, unreviewed).
5. Which atx-risk-v1 style columns carry no theme (size, beta, residual_vol, liquidity, leverage)? Measuring the
   book's variance share on these alone (zero trials) bounds what a DMRS-style hedge can remove.

## Appendix A. Bibliography

Quality: P = peer-reviewed, W = working paper, S = secondary. Check: web / mem / V8L.

| # | source | Q | check | key claim used |
|---|---|---|---|---|
| 1 | DeMiguel, Garlappi, Uppal (2009) RFS 22(5), doi 10.1093/rfs/hhm075 | P | mem | 1/N not beaten out of sample |
| 2 | Jorion (1986) JFQA 21(3), doi 10.2307/2331042 | P | mem | Bayes-Stein means |
| 3 | Tu, Zhou (2011) JFE 99(1) "Markowitz meets Talmud" | P | mem | combine 1/N with MV |
| 4 | Ehsani, Linnainmaa (2022) JF 77, https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13131 | P | web | factor momentum |
| 5 | Arnott, (Clements,) Kalesnik, Linnainmaa (2023) RFS 36(8) "Factor momentum", https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3116974 | P | web | factor momentum, industry momentum |
| 6 | Gupta, Kelly (2019) JPM 45(3), https://doi.org/10.3905/jpm.2019.45.3.013 | P | web | TS factor momentum everywhere |
| 7 | Haddad, Kozak, Santosh (2020) RFS 33(5), https://www.nber.org/papers/w26708 | P | web | factor timing SR .71 |
| 8 | Gu, Kelly, Xiu (2020) RFS 33(5), https://www.nber.org/papers/w25398 | P | web | ML: trees and NNs win via interactions |
| 9 | Kelly, Malamud, Zhou (2024) JF 79, https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13298 | P | web | virtue of complexity |
| 10 | Nagel (2025) NBER w34104, https://www.nber.org/system/files/working_papers/w34104/w34104.pdf | W | web | KMZ = vol-timed momentum |
| 11 | Avramov, Cheng, Metzker (2023) MS 69(5), https://pubsonline.informs.org/doi/10.1287/mnsc.2022.4449 | P | web | ML profits from microcaps / distress |
| 12 | Brandt, Santa-Clara, Valkanov (2009) RFS 22(9) | P | mem | parametric portfolio policies |
| 13 | DeMiguel, Martin-Utrera, Nogales, Uppal (2020) RFS 33(5) | P | V8L | costs raise significant characteristics 6 → 15 |
| 14 | Garleanu, Pedersen (2013) JF 68(6), doi 10.1111/jofi.12080 | P | mem | aim and partial trading |
| 15 | Jensen, Kelly, Malamud, Pedersen, RFS (2026), https://doi.org/10.1093/rfs/hhag022 | P | V8L | implementable frontier 1.38 vs .94 |
| 16 | Moreira, Muir (2017) JF 72(4), doi 10.1111/jofi.12513 | P | mem | vol-managed portfolios |
| 17 | Cederburg, O'Doherty, Wang, Yan (2020) JFE 138(1), https://www.lehigh.edu/~xuy219/research/COWY.pdf | P | V8L | real-time failure in about half of 103 |
| 18 | Barroso, Santa-Clara (2015) JFE 116(1), https://econpapers.repec.org/RePEc:eee:jfinec:v:116:y:2015:i:1:p:111-120 | P | web | managed momentum SR .53 → .97 |
| 19 | Daniel, Moskowitz (2016) JFE 122(2), doi 10.1016/j.jfineco.2015.12.002 | P | mem | dynamic momentum about doubles SR |
| 20 | DeMiguel, Martin-Utrera, Uppal (2024) JF 79(6), https://onlinelibrary.wiley.com/doi/full/10.1111/jofi.13395 | P | web | multifactor vol-management survives costs |
| 21 | Daniel, Mota, Rottke, Santos (2020) RFS 33(5), https://www.nber.org/system/files/working_papers/w24164/revisions/w24164.rev0.pdf | P | web | hedged SR² 1.31 → 2.29 |
| 22 | Qian, Sorensen, Hua (2007) JPM 34(1), https://www.pm-research.com/content/iijpormgmt/34/1/27 | P | web | information horizon and turnover |
| 23 | Novy-Marx, Velikov (2022) JFE "Betting against betting against beta", https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf | P | V8L | BAB construction, costs > 55% |
| 24 | Novy-Marx, Velikov (2019) FAJ 75(1), https://www.tandfonline.com/doi/abs/10.1080/0015198X.2018.1547057 | P | web | banding beats cheap-universe |
| 25 | Hoffstein, Sibears, Faber (2019) JII 10(1); Hoffstein, Faber, Braun (2020) SSRN 3673910 | P/W | web | rebalance timing luck > 100 bps |
| 26 | Soebhag, van Vliet, Verwijmeren (2024) JEF, https://www.sciencedirect.com/science/article/pii/S0927539824000525 | P | V8L | construction dispersion > SE |
| 27 | Novy-Marx (2013) JFE 108(1), https://mysimon.rochester.edu/novy-marx/research/OSoV.pdf | P | web | corr −.57; 50/50 SR .85 |
| 28 | Engelberg, McLean, Pontiff (2018) JF 73(5), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12718 | P | web | 6x on earnings days |
| 29 | Bowles, Reed, Ringgenberg, Thornock (2024) JF 79(5), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13372 | P | web | returns concentrated in month 1 after release |
| 30 | Lou, Polk, Skouras (2019) JFE 134, https://personal.lse.ac.uk/polk/research/TugOfWar.pdf | P | V8L | overnight vs intraday |
| 31 | Bogousslavsky (2021) JFE 141(1), https://ideas.repec.org/a/eee/jfinec/v141y2021i1p172-194.html | P | web | intraday accrual, end-of-day attenuation |
| 32 | McLean, Pontiff (2016) JF 71(1), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12365 | P | mem | −26% OOS, −58% post-publication |
| 33 | Chen, Zimmermann (2022) CFR 11(2), https://www.openassetpricing.com/ | P | mem | open-source replication |
| 34 | Jensen, Kelly, Pedersen (2023) JF 78(5), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13249 | P | web | replication, 13 themes |
| 35 | Chen, Velikov (2023) JFQA, "Zeroing in on the expected returns of anomalies" | P | V8L | net post-2005 about 4 bps/mo |
| 36 | Chen, Welch (2026) arXiv 2607.06502 | W | V8L | 7 bps/mo after 2005 |
| 37 | Green, Hand, Zhang (2017) RFS 30(12), https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2262374 | P | web | 2 independent characteristics after 2003 |
| 38 | Novy-Marx, Medhat (2025) NBER w33601 | W | V8L | profitability retrospective |
| 39 | Cohen, Malloy, Nguyen (2020) JF 75(3), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12885 | P | web | Lazy Prices up to 188 bps/mo |
| 40 | Cohen, Frazzini (2008) JF 63(4) | P | web (S for number) | customer momentum 1.55%/mo |
| 41 | Pinchuk (2023) arXiv 2301.11394 "Customer momentum" | W | web | insignificant after 2004 |
| 42 | Menzly, Ozbas (2010) JF 65(4) | P | mem | IO-table cross-predictability |
| 43 | Lou (2012) RFS 25(12) | P | mem | flow-induced trading |
| 44 | Coval, Stafford (2007) JFE 86(2) | P | mem | fire sales |
| 45 | Wardlaw (2020) JF 75(6), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.12962 | P | web | flow-pressure measure mechanical |
| 46 | Brown, Howard, Lundblad (2022) RFS 35(7) "Crowded trades and tail risk" | P | web | most crowded earn most |
| 47 | Eisfeldt, Kim, Papanikolaou (2022) CFR 11(2), https://www.nber.org/papers/w28056 | P | web | intangible value |
| 48 | Weber (2018) JFE 128(3), https://ideas.repec.org/a/eee/jfinec/v128y2018i3p486-503.html | P | web | duration −1.10%/mo |
| 49 | Xing, Zhang, Zhao (2010) JFQA 45(3) | P | web | smirk −10.9%/yr |
| 50 | Cremers, Weinbaum (2010) JFQA 45(2), https://papers.ssrn.com/sol3/papers.cfm?abstract_id=968237 | P | web | 51 bps/wk, declining |
| 51 | Muravyev, Pearson, Pollet (2025) JF 80(6), https://onlinelibrary.wiley.com/doi/10.1111/jofi.13501 | P | web | anomalies vanish after fees |
| 52 | Muravyev, Pearson, Pollet (2022) JF 77(3), https://onlinelibrary.wiley.com/doi/10.1111/jofi.13129 | P | web | fee-risk premium small |
| 53 | Drechsler, Drechsler (2014) NBER w20282, https://www.nber.org/papers/w20282 | W | web | shorting premium |
| 54 | Johnson, So (2012) JFE 106(2) | P | web | O/S 1.47%/mo |
| 55 | An, Ang, Bali, Cakici (2014) JF 69(5) | P | mem | call/put IV changes |
| 56 | Chang, Hong, Liskovich (2015) RFS 28(1), https://www.nber.org/papers/w19290 | P | web | Russell RD price effects |
| 57 | Greenwood, Sammon (2025) JF 80(2), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13410 | P | web | S&P effect 7.4% → < 1% |
| 58 | Brown, Davies, Ringgenberg (2021) RoF 25(4), https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2872414 | P | web | ETF flows 1-4%/mo |
| 59 | Boehmer, Jones, Zhang, Zhang (2021) JF 76(5), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13033 | P | web | retail imbalance about 10 bps/wk |
| 60 | Barber, Huang, Jorion, Odean, Schwarz (2024) JF 79(4), https://onlinelibrary.wiley.com/doi/abs/10.1111/jofi.13334 | P | web | BJZZ finds 35%, mis-signs 28% |
| 61 | Wermers, Yao, Zhao (2012) RFS 25(12), https://papers.ssrn.com/sol3/papers.cfm?abstract_id=891728 | P | web | skill-weighted holdings |
| 62 | Gervais, Kaniel, Mingelgrin (2001) JF 56(3) | P | mem | high-volume premium |
| 63 | Jegadeesh, Livnat (2006) JAE 41 | P | mem | revenue surprise |
| 64 | Brav, Jiang, Partnoy, Thomas (2008) JF 63(4) | P | mem | 13D activism |
| 65 | Frazzini, Israel, Moskowitz (2018) SSRN 3229719 "Trading costs", https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3229719 | W | web | live costs about 10x lower |
| 66 | Novy-Marx, Velikov (2016) RFS 29(1) "A taxonomy of anomalies and their trading costs" | P | mem | buy/hold spreads; < 50%/mo survives |
| 67 | Landier, Simon, Thesmar (2015) WP | W | V8L | capacity elasticity 2 to decay |
| 68 | Ratcliffe, Miranda, Ang (2017) SSRN 2861324 | W | V8L | 1-day → 5-day: $65bn → $320bn |
| 69 | Harvey, Liu, Zhu (2016) RFS 29(1), doi 10.1093/rfs/hhv059 | P | mem | t > 3 |
| 70 | Bailey, López de Prado (2014) JPM 40(5), https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551 | P | mem | deflated Sharpe |
| 71 | Harvey, Liu (2020) JF 75(5), https://arxiv.org/pdf/2006.04269 | P | mem | prior-dependent thresholds |
| 72 | Chordia, Goyal, Saretto (2020) RFS 33(5), https://academic.oup.com/rfs/article/33/5/2134/5739455 | P | V8L | t 3.8 / 3.4 |
| 73 | Andrews, Kitagawa, McCloskey (2024) QJE 139(1), https://www.nber.org/papers/w25456 | P | web | inference on winners |
| 74 | López de Prado, Lewis (2019) Quantitative Finance [verify venue] | P | mem | effective N by clustering |
| 75 | Arnott, Harvey, Markowitz (2019) JPM "A backtesting protocol" | P | V8L | a modified model is not OOS |
| 76 | Dwork et al. (2015) NeurIPS, reusable holdout | P | V8L | holdout as pass / fail |
| 77 | Freyberger, Neuhierl, Weber (2020) RFS 33(5) "Dissecting characteristics nonparametrically" | P | mem | nonlinear return-characteristic relation (§2.1) |
| 78 | Martineau (2022) CFR "Rest in peace post-earnings announcement drift" | P | V8L | no PEAD in large caps after 2006 |
| 79 | Gerard, Jehl (2025) Quoniam WP | W | V8L | EAR-12m t 2.94 in the top 1,000 |
| 80 | Stambaugh, Yu, Yuan (2012) JFE 104(2) "The short of it" | P | mem | short-leg concentration |
| 81 | Ehsani, Harvey, Li (2023) FAJ (X-4's basis) | P | V8L | within-industry value |
| 82 | Zhang (2006) JF 61(1) "Information uncertainty and stock returns" | P | mem | momentum stronger under uncertainty (§2.2) |
| 83 | Ni, Pearson, Poteshman (2005) JFE 78(1) "Stock price clustering on option expiration dates" | P | mem | expiration pinning (§2.3) |
| 84 | Pástor, Stambaugh, Taylor (2015) JFE 116(1) "Scale and skill in active management" | P | mem | decreasing returns to scale (§4.1) |
| 85 | Dechow, Sloan, Soliman (2004) RAS 9 "Implied equity duration" | P | mem | duration measure (F11) |
| V8L | `.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md` | S (house) | - | prior review: base rates, haircuts, do-not-build |
