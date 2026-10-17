# P9 contamination flags: `source_sample_end` of the book's 67 members and the P9 rules (doc lane PRE-FLAGS)

- **Rule applied (binding; PRE-16 .. PRE-20, H19, H20, PRE-28).** A member is FLAGGED when the main sample of its
  PRIMARY source ends in 2015 or later. The primary source is the paper whose construction the member implements, not a
  later robustness, replication, crash-risk or survey citation. Unverified = flagged. Classes: `clean` (ends 2014 or
  earlier), `flagged` (ends 2015 or later), `flagged-unverified` (no citable sample end). Bound used for old papers (as
  in prereg Appendix III): a paper's sample cannot end after its publication year, so a primary source published in
  2014 or earlier is clean even where its sample years were not read.
- **Date.** 2026-10-04 (web reads the same day; ALSIG-1 method: web lookup of the paper, the published version
  preferred).
- **Read.** `resume4-dispatch.md`, `resume6-addendum.md`; `p9-prereg.md` F.1 "Contamination flags", H19, H20,
  Appendix III; ledger `progress.md` (PRE-16 .. PRE-20, PRE-28), `resume-rulings.md` ALSIG-1, `task-AL-SIG-report.md`
  and `task-AL-COMB-report.md` (ALSIG-1 method); `atx-impl/strategies/fund_industry_ic_v8ysb.json` (ids, families,
  formulas, citations, and the `basis` text of the fields the formulas use); `docs/plans/2026-10-03-p9-sprint-plan.md`
  (reference table) and the note it names, `docs/plans/2026-10-02-p9-literature-review.md` (sections 5.2, 5.3,
  bibliography); `docs/plans/2026-10-01-v9-library-draft.md` lines 141-162 (`nt_late` citation only);
  `scripts/specs/p9/rules/*.json`; the papers (URLs below); the Chen-Zimmermann open-source signal documentation
  (`https://raw.githubusercontent.com/OpenSourceAP/CrossSection/master/SignalDoc.csv`, cited as "SignalDoc", secondary,
  used only as a supplement to the publication-year bound).
- **Blind.** No return / IC / Sharpe / turnover / NAV statistic of our book or candidates was read; no data file, NAV
  dir, `build-equity/`, `trials.jsonl`, `wq-explore*` or plan PNG was opened; no git history was read. Statistics of
  published papers were seen only where they sit beside a sample statement.
- **Access notes.** SSRN, ScienceDirect, INFORMS, ResearchGate and the SMU repository refused automated reads (HTTP
  403 or bot walls). Where only a working-paper or author copy could be read, the table says so. No permission-system
  refusal occurred.

## Count check (step 4)

Library `candidates`: 67 ids, 67 unique. Appendix III lists: 36 clean-by-year + 19 verify (first cited 2015+) + 12
verify (later citation) = 67. Every library id is in exactly one list; no list id is missing from the library; no id
is in two lists. **No discrepancy.**

## Table A. The 31 verified members

Sample end = end of the primary source's main (US) sample. Quotes are verbatim from `pdftotext` of the PDF
downloaded from the URL, except those marked [fetch], which came through the web-fetch tool's text extraction (it
reported them as verbatim; the page itself was not saved).

### A.1 First-cited paper 2015 or later (19)

