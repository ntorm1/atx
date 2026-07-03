# ESG & Sustainability Datasets — vendor schemas + regulatory reconstruction

**Status:** Research, v0.1
**Audience:** ats-eqt engineering team (ingestion, storage, query); ats-core team designing request-shape primitives; product team scoping the ESG/sustainability surface
**Scope:** Field-level schema for the nine principal commercial ESG vendor products (MSCI, S&P Global CSA / DJSI, Sustainalytics, Bloomberg ESG, Refinitiv/LSEG ESG, ISS ESG, FactSet Truvalue, CDP, GRESB); the regulatory disclosure substrate that can replace or augment them (CSRD/ESRS, SEC climate, California SB 253/SB 261, EU SFDR, UK SDR, Japan TCFD-aligned, Form SD, Form 10-K human capital, DEF 14A, Companies House Gender Pay Gap); the academic finding that ESG ratings are functionally uncorrelated; and a recommended bitemporal long-format `esg_metric`/`esg_score`/`ghg_emission` schema for ats-eqt.
**Last updated:** 2026-05-14

---

## 0. Executive summary

ESG is the **single largest open-data competitive wedge** in `ats-eqt`'s Phase-0 stack, for one reason that has nothing to do with ats-eqt's engineering and everything to do with the substrate itself: **commercial ESG ratings disagree.** Berg, Kölbel & Rigobon (*Review of Finance* 2022, "Aggregate Confusion") measured pairwise correlation between six major ESG ratings at **0.38–0.71** — an order of magnitude below the ~0.99 cross-rating-agency correlation in credit (source: <https://academic.oup.com/rof/article/26/6/1315/6590670>). Decomposed: **56% of the divergence is measurement** (different indicators for the same attribute), **38% is scope** (different attributes included), only 6% is weighting (same source). The implication is that there is no defensible "true" ESG signal to replicate. The opportunity is **not** to publish opinionated scores but to publish the **raw disclosed metrics** in a bitemporal, evidence-linked long-format store, and to surface vendor scores only as joinable, attributed overlays.

Five headline findings driving the recommendation:

1. **Vendor ESG ratings are uncorrelated and cannot be averaged.** Any ats-eqt product positioning itself as "another ESG rating" is competing in a saturated low-trust market. The defensible move is **raw underlying disclosure**, not synthesis.
2. **The regulatory substrate is consolidating fast.** EU CSRD/ESRS introduced **1,144 mandatory + 269 voluntary datapoints** across 12 standards from FY2024 (first filings 2025/2026); California SB 253 first Scope 1+2 reports are due **2026-08-10**. The "GAAP for ESG" is being built right now in EU iXBRL. ats-eqt should ingest ESRS from year one (source: <https://envoria.com/insights-news/esrs-data-points-guide-for-successful-csrd-reporting>; <https://ww2.arb.ca.gov/news/carb-approves-climate-transparency-regulation-entities-doing-business-california>).
3. **The SEC Climate Disclosure Rule is dead.** After the 5th Circuit stay (2024-03-15), SEC voluntary stay (2024-04), and a Republican SEC majority's vote to end defense (2025-03-27), the rule has effectively been abandoned at federal level (source: <https://www.sec.gov/newsroom/press-releases/2025-58>). ats-eqt's US-climate ingestion must lean on California SB 253/SB 261 + CDP + voluntary 10-K disclosure, not on Reg S-K climate.
4. **MSCI, S&P, Sustainalytics, Bloomberg, Refinitiv, ISS each define their own materiality map.** MSCI's 35 "Key Issues", SASB's 26 industry-specific categories, ESRS's "double-materiality" of impact-and-financial, and the GRI sectoral standards do not align cleanly. ats-eqt's canonical metric dictionary (`esg_metric_dim`) must be a many-to-many crosswalk, not a single hierarchy.
5. **Pricing is high and bundled.** MSCI ESG Manager institutional licences regularly clear $50k–$500k+; Sustainalytics, Bloomberg ESG, Refinitiv ESG sit in the same band. The fundamentals-cohort price elasticity is real: open-data raw-disclosure feeds that *cite the underlying filing* are credible substitutes for the bottom-of-funnel use case (screening, exclusion lists, fund prospectus disclosure).

The remainder of the document specifies each vendor at field/schema level, what is publicly reconstructable, what is not, and a recommended ats-eqt schema.

---

## 1. Why ESG is fundamentally different from fundamentals

Compare the two substrates:

| Dimension | Fundamentals (10-K/10-Q) | ESG/Sustainability |
|---|---|---|
| Accounting standard | US-GAAP / IFRS — converged, auditable, machine-comparable | None universally adopted. ESRS (EU, mandatory 2024+), SASB (US, industry-specific, voluntary), GRI (global, voluntary), TCFD (climate, voluntary), GHG Protocol (emissions only) |
| Assurance | Mandatory external audit (Big-4) | Mostly voluntary; CSRD requires limited assurance from FY2024, reasonable assurance phased in by 2028. CA SB 253 requires limited assurance from 2026, reasonable from 2030 |
| Comparability | Cross-company, cross-period stable | Methodology-dependent; vendor scores 0.38–0.71 correlated |
| Source format | XBRL-tagged (mandatory since 2009 large filers) | Mixed — narrative PDF, iXBRL (CSRD), CSV uploads (CDP), survey response (CSA) |
| Universe | ~7,000 SEC filers, ~50,000 global filers | CSRD scope ~50,000 EU + non-EU; CDP ~24,000 respondents; MSCI rates ~9,000+; Bloomberg covers 15,000 |
| Restatement | Rare, structured | Routine, often unmarked — companies silently change methodology between annual reports |

The absence of a single chart-of-accounts is the principal reason vendor opinions diverge so widely. There is no "Revenue" equivalent in ESG. Even an apparently-objective metric like *Scope 1 GHG emissions* admits substantive methodology choices (operational vs equity control boundary; biogenic vs fossil; recent acquisitions in or out) that drive 10%+ differences between vendors recording the "same" company.

The regulatory tailwind is the saving grace: ESRS, SB 253, CSDDD, and the GHG Protocol jointly force a structured, machine-readable, externally-assured layer underneath the messy vendor stack. Five-year outlook is that the **raw disclosed metrics** become the trustworthy substrate and the vendor "scores" become opinionated commentary layered on top.

---

## 2. Vendor stack matrix

Headline comparative across the nine principal commercial products. Coverage and pricing are best public estimates as of 2026-05; many vendors do not publish official rate cards.

| Vendor / Product | Rating scale | # of metrics / fields | Methodology | Coverage | History | Pricing tier (US$, /yr) |
|---|---|---|---|---|---|---|
| MSCI ESG Ratings | AAA / AA / A / BBB / BB / B / CCC (7 letter bands) + 0–10 industry-adjusted | 35 Key Issues / 1,000+ data points | Industry-relative; Key Issue exposure × management; Leaders/Average/Laggards bucketing | ~9,500+ companies, 700k+ securities incl. funds | 2007+ (current methodology); legacy KLD back to 1991 | $50k–$500k+ (ESG Manager licence; Direct Equity ESG; ratings-fund overlay) |
| S&P Global CSA / DJSI | CSA Total Score 0–100 + Industry Mover/Leader/Bronze/Silver/Gold | ~23 question dimensions × industry-specific; ~600+ data points per industry questionnaire | Survey-driven; media & stakeholder analysis (MSA) overlay; 61 GICS-derived industry questionnaires (was 23, expanded; "23 industry-specific questionnaires" framing is outdated) | ~5,000 largest companies invited; ~3,500 voluntarily respond annually | 1999+ (SAM heritage; DJSI launched 1999) | $25k–$200k via S&P Marketplace |
| Sustainalytics (Morningstar) ESG Risk Rating | Negligible 0–9.99 / Low 10–19.99 / Medium 20–29.99 / High 30–39.99 / Severe 40+ | 20 Material ESG Issues × subindustry; ~200 indicators, ~1,800 data points | Exposure × Management → Unmanaged Risk; Controversy Rating 1–5; v3.1 methodology (June 2024) | ~16,000+ companies | 2018+ (current "Risk Rating" methodology); legacy ESG rating back to 2009 | $40k–$300k (Morningstar Direct + Sustainalytics premium) |
| Bloomberg ESG | Disclosure Score 0–100 + ES Scores 0–10 | 5,100+ ESG fields; coverage 15,000+ companies; GHG estimates extend to 130,000+ | Field hierarchy: Pillars → Issues → Sub-Issues → Fields; BECS (Bloomberg ESG Classification System) | 15,000+ companies (Disclosure); 130,000+ (GHG estimates) | 2006+ (Disclosure Score); proprietary ES Scores launched 2020 | Bundled into Terminal ($31,980/seat/yr 2026); Data License separate, $100k–$1M+ |
| Refinitiv / LSEG ESG | ESG Score 0–100 (and A+ → D− grade); ESGC overlays controversies | 630+ ESG measures (current); 10 category scores; 23 controversy topics | Percentile-rank within sector for 10 categories; materiality 1–10 per category per industry; ESGC = avg(ESG, Controversies) when controversies > 0 | 25,000+ companies (current); 16,000+ globally with full coverage | 2002+ (Asset4 heritage; rebranded Refinitiv 2018; LSEG 2021+) | Bundled in Workspace ($1.5k–$3k/user/month base + ESG add-on $500–$2k/user/month) |
| ISS ESG | QualityScore 1–10 (decile, 1=best); Climate Solutions; Norm-Based Research pass/fail; SDG Solutions Assessment | 230+ factors (Governance QualityScore); separate climate, norm-based, SDG datasets | Decile rank within region; sub-pillar (Board, Comp, Shareholder Rights, Audit & Risk Oversight); Climate aligned to TCFD | ~6,000 companies (Governance); ~25,000 (Climate); ~10,000 (Norm-Based) | 2002+ (Governance heritage); QualityScore launched 2016 | $30k–$200k+ |
| FactSet Truvalue (acq. 2020) | Insight 0–100, Pulse 0–100, Momentum −/+, Volume # | 26 SASB-aligned categories; NLP-derived from 150k+ unstructured sources | Real-time NLP scoring; SASB materiality map; signed sentiment per category | ~22,000 companies | 2007+ (Truvalue Labs); Insight = long-term, Pulse = ~12-month, Momentum = directional | $50k–$300k via Open:FactSet |
| CDP | Disclosure A/A− / B/B− / C/C− / D/D− / F (3 themes: Climate, Forests, Water) + Supplier Engagement Rating | Climate Change questionnaire ~80–100 questions; Forests ~50; Water ~60 | Scoring methodology (Disclosure → Awareness → Management → Leadership) per theme | ~24,000 corporates responded in 2024; ~750 cities; ~300 states/regions | 2002+ (Climate); 2010+ (Water); 2017+ (Forests) | Scores tier free; full questionnaire data tier from ~$15k/yr |
| GRESB | GRESB Score 0–100; 1–5 star rating (quintile); Management + Performance components | ~50–100 indicators per assessment; Real Estate, Infrastructure Asset, Infrastructure Fund, Development assessments | Relative scoring vs peer group; Management (policy/strategy) + Performance (asset KPIs: GHG, energy, water, waste) | ~2,200 Real Estate entities, ~210 Infrastructure Funds, ~750 Infrastructure Assets (2024 cycle) | 2009+ | $10k–$50k member fee + assessment fee scales by AUM |

Sources for the table: see per-vendor sections §3–§11.

---

## 3. MSCI ESG Ratings + ESG Research

### 3.1 The rating scale

A seven-band letter scale from `AAA` (best) → `CCC` (worst), assigned **industry-relative** so a `AAA` materials company is not directly comparable to a `AAA` software company in absolute risk terms (source: <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Ratings+Methodology.pdf>):

| Band | Score range (industry-adjusted) | Bucket |
|---|---|---|
| AAA | 8.6 – 10.0 | Leader |
| AA | 7.1 – 8.6 | Leader |
| A | 5.7 – 7.1 | Average |
| BBB | 4.3 – 5.7 | Average |
| BB | 2.9 – 4.3 | Average |
| B | 1.4 – 2.9 | Laggard |
| CCC | 0.0 – 1.4 | Laggard |

(source: <https://www.msci.com/data-and-analytics/sustainability-solutions/esg-ratings>; <https://www.blackrock.com/us/financial-professionals/tools/esg-methodology>)

### 3.2 The 35 Key Issues framework

MSCI maintains a framework of **35 ESG Key Issues** grouped into 3 pillars (Environment, Social, Governance) and 10 themes. Industry exposure to each Key Issue is calibrated per GICS sub-industry on a 0–10 scale; the issues that are *not material* for a given industry carry zero weight (i.e., a software company is not penalized on `Toxic Emissions & Waste`). Each material Key Issue typically weights **5%–30%** of the rating.

The published 10-theme × 35-issue map (source: <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Ratings+Methodology.pdf>):

```
Environment Pillar
  Climate Change theme:
    1. Carbon Emissions
    2. Product Carbon Footprint
    3. Financing Environmental Impact
    4. Climate Change Vulnerability
  Natural Capital theme:
    5. Water Stress
    6. Biodiversity & Land Use
    7. Raw Material Sourcing
  Pollution & Waste theme:
    8. Toxic Emissions & Waste
    9. Packaging Material & Waste
    10. Electronic Waste
  Environmental Opportunities theme:
    11. Opportunities in Clean Tech
    12. Opportunities in Green Building
    13. Opportunities in Renewable Energy

Social Pillar
  Human Capital theme:
    14. Labor Management
    15. Health & Safety
    16. Human Capital Development
    17. Supply Chain Labor Standards
  Product Liability theme:
    18. Product Safety & Quality
    19. Chemical Safety
    20. Consumer Financial Protection
    21. Privacy & Data Security
    22. Responsible Investment
    23. Health & Demographic Risk
  Stakeholder Opposition theme:
    24. Controversial Sourcing
    25. Community Relations
  Social Opportunities theme:
    26. Access to Communications
    27. Access to Finance
    28. Access to Health Care
    29. Opportunities in Nutrition & Health

Governance Pillar
  Corporate Governance theme (universally weighted, applies to ALL industries):
    30. Ownership & Control
    31. Board
    32. Pay
    33. Accounting
  Corporate Behavior theme (universally weighted):
    34. Business Ethics
    35. Tax Transparency
```

The 35-issue count is sometimes reported as 33 or 37 in older or different MSCI publications; the canonical 2023 methodology PDF cites **35**. `[partial verification — internal MSCI documents have varied over the years; we cite the 2023 published methodology]`

### 3.3 Exposure × Management calculation

For each material Key Issue, MSCI computes:

- **Exposure score** (0–10): how much the company is exposed to the risk/opportunity, driven by business segment + geographic mix + asset profile.
- **Management score** (0–10): how well the company is managing the risk, from policies, programs, performance metrics, target-setting, controversies.

The **Key Issue Score** = function of `(exposure, management)`. High exposure + high management = decent score; high exposure + low management = laggard; low exposure makes management score moot. The function is non-linear and proprietary, but published as a "Risk Management Approach" with three regions: Major Risks (exposure ≥ 7), Major Opportunities, and Average Risk.

Weighted-average across material Key Issues produces the **Industry-Adjusted Score** (0–10) which maps to a letter via the table above.

### 3.4 The MSCI ESG Manager API + schema

MSCI's institutional delivery is via:

- **MSCI ESG Manager** — the analyst-facing web platform; primary research surface.
- **MSCI ESG Direct API** — REST/JSON; OAuth2; subscriber-only.
- **Datafeed (flat-file SFTP)** — daily/weekly XML/CSV per product.
- **Snowflake share** — newer (2022+) MSCI Marketplace listing.

Representative ESG Manager schema fields (sample, from the published methodology + third-party scorecards) (source: <https://www.thegoodlobby.eu/wp-content/uploads/2024/12/>):

| Field family | Example fields |
|---|---|
| Rating | `IVA_COMPANY_RATING` (AAA-CCC), `INDUSTRY_ADJUSTED_SCORE` (0–10), `WEIGHTED_AVERAGE_KEY_ISSUE_SCORE`, `ESG_RATING_DATE`, `ESG_RATING_TREND` (Positive/Stable/Negative) |
| Pillar/theme scores | `ENVIRONMENTAL_PILLAR_SCORE`, `SOCIAL_PILLAR_SCORE`, `GOVERNANCE_PILLAR_SCORE` |
| Key Issue scores (per issue, ~35) | `CARBON_EMISSIONS_SCORE` (0–10), `CARBON_EMISSIONS_EXP_SCORE`, `CARBON_EMISSIONS_MGMT_SCORE`, … |
| Carbon-specific | `CARBON_EMISSIONS_SCOPE_1` (tCO2e), `CARBON_EMISSIONS_SCOPE_2`, `CARBON_EMISSIONS_SCOPE_3_TOTAL`, `CARBON_INTENSITY_REVENUE`, `WACI` (weighted-avg carbon intensity) |
| Controversies | `CONTROVERSY_FLAG` (Red/Orange/Yellow/Green), `CONTROVERSY_CASES_NUMBER`, controversy-by-category fields |
| Business involvement screens | `WEAPONS_TIE`, `FOSSIL_FUEL_EXTRACTION_TIE`, `TOBACCO_TIE`, etc. (boolean + revenue % from each) |
| Identifier | `ISSUER_ID`, `ISSUER_NAME`, `ISSUER_ISIN`, `ISSUER_CUSIP`, `ISSUER_TICKER`, `GICS_SUB_INDUSTRY` |

`[unverified — exact field names follow MSCI ESG Manager naming conventions; precise spellings vary across product variants]`

### 3.5 Coverage and history

- **9,500+ corporate issuers** rated under the IVA (Intangible Value Assessment) / current Ratings methodology (source: <https://www.msci.com/data-and-analytics/sustainability-solutions/esg-ratings>).
- **700,000+ securities** covered including funds, fixed-income, sovereigns.
- **History depth:** current methodology from ~2007; KLD (Kinder, Lydenberg, Domini) legacy database back to **1991** is bundled in the academic distribution via WRDS (the "KLD STATS" file is the typical academic-citation source).
- **Update cadence:** annual deep review per issuer; continuous Controversies monitoring; rolling daily updates as new disclosures arrive.

### 3.6 Pricing

- **Direct Equity ESG (ratings + key-issue scores):** institutional licence in the **$50k–$200k/yr** range per asset class.
- **ESG Manager full-featured (Climate, Controversies, Business Involvement, Impact):** **$200k–$500k+/yr** for full enterprise tier.
- **Index-linked products** (ESG Leaders Index licences for ETF/index funds): scales with AUM.

MSCI is reported to be the most expensive of the major ESG vendors but the most-cited in regulatory and fund-prospectus contexts (source: <https://www.lseg.com/en/insights/data-analytics/understanding-how-esg-scores-are-measured-their-usefulness-and-how-they-will-evolve>).

---

## 4. S&P Global Corporate Sustainability Assessment (CSA) / DJSI

### 4.1 Heritage and product

The Corporate Sustainability Assessment was built by **SAM Group AG** (Sustainable Asset Management, Zurich) starting 1999; SAM was acquired by **RobecoSAM** and then by **S&P Global in 2019**, integrated into S&P Sustainable1 (source: <https://www.spglobal.com/sustainable1/en/csa>). The CSA is the input that feeds both the Dow Jones Sustainability Indices (DJSI) and the S&P ESG Index family.

### 4.2 The questionnaire

- ~**5,000 largest publicly-listed companies invited annually** to complete the CSA; ~3,500 typically respond.
- **61 industry-specific questionnaires** (frequently miscited as "23" — that was an older taxonomy; the current count maps to GICS-derived industry buckets) (source: <https://www.anthesisgroup.com/insights/the-sp-corporate-sustainability-assessment/>).
- Questions are a mix of **quantitative**, **qualitative**, and **evidence-based** (require URL/document citation).
- Each industry questionnaire covers ~80–120 questions across **20–30 criteria** grouped under E / S / G dimensions.
- The questionnaire window runs annually (typically March–July submission, results in September Sustainability Yearbook).

### 4.3 Media and Stakeholder Analysis (MSA) overlay

CSA scoring is **adjusted** by S&P's **MSA process** — a continuous (~daily) monitoring of news, NGO reports, regulatory actions for ESG-relevant events. Where MSA flags a serious controversy, the criterion-level score is **down-adjusted** mid-cycle. This is functionally S&P's analog of MSCI's Controversies overlay.

### 4.4 Scoring

- **CSA Total Score** = 0–100, weighted across 3 dimensions (E / S / G), each made up of criteria, each made up of questions.
- **Dimension scores** (E_Score, S_Score, G_Score) = 0–100.
- **Criterion scores** = 0–100.
- **DJSI inclusion threshold:** **top 10% in each industry** for DJSI World; broader thresholds for DJSI North America, Europe, etc.
- **Sustainability Yearbook membership tiers:** Top 1% (Gold), top 5% (Silver), top 10% (Bronze), plus "Industry Mover" recognition for largest YoY improvers.

### 4.5 Schema in S&P Marketplace

S&P delivers CSA data via Marketplace (Snowflake share, Xpressfeed) under the **CSA Annual Scores Dataset**. Representative table prefix is `csa_*`:

| Table | Purpose | Primary key |
|---|---|---|
| `csa_company` | Company master (CIQ companyId, GICS, industry-questionnaire assignment, response status) | `companyId` |
| `csa_assessment` | One row per company-year-questionnaire | `(companyId, assessmentYear, industryId)` |
| `csa_dimension_score` | E/S/G dimension scores per company-year | `(companyId, assessmentYear, dimensionCode)` |
| `csa_criterion_score` | Per-criterion score (0–100) | `(companyId, assessmentYear, criterionId)` |
| `csa_question_response` | Per-question response (where disclosure permitted) | `(companyId, assessmentYear, questionId)` |
| `csa_msa_event` | Media/stakeholder analysis events with severity, source URL, criterion impacted | `eventId` |
| `csa_djsi_membership` | Annual DJSI World/Regional/Country index membership flags | `(companyId, indexCode, year)` |
| `csa_yearbook` | Sustainability Yearbook tier flags (Gold/Silver/Bronze/Mover) | `(companyId, year)` |

`[unverified — exact table names follow S&P Marketplace conventions; the `csa_*` prefix family is consistent with S&P's other product schemas (e.g., `ciq*`, `co_*`)]`

### 4.6 Pricing and licensing

- **CSA Annual Score dataset:** **$25k–$80k/yr** for the score-level extract.
- **CSA Full Detail (criterion + question response):** **$80k–$200k/yr** for institutional buyers (the historical preference of academic researchers).
- **DJSI Index licensing:** separate; scales with index-linked AUM.
- **Note:** Companies that don't participate in the CSA can still be **publicly-data-only scored** by S&P; the published rating distinguishes between "Participating" and "Public Information" assessments.

---

## 5. Sustainalytics (Morningstar) ESG Risk Rating

### 5.1 The Risk Rating scale

Sustainalytics restructured its product in 2018 into the **ESG Risk Rating** framework (source: <https://www.sustainalytics.com/docs/knowledgehublibraries/default-document-library/sustainalytics_-esg-risk-ratings_-version-3-1_-methodology-abstract_-june-2024.pdf>):

| Band | Range | Interpretation |
|---|---|---|
| Negligible | 0 – 9.99 | Negligible enterprise ESG risk |
| Low | 10 – 19.99 | Low ESG risk |
| Medium | 20 – 29.99 | Medium ESG risk |
| High | 30 – 39.99 | High ESG risk |
| Severe | 40+ | Severe ESG risk |

**The score is risk-coded: lower is better.** This is the opposite directional convention from MSCI, S&P CSA, Refinitiv, Bloomberg — a frequent integration pitfall.

### 5.2 The Exposure × Management framework

Each company's score is built from **20 Material ESG Issues** per subindustry. For each issue:

- **Exposure score** — beta-style measure of how exposed the business model is to that issue, calibrated at the subindustry level.
- **Manageable Exposure** — the portion of exposure the company could realistically influence through policy, programs, performance.
- **Management score** (0–100) — assesses policies, programs, performance, controversies tied to that issue.
- **Managed Risk** = Manageable Exposure × Management.
- **Management Gap** = Manageable Exposure × (100% − Management).
- **Unmanageable Risk** = Exposure − Manageable Exposure (industry-inherent residual).
- **Unmanaged Risk** = Management Gap + Unmanageable Risk = **what flows into the final ESG Risk Rating.**

The total **ESG Risk Rating** is the sum of Unmanaged Risk across all 20 Material ESG Issues for that subindustry.

The May 2024 v3.1 methodology overhauled the **Corporate Governance** assessment to be more dynamic; this was the largest single update since launch (source: <https://www.sustainalytics.com/docs/knowledgehublibraries/default-document-library/sustainalytics_-esg-risk-ratings_-version-3-1_-methodology-abstract_-june-2024.pdf>).

### 5.3 Controversies (separate product)

Sustainalytics' **Controversies Rating** is a 1–5 categorical scale (Category 1 = Low Impact → Category 5 = Severe), assigned to specific controversy events tied to a company. The 1–5 controversy event feeds the Management scoring of the relevant Material Issue.

### 5.4 Carbon Risk Rating

Separate **Carbon Risk Rating** product (the same 0–100+ "lower-is-better" scale) drilling into transition-risk exposure under multiple temperature scenarios (1.5°C, 2.0°C). Built on the same Exposure × Management framework but with only carbon-relevant issues.

### 5.5 Schema and API

- **Sustainalytics ESG Risk Rating Data Service** — REST API; OAuth2; rate-limited.
- **Bulk extract** via Morningstar Direct.
- **WRDS distribution** for academic users.

Representative fields (sample):

| Field | Notes |
|---|---|
| `entity_id`, `entity_name`, `isin`, `cusip` | Identifiers |
| `industry_group`, `subindustry` | Sustainalytics own classification |
| `esg_risk_score` | 0–100+, the headline (lower is better) |
| `esg_risk_category` | Negligible / Low / Medium / High / Severe |
| `exposure_score` | 0–100 |
| `management_score` | 0–100 |
| `controversy_rating` | 1–5 |
| `controversy_level` | Low/Moderate/Significant/High/Severe |
| Per-Material-Issue (×20): `mei_<issue>_exposure`, `mei_<issue>_mgmt`, `mei_<issue>_unmanaged_risk` | The 20 subindustry-material issues |
| `governance_score` | Standalone since v3.1 (May 2024) |
| `assessment_date`, `methodology_version` | Provenance |

(source: <https://www.morningstar.com/content/dam/marketing/shared/research/methodology/SustainabilityRatingMethodology_2021.pdf>; <https://www.thegoodlobby.eu/wp-content/uploads/2024/12/TGL-Scorecard-Sustainalytics.pdf>)

### 5.6 Coverage, history, pricing

- **16,000+ companies** rated globally.
- **History depth:** current Risk Rating from 2018; legacy ESG Rating series goes back to ~2009.
- **Pricing:** Morningstar Direct + Sustainalytics premium bundle clears **$40k–$300k/yr** depending on user count and data depth.

---

## 6. Bloomberg ESG

### 6.1 The 5,100+ field universe

Bloomberg ESG covers **5,100+ ESG fields** across **15,000+ companies** (source: <https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf>). Fields use the standard Bloomberg upper-snake-case mnemonic convention.

Representative published field families (a small subset of the 5,100):

```
Climate / Emissions
  GHG_SCOPE_1                              -- Scope 1 emissions, tCO2e
  GHG_SCOPE_2_LOCATION_BASED               -- Scope 2 location-based, tCO2e
  GHG_SCOPE_2_MARKET_BASED                 -- Scope 2 market-based, tCO2e
  GHG_SCOPE_3_TOTAL                        -- Scope 3 total, tCO2e
  GHG_SCOPE_3_<CATEGORY>                   -- 15 GHG Protocol Scope 3 categories
  GHG_INTENSITY_PER_SALES                  -- tCO2e / $M revenue
  ENERGY_CONSUMPTION_TOTAL                 -- MWh
  RENEWABLE_ENERGY_CONSUMPTION_PCT
  ENERGY_INTENSITY_PER_SALES

Water
  WATER_WITHDRAWAL_TOTAL                   -- m3
  WATER_DISCHARGE_TOTAL
  WATER_CONSUMPTION_TOTAL
  WATER_RECYCLED_PCT

Waste
  WASTE_GENERATED_TOTAL                    -- tonnes
  WASTE_RECYCLED_PCT
  HAZARDOUS_WASTE_TOTAL

Workforce
  EMPLOYEES_TOTAL                          -- headcount
  EMPLOYEE_TURNOVER_PCT
  EMPLOYEES_FEMALE_PCT
  WOMEN_IN_MGMT_PCT
  TRAINING_HOURS_PER_EMPLOYEE
  LOST_TIME_INJURY_RATE
  FATALITIES_TOTAL

Governance
  PCT_BOD_INDEPENDENT                      -- % independent directors
  PCT_BOD_FEMALE                           -- % female directors
  BOARD_SIZE
  CHAIRMAN_CEO_SAME_PERSON
  SAY_ON_PAY_VOTE_PCT_FOR
  CEO_PAY_RATIO
  AUDIT_COMMITTEE_INDEPENDENT_PCT
```

(source field names: <https://bautheac.github.io/BBGsymbols/>; <https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf>; <https://assets.bbhub.io/professional/sites/10/GHG.pdf>)

### 6.2 Bloomberg ESG Scores (proprietary aggregate)

Bloomberg launched **proprietary ES Scores** in 2020 to overlay opinions on top of the disclosure fields (source: <https://www.bloomberg.com/company/press/bloomberg-launches-proprietary-esg-scores/>). The scoring hierarchy:

```
Pillars (E, S, G)
  → Issues (e.g. Climate Change, Health & Safety, Board Composition) — ~30+ risk factors
    → Sub-Issues (more granular underneath each Issue)
      → Fields (the 5,100+ data fields)
```

- **Field-level data → Sub-Issue scores → Issue scores → Pillar scores → Overall ES Score.**
- All scoring done on the **Bloomberg ESG Classification System (BECS)**, Bloomberg's proprietary industry taxonomy purpose-built for ESG (not GICS, not BICS).
- Bloomberg deliberately separates **E and S scores** from **G** (governance scored separately) so users can construct E/S-only or G-only views.
- Scores normalized for size bias, peer-relative within BECS industry.
- Scoring is **fully transparent**: every Sub-Issue → Field mapping is documented in the methodology PDF.

### 6.3 Bloomberg Industry-Adjusted ESG Score

A later (late 2010s) overlay that adjusts the absolute ES Score for industry exposure, conceptually similar to MSCI's Industry-Adjusted Score. `[unverified — Bloomberg's terminology has evolved; "ES Score" is the current naming with industry adjustment built into the Sub-Issue → Issue aggregation rules]`

### 6.4 GHG Emissions Estimates

Bloomberg uniquely runs a **GHG Estimates model** that extends the reported-emissions universe (15k companies) to **130,000+ companies** by predicting emissions for non-disclosing entities (source: <https://www.bloomberg.com/professional/insights/sustainable-finance/bloombergs-greenhouse-gas-emissions-estimates-model-a-summary-of-challenges-and-modeling-solutions/>; <https://assets.bbhub.io/professional/sites/10/GHG.pdf>). The model uses industry-segment baselines + revenue + region. Estimated emissions are flagged distinctly from reported.

### 6.5 Coverage and history

- **15,000+ companies** with structured ESG disclosure fields.
- **130,000+ companies** with GHG estimates (via the model).
- **History depth:** ESG Disclosure Scores from ~2006; ES Scores from 2020.
- **Field-level history:** depends on company disclosure cadence; typically annual; some intra-year refresh for assured Scope 1+2.

### 6.6 Delivery and pricing

- **Terminal** (`ESG <GO>`) — bundled in standard subscription, $31,980/seat/yr 2026.
- **Data License (DL)** — bulk via SFTP / Data License Plus / Snowflake Native App.
- **BQL/BQNT** — programmatic.
- **Bloomberg ESG-specific add-on for enterprise:** **$100k–$500k+/yr** beyond Terminal.

---

## 7. Refinitiv ESG (now LSEG Data & Analytics ESG)

### 7.1 Heritage

The Refinitiv ESG data product originates from **Asset4**, a Swiss firm acquired by Thomson Reuters in 2009, becoming "Thomson Reuters ESG", then "Refinitiv ESG" (2018), now "LSEG ESG" (2021+) (source: <https://www.lseg.com/content/dam/data-analytics/en_us/documents/methodology/lseg-esg-scores-methodology.pdf>).

### 7.2 The 630+ ESG measures

LSEG's ESG product collects **630+ ESG measures** per company (some sources cite 450; the current methodology PDF cites 630+) across the 3 pillars (source: <https://blogs.cranfield.ac.uk/wp-content/uploads/2021/05/refinitiv-esg-scores-methodology-May22-1.pdf>; <https://www.lseg.com/content/dam/data-analytics/en_us/documents/methodology/lseg-esg-scores-methodology.pdf>). Measures organized into 10 categories:

```
Environment Pillar — 3 categories:
  Resource Use
  Emissions
  Innovation (env-related)

Social Pillar — 4 categories:
  Workforce
  Human Rights
  Community
  Product Responsibility

Governance Pillar — 3 categories:
  Management
  Shareholders
  CSR Strategy
```

### 7.3 Scoring methodology

- Each of the ~630 measures is scored using **percentile ranking within sector** (TRBC).
- Category-level scores aggregate to pillar scores aggregate to overall **ESG Score (0–100)**.
- A separate **ESG Controversies Score (0–100)** is computed from **23 ESG controversy topics** (e.g., business ethics, anti-competitive behavior, product quality, intellectual property), reflected as a sector-relative percentile.
- **ESGC (ESG Combined Score)** = `average(ESG Score, Controversies Score)` *when* the Controversies Score is **lower** than the ESG Score (i.e., controversies drag down). Otherwise `ESGC = ESG Score`. This asymmetric combination is the methodology's distinctive choice.

### 7.4 Letter grade

Scores 0–100 are mapped to a 12-band letter grade A+/A/A−/B+/B/B−/C+/C/C−/D+/D/D− using fixed thresholds.

### 7.5 Datastream field naming

In Datastream/Workspace/DSWS, the headline fields are:

| Mnemonic | Field |
|---|---|
| `TRESGS` | ESG Score (overall, 0–100) |
| `TRESGCS` | ESG Combined Score with Controversies |
| `ENSCORE` | Environmental Pillar Score |
| `SOSCORE` | Social Pillar Score |
| `CGSCORE` | Governance Pillar Score |
| `TRESGCCS` | Controversies Score (standalone) |
| `ENRRP` | Environmental Resource Use category |
| `ENERP` | Environmental Emissions category |
| `ENPI` | Environmental Innovation category |
| `SOWO` | Social Workforce category |
| `SOHR` | Social Human Rights category |
| `SOCO` | Social Community category |
| `SOPR` | Social Product Responsibility category |
| `CGVS` | CG Management category |
| `CGSR` | CG Shareholders category |
| `CGVISION` | CG CSR Strategy category |

(source: <https://libguides.cbs.dk/c.php?g=669247&p=4910785>; <https://www.lseg.com/content/dam/data-analytics/en_us/documents/methodology/lseg-esg-scores-methodology.pdf>)

### 7.6 Materiality matrix

LSEG publishes its own **materiality matrix** assigning each category a weight 1–10 per TRBC sector. Same measure is weighted differently for a software company vs a mining company.

### 7.7 Coverage, history, pricing

- **25,000+ companies** in current coverage (1,000+ ETFs/funds also covered).
- **History depth:** **2002+** for the original Asset4 universe; significant universe expansion 2009-2015.
- **Pricing:** bundled in LSEG Workspace (base $1.5k–$3k/user/month) with ESG add-on $500–$2k/user/month; standalone Datafeed $50k–$300k/yr for mid-tier institutional.

---

## 8. ISS ESG (Institutional Shareholder Services)

### 8.1 The product family

ISS ESG, the data arm of ISS (now part of Deutsche Börse / Genstar), packages several distinct datasets:

- **Governance QualityScore (GQS)** — 1–10 decile rank, governance only.
- **Climate Solutions** — TCFD-aligned climate data, emissions, transition risk.
- **Norm-Based Research** — UN Global Compact alignment, controversies (pass/fail).
- **SDG Solutions Assessment** — alignment with the 17 UN SDGs.
- **Carbon & Climate dataset** — emissions, scenario-aligned metrics.
- **E&S Disclosure QualityScore** — analog of governance score for E and S disclosure quality.

### 8.2 Governance QualityScore (GQS)

- **Decile rank 1–10** within market region.
- **1 = lowest governance risk (best); 10 = highest governance risk (worst).**
- **230+ underlying factors** grouped into 4 sub-pillars:
  - Board Structure
  - Compensation
  - Shareholder Rights
  - Audit & Risk Oversight
- Sub-pillar deciles published separately in addition to overall GQS.
- Methodology updates published annually (see 2022 and 2023 update releases) (source: <https://insights.issgovernance.com/posts/iss-esg-releases-methodology-updates-for-governance-qualityscore-2023/>; <https://www.issgovernance.com/file/publications/methodology/Governance-QualityScore-Methodology.pdf>).

### 8.3 Climate Solutions

- TCFD-aligned datapoints: Scope 1, 2, 3 emissions; carbon intensity; "Climate Performance Score"; "2°C Alignment" portfolio-temperature metric; physical risk exposure tagged by asset location.

### 8.4 Norm-Based Research

- Pass/fail screen against UN Global Compact's 10 principles + OECD MNE Guidelines + UN Guiding Principles on Business & Human Rights.
- Outputs a binary `compliant / watch / fail` flag plus narrative evidence per company.
- Heavily used by European pension funds for SFDR Article 8/9 exclusion screens.

### 8.5 SDG Solutions Assessment

- Net-impact alignment with each of the 17 UN SDGs, on a per-product-line revenue-attribution basis.
- Produces "SDG impact score" per SDG per company (positive/neutral/negative).

### 8.6 Delivery and pricing

- **ISS ESG Data Portal** — web platform.
- **DataDesk API** — REST/JSON.
- **Bulk feed** via SFTP or Snowflake share.
- **Pricing:** **$30k–$200k+/yr** depending on product mix; Governance QualityScore standalone is the cheapest entry point at the lower end.

(source for ISS data family: <https://www.issgovernance.com/sustainability/ratings/governance-qualityscore/>; <https://insight.factset.com/resources/iss-gqs-datafeed-at-a-glance>)

---

## 9. FactSet Truvalue Labs

### 9.1 The product

Truvalue Labs, founded 2013 in San Francisco, was acquired by FactSet in November 2020. The product is **real-time NLP-based ESG scoring** ingesting **150,000+ unstructured sources in 30+ languages** (news, NGO reports, trade blogs, social media, regulatory disclosures) and scoring each article against the **SASB Materiality Map** (source: <https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-scores-datafeed>; <https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/esg-data-and-analytics-from-truvalue-labs-brochure.pdf>).

### 9.2 The 26 SASB-aligned categories

SASB defines **26 General Issue Categories** (e.g., GHG Emissions, Air Quality, Water Management, Customer Privacy, Data Security, Employee Health & Safety, Labor Practices, Business Ethics, Product Quality & Safety, etc.). Truvalue scores each piece of content into one of these 26 buckets, signed positive or negative for the company.

### 9.3 The four score types

| Score | Description | Time horizon |
|---|---|---|
| **Insight Score** | Long-term ESG track record; cumulative average of all category-level scores over the company's history | Multi-year (since-inception) |
| **Pulse Score** | Recent ESG behavior; weighted-recent-events score | ~12 months trailing |
| **Momentum Score** | Directional indicator: whether the company is trending +/− on Pulse | Directional |
| **Volume Score** | Information flow: number of articles/events about the company | Recent N-day window |

All four scores are **updated daily** and provided **per company × per SASB category** (i.e., for each company there are 26 Insight scores, 26 Pulse scores, etc.).

`[unverified — the project brief mentions a "SPECTRUM score" that was not found in the current FactSet Truvalue documentation; this may be an older or renamed score]`

### 9.4 SASB Spotlight

A separate **SASB Spotlight** dataset surfaces individual high-relevance events flagged by the NLP pipeline; each Spotlight is an event with company, SASB category, sentiment, source URL, and severity (source: <https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-spotlight-datafeed>).

### 9.5 Schema in Open:FactSet

| Table / Endpoint | Purpose | Fields |
|---|---|---|
| `truvalue_insight` | Insight scores per company × SASB category × date | `(entity_id, sasb_category, score_date, insight_score)` |
| `truvalue_pulse` | Pulse scores | similar |
| `truvalue_momentum` | Momentum | similar |
| `truvalue_volume` | Volume | similar |
| `truvalue_spotlight` | Individual high-relevance events | `(event_id, entity_id, sasb_category, event_date, sentiment, source_url, severity)` |
| `truvalue_overall` | Aggregate Insight across all 26 categories | `(entity_id, score_date, overall_insight)` |

(source field families: <https://developer.factset.com/api-catalog/factset-esg-api>; <https://developer.truvaluelabs.com/data/sasb-scores-data-service>)

### 9.6 Coverage and pricing

- **22,000+ companies** with daily-scored coverage.
- **History depth:** ~2007+ (the NLP pipeline backfilled archives).
- **Delivery:** Open:FactSet Marketplace; Snowflake share; REST API.
- **Pricing:** **$50k–$300k/yr** via Open:FactSet.

---

## 10. CDP (Carbon Disclosure Project)

### 10.1 The three datasets

CDP runs three primary annual disclosure cycles (source: <https://www.cdp.net/en/data>):

1. **CDP Climate Change Questionnaire** — corporate emissions, targets, governance of climate, scenario analysis. ~24,000 corporates responded in 2024.
2. **CDP Forests Questionnaire** — deforestation-linked commodities (palm oil, soy, cattle, timber, rubber, coffee, cocoa).
3. **CDP Water Security Questionnaire** — water withdrawal, discharge, intensity, dependency.

Plus separate **Cities**, **States & Regions**, **Supply Chain Engagement**, and **Plastics** disclosure cycles.

### 10.2 The scoring methodology

CDP scores each respondent on a four-stage scale per theme:

- **Disclosure** (level 1) — has the company provided the requested information?
- **Awareness** (level 2) — does the company understand the issue's impact?
- **Management** (level 3) — does the company manage the issue with policies/processes?
- **Leadership** (level 4) — is the company demonstrating best practice?

Each level has a percentage threshold; final score letter:

| Score | Range |
|---|---|
| A | ≥80% Leadership |
| A− | ≥45% Leadership |
| B | ≥80% Management |
| B− | ≥45% Management |
| C | ≥80% Awareness |
| C− | ≥45% Awareness |
| D | ≥45% Disclosure |
| D− | <45% Disclosure |
| F | No response |

The **A-List** is the public list of A-rated companies, published annually in December.

### 10.3 Schema (questionnaire structure)

The CDP Climate Change questionnaire (latest, ~2025 cycle) contains ~80–100 questions across modules:

```
Module C0 — Introduction (company info)
Module C1 — Governance (board oversight, exec compensation tied to climate)
Module C2 — Risks & Opportunities (scenario analysis, TCFD-aligned)
Module C3 — Business Strategy (transition plan, financial planning)
Module C4 — Targets and Performance (SBTi-validated, net-zero targets)
Module C5 — Emissions Methodology (boundary, accounting approach)
Module C6 — Emissions Data (Scope 1, 2 location, 2 market, 3 by category)
Module C7 — Emissions Breakdown (by gas, country, business unit)
Module C8 — Energy (consumption, renewable, intensity)
Module C9 — Additional Metrics
Module C10 — Verification (assurance reports)
Module C11 — Carbon Pricing (internal carbon price)
Module C12 — Engagement (suppliers, customers, policy)
Module C13 — Other Land Management (where applicable)
Module C14 — Sign Off
```

(source: <https://data.cdp.net/>; <https://www.cdp.net/en/data>)

### 10.4 Scope 1/2/3 emissions schema in CDP

The Scope 3 emissions inventory follows GHG Protocol's **15 categories**:

| # | Scope 3 Category | Upstream/Downstream |
|---|---|---|
| 1 | Purchased goods and services | Upstream |
| 2 | Capital goods | Upstream |
| 3 | Fuel-and-energy-related activities | Upstream |
| 4 | Upstream transportation and distribution | Upstream |
| 5 | Waste generated in operations | Upstream |
| 6 | Business travel | Upstream |
| 7 | Employee commuting | Upstream |
| 8 | Upstream leased assets | Upstream |
| 9 | Downstream transportation and distribution | Downstream |
| 10 | Processing of sold products | Downstream |
| 11 | Use of sold products | Downstream |
| 12 | End-of-life treatment of sold products | Downstream |
| 13 | Downstream leased assets | Downstream |
| 14 | Franchises | Downstream |
| 15 | Investments | Downstream |

CDP requires each category to be reported separately with a methodology code (e.g., "Spend-based", "Average-data method", "Hybrid method") and emissions in tCO2e.

### 10.5 Access and pricing

- **Free aggregated scores** (the A-List, sector summaries) via CDP's open data portal.
- **Full questionnaire responses** — historically free via the open data portal (pre-2018); progressively restricted; full corporate-data access now requires CDP partnership (paid) or CDP Disclosure API (2025+, paid tier).
- **Free tier sufficient for:** screening, A-List flagging, sector comparison.
- **Paid tier required for:** full numeric Scope 1/2/3, target details, scenario disclosures.
- **Pricing:** **$15k–$100k/yr** for institutional data access depending on universe and depth.
- **License:** Mixed — public scores under attribution; full data subject to CDP membership.

(source: <https://data.cdp.net/>; <https://www.cdp.net/en/data>; <https://www.cdp.net/en/insights/cdp-launches-2025-disclosure-api>)

---

## 11. GRESB (real assets — real estate, infrastructure)

### 11.1 The four assessments

GRESB (formerly Global Real Estate Sustainability Benchmark; rebranded 2014 to cover infrastructure) runs four annual ESG benchmarks (source: <https://www.gresb.com/real-estate-assessment/>; <https://documents.gresb.com/generated_files/real_estate/2025/real_estate/scoring_document/complete.html>):

1. **GRESB Real Estate Assessment** — listed and private real estate funds/companies.
2. **GRESB Real Estate Development Assessment** — for portfolios with significant development activity.
3. **GRESB Infrastructure Asset Assessment** — per-asset (one asset = one submission).
4. **GRESB Infrastructure Fund Assessment** — fund/portfolio-level for infrastructure.

### 11.2 Scoring structure

Each assessment is scored 0–100 from two components:

```
GRESB Score = Management Component Score + Performance Component Score
          (or = Management + Development for the Development Benchmark)

Management Component (~30 points):
  - Leadership (org structure, ESG governance)
  - Policies (ESG, environmental, social, governance policies)
  - Reporting (frameworks aligned to TCFD, GRI, SASB)
  - Risk Management (climate risk, transition planning)
  - Stakeholder Engagement

Performance Component (~70 points):
  - Risk Assessments (climate risk per asset)
  - Targets (energy / GHG / water / waste)
  - Tenants & Community
  - Energy (kWh per m2, per FTE)
  - GHG (Scope 1, 2, 3, intensity per m2)
  - Water (m3 per m2)
  - Waste (tonnes diverted from landfill)
  - Data Monitoring & Review
  - Building Certifications (LEED, BREEAM, etc.)
```

### 11.3 Rating system

After scoring, entities receive a **quintile-based 1–5 star rating**:

| Star rating | Quintile |
|---|---|
| 5 stars | Top 20% (Global Sector Leader candidates) |
| 4 stars | 60th–80th percentile |
| 3 stars | 40th–60th percentile |
| 2 stars | 20th–40th percentile |
| 1 star | Bottom 20% |

### 11.4 Schema

GRESB delivers data via:

- **GRESB Portal** for participants (submit + view).
- **GRESB Asset Portal** for asset-level infrastructure data.
- **GRESB API** (paid investor access).
- **GRESB Data Feed** (bulk to investors, fund managers).

Representative fields per Real Estate Assessment row:

| Field | Notes |
|---|---|
| `entity_id`, `entity_name` | Real estate entity (often the listed REIT or private fund vehicle) |
| `assessment_year` | Cycle year (Jan–Jun submission window) |
| `sector_lead` | Sector ranking flag |
| `gresb_score`, `mgmt_score`, `perf_score` | Headline scores |
| `gresb_rating` | 1–5 star |
| `quintile` | 1–5 |
| `policy_*`, `reporting_*`, `risk_mgmt_*` | Management indicator breakdown |
| `energy_data_coverage_pct`, `ghg_intensity_per_sqm`, `water_intensity_per_sqm`, `waste_recycling_pct` | Performance metrics |
| `building_certifications_pct` | % portfolio certified |

### 11.5 Coverage and pricing

- ~**2,200 Real Estate entities** in 2024 cycle (~$8T AUM coverage).
- ~**210 Infrastructure Funds**, ~**750 Infrastructure Assets**.
- **History depth:** 2009+ (Real Estate); 2016+ (Infrastructure).
- **Pricing:** Participant member fee + assessment fee scales by AUM, typically **$10k–$50k/yr** for participants; investor access **$10k–$30k/yr**.

---

## 12. Public-data reconstruction

The vendor stack above costs $300k–$2M+/yr fully loaded. The reconstruction path from public regulatory disclosure is the strategic wedge — substantial enough to merit its own multi-quarter ats-eqt module.

### 12.1 SEC 10-K Item 1 / 1A (narrative)

- **Item 1 (Business)** since the 2020 SEC Human Capital Disclosure rule (Reg S-K Item 101(c)) requires disclosure of:
  - Headcount (total, by region where material)
  - Human capital measures the company uses (turnover, training spend, diversity)
  - Worker safety where material
  - Workforce composition (full-time, part-time, contractor split)
- **Item 1A (Risk Factors)** routinely includes:
  - Climate physical risk language (hurricane / wildfire / sea-level exposure)
  - Supply-chain risk (geopolitical, single-supplier)
  - ESG litigation / activist exposure
  - Cybersecurity / data privacy risk
- **NLP extraction** of these narratives is a tractable open-source target. Quality of structured extraction is the moat — there is no XBRL tagging for Item 1/1A as of 2026-05.
- **Volume:** ~7,000 10-Ks per year × ~50–100 pages each = ~700k structured-extractable pages.

### 12.2 SEC 10-K Exhibit 21 (Subsidiary list)

- Mandatory under Reg S-K Item 601(b)(21); free-text format.
- Disclosure of **significant subsidiaries** with jurisdiction of incorporation.
- Cross-references to ESG: when joined with import-customs data (Panjiva, US AMS), allows attribution of imports to the parent issuer rather than only the consignee. Joins to ESG metrics by parent → subsidiary supply-chain rollup.
- See `13f_holdings.md` and `public_data_sources.md` for full ingestion path. Mentioned here for ESG-supply-chain materiality joins.

### 12.3 SEC Form SD (Conflict Minerals)

- Mandatory annual disclosure under **Dodd-Frank Section 1502** for issuers using tin, tungsten, tantalum, gold (3TG).
- Reports include **smelter and refiner lists**, country-of-origin determinations, supply-chain due diligence narrative.
- Joins to **Responsible Minerals Initiative (RMI)** Conformant Smelter & Refiner List for structured smelter ID resolution (source: <https://www.responsiblemineralsinitiative.org/>).
- **Volume:** ~1,000 filers/yr.
- **History:** 2014+ (first filing for FY2013).
- **ESG value:** This is the single most structured public conflict-minerals data globally; vendor ESG products treat conflict-minerals indicators as a primary input to "Controversial Sourcing" Key Issue scores.

### 12.4 SEC Climate Disclosure Rule (2024 final, since abandoned)

**Status as of 2026-05:**

| Date | Event |
|---|---|
| 2024-03-06 | SEC adopts final Climate-Related Disclosure Rule (Reg S-K Item 1500–1508 + Reg S-X Article 14) |
| 2024-03-15 | **5th Circuit grants stay** on petition by industry groups |
| 2024-03-21 | Judicial Panel on Multidistrict Litigation consolidates all challenges in 8th Circuit |
| 2024-04-04 | **SEC voluntarily stays implementation** pending judicial review |
| 2025-02-11 | SEC Acting Chairman Uyeda statement signaling potential withdrawal |
| 2025-03-27 | SEC Commissioner Crenshaw "The Commission has Left the Building" — formally noting SEC's withdrawal of defense |
| 2025-04 | **SEC votes to end defense of rule** (Press release 2025-58) — effectively dead at federal level |
| 2025-07-23 | SEC status report confirms intent not to revisit |
| 2026-05 | No federal climate disclosure rule in force; California SB 253/261 is now the operative U.S. anchor |

(sources: <https://www.sec.gov/newsroom/press-releases/2025-58>; <https://www.sec.gov/newsroom/speeches-statements/crenshaw-statement-climate-related-disclosures-032725>; <https://corpgov.law.harvard.edu/2025/09/30/regulatory-climate-shift-updates-on-the-sec-climate-related-disclosure-rules/>; <https://www.sidley.com/en/insights/newsupdates/2025/04/sec-ends-defense-of-climate-related-disclosure-rules>; <https://www.esgdive.com/news/sec-stays-climate-risk-disclosure-rule-until-legal-challenges-complete-8th-circuit/712354/>)

**Practical impact for ats-eqt:** Do not build ingestion against Reg S-K Item 1500. Pivot US climate ingestion to California SB 253 (effective 2026 reporting), CDP (voluntary), and 10-K Item 1A narrative NLP.

### 12.5 EU CSRD / ESRS — the "GAAP for ESG"

The **Corporate Sustainability Reporting Directive (CSRD)** (Directive (EU) 2022/2464) mandates that ~50,000 EU and non-EU large companies report against the **European Sustainability Reporting Standards (ESRS)**, in iXBRL using the ESRS XBRL taxonomy (source: <https://www.efrag.org/>).

**Phased applicability:**

| Cohort | First reporting year (FY) | First filings |
|---|---|---|
| EU PIE >500 employees (already-NFRD) | FY2024 | 2025 |
| All other EU large companies | FY2025 | 2026 |
| EU-listed SMEs | FY2026 | 2027 (opt-out until 2028) |
| Non-EU companies with EU revenue >€150M | FY2028 | 2029 |

**The 12 ESRS standards and their datapoint counts:**

| Standard | Topic | Datapoints | Notes |
|---|---|---|---|
| ESRS 1 | General requirements | n/a (framework) | Architecture of the system |
| ESRS 2 | General disclosures | 219 | Mandatory for all reporters |
| **Environmental** | | | |
| ESRS E1 | Climate change | **214** | Scope 1/2/3, transition plan, physical risk, internal carbon price, EU Taxonomy linkage |
| ESRS E2 | Pollution | 96 | Air, water, soil, substances of concern |
| ESRS E3 | Water and marine resources | 48 | Withdrawal, discharge, water-stressed sites |
| ESRS E4 | Biodiversity and ecosystems | 122 | Sites near protected areas, deforestation, ecosystem impact |
| ESRS E5 | Resource use & circular economy | 64 | Material flow, recycled content, waste |
| **Social** | | | |
| ESRS S1 | Own workforce | 196 | Headcount by region/gender/contract; turnover; training; wages; health & safety; collective bargaining |
| ESRS S2 | Workers in the value chain | 67 | Supplier-workforce, conflict minerals, child labor, forced labor |
| ESRS S3 | Affected communities | 66 | Indigenous rights, land rights, community engagement |
| ESRS S4 | Consumers and end-users | 66 | Privacy, product safety, marketing practices |
| **Governance** | | | |
| ESRS G1 | Business conduct | 53 | Anti-corruption, lobbying, supplier payment practices, whistleblower |

**Total: 1,144 mandatory + 269 voluntary "may disclose" datapoints** across the 12 standards (source: <https://envoria.com/insights-news/esrs-data-points-guide-for-successful-csrd-reporting>; <https://www.efrag.org/sites/default/files/sites/webpublishing/SiteAssets/EFRAG%20IG%203%20List%20of%20ESRS%20Data%20Points%20-%20Explanatory%20Note.pdf>; <https://xbrl.efrag.org/e-esrs/esrs-set1-2023.html>).

**Materiality logic:** ESRS introduces **double materiality** — companies must report on a topic if it is material from either the *financial* perspective (affects enterprise value) **OR** the *impact* perspective (the company affects society/environment). This is broader than SASB (financial materiality only).

**Digital reporting:** ESRS reports are tagged with the **ESRS XBRL taxonomy** published by EFRAG, embedded in the iXBRL ESEF report packages on national OAMs (Officially Appointed Mechanisms) and aggregated at <https://filings.xbrl.org/>.

**Simplification proposals (2026-01):** The European Commission's "Omnibus" proposal would simplify ESRS materially, reducing scope and datapoint count — but as of 2026-05 the 2023-published ESRS Set 1 with 1,144 datapoints remains the operative standard. `[unverified — Omnibus proposal status]`

(source: <https://www.ey.com/content/dam/ey-unified-site/ey-com/en-gl/technical/csrd-technical-resources/documents/ey-gl-efrag-proposes-major-esrs-simplifications-01-2026.pdf>)

### 12.6 EU SFDR — Article 8/9 + PAI

The **Sustainable Finance Disclosure Regulation (SFDR)** (Regulation (EU) 2019/2088) applies to financial market participants and advisors. Two relevant disclosures:

- **Article 6, 8, 9 classification:** funds self-classify as Article 6 (no sustainability claim), Article 8 ("light-green" — promotes E/S characteristics), or Article 9 ("dark-green" — sustainable investment objective).
- **PAI (Principal Adverse Impact) statement:** mandatory for FMPs >500 employees; 14 mandatory + 2 minimum-additional indicators (1 environmental, 1 social) per Annex I of the SFDR RTS.

**The 18 mandatory PAI indicators:**

```
Climate and other environment-related (14 mandatory):
  1. GHG emissions Scope 1, 2, 3
  2. Carbon footprint
  3. GHG intensity of investee companies
  4. Exposure to fossil fuel sector
  5. Share of non-renewable energy consumption/production
  6. Energy consumption intensity per high-impact climate sector
  7. Activities negatively affecting biodiversity-sensitive areas
  8. Emissions to water
  9. Hazardous waste / radioactive waste ratio

Social and employee, respect for human rights, anti-corruption,
anti-bribery matters (5 mandatory):
  10. Violations of UN Global Compact / OECD MNE Guidelines
  11. Lack of processes to monitor UNGC / OECD compliance
  12. Unadjusted gender pay gap
  13. Board gender diversity
  14. Exposure to controversial weapons (anti-personnel mines, cluster munitions, etc.)
```

Plus 2+ minimum opt-in additional indicators chosen from Annex I Table 2 (environmental) and Table 3 (social).

ats-eqt-relevant: every PAI indicator above is reconstructable from CSRD/ESRS once filers are in scope; for pre-CSRD vintages it must be patched from CDP + 10-K + DEF 14A + Gender Pay Gap registries.

### 12.7 UK SDR — Sustainability Disclosure Requirements

The UK's **SDR (Sustainability Disclosure Requirements)** + **Investment Labels** regime came into force in stages 2024–2026 (source: FCA PS23/16). It mandates a **fund-label** taxonomy:

- Sustainability Focus
- Sustainability Improvers
- Sustainability Impact
- Sustainability Mixed Goals

Plus **anti-greenwashing rule** (effective 2024-05-31) requiring sustainability claims to be "fair, clear and not misleading". UK-listed companies face additional TCFD-aligned reporting via FCA Listing Rules 9.8.6R and 14.3.27R.

### 12.8 Japan TCFD-aligned mandatory disclosure

**Tokyo Stock Exchange Prime Market** (top tier, ~1,800 companies as of 2024) requires **TCFD-aligned climate disclosure** in the annual *yuho* (securities report) under the FSA's June 2023 revision of the Disclosure Ordinance. Disclosure structure: Governance, Strategy, Risk Management, Metrics & Targets — the four TCFD pillars.

Additionally, the **SSBJ (Sustainability Standards Board of Japan)** issued ISSB-aligned standards (SSBJ S1 / S2) in March 2025, mandatory for Prime Market filers from FY2027.

### 12.9 California SB 253 / SB 261

The **Climate Corporate Data Accountability Act (SB 253)** and **Climate-Related Financial Risk Act (SB 261)** are the de facto US climate disclosure regime post-SEC-rule-abandonment.

**SB 253 — Climate Corporate Data Accountability Act:**
- US-formed entities with **>$1B annual revenue doing business in California**.
- Mandatory Scope 1 + Scope 2 emissions disclosure annually from 2026.
- Mandatory Scope 3 disclosure annually from 2027.
- **GHG Protocol-aligned** accounting.
- **First reporting deadline: 2026-08-10** (CARB final regulation Feb 2026).
- **Limited assurance** for Scope 1+2 from 2026; **reasonable assurance** from 2030.
- ~5,300 entities estimated in scope.

**SB 261 — Climate-Related Financial Risk Act:**
- US-formed entities with **>$500M annual revenue doing business in California**.
- Biennial TCFD-aligned climate risk disclosure.
- Originally due 2026-01-01; **CARB will not enforce** the 2026-01-01 deadline due to a 9th Circuit injunction pending appeal; alternative date TBD.
- ~10,000 entities estimated in scope.

(sources: <https://ww2.arb.ca.gov/news/carb-approves-climate-transparency-regulation-entities-doing-business-california>; <https://www.persefoni.com/blog/california-sb253-sb261>; <https://dart.deloitte.com/USDART/home/publications/deloitte/sustainability-spotlight/2025/california-climate-legislation-reporting-updates-2026>; <https://leginfo.legislature.ca.gov/faces/billTextClient.xhtml?bill_id=202320240SB253>)

### 12.10 EU Corporate Sustainability Due Diligence Directive (CSDDD)

The **CSDDD** (Directive (EU) 2024/1760), adopted July 2024, requires large EU and non-EU companies (€450M+ EU revenue + 1,000+ employees) to conduct human-rights and environmental **due diligence** across their **own operations + subsidiaries + chains of activities**. Phased in:

- 2027 — companies >5,000 employees + €1.5B EU revenue
- 2028 — companies >3,000 employees + €900M EU revenue
- 2029 — companies >1,000 employees + €450M EU revenue

**Output:** annual due-diligence statement (separate from CSRD/ESRS report) listing actual + potential adverse impacts in operations and value chain, with remediation plan. Publicly accessible.

### 12.11 SEC DEF 14A (proxy statements)

Mandatory annual filing for any company holding a shareholder vote. Contains structured disclosures relevant to G-pillar metrics:

- **Board composition** — director names, ages, tenure, committee assignments, independence flag.
- **Board diversity** — many issuers now voluntarily disclose race/ethnicity/gender mix (the 2021 Nasdaq Board Diversity Rule was vacated by 5th Circuit Dec 2024, removing the mandate, but voluntary disclosure remains widespread).
- **Executive compensation** — Summary Compensation Table, CEO Pay Ratio (mandatory since 2018 under Dodd-Frank §953(b)), pay-vs-performance disclosure (effective 2023 under Item 402(v)).
- **Say-on-Pay vote results** — non-binding shareholder vote; pass/fail + % support.
- **Shareholder proposals** — full text of ESG-related shareholder proposals + management response + vote outcome.
- **Auditor information** — auditor identity, tenure, audit fees.

**ats-eqt extraction:** DEF 14A is partly XBRL-tagged (exec comp tables since 2018; pay-vs-performance since 2023). The narrative portions are NLP-extracted. Open-source projects (`secedgar`, `edgartools`) parse the structured tables; the narrative remains a competitive moat.

### 12.12 SEC Form 10-K Human Capital disclosure

Reg S-K Item 101(c) was amended in 2020 to require disclosure of "human capital resources, including… the number of persons employed by the registrant, and any human capital measures or objectives that the registrant focuses on in managing the business". Effective 2020-11-09.

This produces, in narrative form within Item 1, fields that map to ESRS S1:

- Total headcount (typically broken down by region; sometimes by gender)
- Employee turnover rate
- Training hours / spend per employee
- Diversity statistics (voluntary at federal level; some states have mandated)
- Workforce safety metrics (lost-time injury rate, fatalities)
- Workforce engagement / culture surveys (voluntary)

NLP extraction quality is the moat. Vendors charge for the structured layer.

### 12.13 Companies House (UK) Gender Pay Gap

The **UK Gender Pay Gap Reporting Regulations 2017** require every employer with 250+ employees to publicly report gender pay gap statistics on the gov.uk service annually.

**Mandatory metrics:**
- Mean gender pay gap (%)
- Median gender pay gap (%)
- Mean gender bonus gap (%)
- Median gender bonus gap (%)
- Proportion of male/female employees receiving bonus
- Proportion of male/female employees in each pay quartile

**Free, machine-readable** via <https://gender-pay-gap.service.gov.uk/>. Snapshot date is annual.

**ats-eqt value:** This is the **only government-run, free, structured, multi-year gender-pay-gap registry in the world.** Directly maps to ESRS S1 (Indicator 13 on the SFDR PAI list) and is reusable across SFDR Article 8/9 fund disclosures.

### 12.14 GHG Protocol

The **GHG Protocol Corporate Standard** (revised 2015) is the de-facto global standard for emissions accounting:

- **Scope 1** — direct emissions from owned/controlled sources.
- **Scope 2** — indirect emissions from purchased electricity, heat, steam. Two methods: **location-based** (grid-average emissions factor) and **market-based** (contractual instruments like RECs/PPAs).
- **Scope 3** — all other indirect emissions in the value chain, broken into 15 categories (see §10.4 above).

Boundary choice: **equity share, financial control, or operational control** — the company must disclose which boundary it uses. Restatements occur when the boundary changes.

GHG Protocol is *voluntary* but adopted by CSRD/ESRS E1, CDP, SB 253, SBTi (Science Based Targets Initiative), and TCFD. For ats-eqt's `ghg_emission` table, GHG Protocol categorization is the canonical schema (see §13).

### 12.15 WBCSD / GRI Standards

- **GRI (Global Reporting Initiative) Standards** — voluntary global ESG reporting framework, organized as **Universal** (GRI 1-3), **Sector** (GRI 11-17, more being added), and **Topic** (GRI 200-series economic, 300 environmental, 400 social). GRI predates ESRS by ~25 years and the ESRS taxonomy was deliberately mapped to GRI for backward compatibility.
- **WBCSD (World Business Council for Sustainable Development)** — publisher of frameworks (e.g., the Greenhouse Gas Protocol jointly with WRI) and the originator of many concepts now structured in CSRD.

### 12.16 SASB Standards

The **SASB (Sustainability Accounting Standards Board) Standards**, now under the **ISSB (International Sustainability Standards Board)** (consolidated 2022), define **77 industry-specific standards** organized into **11 sectors**. Each industry standard identifies the **financially material** sustainability topics (the SASB Materiality Map) and prescribes **General Issue Categories** + **Topic-specific Accounting Metrics** (typically 6–12 metrics per industry).

**26 General Issue Categories** — these are the same 26 used by FactSet Truvalue. They span environmental capital, human capital, social capital, business model & innovation, leadership & governance (source: <https://sasb.ifrs.org/standards/materiality-map/>).

ISSB has folded SASB into the **IFRS S1 (General Requirements) + IFRS S2 (Climate)** issued 2023 — these are voluntary global standards but referenced by Japan SSBJ, Singapore, Hong Kong, UK SDR, and others as the basis for forthcoming mandatory regimes.

### 12.17 TCFD

The **Task Force on Climate-related Financial Disclosures (TCFD)** final report (2017) defined four reporting pillars:

```
1. Governance
   - Board oversight of climate
   - Management role in climate
2. Strategy
   - Identified climate-related risks/opportunities (short/medium/long-term)
   - Impact on businesses, strategy, financial planning
   - Resilience under scenario analysis (incl. 2°C or lower)
3. Risk Management
   - Process for identifying/assessing climate risks
   - Process for managing climate risks
   - Integration into overall risk management
4. Metrics and Targets
   - Metrics used to assess climate risks
   - Scope 1, 2, 3 GHG emissions
   - Targets used to manage climate risks
```

TCFD was disbanded in 2023 with its mandate transferred to ISSB. TCFD-aligned disclosure remains the global default backbone — referenced by UK FCA Listing Rules, Japan TSE Prime, EU CSRD/ESRS E1, SEC (rule abandoned but TCFD reference remained), and California SB 261.

---

## 13. Cross-vendor score correlation — Berg/Kölbel/Rigobon and follow-up

Berg, Kölbel & Rigobon, *Review of Finance* 2022, **"Aggregate Confusion: The Divergence of ESG Ratings"** is the seminal academic finding for ats-eqt's product positioning (source: <https://academic.oup.com/rof/article/26/6/1315/6590670>; <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3438533>; MIT Aggregate Confusion Project: <https://mitsloan.mit.edu/sustainability-initiative/aggregate-confusion-project>).

**Headline number:** Pairwise correlation between the six major ESG ratings (KLD, Sustainalytics, Vigeo-Eiris (Moody's), RobecoSAM (S&P), Asset4 (Refinitiv), MSCI) ranges from **0.38 to 0.71**. Average ~**0.54**.

For comparison:
- Credit ratings (S&P vs Moody's vs Fitch): ~0.99 correlation.
- Equity analyst consensus EPS estimates: ~0.95 correlation cross-broker.

**Decomposition of the divergence:**

| Source | % contribution to divergence |
|---|---|
| **Measurement divergence** — same attribute measured by different indicators | **56%** |
| **Scope divergence** — different attributes included (e.g., lobbying in/out) | **38%** |
| **Weight divergence** — same data, different aggregation weights | 6% |

The dominant source — measurement — is *not* a methodology-cleanup issue; it is fundamental to the absence of a single chart-of-accounts for ESG. Even after harmonizing scope and weights, measurement disagreement persists because there's no GAAP-equivalent telling raters how to measure (say) "supply-chain labor standards".

**Follow-up findings (Berg, Heeb, Kölbel and others):**
- *"Rewriting History II"* (Berg et al., 2025) found that ESG ratings get retroactively rewritten over time — the "as-of" value for 2018 reported in 2018 differs systematically from the "as-of" value for 2018 reported in 2024 by the same vendor. Bitemporal modeling at the *score* level is therefore essential.
- *"ESG Confusion and Stock Returns"* NBER WP 30562 demonstrated that the rating disagreement materially weakens the empirical link between ESG and stock returns — i.e., apparent E-S-G-return correlations partly reflect noise.

**Implication for ats-eqt:** Marketing "another ESG score" against MSCI/Sustainalytics/etc. is competing in a saturated low-trust market. The defensible positioning is **"raw disclosed metrics + bitemporal evidence trail + vendor overlay for joins"** — i.e., publish ESRS E1 datapoints, GHG Protocol Scope 1/2/3 with methodology flag, CDP scores, joined to filing exhibits, with vendor scores as *attributed overlay columns* (so clients with existing MSCI/Sustainalytics licences can join them in). This is similar to how the 13F / N-PORT product (`13f_holdings.md` §C–I) positions vs FactSet Ownership: not replication, augmentation.

---

## 14. Recommended ats-eqt ESG schema

This fits the bitemporal long-format pattern in `schemas/data_models_and_methodology.md`. The pattern mirrors `holding_13f`: long-format facts, entity-keyed, bitemporal (`valid_from/valid_to/knowledge_from/knowledge_to`), source-attributed.

### 14.1 The canonical metric dictionary

```sql
-- The single source of truth for every ESG metric ats-eqt tracks.
-- Cross-walks to ESRS, SASB, GRI, TCFD, PAI, GHG Protocol identifiers.
CREATE TABLE esg_metric_dim (
  metric_id            BIGINT       PRIMARY KEY,    -- ats-eqt stable key
  metric_code          TEXT         NOT NULL UNIQUE, -- canonical mnemonic, e.g. 'GHG_SCOPE_1_TCO2E'
  metric_name          TEXT         NOT NULL,        -- e.g. 'GHG Scope 1 Emissions (tCO2e)'
  description          TEXT         NULL,
  unit                 TEXT         NULL,            -- e.g. 'tCO2e', 'MWh', 'pct', 'count', 'usd'
  data_type            TEXT         NOT NULL,        -- 'numeric' | 'boolean' | 'enum' | 'text' | 'identifier'
  pillar               CHAR(1)      NOT NULL,        -- 'E' | 'S' | 'G'
  -- Cross-walks (many-to-many — most metrics live in multiple frameworks)
  esrs_code            TEXT         NULL,            -- e.g. 'E1-6_GHG_SCOPE_1'
  sasb_topic           TEXT         NULL,            -- e.g. 'GHG_EMISSIONS'
  gri_code             TEXT         NULL,            -- e.g. 'GRI 305-1'
  tcfd_pillar          TEXT         NULL,            -- 'Governance' | 'Strategy' | 'RiskMgmt' | 'Metrics'
  pai_indicator        INTEGER      NULL,            -- 1–18 if mapped to SFDR PAI
  ghg_protocol_scope   INTEGER      NULL,            -- 1 | 2 | 3 (NULL if not GHG)
  ghg_protocol_cat     INTEGER      NULL,            -- 1–15 for Scope 3 categories
  is_assurable         BOOLEAN      NOT NULL DEFAULT FALSE, -- quantitative, externally-assurable
  introduced_year      INTEGER      NOT NULL,
  retired_year         INTEGER      NULL
);

CREATE INDEX ix_metric_dim_esrs ON esg_metric_dim(esrs_code) WHERE esrs_code IS NOT NULL;
CREATE INDEX ix_metric_dim_sasb ON esg_metric_dim(sasb_topic) WHERE sasb_topic IS NOT NULL;
```

This is the table that does the heavy lifting: ats-eqt's positioning is "raw metrics with canonical crosswalk", so this dictionary is the product. Initial population: ~1,500 metrics (the union of ESRS 1,144 + SASB ~600 + PAI 18 + GRI 200/300/400 + GHG Protocol Scope 3 × 15 categories), deduplicated by `metric_code`.

### 14.2 The metric fact table (long-format core)

```sql
CREATE TABLE esg_metric (
  entity_id            BIGINT       NOT NULL,        -- → entity (the reporting issuer)
  metric_id            BIGINT       NOT NULL,        -- → esg_metric_dim
  period_id            BIGINT       NOT NULL,        -- → period (typically annual)
  value_numeric        NUMERIC(28,6) NULL,           -- when data_type = 'numeric'
  value_boolean        BOOLEAN      NULL,            -- when data_type = 'boolean'
  value_enum           TEXT         NULL,            -- when data_type = 'enum'
  value_text           TEXT         NULL,            -- when data_type = 'text'
  unit_id              BIGINT       NULL,            -- → unit (overrides metric_dim default)
  -- Provenance
  source_filing_id     BIGINT       NULL,            -- → filing (10-K, 20-F, ESRS report, CDP response)
  source_url           TEXT         NULL,            -- direct link to source document/page
  source_type          TEXT         NOT NULL,        -- 'CSRD_ESRS' | 'SEC_10K' | 'CDP' | 'CA_SB253'
                                                     --  | 'UK_GPG' | 'CONSULTANT' | 'VENDOR' | 'INFERRED'
  reporting_boundary   TEXT         NULL,            -- 'OPERATIONAL_CONTROL' | 'FINANCIAL_CONTROL' | 'EQUITY_SHARE'
  methodology_code     TEXT         NULL,            -- e.g. 'GHG_PROTOCOL_2015', 'IPCC_AR5_GWP100'
  assured_flag         BOOLEAN      NOT NULL DEFAULT FALSE,
  assurance_level      TEXT         NULL,            -- 'NONE' | 'LIMITED' | 'REASONABLE'
  assurer              TEXT         NULL,            -- 'EY' | 'DELOITTE' | 'KPMG' | 'PWC' | etc.
  estimated_flag       BOOLEAN      NOT NULL DEFAULT FALSE, -- TRUE if vendor-estimated rather than reported
  -- Bitemporal
  valid_from           DATE         NOT NULL,        -- period start (e.g. 2024-01-01)
  valid_to             DATE         NOT NULL,        -- period end (e.g. 2024-12-31)
  knowledge_from       TIMESTAMP    NOT NULL,        -- when ats-eqt learned the fact
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (entity_id, metric_id, period_id, source_type, knowledge_from)
);

CREATE INDEX ix_esg_metric_entity_period ON esg_metric(entity_id, period_id);
CREATE INDEX ix_esg_metric_metric_period ON esg_metric(metric_id, period_id);
CREATE INDEX ix_esg_metric_source ON esg_metric(source_filing_id);
```

Key properties:

- **Long-format**, one row per `(entity, metric, period, source, knowledge_from)`. Pivot to wide at query time.
- **Bitemporal** so restatements are non-destructive; a 2023 Scope 1 number reported in early 2024 and revised in early 2025 produces two rows, both queryable.
- **Multi-source coexistence**: the same `(entity, metric, period)` can carry rows from `CSRD_ESRS`, `CDP`, and a `VENDOR` overlay simultaneously — no winner-takes-all collapse.
- **Methodology and boundary** are first-class columns, not buried in notes — essential for the GHG Protocol boundary ambiguity.

### 14.3 Vendor score overlay

```sql
-- Vendor-attributed scores, sourced from clients' existing ESG subscriptions.
-- ats-eqt does NOT compute its own ESG score. This table joins to client-supplied vendor feeds.
CREATE TABLE esg_score (
  entity_id            BIGINT       NOT NULL,
  vendor_id            BIGINT       NOT NULL,        -- → vendor ('MSCI', 'SUSTAINALYTICS', 'BLOOMBERG', ...)
  product_code         TEXT         NOT NULL,        -- 'MSCI_IVA', 'SUSTAINALYTICS_RISK', 'BLOOMBERG_ES',
                                                     --  'REFINITIV_TRESGS', 'CDP_CLIMATE', 'GRESB_RE', ...
  score_type           TEXT         NOT NULL,        -- 'RATING_LETTER' | 'SCORE_NUMERIC' | 'PERCENTILE' | 'BAND'
  score_numeric        NUMERIC(8,4) NULL,            -- 0–100, or 0–10 (industry-adjusted), depending on vendor
  score_letter         TEXT         NULL,            -- 'AAA'..'CCC' for MSCI, 'A'..'F' for CDP, etc.
  score_band           TEXT         NULL,            -- 'NEGLIGIBLE','LOW','MEDIUM','HIGH','SEVERE' for Sustainalytics
  pillar_e_score       NUMERIC(8,4) NULL,            -- if vendor provides
  pillar_s_score       NUMERIC(8,4) NULL,
  pillar_g_score       NUMERIC(8,4) NULL,
  controversies_score  NUMERIC(8,4) NULL,            -- separate vendor field
  industry_relative_pct NUMERIC(5,2) NULL,           -- percentile within vendor's industry classification
  vendor_industry      TEXT         NULL,            -- vendor's own industry tag
  -- Provenance
  source_subscription  TEXT         NULL,            -- client-supplied entitlement reference
  redistributable      BOOLEAN      NOT NULL DEFAULT FALSE, -- almost always FALSE
  methodology_version  TEXT         NULL,            -- e.g. 'MSCI_2023_METHODOLOGY'
  -- Bitemporal
  valid_from           DATE         NOT NULL,        -- when the score was "as of"
  valid_to             DATE         NOT NULL,
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (entity_id, vendor_id, product_code, valid_from, knowledge_from)
);

CREATE INDEX ix_esg_score_entity ON esg_score(entity_id, vendor_id);
```

The crucial column is `redistributable BOOLEAN`. Vendor ESG scores are almost universally non-redistributable; ats-eqt's role is **enrichment for clients with their own subscriptions**, not score replication. The `esg_score` rows are populated *only* from a client's own licensed feed, persisted on ats-eqt for join convenience, and never re-served to other clients.

### 14.4 Controversies and events

```sql
CREATE TABLE esg_controversy (
  controversy_id       BIGINT       PRIMARY KEY,
  entity_id            BIGINT       NOT NULL,
  event_date           DATE         NOT NULL,        -- when the controversy began
  resolution_date      DATE         NULL,            -- when (if) resolved
  category             TEXT         NOT NULL,        -- 'ENV_POLLUTION' | 'LABOR_VIOLATION'
                                                     --  | 'PRODUCT_SAFETY' | 'CORRUPTION' | 'PRIVACY'
                                                     --  | 'HUMAN_RIGHTS' | 'GOVERNANCE' | ...
  severity             INTEGER      NOT NULL,        -- 1–5 (Sustainalytics-aligned; 1=low, 5=severe)
  short_description    TEXT         NOT NULL,
  long_description     TEXT         NULL,
  source_type          TEXT         NOT NULL,        -- 'NEWS' | 'NGO' | 'REGULATORY' | 'LITIGATION' | 'SELF_DISCLOSED'
  source_url           TEXT         NULL,
  monetary_impact_usd  NUMERIC(20,2) NULL,           -- fine / settlement / damages where quantified
  monetary_impact_type TEXT         NULL,            -- 'FINE' | 'SETTLEMENT' | 'REMEDIATION_COST'
  regulator            TEXT         NULL,            -- 'EPA' | 'SEC' | 'OSHA' | 'DOJ' | 'FCA' | 'BAFIN' | ...
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity'
);

CREATE INDEX ix_controversy_entity ON esg_controversy(entity_id, event_date);
```

`esg_controversy` is event-shaped (point-in-time), not period-shaped. Joins to `esg_metric` and `esg_score` at query time via entity + date overlap.

### 14.5 GHG emissions — first-class table

GHG emissions warrant their own table because they are the only ESG metric with a fully structured global standard (GHG Protocol) and the highest-volume / highest-value field in the entire ESG corpus:

```sql
CREATE TABLE ghg_emission (
  entity_id            BIGINT       NOT NULL,
  period_id            BIGINT       NOT NULL,
  scope                INTEGER      NOT NULL,        -- 1, 2, or 3
  scope_2_method       TEXT         NULL,            -- 'LOCATION_BASED' | 'MARKET_BASED' (NULL if scope ≠ 2)
  scope_3_category     INTEGER      NULL,            -- 1–15 (NULL if scope ≠ 3)
  gas                  TEXT         NOT NULL,        -- 'CO2' | 'CH4' | 'N2O' | 'HFC' | 'PFC' | 'SF6' | 'NF3' | 'CO2E'
                                                     --  ('CO2E' is the aggregate)
  value_tonnes         NUMERIC(20,6) NOT NULL,       -- in tonnes; if gas != CO2E, native; else tCO2e
  gwp_basis            TEXT         NULL,            -- 'AR5_GWP100' | 'AR6_GWP100' | 'AR4_GWP100'
  boundary             TEXT         NOT NULL,        -- 'OPERATIONAL_CONTROL' | 'FINANCIAL_CONTROL' | 'EQUITY_SHARE'
  methodology          TEXT         NOT NULL,        -- 'GHG_PROTOCOL_2015' | 'ISO_14064' | 'EPA_GHGRP' | ...
  scope_3_method       TEXT         NULL,            -- 'SPEND_BASED' | 'AVERAGE_DATA' | 'HYBRID' | 'SUPPLIER_SPECIFIC'
  -- Quality
  estimated_flag       BOOLEAN      NOT NULL DEFAULT FALSE,
  verified_flag        BOOLEAN      NOT NULL DEFAULT FALSE,
  verifier             TEXT         NULL,
  assurance_level      TEXT         NULL,            -- 'NONE' | 'LIMITED' | 'REASONABLE'
  -- Provenance
  source_filing_id     BIGINT       NULL,
  source_type          TEXT         NOT NULL,
  source_url           TEXT         NULL,
  -- Bitemporal
  valid_from           DATE         NOT NULL,
  valid_to             DATE         NOT NULL,
  knowledge_from       TIMESTAMP    NOT NULL,
  knowledge_to         TIMESTAMP    NOT NULL DEFAULT 'infinity',
  PRIMARY KEY (entity_id, scope, scope_2_method, scope_3_category, gas, source_type, valid_from, knowledge_from)
);

CREATE INDEX ix_ghg_entity_period ON ghg_emission(entity_id, valid_from, valid_to);
```

Notes:

- **Scope 2 split**: both location-based and market-based are stored where the company reports both; queries pick the appropriate one. Many ESG analyses default to market-based; transition-risk analyses prefer location-based.
- **Per-gas breakdown**: where the company discloses CH4 / N2O separately (e.g., oil & gas filers), they're stored as individual rows; the aggregate `CO2E` row co-exists.
- **GWP basis**: AR5 vs AR6 changes the CH4 multiplier from 28 to 27.9 (closer) but other GHGs shift more meaningfully; tracking the basis enables restatement reconciliation.
- **Methodology** for Scope 3 is essential because Spend-Based vs Supplier-Specific can produce 5× differences in reported Category 1 emissions.

### 14.6 Convenience views

```sql
-- Latest assured Scope 1+2 per entity (the SFDR/SB253-relevant view)
CREATE MATERIALIZED VIEW current_scope_1_2 AS
SELECT entity_id, valid_from AS period_end,
       SUM(value_tonnes) FILTER (WHERE scope = 1) AS scope_1_tco2e,
       SUM(value_tonnes) FILTER (WHERE scope = 2 AND scope_2_method = 'MARKET_BASED') AS scope_2_market_tco2e,
       SUM(value_tonnes) FILTER (WHERE scope = 2 AND scope_2_method = 'LOCATION_BASED') AS scope_2_location_tco2e,
       MAX(assurance_level) AS assurance_level
FROM ghg_emission
WHERE gas = 'CO2E' AND knowledge_to = 'infinity'
GROUP BY entity_id, valid_from;

-- Entity-pillar score panel: every vendor score for every entity, period-keyed
CREATE MATERIALIZED VIEW entity_vendor_score_panel AS
SELECT entity_id, valid_from AS period_end,
       MAX(score_numeric) FILTER (WHERE product_code='MSCI_IVA')         AS msci_iva,
       MAX(score_numeric) FILTER (WHERE product_code='SUSTAINALYTICS_RISK') AS sus_risk,
       MAX(score_numeric) FILTER (WHERE product_code='BLOOMBERG_ES')     AS bloomberg_es,
       MAX(score_numeric) FILTER (WHERE product_code='REFINITIV_TRESGS') AS refinitiv_tresgs,
       MAX(score_numeric) FILTER (WHERE product_code='SP_CSA')           AS sp_csa,
       MAX(score_letter)  FILTER (WHERE product_code='CDP_CLIMATE')      AS cdp_climate
FROM esg_score
WHERE knowledge_to = 'infinity'
GROUP BY entity_id, valid_from;
```

The `entity_vendor_score_panel` view directly enables the Berg/Kölbel/Rigobon style cross-vendor disagreement analysis on the client's own joined data — a meaningful research-product hook on its own.

### 14.7 Ingestion pipeline outline

1. **Discover** sources per cycle:
   - ESRS: poll national OAMs + <https://filings.xbrl.org/> daily.
   - CDP: annual extract (June each year) of free A-List + score data; paid tier for full questionnaire.
   - California SB 253: annual extract from CARB reporting portal (first cycle 2026-08).
   - SEC 10-K / DEF 14A: continuous EDGAR poll.
   - Form SD: annual (May) extract.
   - UK Gender Pay Gap: annual extract from gov.uk service.
2. **Parse**: ESRS iXBRL via the EFRAG-published XBRL taxonomy; CDP via the CDP Disclosure API JSON; SEC narrative via NLP pipeline; UK GPG as structured CSV.
3. **Map** each disclosed field to `esg_metric_dim.metric_id` via the per-source crosswalk table.
4. **Persist** to `esg_metric` (or `ghg_emission` for emissions, `esg_controversy` for events).
5. **Bitemporal merge**: if `(entity_id, metric_id, period_id, source_type)` already exists with different value, close the old row (`knowledge_to=now`) and insert new.
6. **Vendor overlay** is ingested only from client-supplied feeds, persisted with `redistributable=FALSE`.

---

## 15. Open questions and wave-3 gaps

The following are the principal unknowns surfaced by this wave-2 pass that wave-3 should close:

1. **ESRS Omnibus simplification status (2026-05).** The European Commission's "Omnibus" proposal would reduce ESRS datapoint count materially. Resolution by end of 2026 will determine whether 1,144 datapoints persists or collapses to ~400. `[Open]`
2. **ISSB IFRS S1/S2 adoption tracker by jurisdiction.** Singapore, Hong Kong, Japan SSBJ, Australia, UK SDR all reference ISSB; mandatory adoption dates and scope vary. `[Open — needs a per-jurisdiction tracker]`
3. **California SB 261 enforcement date** post-injunction resolution. CARB has not published an alternative reporting date as of 2026-05. `[Open]`
4. **CSDDD implementation acts** by each EU member state — the directive sets the framework but member-state transposition varies. `[Open]`
5. **Berg/Kölbel/Rigobon update (2025+).** Whether vendor correlations have improved since 2022 with CSRD-disciplined disclosure inputs. `[Open]`
6. **Exact MSCI 35 Key Issues list current version.** The methodology PDF has been republished multiple times since 2019; the current 2023 version is cited here but the count has flipped between 33/35/37 across editions. Wave-3 should pin to a specific publication date. `[Partially verified]`
7. **Bloomberg ESG Industry-Adjusted Score detailed methodology.** Bloomberg has published the framework but the specific industry-adjustment formula is opaque. `[Open]`
8. **Truvalue SPECTRUM Score.** Project brief mentions a fifth score type not found in current FactSet documentation. Possibly retired or renamed; needs confirmation. `[Open]`
9. **Sustainalytics post-acquisition Morningstar Direct API authentication and rate limits.** No public documentation. `[Open]`
10. **GRESB Infrastructure Asset assessment field-level schema.** Real Estate Assessment is publicly documented at indicator level; Infrastructure Asset is less so. `[Open]`
11. **CDP Disclosure API (2025+) detailed endpoint schema.** Announced launch; specifications not fully public. `[Open]`
12. **ISS ESG Climate Solutions field list.** Less documented than Governance QualityScore. `[Open]`
13. **GHG Protocol "Land Sector and Removals Guidance"** (2024 draft) — when finalized, will add forestry/agriculture accounting. Impact on Scope 1/3 accounting unclear. `[Open]`
14. **EU Taxonomy alignment percentage** — Article 8 of EU Taxonomy Regulation requires large companies to report % of revenue/CapEx/OpEx aligned to the Taxonomy. Maps to ESRS E1 but its schema is its own. `[Open — needs its own section in wave-3]`
15. **Bridging vendor universes.** MSCI, Sustainalytics, Bloomberg, Refinitiv each have proprietary entity identifiers. Cross-walks via FIGI/LEI/ISIN are imperfect (e.g., MSCI rates the issuer, vendor B rates the parent holdco). `[Open — entity-resolution layer needed]`

---

## 16. Sources

### Academic / cross-vendor correlation
- <https://academic.oup.com/rof/article/26/6/1315/6590670> — Berg, Kölbel, Rigobon "Aggregate Confusion: The Divergence of ESG Ratings", *Review of Finance* 2022
- <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3438533> — SSRN preprint of same
- <https://www.zora.uzh.ch/server/api/core/bitstreams/9bf5675d-554c-4767-841e-30cc54519452/content> — ZORA archived copy
- <https://mitsloan.mit.edu/sustainability-initiative/aggregate-confusion-project> — MIT Aggregate Confusion Project landing
- <https://www.nber.org/system/files/working_papers/w30562/w30562.pdf> — NBER WP 30562 "ESG Confusion and Stock Returns"
- <https://onlinelibrary.wiley.com/doi/full/10.1002/csr.70007> — Ferro 2025 "Uncovering ESG Ratings"
- <https://www.sciencedirect.com/science/article/pii/S1057521925003266> — "Chasing ESG performance: How methodologies shape outcomes"
- <https://www.sciencedirect.com/science/article/pii/S104244312500023X> — "ESG ratings: Disagreement across providers and effects on stock returns"
- <https://onlinelibrary.wiley.com/doi/10.1002/ijfe.70043> — Balan 2026 "The Black-Box of ESG Scores"
- <https://www.msci-institute.com/paper/aggregate-confusion-the-divergence-of-esg-ratings/> — MSCI Institute hosting of the paper

### MSCI ESG
- <https://www.msci.com/data-and-analytics/sustainability-solutions/esg-ratings> — MSCI ESG Ratings product page
- <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Ratings+Methodology.pdf> — MSCI ESG Ratings Methodology
- <https://www.msci.com/downloads/documents/access/sustainability-and-climate-resources-and-disclosures/msci-sustainability-and-climate-methodologies/esg-ratings/core-methodologies/esg-ratings-methodology.pdf> — Methodology download
- <https://www.msci.com/downloads/documents/access/sustainability-and-climate-resources-and-disclosures/msci-sustainability-and-climate-methodologies/esg-ratings/core-methodologies/msci-esg-ratings-process.pdf> — ESG Ratings Process
- <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Fund+Ratings+Methodology.pdf> — MSCI ESG Fund Ratings
- <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Government+Ratings+Methodology.pdf> — MSCI ESG Government Ratings
- <https://www.blackrock.com/us/financial-professionals/tools/esg-methodology> — BlackRock summary of MSCI ESG methodology
- <https://www.apiday.com/blog-posts/what-are-the-msci-esg-ratings> — Apiday MSCI breakdown
- <https://www.breatheesg.com/resources/navigating-the-msci-esg-screened-landscape-a-comprehensive-guide> — MSCI ESG Screened framework

### S&P Global CSA / DJSI
- <https://www.spglobal.com/sustainable1/en/csa> — S&P Corporate Sustainability Assessment landing
- <https://portal.s1.spglobal.com/survey/documents/CSA_'How_To'_Guide_2025.pdf> — 2025 CSA How-To Guide
- <https://www.anthesisgroup.com/insights/the-sp-corporate-sustainability-assessment/> — Anthesis comprehensive guide to CSA
- <https://www.anthesisgroup.com/insights/s-p-corporate-sustainability-assessment-guide/> — Anthesis Guide to CSA 2023 scores
- <https://www.adecesg.com/resources/blog/a-guide-to-the-sp-corporate-sustainability-assessment-csa/> — ADEC ESG CSA guide
- <https://www.spglobal.com/content/dam/spglobal/s1/en/documents/esg/CSA-as-a-Service-for-Investors---Factsheet.pdf> — CSA-as-a-Service factsheet
- <https://fatfire.com/sp-csa/> — Tertiary CSA primer
- <https://www.scribd.com/document/710277222/DJSI-CSA-Measuring-Intangibles> — DJSI/CSA "Measuring Intangibles" PDF
- <https://portal.s1.spglobal.com/> — S&P Sustainability portal

### Sustainalytics
- <https://www.sustainalytics.com/esg-data> — Sustainalytics ESG Data landing
- <https://www.sustainalytics.com/docs/knowledgehublibraries/default-document-library/sustainalytics_-esg-risk-ratings_-version-3-1_-methodology-abstract_-june-2024.pdf> — ESG Risk Ratings v3.1 Methodology Abstract (June 2024)
- <https://connect.sustainalytics.com/hubfs/INV/Methodology/Sustainalytics_ESG%20Ratings_Methodology%20Abstract.pdf> — Methodology Abstract
- <https://indexes.morningstar.com/docs/calculation-and-methodology/sustainalytics-esg-risk-ratings-full-methodology> — Morningstar Indexes — full methodology
- <https://www.morningstar.com/content/dam/marketing/shared/research/methodology/SustainabilityRatingMethodology_2021.pdf> — Morningstar Sustainability Rating
- <https://www.morningstar.com/content/dam/marketing/shared/research/methodology/744156_Morningstar_Sustainability_Rating_for_Funds_Methodology.pdf> — Sustainability Rating for Funds
- <https://www.thegoodlobby.eu/wp-content/uploads/2024/12/TGL-Scorecard-Sustainalytics.pdf> — Independent Sustainalytics scorecard
- <https://onestopesg.com/esg-resources/sustainalytics-esg-risk-ratings-methodology> — Methodology summary
- <https://knowesg.com/featured-article/sustainalytics-esg-risk-rating-a-guide-for-responsible-investing> — KnowESG Sustainalytics guide

### Bloomberg ESG
- <https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf> — Bloomberg ESG Scores Methodology
- <https://assets.bbhub.io/professional/sites/10/Environmental-Social-Scores-Fact-Sheet1.pdf> — Bloomberg E&S Scores Fact Sheet
- <https://data.bloomberglp.com/professional/sites/10/ESG_Environmental-Social-Scores.pdf> — Bloomberg E&S Scores
- <https://professional.bloomberg.com/solutions/sustainable-finance/scores/> — Bloomberg Sustainability Scores
- <https://www.bloomberg.com/professional/insights/sustainable-finance/transparency-methodology-and-consistency-in-esg-scoring/> — Bloomberg methodology insights
- <https://www.bloomberg.com/company/press/bloomberg-launches-proprietary-esg-scores/> — Bloomberg ES Scores launch press
- <https://www.bloomberg.com/professional/insights/sustainable-finance/bloombergs-greenhouse-gas-emissions-estimates-model-a-summary-of-challenges-and-modeling-solutions/> — GHG Estimates model
- <https://assets.bbhub.io/professional/sites/10/GHG.pdf> — Bloomberg GHG fact sheet
- <https://bautheac.github.io/BBGsymbols/> — Bloomberg field mnemonic catalog
- <https://www.thegoodlobby.eu/wp-content/uploads/2024/12/TGL-Scorecard-Bloomberg.pdf> — Bloomberg ESG independent scorecard

### Refinitiv / LSEG ESG
- <https://www.lseg.com/content/dam/data-analytics/en_us/documents/methodology/lseg-esg-scores-methodology.pdf> — LSEG ESG Scores Methodology (Oct 2024)
- <https://blogs.cranfield.ac.uk/wp-content/uploads/2021/05/refinitiv-esg-scores-methodology-May22-1.pdf> — Refinitiv ESG Scores Methodology (May 2022)
- <https://www.lseg.com/content/dam/data-analytics/en_us/documents/fact-sheets/esg-scores-fact-sheet.pdf> — LSEG ESG Scores Fact Sheet
- <https://21775616.fs1.hubspotusercontent-na1.net/hubfs/21775616/ESG%20METHODOLOGY%20AND%20GLOSSARY.pdf> — ESG Methodology & Glossary
- <https://www.lseg.com/en/insights/data-analytics/understanding-how-esg-scores-are-measured-their-usefulness-and-how-they-will-evolve> — LSEG ESG insights
- <https://libguides.cbs.dk/c.php?g=669247&p=4910785> — CBS Library guide to ESG in Datastream

### ISS ESG
- <https://www.issgovernance.com/sustainability/ratings/governance-qualityscore/> — Governance QualityScore landing
- <https://www.issgovernance.com/file/publications/methodology/Governance-QualityScore-Methodology.pdf> — GQS methodology PDF
- <https://corpgov.law.harvard.edu/2021/02/25/qualityscore-methodology-guide/> — Harvard Corp Gov Forum on GQS
- <https://insights.issgovernance.com/posts/iss-esg-releases-methodology-updates-for-governance-qualityscore-2023/> — 2023 GQS update
- <https://insights.issgovernance.com/posts/iss-esg-releases-2022-methodology-updates-for-governance-qualityscore/> — 2022 GQS update
- <https://www.issgovernance.com/file/faq/Environmental-Social-QualityScore-FAQ.pdf> — E&S QualityScore FAQ
- <https://www.issgovernance.com/file/faq/es-disclosure-qualityscore-key-issues.pdf> — E&S Disclosure QualityScore Key Issues
- <https://insight.factset.com/resources/iss-gqs-datafeed-at-a-glance> — FactSet on ISS GQS DataFeed
- <https://www.environmental-finance.com/content/guides/esg-guide-entry.html?productid=2264&companyid=28> — Environmental Finance entry on ISS ESG
- <https://iriscarbon.com/a-beginners-guide-to-esg-rating-agencies-and-methodologies/> — IRIS Carbon comparative guide

### FactSet Truvalue
- <https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-scores-datafeed> — FactSet Truvalue SASB Scores at-a-glance
- <https://insight.factset.com/resources/at-a-glance-factset-truvalue-sasb-spotlight-datafeed> — Truvalue SASB Spotlight
- <https://go.factset.com/hubfs/Website/Resources%20Section/Brochures/esg-data-and-analytics-from-truvalue-labs-brochure.pdf> — Truvalue Labs brochure
- <https://developer.factset.com/api-catalog/factset-esg-api> — FactSet ESG API catalog
- <https://developer.truvaluelabs.com/data/sasb-scores-data-service> — Truvalue SASB Scores data service
- <https://open.factset.com/partners/truvalue-labs/en-us> — Open:FactSet Truvalue partner page
- <https://www.factset.com/marketplace/catalog/product/factset-truvalue-scores-and-spotlights> — FactSet Marketplace Truvalue product
- <https://aws.amazon.com/marketplace/pp/prodview-5vbgcnay5qkzk> — AWS Marketplace Truvalue SASB Spotlight
- <https://www.environmental-finance.com/content/guides/esg-guide-entry.html?productid=443&editionid=9&planid=1> — Environmental Finance Truvalue entry
- <https://www.thegoodlobby.eu/wp-content/uploads/2024/12/TGL-Scorecard-FactSet-Truvalue-SASB-Scores-DataFeed.pdf> — Truvalue independent scorecard

### CDP
- <https://www.cdp.net/en/data> — CDP Data overview
- <https://data.cdp.net/> — CDP Open Data Portal
- <https://www.cdp.net/en/insights/cdp-launches-2025-disclosure-api> — CDP 2025 Disclosure API launch

### GRESB
- <https://www.gresb.com/real-estate-assessment/> — GRESB Real Estate Assessment
- <https://documents.gresb.com/generated_files/real_estate/2025/real_estate/scoring_document/complete.html> — 2025 Real Estate Scoring Document
- <https://documents.gresb.com/generated_files/real_estate/2024/real_estate/reference_guide/complete.html> — 2024 Real Estate Reference Guide
- <https://documents.gresb.com/generated_files/real_estate/2025/real_estate/reference_guide/complete.html> — 2025 Real Estate Reference Guide
- <https://www.gresb.com/wp-content/uploads/2017/07/GRESB-RE-Scoring-Methodology.pdf> — Original Real Estate Scoring Methodology

### CSRD / ESRS
- <https://www.efrag.org/> — EFRAG (ESRS publisher)
- <https://xbrl.efrag.org/e-esrs/esrs-set1-2023.html> — ESRS Set 1 (2023) XBRL taxonomy
- <https://envoria.com/insights-news/esrs-data-points-guide-for-successful-csrd-reporting> — ESRS datapoint counts
- <https://csr-tools.com/en/blog-en/esrs-data-point-mapping-7-essential-tips-insights/> — ESRS datapoint mapping
- <https://parseport.com/csrd/> — CSRD requirements primer
- <https://www.keyesg.com/article/the-complete-list-of-esrs-metrics-and-data-points> — Full ESRS metric list
- <https://www.bdo.global/getmedia/21a47c91-2e5e-4ecd-89fb-0fff9d44108e/Sustainability-At-a-Glance-ESRS-(2024-12-31)-(1).pdf?ext=.pdf> — BDO ESRS at-a-glance
- <https://www.coolset.com/esrs-cheatsheet> — ESRS cheatsheet (all 12 standards)
- <https://www.efrag.org/sites/default/files/sites/webpublishing/SiteAssets/EFRAG%20IG%203%20List%20of%20ESRS%20Data%20Points%20-%20Explanatory%20Note.pdf> — EFRAG IG3 ESRS Datapoints
- <https://www.mofo.com/resources/insights/260513-european-commission-proposes-revised-esrs> — Morrison Foerster on revised ESRS
- <https://www.ey.com/content/dam/ey-unified-site/ey-com/en-gl/technical/csrd-technical-resources/documents/ey-gl-efrag-proposes-major-esrs-simplifications-01-2026.pdf> — EY EFRAG ESRS simplifications (Jan 2026)

### SEC Climate Disclosure Rule (history)
- <https://www.sec.gov/newsroom/press-releases/2025-58> — SEC votes to end defense of Climate Disclosure Rules (April 2025)
- <https://www.sec.gov/newsroom/speeches-statements/crenshaw-statement-climate-related-disclosures-032725> — Crenshaw "Commission has Left the Building"
- <https://www.sec.gov/newsroom/speeches-statements/crenshaw-statement-climate-related-disclosure-rules-litigation-072325> — Crenshaw status report statement
- <https://www.sec.gov/newsroom/speeches-statements/uyeda-statement-climate-change-021025> — Uyeda statement on Climate-Related Disclosure Rules
- <https://www.sidley.com/en/insights/newsupdates/2025/04/sec-ends-defense-of-climate-related-disclosure-rules> — Sidley on SEC ending defense
- <https://www.whitecase.com/insight-alert/sec-voluntarily-stays-its-climate-rules-pending-judicial-review> — White & Case on voluntary stay
- <https://corpgov.law.harvard.edu/2025/09/30/regulatory-climate-shift-updates-on-the-sec-climate-related-disclosure-rules/> — Harvard Corp Gov regulatory climate shift
- <https://eelp.law.harvard.edu/eighth-curcuit-says-sec-must-defend-or-revise-climate-risk-disclosure-rule/> — Harvard EELP Eighth Circuit
- <https://news.law.fordham.edu/jcfl/2025/10/27/the-rollback-of-the-secs-climate-disclosure-rule-and-its-implications-on-corporate-america/> — Fordham JCFL on rollback
- <https://www.esgdive.com/news/sec-stays-climate-risk-disclosure-rule-until-legal-challenges-complete-8th-circuit/712354/> — ESG Dive on stay

### California SB 253 / SB 261
- <https://leginfo.legislature.ca.gov/faces/billTextClient.xhtml?bill_id=202320240SB253> — SB 253 bill text
- <https://ww2.arb.ca.gov/news/carb-approves-climate-transparency-regulation-entities-doing-business-california> — CARB regulation approval Feb 2026
- <https://www.persefoni.com/blog/california-sb253-sb261> — Persefoni guide
- <https://www.bakertilly.com/insights/california-climate-disclosure-regulations-sb-253-and-sb-261> — Baker Tilly SB253/261 update
- <https://www.pwc.com/us/en/ghosts/california-climate-reporting-sb-253-and-sb-261-explained.html> — PwC explainer
- <https://dart.deloitte.com/USDART/home/publications/deloitte/sustainability-spotlight/2025/california-climate-legislation-reporting-updates-2026> — Deloitte Sustainability Spotlight
- <https://www.energycap.com/blog/sb-261-sb-253-california/> — EnergyCAP California laws
- <https://www.world-kinect.com/news-insights/california-climate-laws> — World Kinect California climate
- <https://watershed.com/blog/california-disclosures-a-guide-for-companies> — Watershed California disclosures guide

### Other public-data and standards
- <https://www.responsiblemineralsinitiative.org/> — Responsible Minerals Initiative smelter list
- <https://www.sec.gov/about/forms/formsd.pdf> — SEC Form SD
- <https://gender-pay-gap.service.gov.uk/> — UK Gender Pay Gap Service
- <https://sasb.ifrs.org/standards/materiality-map/> — SASB Materiality Map (ISSB)
- <https://www.globalreporting.org/standards/> — GRI Standards
- <https://www.fsb-tcfd.org/recommendations/> — TCFD final recommendations
- <https://ghgprotocol.org/> — GHG Protocol
- <https://www.gleif.org/en/lei-data/lei-mapping> — GLEIF LEI mappings

---

**Confirm:**

- File path: `c:/Users/natha/OneDrive/Desktop/C/ats/ats-eqt/research/datasets/esg_sustainability.md`
- Date: 2026-05-14
- Section count: **17 top-level sections** (0 Executive summary; 1 Why ESG differs; 2 Vendor matrix; 3 MSCI; 4 S&P CSA/DJSI; 5 Sustainalytics; 6 Bloomberg; 7 Refinitiv/LSEG; 8 ISS; 9 FactSet Truvalue; 10 CDP; 11 GRESB; 12 Public-data reconstruction; 13 Cross-vendor correlation; 14 Recommended ats-eqt schema; 15 Open questions; 16 Sources), plus 60+ sub-sections.
- Source count: **~110 URLs** across academic papers, vendor methodologies, regulatory texts, and library guides.
