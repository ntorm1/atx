# RenTech / Medallion — Broad/Primary Research (foundational architecture, signals, data, modeling philosophy)

Angle: broad/primary. Anchor account of RenTech's overall system structure, the kinds of
signals and data they ingest, and their core modeling philosophy.

## Top sources (ranked by relevance to the original question)

1. **The Man Who Solved the Market (Gregory Zuckerman) — book notes / summaries** — HIGH
   - Novel Investor notes: https://novelinvestor.com/notes/the-man-who-solved-the-market-by-gregory-zuckerman/
   - InvestmentNews guide: https://www.investmentnews.com/guides/what-the-man-who-solved-the-market-teaches-about-quantitative-investing/265700
   - "Some Ben?" detailed chapter notes: https://blog.someben.com/2019/11/notes-on-man-who-solved-the-market-jim-simons/
   - The single most authoritative non-fiction account. Documents the IDA/NSA codebreaking lineage,
     Baum (HMM/Baum-Welch), the IBM speech-recognition recruits Mercer & Brown, and the 1990s
     re-architecture that unified all signals into one trading system.

2. **Daniel Scrivner — Renaissance Technologies business breakdown** — HIGH
   - https://www.danielscrivner.com/renaissance-technologies-business-breakdown/
   - Structured breakdown of the quantitative model, returns, moat, culture, origin story.

3. **navnoorbawa Substack — "$100 Billion Built on Statistical Arbitrage"** — HIGH
   - https://navnoorbawa.substack.com/p/renaissance-technologies-the-100
   - Stat-arb framing; thousands of small, faint-edge trades + law of large numbers.

4. **quartr.com — Renaissance Technologies and The Medallion Fund** — MEDIUM/HIGH
   - https://quartr.com/insights/edge/renaissance-technologies-and-the-medallion-fund
   - Covers both the firm structure AND the speech-recognition/HMM lineage (Mercer/Brown ex-IBM).

5. **quantvps.com — Jim Simons Trading Strategy Explained** — MEDIUM
   - https://www.quantvps.com/blog/jim-simons-trading-strategy
   - Practitioner-oriented overview of signals, automation, "never override the computer".

6. **acontinuallearner Medium — Mathematics behind the world's most profitable hedge fund** — MEDIUM
   - https://acontinuallearner.medium.com/uncovering-the-mathematics-behind-the-worlds-most-profitable-hedge-fund-79770d772997
   - Math-leaning treatment (HMMs, signal modeling) bridging into the modeling-philosophy angle.

## Key transferable system-design lessons extracted

- **Unify signals into one system.** The performance inflection came when all trading signals and
  portfolio requirements were integrated into a single trading system rather than siloed strategies.
- **Three-step signal vetting (Medallion, ~1997):** (1) find anomalous patterns in historic pricing
  data; (2) confirm the anomaly is statistically significant, consistent over time, and non-random;
  (3) require a plausible explanation for the pricing behavior. ("Anomalous patterns we would not
  expect to occur at random." — Simons)
- **Data hygiene as a moat.** Massive collection + meticulous cleaning of financial data, back to the
  1700s (World Bank, Federal Reserve), plus commodities, FX, weather, shipping, satellite imagery,
  filings, insider trades, news, insurance claims; ~1 TB added annually.
- **Many faint edges, not few big bets.** 150k–300k trades/day, each with a slight statistical edge;
  the law of large numbers compounds them.
- **HMM / speech-recognition lineage.** Markets modeled as chains of hidden Markov states; the
  Baum-Welch algorithm (Lenny Baum) estimates HMM parameters on historical financial data to forecast
  market "states" — directly transplanted from IBM speech recognition (Mercer, Brown) and IDA crypto.
- **Discipline / no-override.** "Never override the computer" — eliminate emotional/discretionary bias;
  let continuously-evolving algorithms update rules from live data without human intervention.