| id | primary source | why primary | sample end | evidence (URL + quote) | class |
|---|---|---|---|---|---|
| opbe | Fama, French (2015), "A five-factor asset pricing model", JFE 116(1) 1-22 | `oi_ttm / be` is FF's operating profitability (RMW sort variable); sole citation | Dec 2013 | https://tevgeniou.github.io/EquityRiskFactors/bibliography/FiveFactor.pdf (typeset JFE article), Table 1 caption: "B/M, Size and OP, Size and Inv; July 1963-December 2013, 606 months." | clean |
| cbop_f49 | Ball, Gerakos, Linnainmaa, Nikolaev (2016), "Accruals, cash flows, and operating profitability in the cross section of stock returns", JFE 121(1) 28-45 | (CFO + R&D) / assets is BGLN's cash-based operating profitability; BGLN 2015 cited only for the R&D add-back | Dec 2014 | https://www.anderson.ucla.edu/documents/sites/about/clubs/asam/6-Accruals-cash-flows-and-operating-profitability.pdf (typeset JFE), Table 1 legend: "Our sample period starts in July 1963 and ends in December 2014." | clean |
| roe_q | Hou, Xue, Zhang (2015), "Digesting anomalies: An investment approach", RFS 28(3) 650-705 | quarterly NI / one-quarter-lagged BE is the q-factor ROE; sole citation | Dec 2012 | https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/houxuezhang2015rfs.pdf (published version), data section: "The sample is from January 1972 to December 2012." (NBER w18435, 2012 draft: Jan 1972-Dec 2011) | clean |
| bac | Asness, Frazzini, Gormsen, Pedersen (2020), "Betting against correlation: Testing theories of the low-risk effect", JFE 135(3) 629-652 | negative 250-day correlation of overlapping 3-day returns with the market is AFGP's BAC sort variable | Dec 2015 | https://yiqianlu.org/wp-content/uploads/2017/02/betting-against-correlation.pdf (SSRN 2913508 version): "The U.S. sample consists of all available common stocks on the CRSP tape from January 1926 to December 2015."; corroborated [fetch] https://www.cxoadvisory.com/volatility-effects/betting-against-correlation/ "...all through December 2015". Published JFE text not read (403); the working paper already ends 2015. | flagged |
| smax | Asness, Frazzini, Gormsen, Pedersen (2020), as `bac` | max daily return scaled by volatility is AFGP's SMAX idea (Bali-Cakici-Whitelaw 2011 MAX is the unscaled ingredient); see Open item 9 | Dec 2015 | as `bac` | flagged |
| dtc | Hong, Li, Ni, Scheinkman, Yan (2015), "Days to cover and stock returns", NBER WP 21166 | days to cover (short ratio / turnover) is HLNSY's measure; sole citation | Dec 2012 | https://www.nber.org/system/files/working_papers/w21166/w21166.pdf: "Our sample period runs from 1988 to 2012."; Table 2 text: "starting in January 1988 and ending in December 2012." | clean |
| si_change | Rapach, Ringgenberg, Zhou (2016), "Short interest and aggregate stock returns", JFE 121(1) 46-65 | the only source cited, for the direction (rising short interest predicts lower returns); it is an aggregate time-series paper, see Open item 5 | Dec 2014 | https://down.aefweb.net/WorkingPapers/w716.pdf (typeset JFE): "...for the 42-year period from January 1973 through December 2014."; "1973:01 to 2014:12 sample period" | clean |
| sv_flow | Wang, Yan, Zheng (2020), "Shorting flows, public disclosure, and market efficiency", JFE 135(1) 191-212 | 126-session FINRA short-volume ratio is WYZ's long-term shorting flow; FINRA cited as data only | Dec 2015 | https://www.lehigh.edu/~xuy219/research/Short_Final_Version.pdf (typeset JFE), table notes: "...listed on the NYSE, AMEX, and NASDAQ from January 2010 to December 2015." | flagged |
| qmj_safety | Asness, Frazzini, Pedersen (2019), "Quality minus junk", Review of Accounting Studies 24(1) 34-112 | mean rank of low beta, low leverage, low ROE volatility is QMJ's safety component (BAB, LEV, EVOL); Frazzini-Pedersen 2014 cited for the BAB input | Dec 2016 | [fetch] https://link.springer.com/article/10.1007/s11142-018-9470-2 (published full text), Fig. 2 caption (US long sample): "The sample period runs from June 1957 to December 2016." | flagged |
| q5_eg_f49g | Hou, Mo, Xue, Zhang (2021), "An augmented q-factor model with expected growth", Review of Finance 25(1) 1-41 | the formula is HMXZ's Table I Panel D expected-growth regression slopes; sole citation | Dec 2018 | https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/q5rf2021.pdf (published version): "Table II. Properties of the expected growth deciles (January 1967 to December 2018)" | flagged |
| smax5 | Asness, Frazzini, Gormsen, Pedersen (2020), as `bac` | mean of the 5 largest daily returns over 21 sessions / 21-session volatility is AFGP's SMAX exactly | Dec 2015 | as `bac` | flagged |
| ea_overdue | Johnson, So (2018), "Time will tell: Information in the timing of scheduled earnings news", JFQA 53(6) 2431-2464 | first cited; announcement later than expected predicts bad news (Bagnoli-Kross-Watts 2002, also cited, is the older "a day late" source: pre-2015 either way) | 2013 | https://www.travislakejohnson.com/pdfs/Johnson%20So%20TWT%202018%20(JFQA).pdf (typeset JFQA): "...earnings calendar data provided by Wall Street Horizon from 2006 through 2013."; "The sample consists of 18,959 firm-quarters spanning 2006-2013" | clean |
| ear_mom_12m | Gerard, Jehl (2025), "The Many Facets of Stock Momentum: Distinguishing Factor and Stock Components", FAJ 82(1) | 12-month mean of earnings-announcement abnormal returns is GJ's stock-specific momentum; Chan-Jegadeesh-Lakonishok 1996 is the single-quarter EAR ancestor | Sep 2024 | https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf (typeset FAJ): "...I/B/E/S database for the period July 1992 to September 2024."; "Panel A: Largest 1,000 US Stocks from July 1992 to September 2024" (lit 5.2 (b) agrees: 1992-2024) | flagged |
| op_rd | Novy-Marx, Medhat (2025), "Profitability Retrospective: What Have We Learned?", NBER WP 33601 | first cited; profitability "unpunished for R&D" is NM-M's recommended measure; lit 5.2 (b) names it. Arguable, see Open item 2 | Dec 2023 (some tables Jun 2024) | https://www.nber.org/system/files/working_papers/w33601/w33601.pdf: "The sample covers July 1963 through December 2023."; "The sample covers July 1974 through June 2024" | flagged |
| dtc_slow | Hong, Li, Ni, Scheinkman, Yan (2015), as `dtc` | short shares / 126-session mean volume is HLNSY's days to cover; sole citation | Dec 2012 | as `dtc` | clean |
| k8_intensity | Zhao (2017), "Does information intensity matter for stock returns? Evidence from Form 8-K filings", Management Science 63(5) 1382-1404 | count of 8-K filings over past quarters (negative sign) is Zhao's information intensity; sole citation | not established | Full text not readable (SSRN, INFORMS, ACM, ResearchGate: 403; no open copy found via OpenAlex / Semantic Scholar / Wayback). SSRN abstract (archived: https://web.archive.org/web/20240907080803/https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1916471) states no years; "Date Written: October 15, 2015", "Posted: 4 Jan 2012". Form 8-K's expanded regime starts Aug 2004 (lower end only). | flagged-unverified |
| nt_late | Bartov, Konchitchki (2017), "SEC filings, regulatory deadlines, and capital market consequences", Accounting Horizons 31(4) 109-131 | short after a first NT 10-K / NT 10-Q notice is BK's event; sole citation | 2009 (NT filings to 1 Sep 2009; fiscal period-ends 2000-2008) | https://faculty.haas.berkeley.edu/yaniv/files/Papers_Publications/SEC-Filings-Regulatory-Deadlines-Capital-Market-Consequences_Bartov-Konchitchki_AH.pdf (typeset AH): "We obtain our sample of NT filers from the Audit Analytics Non-Timely Module Feed (NT) dataset for all 10-Q and 10-K late filings with fiscal period-ends between 2000 and 2008, where the NT filing data is on or prior to September 1, 2009." | clean |
| iv_vol_of_vol | Baltussen, van Bekkum, van der Grient (2018), "Unknown unknowns: Uncertainty about risk and stock returns", JFQA 53(4) 1615-1651 | volatility of ATM implied volatility over a month, scaled by its mean, is BVV's vol-of-vol; sole citation | Dec 2014 (returns to Oct 2014 monthly, Dec 2014 longer horizons) | https://personal.eur.nl/vanbekkum/2018JFQA_unknown_unknowns.pdf (typeset JFQA): "The option data run from Jan. 1, 1996 ... until Sept. 30, 2014. We use these data to analyze future returns from Feb. 1996 until Oct. 2014 (for monthly returns) or Dec. 2014 (for longer horizons)." (2012 draft: 1996-2009) | clean |
| day_rev_freq | Akbas, Boehmer, Jiang, Koch (2022), "Overnight returns, daytime reversals, and future stock returns", JFE 145(3) 850-875 | monthly frequency of positive-overnight / negative-daytime days is ABJK's tug-of-war measure; sole citation | Dec 2017 | [fetch] https://www.cxoadvisory.com/calendar-effects/tug-of-war-risk-and-future-stock-returns/ (summary of the Jan 2019 working paper, SSRN 3324880): "...during May 1993 through December 2017". Published text not read (ScienceDirect, SMU, ResearchGate: 403); the working paper already ends 2017. | flagged |

### A.2 A later citation 2015 or later beside an earlier source (12)

| id | primary source | why primary | sample end | evidence (URL + quote) | class |
|---|---|---|---|---|---|
| value_composite | Israel, Laursen, Richardson (2021), "Is (Systematic) Value Investing Dead?", JPM 47(2) 38-62 | the member is a sector-neutral rank composite of B/P, E/P, CF/P on current price, which is ILR's construction ("composite value"; Asness-Frazzini 2013 for current price); Fama-French 1992 and LSV 1994 supply the single ratios, not the composite. Arguable, see Open item 1 | 2020 | https://www.aqr.com/-/media/AQR/Documents/Journal-Articles/AQR-JPMQuant21IsValueInvestingDead.pdf (typeset JPM): "The sample period begins between 1984 and 2002 depending on country"; "the average coefficient over the 1987-2020 period is 0.02"; Exhibit 7 has yearly rows to 2020. Construction: "we group stocks into sectors (peer groups) within each country and subtract the median value score ... We then rank and standardize". [fetch] CXO summary of the Mar 2020 WP (https://www.cxoadvisory.com/value-premium/value-investing-still-on-death-row/): "all extending through 2019". | flagged |
| bm | Fama, French (1992), "The cross-section of expected stock returns", JF 47(2) (Rosenberg-Reid-Lanstein 1985 cited first) | B/M within industry; both are B/M sources. Linnainmaa-Roberts 2018 is a pre-1963 robustness citation and "weak 2017-2020" is a remark, not a source | 1990 (RRL: 1984) | bound: published 1992 (1985). SignalDoc rows `BMdec` (FF 1992) 1963-1990, `BMq` (RRL 1985) 1973-1984 | clean |
| gpa_f49 | Novy-Marx (2013), "The other side of value: The gross profitability premium", JFE 108(1) 1-28 | gross profit / assets is NM's measure; Hou-Xue-Zhang 2020 is a replication | Dec 2010 | https://mysimon.rochester.edu/novy-marx/research/OSoV.pdf (June 2012 author version): "...covers July 1963 to December 2010."; SignalDoc `GP` 1963-2010 | clean |
| res_mom_12_1 | Blitz, Huij, Martens (2011), "Residual momentum", JEF 18(3) 506-521 | 12-1 residual return standardised by residual volatility is BHM's residual momentum (here a market-only residual); Blitz-Hanauer-Vidojevic 2020 is a later extension, Grundy-Martin 2001 a precursor. See Open item 6 | Dec 2009 | http://repub.eur.nl/res/pub/22252/ResidualMomentum-2011.pdf (accepted JEF manuscript): "Our sample period covers the period January 1926 to December 2009."; strategies "January 1930 to December 2009" | clean |
| mom_12_1 | Jegadeesh, Titman (1993), "Returns to buying winners and selling losers", JF 48(1) | 12-1 price momentum; Daniel-Moskowitz 2016 is a crash-risk citation | 1989 | bound: published 1993. SignalDoc `Mom12m` 1964-1989 | clean |
| ind_adj_rev_5 | Hameed, Mian (2015), "Industries and stock return reversals", JFQA 50(1-2) 89-117; Da, Liu, Schaumburg (2014), "A closer look at the short-term return reversal", MS 60(3) 658-674 | within-industry reversal is the construction of both; either reading is clean | Dec 2010 (HM draft); Mar 2009 (DLS) | HM: https://web2-bschool.nus.edu.sg/wp-content/uploads/media_rp/publications/BwXb81392625636.pdf (author draft): "The sample is obtained for the time period from January 1968 to December 2010." DLS: https://www3.nd.edu/~zda/Reversal.pdf (typeset MS): "The full sample period is from January 1982 through March 2009." | clean |
| nincr | Barth, Elliott, Finn (1999), "Market rewards associated with patterns of increasing earnings", JAR 37(2) 387-413 | count of consecutive year-on-year quarterly earnings increases is BEF's pattern; Green-Hand-Zhang 2017 codes it as `nincr` (also pre-2015 sample); Han-He-Rapach-Zhou 2018 is counter-evidence | <= 1999 (BEF); 2014 (GHZ) | BEF: bound, published 1999 (sample years not read). GHZ: [fetch] abstract at https://ideas.repec.org/a/oup/rfinst/v30y2017i12p4389-4436..html gives the period as 1980-2014 (paraphrased by the fetch tool, not a verbatim quote) | clean |
| res_mom_ind | Blitz, Huij, Martens (2011), as `res_mom_12_1` | standardised residual (here vs the FF49 industry mean) momentum is BHM's construction; see Open item 6 | Dec 2009 | as `res_mom_12_1` | clean |
| inst_best_ideas | Cohen, Polk, Silli (2010), "Best ideas", LSE FMG DP 624 / Paul Woolley Centre WP 3 | the field is CPS's conviction tilt (field formula id `13f-asof45-best-ideas-cps-v1`); Anton-Cohen-Polk 2021 is a later version, Brown-Howard-Lundblad 2022 a crowding citation. See Open item 7 | Dec 2005 | https://eprints.lse.ac.uk/24471/1/Best%20ideas(published).pdf: "Our sample consists of US domestic equity funds that report their holdings in the period from January 1991 to December 2005." | clean |
| ftd_fail | Stratmann, Welborn (2016), "Informed short selling, fails-to-deliver, and abnormal returns", JEF 38(A) 81-102 | a continuous FTD / shares sort with negative returns "proportional to their FTD levels" is SW's portfolio test; Evans-Geczy-Musto-Reed 2009 and Miller 1977 give the mechanism, Autore-Boulton-Braga-Alves 2015 tests threshold-list entry. See Open item 3 | not established for the published paper (author's dissertation version: 2008) | Published text not readable (SSRN, ScienceDirect, Yale EliScholar: 403). SSRN abstract (archived: https://web.archive.org/web/20250912091940/https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2461088) has no years; "Posted: 2 Jul 2014", "Date Written: May 18, 2016". Dissertation [fetch] (https://ices.gmu.edu/defenses/685): "I find that high FTD stocks from the Russell 3000 Index experienced abnormal negative returns from 2004 through 2008." | flagged-unverified |
| earn_surprise_comp | Novy-Marx (2015), NBER WP (cited "composite: Novy-Marx (2015, WP)"); components: Bernard-Thomas 1989 (SUE), Thomas-Zhang 2011 (tax change), Hou-Mo-Xue-Zhang 2021 (dROE); Martineau 2022 is a robustness citation | the member is the equal-rank composite; the composite is attributed to Novy-Marx 2015. The WP is not named: "Fundamentally, momentum is fundamental momentum" (w20984) or "Backtesting strategies based on multiple signals" (w21329), both read. See Open item 8 | Dec 2012 (w20984); Dec 2014 (w21329) | https://www.nber.org/system/files/working_papers/w20984/w20984.pdf: "The full sample covers January 1975 through December 2012"; https://www.nber.org/system/files/working_papers/w21329/w21329.pdf: "the sample covers July 1993 through December 2014". BT 1989: bound | clean |
| season_y2_5 | Heston, Sadka (2008), "Seasonality in the cross-section of stock returns", JFE 87(2) 418-445 | mean same-calendar-month return at annual lags 2-5 is HS's "years 2 to 5" seasonality (field formula id `hs-season-y2-5-v1`); Keloharju-Linnainmaa-Nyberg 2016 is later evidence | 2002 | bound: published 2008. SignalDoc `MomSeason` "Return seasonality years 2 to 5", HS 2008, 1965-2002 | clean |

## Table B. The 36 members clean by publication year (step 2)

Years are every year in the member's library `citation` string (extracted by script). None is 2015 or later. The
literature note (`2026-10-02-p9-literature-review.md`) names seven of these ids (`deal_target`, `fscore_hbm`, `mom_turn`,
`so_wang_rev`, `stio_trade` at :46; `iv_rv_spread` :233; `ea_uvol` :239) and cites no other source for any of them. No
member moves to Table A.

| id | cited years | class |
|---|---|---|
| ep | 1977, 1992 | clean |
| cfp | 1994, 2004 | clean |
| fcfp | 1994, 1994 | clean |
| ebit_ev_f49 | 2011 | clean |
| net_payout | 2007 | clean |
| sp | 1996 | clean |
| rd_me | 2001 | clean |
| roa | 2010, 2011 | clean |
| accruals_f49 | 1996, 2002, 2011 | clean |
| fscore | 2000 | clean |
| asset_growth_f49 | 2008, 2008 | clean |
| noa_f49 | 2004 | clean |
| issuance_xbrl | 2008, 2006, 2008 | clean |
| issuance_vendor | 2006, 2008 | clean |
| ear | 1996, 2008 | clean |
| ind_mom_12_1 | 1999 | clean |
| within_ind_mom | 2000 | clean |
| high_52w | 2004 | clean |
| si_ratio | 2005, 2010 | clean |
| seasonality_same_month | 2008 | clean |
| iv_rv_spread | 2009 | clean |
| opex_at_f49 | 2011 | clean |
| ins_opp_buy | 2001, 2003, 2012 | clean |
| pct_accruals | 2011 | clean |
| fip_id | 2014 | clean |
| si_low_io | 2005, 2005 | clean |
| inst_persist | 2011 | clean |
| div_season | 2013 | clean |
| vol_beta | 2006 | clean |
| so_wang_rev | 2014 | clean |
| mom_turn | 2000 | clean |
| ea_uvol | 2006 | clean |
| dato | 2008 | clean |
| fscore_hbm | 2000 | clean |
| stio_trade | 2009, 2005 | clean |
| deal_target | 2001, 2002 | clean |

## P9 rules (step 3)

| rule | registration `source_sample_end` | basis in the registration | check | class under H19 |
|---|---|---|---|---|
| theme-cov-hedge-v1 | 2017 (top level and the nested copy at :64) | Daniel, Mota, Rottke, Santos (2020) RFS 33(5): "1963/07 - 2017/06", read in NBER w24164 rev0 | confirmed = Appendix III | flagged (2017) |
| mom-volman-v1 | 2016 (top level and the nested copy at :70) | Cederburg, O'Doherty, Wang, Yan (2020) JFE 138(1), samples "end in December 2016"; Barroso-Santa-Clara 2015 (1927:03-2011:12) noted as the hypothesis's own source | confirmed = Appendix III | flagged as registered (2016); see Open item 10 |
| vol-target-v1 | - | prereg: not read on history | not in scope | - |
| info-clock-rate-v1 | pending: AL-CLOCK registration | no file in `scripts/specs/p9/rules/` at read time | - | - |
| mv-aim-v1 | pending: COV-MV registration | no file in `scripts/specs/p9/rules/` at read time | - | - |

## FINAL FLAG LIST

**Flagged (10):** bac, smax, smax5, sv_flow, qmj_safety, q5_eg_f49g, ear_mom_12m, op_rd, day_rev_freq, value_composite

**Flagged-unverified (2):** k8_intensity, ftd_fail

**Counts:** clean 55 (36 + 19 of the 31) / flagged 10 / flagged-unverified 2 = 67. Drop list for the unflagged book
(H20): 12 members. P9 rules: theme-cov-hedge-v1 and mom-volman-v1 carry 2017 and 2016 (both 2015 or later); the
other two are pending.

Changes from Appendix III's expectation: `value_composite` (listed "(1992; 2021)", expected clean) is flagged because
the composite construction is ILR's; `earn_surprise_comp` also cites Hou-Mo-Xue-Zhang 2021 (not listed in Appendix
III) and stays clean under the composite reading.

## Open for the PM

1. **value_composite**: primary ILR 2021 (composite, sample to 2020) flags it (as listed); primary Fama-French 1992 /
   LSV 1994 (single ratios, samples to 1990) makes it clean (flagged 9).
2. **op_rd**: primary Novy-Marx-Medhat 2025 (first cited, to 2023; scales by BE+MIB and deducts interest) flags it (as
   listed); primary Ball-Gerakos-Linnainmaa-Nikolaev 2015 (operating profit with R&D added back over total assets,
   the member's deflator; Booth WP 14-10 draft "The sample starts in July 1963 and ends in December 2012",
   https://www.ivey.uwo.ca/media/3775544/linnainmaa.pdf) makes it clean (flagged 9).
3. **ftd_fail**: Stratmann-Welborn 2016's published sample is unread, so flagged-unverified (as listed); accepting the
   dissertation's 2004-2008, or taking Autore-Boulton-Braga-Alves 2015 (verified "January 7, 2005 through December 31,
   2008", http://web.archive.org/web/20240416004841/https://activityinsight.pace.edu/mbragaalves/intellcont/abb%20fr%202015-1.pdf)
   or Evans et al. 2009 as primary, makes it clean (unverified 1).
4. **k8_intensity**: unverified (as listed); a readable copy of Zhao 2017 would settle it; nothing citable bounds the
   end below 2015 (revision dated Oct 2015). Clean only if the text shows an end of 2014 or earlier (unverified 1).
5. **si_change**: Rapach-Ringgenberg-Zhou 2016 (aggregate time series, to Dec 2014) as primary: clean (as listed); if
   it is ruled a direction citation only, no cited paper implements the cross-sectional 21-day change in the short
   ratio, so flagged-unverified (unverified 3).
6. **res_mom_12_1, res_mom_ind**: Blitz-Huij-Martens 2011 (to 2009) as primary: clean (as listed); Blitz-Hanauer-
   Vidojevic 2020 as primary (sample "from 1926 through 2015" per the Swedroe summary [fetch],
   https://finance.yahoo.com/news/swedroe-spotlight-enhancing-momentum-strategies-150000405.html; paper unread) flags
   both (flagged 12).
7. **inst_best_ideas**: Cohen-Polk-Silli 2010 (to 2005) as primary: clean (as listed); Anton-Cohen-Polk 2021 ("January
   1983 to December 2018", https://personal.lse.ac.uk/polk/research/bestideas.pdf) as primary flags it (flagged 11).
8. **earn_surprise_comp**: the composite (Novy-Marx 2015, either WP, to 2012 or 2014) or Bernard-Thomas 1989 as
   primary: clean (as listed); counting the dROE component's cited source, Hou-Mo-Xue-Zhang 2021 (to 2018), flags it
   (flagged 11). An older dROE source exists (Balakrishnan-Bartov-Faurel 2010, SignalDoc `ChangeRoE` 1976-2005) but
   is not cited.
9. **smax**: the member is max 1-day return / 252-day volatility, not AFGP's MAX5 / 1-month volatility; AFGP as primary
   (the vol scaling) flags it (as listed); Bali-Cakici-Whitelaw 2011 MAX (SignalDoc `MaxRet` 1962-2005) as primary
   makes it clean (flagged 9).
10. **P9 rules**: mom-volman-v1 is registered at 2016 (Cederburg); with Barroso-Santa-Clara 2015 as primary (sample to
    2011:12) it is clean. Separately, H20 drops members only: the PM must rule whether a flagged rule (theme-cov-hedge
    2017, mom-volman 2016) is switched off in the "P9-F0 without flagged members" line or stays on.
