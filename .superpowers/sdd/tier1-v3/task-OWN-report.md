# task-OWN report (tier1-v3 lane OWN: ownership, insiders, short side)

Tasks: S5.1-S5.5 of `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md`.
Stopped at the owner stop (2026-09-30 ~02:10Z). All code is committed. Every loop, waiter and guard-queued job I
started as a task is stopped. **Two document landings are still running** (see section 8): stopping them was
refused by the permission classifier ("Interfere With Workloads"), so the owner has to stop them.

## 1. Outcome

PARTIAL. S5.5 is done. S5.4 insider measures are built. S5.3 XML 13D is done and the pre-2024 hand check is done:
stake fields pass, reporting-person names and the activism flag fail. S5.1 and S5.2 have all code, tests and
landings needed, but their builds were not run (S5.1 inputs completed only at the stop; S5.2 has 16 of 27 quarters
parsed).

## 2. Done criteria

| task | criterion | status | measured |
|---|---|---|---|
| S5.1 | filer type for >= 90% of 13F SH value every quarter 2019q1+ | NOT RUN | All inputs are landed: 82 ADV rosters (2018-10 .. 2026-08; Feb / May / Aug / Nov from 2020-02), 7 ADV bulk tables, 31 13F cover / included-manager sets, `filer_values.parquet` (213,248 filer-quarters 2018q4-2026q2). `filer_type build` has never run. |
| S5.2 | fund ownership for >= 95% of member_equity cells from 2020 | NOT RUN | 16 of 27 quarters parsed (2019q4-2023q3: 9,117-13,172 filings and 1.42-1.93 M equity holdings per quarter). The build (security map, fund_ownership, fund_flows, coverage) is implemented and unit-tested but has not run. |
| S5.3 | >= 95% of 2025 13D/13G filings parsed | PARTIAL | 13D: 2025 SCHEDULE 13D 1,115/1,115 parse_ok, 13D/A 4,516/4,522 (99.87%). 13G: only 1,456 of 24,740 XML documents landed at the stop, so 2025 13G parse_ok is 588/7,820 and 865/16,710 of notices. Parse success on landed 13G documents is 1,453/1,456. |
| S5.3 | precision >= 95% on a 100-filing hand-checked pre-2024 13D sample | FAIL (2 of 4 fields) | Stake fields (issuer, CUSIP, event date, max shares, max percent) are all correct in 97/100 filings: shares 100/100, percent 98/100, CUSIP 99/100, issuer 100/100, event date 100/100. Reporting-person names are correct in only 65/100. The activism flag is right in 72/98 judgeable cases (73.5%); precision of flag = true is 17/29 (58.6%), recall 17/31 (54.8%). All 100 cases are in section 6. |
| S5.4 | 144 notices with available_at | PARTIAL | Code done. `form144 fetch` never ran (the SEC budget was spent on 13D/G first), and `form144 notices` was not built. The notices list is 127,205 accessions 2019+ in `sec_filings`, all with acceptance clocks. |
| S5.4 | insider net measures per issuer and month 2019+ | PASS (with open issue) | `insider_ext/net_buying_monthly.parquet`: 136,164 issuer-months 2019-01..2026-09, about 4,200-4,800 issuers per year. `insider_trades.parquet`: 873,513 P/S trades, CMP-labelled (2019: 20,459 routine / 15,583 opportunistic / 75,511 unclassified). Open issue: value sums carry price outliers (see section 9). |
| S5.5 | Reg SHO threshold-list completeness, 5 listing markets, 2018+ | PASS | Every settlement session is present for all 5 markets and FINRA OTC. The only sessions without a list are Columbus Day and Veterans Day each year (no settlement), and 0 empty lists break symbol continuity. Details in section 7. |

## 3. What was built and committed

| commit | contents |
|---|---|
| b8724a50 | `sec_docs.py` (SEC document store, zip range reads, row-group scanner), `adv.py`, `filer_type.py` + tests (S5.1) |
| 2b3df8f5 | `nport.py` + tests (S5.2) |
| 8174a949 | `stakes.py` + tests (S5.3) |
| e9928849 | `form144.py`, `insider_measures.py` + tests (S5.4) |
| 2dd47069 | stakes fix: `issuerCusip` tag of live 13G XML (the trial build mapped 0 of 1,456 13G CUSIPs), issuer-name rule, split event dates |
| (docs) | `atx-db/docs/ALPHA_PANEL_OWNERSHIP.md`, this report |

Tests: `tests/test_alpha_panel_{adv,filer_type,nport,stakes,form144,insider_measures}.py`, 34 tests, all offline
fixtures, all passing (`python -m pytest tests/test_alpha_panel_{adv,filer_type,nport,stakes,form144,insider_measures}.py -q`
-> 34 passed). `ruff check` is clean on all owned files.

## 4. Stages and outputs on disk

| path (under `atx-db/data/alpha_panel/v1/`) | state | notes |
|---|---|---|
| `insider_ext/` insider_trades.parquet, net_buying_monthly.parquet | published; manifest sha256 1d7fb338bffb6d8449dc6451b97e734ef80d086f51a1195497b37cad9fd6221f | guarded build, 18.5 s, native peak 0.43 GiB; `form144_monthly` absent (no 144 notices built) |
| `stakes/` filings, persons, notices | published from a PARTIAL landing; manifest sha256 b43a94d96fd892e164830e853a3cc80f064a0333c693d7387effd50e4b6c9983 | trial build 2 with the old issuer rule and without the 13G CUSIP fix. **Rebuild after the fetches finish.** 11,979 filings, 45,683 persons, 240,834 notices |
| `thirteenf_filer_type/` adv/rosters (82), adv/bulk (7 tables), thirteenf_cover (31 sets), filer_values.parquet | inputs only; no manifest | `filer_type build` not run |
| `nport/parts/quarter=2019q4 .. 2023q3` | parts only; no manifest | `quarter=2023q4/` is a leftover from a guard-killed parse and is redone on resume |

## 5. Sources

| source | url | bytes landed | span | cadence | terms / rate limit |
|---|---|---|---|---|---|
| Form ADV monthly rosters | https://www.sec.gov/data-research/sec-markets-data/information-about-registered-investment-advisers-exempt-reporting-advisers | 586 MB over 84 files (parsed, then deleted) | 2018-10-01 .. 2026-08-03 (82 parsed, 2 shutdown notices) | monthly | SEC public data; approved UA; host-wide 5 req/s |
| Form ADV Part 1 bulk (members by HTTP range) | https://www.sec.gov/data-research/sec-markets-data/form-adv-data | 182.6 MB of ranges (7 members) | 2011-11-05 .. 2024-12-31 (part1 LM 2026-05-01, part2 LM 2025-09-26) | frozen; none after 2024 | same |
| 13F data sets, COVERPAGE + OTHERMANAGER2 members | https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets | small ranges, 31 sets | 2019q1 .. 2026 | quarterly | same |
| Form N-PORT data sets (6 members by range) | https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets | 5.48 GB of ranges for 16 quarters (deleted after parse) | 2019q4 .. 2023q3 landed; 2023q4 .. 2026q2 to do | quarterly | same |
| Schedule 13D/13G primary documents | EDGAR Archives `primary_doc.xml` / primary HTML | 542 MB served for 16,821 fetches (all HTTP 200), 112 MB gzip store | 13D XML 2024-12 .. 2026-09 complete; 13G XML partial; text 13D partial | per filing | same |
| Form 144 primary documents | EDGAR Archives | 1 test fetch | - | per filing | same |

SEC requests used by the lane: 17,063 limiter grants at the time of writing (`data/raw/own_sec_rate_log.jsonl`,
`ATX_SEC_RATE_LOG`), below the 60,000 cap. Two landings are still running (section 8), capped by their
`--max-new` (xml 35,000 in total over both phases, text13d 7,400), so the lane cannot exceed about 44,000 grants.
Disk: `data/raw/sec_13dg` 112 MB, `sec_adv` 0.2 MB, `sec_nport` 0.04 MB, `sec_144` 5 KB;
stages `nport/` 1.2 GB, `thirteenf_filer_type/` 256 MB, `insider_ext/` 22 MB, `stakes/` 8.5 MB. C: had 54 GB free.

Runs unguarded per C-1, with measured peaks (tracemalloc Python heap / pyarrow pool high-water mark):
- ADV rosters: 0.105 / 0.013 GiB;
- ADV bulk: Base A 0.04 GiB pool;
- 13F cover: 0.044 GiB;
- stakes trial build 2: 0.179 / 0.118 GiB. The CUSIP map is now restricted per phase, which lowers the heap part.
  Not re-measured; the next stakes build should be guarded if the sum must stay <= 0.25 GiB.
- stakes / 13D-G landings: network only.

Guarded runs:
- `filer_type values`: 0.41 GiB native. The first try was stopped by the guard for host low commit.
- `insider_measures`: 0.43 GiB.
- `nport fetch`: 0.59-0.63 GiB native. Stopped three times by host low commit, which is host pressure and not the
  job's cap; PARSE_MEMORY was lowered to 300 MB.

## 6. S5.3 pre-2024 13D hand check (100 filings)

**Sample.** A seeded draw (`random.Random(20260929)`) of 100 from the 6,246 SC 13D filings dated 2019-01-01 ..
2023-12-31. Their documents were landed (100 requests, part of the text13d phase anyway). Each was parsed with the
committed `parse_text_13d` (rule `13d-text-v1` with the new issuer / date rule).

**Check.** Four independent reader agents each took 25 cases. Each read the rendered title page, every cover page
and the extracted Item 4, and compared the parsed values field by field against the document text.

**Activism flag definition.** Item 4 discloses a specific plan, proposal, agreement or action of the Item 4 (a)-(j)
kinds, beyond the generic reservation of rights.

Case files: `C:/Users/natha/AppData/Local/Temp/claude/c--atx/001b5e31-2cd3-481f-a401-fe5dd896bd28/scratchpad/own/handcheck/case_NNN.txt`
(scratch; re-creatable with `scratchpad/own/handcheck_sample.py`).

| field | correct |
|---|---|
| issuer name | 100/100 |
| CUSIP | 99/100 |
| event date | 100/100 |
| max shares (row 11) | 100/100 |
| max percent (row 13) | 98/100 |
| all five stake fields together | 97/100 |
| reporting-person names | 65/100 |
| activism flag | 72/98 (2 not judgeable) |

Activism flag errors: 12 false positives and 14 false negatives.
- False positives come from `demand` in registration or appraisal rights, `strategic alternatives` or `tender offer`
  inside the "may" reservation, and `nominating` in committee names.
- False negatives are board-designation rights, merger, rollover or support agreements, and letters to management
  that the term list does not catch.

Per-case record (parsed values; verdict from the readers):

| case | accession | issuer (parsed) | CUSIP | shares max | % max | n persons | activism flag | verdict |
|---:|---|---|---|---:|---:|---:|---|---|
| 1 | 0001193125-20-157437 | Lyra Therapeutics, Inc. | 55234L105 | 3,222,561 | 24.9 | 4 | False | persons: names null |
| 2 | 0001193805-20-001284 | Oncorus, Inc. | 68236R103 | 2,848,970 | 13.04 | 8 | False | all correct |
| 3 | 0001685715-19-000033 | PARSLEY ENERGY, INC. | 701877102 | 32,722,834 | 10.3 | 1 | False | activism FN: party to merger + voting agreement |
| 4 | 0001104659-23-119164 | GLOBAL LIGHTS ACQUISITION CORP | G3937F101 | 2,075,000 | 23.12 | 2 | False | all correct |
| 5 | 0001493152-22-004771 | SQL TECHNOLOGIES CORP. | 78471E105 | 15,075,330 | 19.5 | 3 | False | persons: names cut at line break |
| 6 | 0001193125-22-076321 | OAKTREE STRATEGIC CREDIT FUND | None | 4,000,000 | 100.0 | 8 | True | activism FP: 'strategic alternatives' inside reservation |
| 7 | 0001062993-19-004535 | Mountain Province Diamonds | None | 63,118,468 | 30.041 | 1 | False | all correct |
| 8 | 0001193125-21-257624 | FINANCE OF AMERICA COMPANIES INC. | 31738L107 | 76,682,162 | 56.9 | 3 | False | persons: names null |
| 9 | 0001104659-21-103746 | SCOPUS BIOPHARMA INC. | 809171101 | 3,231,242 | 15.3 | 3 | False | all correct |
| 10 | 0001214659-23-014895 | Flexsteel Industries, Inc. | 339382103 | 263,004 | 5.06 | 6 | False | activism FN (borderline): seeks governance rights / board |
| 11 | 0001193125-19-244738 | JUST ENERGY GROUP INC. | 48213W101 | 7,709,408 | 5.2 | 2 | True | persons: names null; activism FP: 'strategic alternatives', text says no plans |
| 12 | 0001193125-22-306004 | QUOTIENT LIMITED | G73268149 | 125,703 | 3.4 | 4 | False | activism FN: transaction support agreement |
| 13 | 0000921895-19-001136 | Quest Resource Holding Corporation | 74836W104 | 2,080,000 | 13.6 | 3 | True | all correct |
| 14 | 0001193125-23-254235 | Lithium Americas Corp. | 53681J103 | 62,162,326 | 30.0 | 2 | False | all correct |
| 15 | 0000807249-22-000086 | Ellsworth Growth & Income Fund Ltd. | 289074205 | 390,000 | 10.53 | 7 | False | all correct |
| 16 | 0001140361-21-029612 | AxonPrime Infrastructure Acquisition C | 05467C108 | 5,087,500 | 27.4 | 8 | False | all correct |
| 17 | 0001193125-23-196184 | AIM IMMUNOTECH INC. | 00901B105 | 1,716,100 | 3.5 | 2 | True | all correct |
| 18 | 0001213900-23-045092 | Soleno Therapeutics, Inc. | 834203309 | 8,418,093 | 50.8 | 2 | True | persons: names empty; activism FP: 'demand' registration rights |
| 19 | 0001214659-20-002622 | comScore, Inc. | 20564W105 | 3,950,000 | 5.6 | 4 | False | persons: names cut; Item 4 excerpt empty (flag still right) |
| 20 | 0001193125-21-189246 | The Original BARK Company | 68622E104 | 6,108,750 | 3.7 | 3 | False | all correct |
| 21 | 0001193125-22-046711 | Quanergy Systems, Inc. | 74764U104 | 24,602,394 | 27.1 | 1 | False | all correct |
| 22 | 0001193125-21-328063 | Entrada Therapeutics, Inc. | 29384C108 | 4,427,092 | 14.8 | 14 | False | all correct |
| 23 | 0001193125-23-184169 | Abacus Life, Inc. | None | 13,293,750 | 21.0 | 1 | False | all correct |
| 24 | 0001193125-21-337691 | Telesat Corporation | 879512309 | 18,096,228 | 36.5 | 12 | False | all correct |
| 25 | 0001493152-23-003058 | Motorsport Games Inc. | 62011B102 | 1,038,983 | 61.2 | 2 | True | persons: names cut; activism FP: 'demand' registration rights |
| 26 | 0001013594-19-000276 | Adverum Biotechnologies, Inc. | 00773U108 | 3,495,566 | 5.5 | 2 | True | activism FP: 'nominating procedures' in reservation |
| 27 | 0001567619-19-007560 | BlackRock New York Municipal Income Qu | 09249U303 | 405 | 100.0 | 2 | False | persons: caption text instead of names |
| 28 | 0000914121-22-002824 | ADAGIO THERAPEUTICS, INC. | 00534A102 | 1,201,680 | 62.0 | 6 | True | percent: '.33%' / '.62%' read as 33 / 62 (max 62 vs 1.1) |
| 29 | 0001104659-19-029719 | PATHFINDER BANCORP, INC. | 70319R109 | 464,710 | 9.9 | 4 | False | activism FN: board-appointment right |
| 30 | 0001193125-23-010244 | Vitru Limited | G9440D103 | 3,852,266 | 11.5 | 5 | False | persons: names null; activism FN: board seats under investment agreement |
| 31 | 0001193125-21-245834 | Hippo Holdings Inc. | G74847107 | 41,492,841 | 7.3 | 1 | False | all correct |
| 32 | 0000950170-23-027168 | GreenLight Biosciences Holdings, PBC | 39536G105 | 9,984 | 0.01 | 1 | True | all correct |
| 33 | 0001104659-19-035707 | Myovant Sciences Ltd. | G637AM102 | 40,765,599 | 45.6 | 1 | False | persons: name null |
| 34 | 0001104659-22-010232 | Rani Therapeutics Holdings, Inc. | 753018100 | 5,202,298 | 26.39 | 2 | False | all correct |
| 35 | 0001474506-19-000056 | FRP HOLDINGS, INC. | 30292L107 | 1,387,622 | 13.9 | 3 | True | all correct |
| 36 | 0001140361-22-019435 | Twitter, Inc. | 90184L102 | 60,048 | 0.008 | 1 | False | activism FN: equity commitment for take-private |
| 37 | 0001193125-23-110738 | Cerevel Therapeutics Holdings, Inc. | 15678U128 | 10,088,385 | 6.4 | 3 | False | activism FN (borderline): preliminary talks on strategic transaction |
| 38 | 0001062993-19-001279 | Western Asset High Income Fund II, Inc | 95766J102 | 10,477,665 | 12.21 | 2 | False | all correct |
| 39 | 0000807249-22-000074 | Dril-Quip, Inc. | 262037104 | 1,522,044 | 4.41 | 9 | False | all correct |
| 40 | 0001011438-22-000280 | Coupa Software Incorporated | 22266L106 | 4,381,483 | 5.8 | 3 | False | persons: names null; activism FN: unsolicited sale views sent |
| 41 | 0001193125-23-209204 | Electriq Power Holdings, Inc. | 285046108 | 6,159,352 | 15.7 | 2 | True | activism not judgeable (excerpt cut at 4000 chars) |
| 42 | 0001213900-20-038943 | Triterras, Inc. | G6455A107 | 51,622,419 | 62.6 | 3 | False | persons: names null |
| 43 | 0001193125-21-322942 | AltEnergy Acquisition Corp. | 02157M108 | 5,750,000 | 20.0 | 2 | True | persons: names null; activism FP: 'demand' (SPAC sponsor) |
| 44 | 0001193125-22-031205 | System1, Inc. | 87200P109 | 29,875,499 | 29.8 | 3 | False | all correct |
| 45 | 0001564590-20-046531 | Arcus Biosciences, Inc. | 03969F109 | 1,894,967 | 2.92 | 3 | False | persons: names '.' |
| 46 | 0001587987-22-000002 | NEWTEK BUSINESS SERVICES CORP. | 652526203 | 1,160,037 | 4.8 | 1 | False | all correct |
| 47 | 0001493152-23-004408 | AiXin Life International, Inc. | 009603200 | 3,768,673 | None | 1 | False | persons: name cut; percent printed without '%' parsed null |
| 48 | 0001193125-19-304450 | Alpine Income Property Trust, Inc. | 02083X103 | 2,039,644 | 22.4 | 1 | False | all correct |
| 49 | 0001753926-20-000332 | AMARANTUS BIOSCIENCE HOLDINGS, INC. | 02300U205 | 14,176,424 | 4.99 | 2 | False | persons: names cut; activism FN: letter alleging breaches |
| 50 | 0001123292-22-000071 | GreenLight BioSciences Holdings, PBC | 39536G105 | 8,901,814 | 7.3 | 4 | False | all correct |
| 51 | 0001104659-20-058870 | NGL Energy Partners LP | 62913M107 | 11,687,500 | 8.0 | 1 | True | all correct |
| 52 | 0001193125-21-240727 | DA32 Life Science Tech Acquisition Cor | 23312M106 | 6,115,000 | 23.8 | 3 | True | activism FP: 'demand' (SPAC sponsor) |
| 53 | 0001193125-21-284201 | Vicarious Surgical Inc. | 92561V109 | 20,956,122 | 21.2 | 6 | False | persons: names null (label wraps) |
| 54 | 0001104659-19-045201 | Milacron Holdings Corp. | 59870L106 | 4,253,315 | 6.03 | 4 | False | persons: names null (label with colon) |
| 55 | 0001493152-22-007681 | Yacht Finders, Inc. | None | 3,404,800 | 65.5 | 2 | False | cusip: 'U98424 101' split by line break -> null; persons: names cut; activism FN: control purchase + board |
| 56 | 0000905148-19-000718 | FEDNAT HOLDING COMPANY | 31431B109 | 839,651 | 6.5 | 2 | True | all correct |
| 57 | 0001213900-22-081916 | iClick Interactive Asia Group Limited | G47048106 | 2,564,103 | 5.7 | 2 | True | persons: names null (label with colon) |
| 58 | 0001193125-20-001517 | Myovant Sciences Ltd. | G637AM102 | 45,008,604 | 50.2 | 3 | True | all correct |
| 59 | 0000898432-23-000629 | ARCO PLATFORM LIMITED | G04553106 | 4,735,455 | 12.16 | 4 | False | activism FN: rollover/support agreement for take-private |
| 60 | 0001193125-21-120962 | RENEO PHARMACEUTICALS, INC. | 75974E103 | 2,409,220 | 10.0 | 12 | False | persons: label text instead of names |
| 61 | 0001104659-22-038127 | MiX Telematics Limited | 60688N102 | 33,761,850 | 6.11 | 3 | False | persons: names null |
| 62 | 0001915593-22-000001 | Tax Free Target Maturity Fund for Puer | 87677T105 | 1,201,916 | 5.0 | 2 | False | all correct |
| 63 | 0001193805-23-000094 | Rogers Corporation | 775133101 | 1,222,000 | 6.5 | 18 | True | persons: names cut |
| 64 | 0001213900-22-058973 | Rumble Inc. | 78137L105 | 140,182,173 | 44.6 | 1 | False | all correct |
| 65 | 0001104659-21-124586 | GENERATIONS BANCORP NY, INC. | 37149G108 | 243,606 | 9.91 | 5 | True | persons: label text; activism FP: history of other campaigns |
| 66 | 0001144204-19-027109 | Zayo Group Holdings, Inc. | 98919V105 | 25,000 | 0.1 | 2 | True | all correct |
| 67 | 0001193125-21-253941 | Eliem Therapeutics, Inc. | 28658R106 | 5,009,400 | 19.8 | 4 | False | persons: names null |
| 68 | 0001731122-23-001969 | WIDEPOINT CORPORATION | 967590209 | 405,200 | 4.31 | 2 | True | all correct |
| 69 | 0001437749-20-024764 | CKX Lands, Inc. | 12562N104 | 104,197 | 5.4 | 1 | False | all correct |
| 70 | 0001193125-23-050791 | Owlet, Inc. | 69120X107 | 7,473,415 | 6.3 | 1 | True | persons: name null; activism FP: 'strategic alternatives' in reservation |
| 71 | 0001551163-19-000184 | Prevention Insurance.com | 741375208 | 1,563,809 | 70.0 | 3 | False | persons: label text |
| 72 | 0001104659-20-041176 | Rezolute, Inc. | 037230208 | 91,300,933 | 31.1 | 1 | False | activism not judgeable (no Item 4 found) |
| 73 | 0000895345-19-000016 | Nuverra Environmental Solutions, Inc. | 67091K302 | 7,021,879 | 44.97 | 11 | False | activism FN: board designation rights |
| 74 | 0001104659-19-014879 | Antero Midstream Corporation | 03676B102 | 107,000,001 | 21.1 | 3 | False | all correct |
| 75 | 0001062993-23-021192 | NEUBERGER BERMAN MUNICIPAL FUND INC | 64124P101 | 1,555,834 | 5.25 | 3 | True | activism FP: reservation language |
| 76 | 0001013762-23-001590 | Bridgetown Holdings Limited | G1355U113 | 1,600,000 | 5.3 | 1 | True | persons: name null |
| 77 | 0001628280-21-023405 | Nextdoor Holdings, Inc. | 65345M108 | 24,185,310 | 23.45 | 5 | False | persons: names empty |
| 78 | 0001213900-21-067087 | Bluejay Diagnostics, Inc. | 095633103 | 4,540,148 | 35.76 | 3 | False | all correct |
| 79 | 0001096906-22-002073 | AXIM Biotechnologies, Inc. | 05463J107 | 19,800,000 | 14.34 | 1 | False | all correct |
| 80 | 0000929638-22-000896 | Trinseo PLC | G9059U107 | 7,628,044 | 21.1 | 3 | False | persons: caption text |
| 81 | 0000950142-23-002836 | Mallinckrodt plc | None | 1,305,403 | 6.6 | 3 | True | all correct |
| 82 | 0001398344-20-019795 | Gulfport energy corpORATION | 402635304 | 13,498,459 | 8.43 | 3 | False | all correct |
| 83 | 0001193125-21-297019 | Velo3D, Inc. | 92259N104 | 22,874,407 | 12.5 | 4 | False | persons: names null |
| 84 | 0000921895-20-001723 | EATON VANCE SENIOR INCOME TRUST | 27826S103 | 6,009,697 | 15.9 | 3 | True | all correct |
| 85 | 0000919574-20-004612 | Tenax Therapeutics, Inc. | 88032L209 | 2,773,455 | 19.99 | 3 | False | activism FN: director designation rights |
| 86 | 0001072613-19-000121 | Sienna Biopharmaceuticals, Inc. | 82622H108 | 5,810,850 | 19.9 | 9 | False | all correct |
| 87 | 0001104659-22-001308 | IBEX LIMITED | G4690M101 | 2,019,739 | 10.97 | 5 | False | persons: names null |
| 88 | 0001161697-19-000398 | Parks! America, Inc. | 701455107 | 5,547,466 | 7.4 | 1 | False | all correct |
| 89 | 0001104659-21-069853 | AMREP Corporation | 032159105 | 519,782 | 7.1 | 2 | False | persons: name cut |
| 90 | 0001477932-19-003691 | Loop Industries, Inc. | 543518104 | 4,093,567 | 10.5 | 2 | False | all correct |
| 91 | 0001104659-19-076991 | Indonesia Energy Corporation Limited | G4760X102 | 5,222,222 | 70.93 | 2 | False | persons: names '.' |
| 92 | 0001062993-23-020160 | EATON VANCE CALIFORNIA MUNICIPAL INCOM | 27826F101 | 792,497 | 11.27 | 3 | True | activism FP: reservation language |
| 93 | 0001011438-23-000577 | ProKidney Corp. | G7S53R104 | 89,388,913 | 38.0 | 2 | False | all correct |
| 94 | 0001140361-20-005768 | Transphorm, Inc. | None | 21,175,980 | 60.2 | 8 | False | all correct |
| 95 | 0001437749-23-027753 | GLASSBRIDGE ENTERPRISES, INC. | 377185202 | 7,578 | 30.0 | 3 | False | activism FN: director designation |
| 96 | 0001193125-21-043391 | Q&K International Group Limited | G7308L100 | 433,814,924 | 31.6 | 7 | False | all correct |
| 97 | 0001193805-20-000442 | Bed Bath & Beyond Inc. | 075896100 | 6,869,562 | 5.41 | 8 | True | all correct |
| 98 | 0001213900-19-021818 | DOCUMENT SECURITY SYSTEMS, INC. | 25614T200 | 1,038,304 | 3.5 | 3 | True | all correct |
| 99 | 0001140361-20-015058 | Royalty Pharma plc | G7709Q104 | 46,316,170 | 12.7 | 8 | False | all correct |
| 100 | 0001341004-23-000222 | Better Home & Finance Holding Company | 08774B102 | 52,846,441 | 36.7 | 3 | True | persons: names null; activism FP: 'tender offer' in reservation |

## 7. S5.5 Reg SHO threshold-list completeness (read-only check of `regsho_threshold/`)

Measured with pyarrow over `regsho_threshold/lists.parquet` (13,074 list-days) and `year=*/threshold.parquet`
against `calendar.parquet` (sessions to 2026-09-18). Suspicious empty = an empty list on a session where some symbol
is on the list on both neighbouring non-empty sessions within 3 sessions; 0 everywhere.

Findings:

- All 5 listing markets (Nasdaq, NYSE, NYSE American, NYSE Arca, Cboe BZX) and FINRA OTC have a list for every
  session 2018-01-02 .. 2026-09-25.
- The only gaps are Columbus Day and Veterans Day each year. These are equity sessions but bank holidays with no
  settlement, so no market publishes a list. The gap is structural, not missing data.
- The frequent empty lists on NYSE (2-77 a year) and NYSE American (5-129 a year) never break symbol continuity,
  so they are genuine empty lists.
- `vintage_risk` is set on every non-Nasdaq list (derived clock; Cboe 2026 partly has Last-Modified).
- On-list rows map to `security_id` at 95.5-100%, except NYSE 2018 at 91.6%. FINRA OTC maps at 0-4%: OTC names are
  outside the vendor universe, by design.
- The calendar ends 2026-09-18, while the lists run to 2026-09-25 (5 later list days per market).

| market | year | sessions | lists | missing (dates) | empty lists | suspicious empty | median rows | on-list rows mapped | vintage_risk lists |
|---|---|---:|---:|---|---:|---:|---:|---:|---:|
| nasdaq | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 0 | 0 | 17 | 0.9744 | 0 |
| nasdaq | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 0 | 0 | 16.0 | 0.9813 | 0 |
| nasdaq | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 0 | 0 | 19 | 0.9837 | 0 |
| nasdaq | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 0 | 0 | 24.0 | 0.9943 | 0 |
| nasdaq | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 0 | 0 | 19 | 0.9721 | 0 |
| nasdaq | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 0 | 0 | 19.0 | 0.9554 | 0 |
| nasdaq | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 0 | 0 | 32.0 | 0.9796 | 0 |
| nasdaq | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 0 | 0 | 44.0 | 0.9635 | 0 |
| nasdaq | 2026 | 179 | 179 | 0 | 0 | 0 | 63 | 0.9845 | 0 |
| nyse | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 61 | 0 | 1 | 0.9156 | 249 |
| nyse | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 77 | 0 | 1.0 | 0.9849 | 250 |
| nyse | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 2 | 0 | 5 | 0.9926 | 251 |
| nyse | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 9 | 0 | 4.0 | 1.0 | 250 |
| nyse | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 30 | 0 | 2 | 1.0 | 249 |
| nyse | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 45 | 0 | 2.0 | 1.0 | 248 |
| nyse | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 50 | 0 | 1.0 | 0.9798 | 250 |
| nyse | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 48 | 0 | 1.0 | 1.0 | 248 |
| nyse | 2026 | 179 | 179 | 0 | 46 | 0 | 1 | 1.0 | 179 |
| nyse_american | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 96 | 0 | 1 | 1.0 | 249 |
| nyse_american | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 85 | 0 | 1.0 | 1.0 | 250 |
| nyse_american | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 47 | 0 | 1 | 1.0 | 251 |
| nyse_american | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 55 | 0 | 1.0 | 1.0 | 250 |
| nyse_american | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 129 | 0 | 0 | 1.0 | 249 |
| nyse_american | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 129 | 0 | 0.0 | 0.9934 | 248 |
| nyse_american | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 58 | 0 | 2.0 | 0.9952 | 250 |
| nyse_american | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 22 | 0 | 2.0 | 1.0 | 248 |
| nyse_american | 2026 | 179 | 179 | 0 | 5 | 0 | 2 | 1.0 | 179 |
| nyse_arca | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 0 | 0 | 20 | 0.9961 | 249 |
| nyse_arca | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 0 | 0 | 23.0 | 1.0 | 250 |
| nyse_arca | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 0 | 0 | 26 | 0.9989 | 251 |
| nyse_arca | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 0 | 0 | 19.0 | 1.0 | 250 |
| nyse_arca | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 0 | 0 | 23 | 1.0 | 249 |
| nyse_arca | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 0 | 0 | 11.0 | 1.0 | 248 |
| nyse_arca | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 0 | 0 | 11.0 | 0.9963 | 250 |
| nyse_arca | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 0 | 0 | 11.0 | 1.0 | 248 |
| nyse_arca | 2026 | 179 | 179 | 0 | 0 | 0 | 15 | 1.0 | 179 |
| cboe_bzx | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 11 | 0 | 3 | 1.0 | 249 |
| cboe_bzx | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 0 | 0 | 6.0 | 1.0 | 250 |
| cboe_bzx | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 0 | 0 | 11 | 0.9997 | 251 |
| cboe_bzx | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 0 | 0 | 6.0 | 1.0 | 250 |
| cboe_bzx | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 0 | 0 | 8 | 1.0 | 249 |
| cboe_bzx | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 0 | 0 | 6.0 | 1.0 | 248 |
| cboe_bzx | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 0 | 0 | 6.0 | 0.9921 | 250 |
| cboe_bzx | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 0 | 0 | 10.0 | 1.0 | 248 |
| cboe_bzx | 2026 | 179 | 179 | 0 | 0 | 0 | 25 | 1.0 | 61 |
| finra_otc | 2018 | 251 | 249 | 2018-10-08, 2018-11-12 | 17 | 0 | 3 | 0.0 | 249 |
| finra_otc | 2019 | 252 | 250 | 2019-10-14, 2019-11-11 | 85 | 0 | 1.0 | 0.0 | 250 |
| finra_otc | 2020 | 253 | 251 | 2020-10-12, 2020-11-11 | 35 | 0 | 1 | 0.0 | 251 |
| finra_otc | 2021 | 252 | 250 | 2021-10-11, 2021-11-11 | 107 | 0 | 1.0 | 0.0 | 250 |
| finra_otc | 2022 | 251 | 249 | 2022-10-10, 2022-11-11 | 157 | 0 | 0 | 0.0 | 249 |
| finra_otc | 2023 | 250 | 248 | 2023-10-09, 2023-11-10 | 150 | 0 | 0.0 | 0.0085 | 248 |
| finra_otc | 2024 | 252 | 250 | 2024-10-14, 2024-11-11 | 169 | 0 | 0.0 | 0.0288 | 250 |
| finra_otc | 2025 | 250 | 248 | 2025-10-13, 2025-11-11 | 195 | 0 | 0.0 | 0.0441 | 248 |
| finra_otc | 2026 | 179 | 179 | 0 | 69 | 0 | 1 | 0.0 | 179 |


## 8. Still running at the stop (owner action needed)

Stopping these was refused twice by the auto-mode classifier. They are resumable and bounded by `--max-new`. They
make only SEC GETs through the shared limiter and write only to `data/raw/sec_13dg/`.

| pid (venv launcher / worker) | command | log |
|---|---|---|
| 19884 / 17224 | `python -m atx_db.alpha_panel.stakes fetch --phase xml13d --phase xml13g --max-new 35000 --threads 2` | `data/alpha_panel/v1/_logs/own_stakes_fetch_xml.log` |
| 3996 / 21832 | `python -m atx_db.alpha_panel.stakes fetch --phase text13d --max-new 7400 --threads 2` | `data/alpha_panel/v1/_logs/own_stakes_fetch_text_early.log` |

To stop: `Stop-Process -Id 17224,19884,21832,3996`. A kill mid-request is safe: the fetch-ledger append is locked
and fsynced, and object temp files are swept on the next start.

Everything else I started has stopped:
- chain, waiters and the N-PORT retry loop (TaskStop);
- the orphaned queued N-PORT guard 1904 / 27912 (stopped);
- the ADV roster landing (finished);
- `insider_measures` (finished).

## 9. Resume instructions (in order)

Run all commands from `C:/atx/atx-db` with
`PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1 ATX_SEC_RATE_LOG=C:/atx/atx-db/data/raw/own_sec_rate_log.jsonl`.
Guard prefix: `.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb <X> --wait-minutes 900 --`.
Budget left: 60,000 minus `wc -l data/raw/own_sec_rate_log.jsonl`.

1. **S5.1.** Run `<guard 0.55> .venv/Scripts/python.exe -m atx_db.alpha_panel.filer_type build`, then read
   `coverage` in `thirteenf_filer_type/manifest.json`. Target: `typed_value_share` >= 0.90 every quarter 2019q1+.
   If a quarter misses, list its largest unclassified filers and extend the rules. Candidate misses: holding
   companies without OTHERMANAGER2, and market makers.
2. **S5.2.**
   - Run `<guard 0.6> .venv/Scripts/python.exe -m atx_db.alpha_panel.nport fetch`. It resumes at 2023q4; 11
     quarters remain, about 11 x 0.4 GB of ranges, one quarter on disk at a time.
   - Then run `<guard 0.6> ... -m atx_db.alpha_panel.nport build` and check `coverage_by_year` (target >= 0.95 of
     member_equity cells 2020+).
   - The build reads `panel/`; run it when no panel assemble is writing.
   - If the host is short of commit, use the retry wrapper: `scratchpad/own/retry_guarded.sh` in this session's
     scratch, or re-create it; it retries only on `stop_reason: low_commit`.
3. **S5.3 landings.** Resume the unguarded landings:
   - `stakes fetch --phase xml13g --max-new <n>` (needed for the 2025 13G criterion);
   - `stakes fetch --phase text13d --max-new 7400`.
4. **S5.3 parser fixes** (text 13D), with tests, before rebuilding:
   - Person names: accept `NAME(S) OF REPORTING PERSON(S)` labels followed by `:` or `.` or wrapped over two lines.
     Skip the `I.R.S. IDENTIFICATION NO(S). OF ABOVE PERSON(S) (ENTITIES ONLY)` caption. Join continuation lines up
     to row 2 (`CHECK THE APPROPRIATE BOX`).
   - Percent: accept `.33%` (leading dot), a row-13 cell without `%`, and `Less than 1%`.
   - CUSIP: join a CUSIP split by a line break (`U98424 101`).
   - Activism:
     - drop the terms `demand` and `strategic alternatives` unless they sit outside a may / could / reserve
       sentence, and do not match `nominat` inside "Nominating ... Committee";
     - add board designation / appointment rights, rollover / support / voting agreements, equity commitment,
       transaction support agreement and "letter to management".
   - Then re-run the 100-case check with `handcheck_sample.py` (same seed) on the changed fields.
5. **S5.3 rebuild.** Run `.venv/Scripts/python.exe -m atx_db.alpha_panel.stakes build`, guarded, or unguarded if
   the measured peak is <= 0.25 GiB. Check `coverage_by_year_form` for 2025 (target parse_ok >= 0.95 of notices for
   SCHEDULE 13D/13D/A/13G/13G/A).
6. **S5.4.**
   - Run `form144 fetch --max-new <remaining budget>` (newest first), then
     `.venv/Scripts/python.exe -m atx_db.alpha_panel.form144 notices` (pyarrow only).
   - Then re-run `<guard 0.55> -m atx_db.alpha_panel.insider_measures` so `form144_monthly.parquet` is built.
   - Add a price sanity rule to `TRADES_SQL`: value NULL when the price is more than 5x or less than 0.2x the
     issuer's panel close near the trade date. The current monthly `net_value` sums are dominated by outliers, e.g.
     2021 net value 1.9e16 USD.
7. Copy the lane receipts into `data/archive/receipts/` per ruling D7:
   - `data/raw/sec_adv/receipts.jsonl`
   - `sec_adv/thirteenf_cover/receipts.jsonl`
   - `sec_nport/receipts.jsonl`
   - `sec_13dg/fetch-ledger.jsonl`
   - `sec_144/fetch-ledger.jsonl`
8. Fill the "Measured" section of `atx-db/docs/ALPHA_PANEL_OWNERSHIP.md` from the manifests.

## 10. Deviations and open issues

**Deviations.**
- Extra module `sec_docs.py`, shared by the lane's SEC document fetchers and zip range reads.
- `filer_subtype` is a finer taxonomy on top of the 7 required types.
- ADV rosters are kept only for Feb / May / Aug / Nov from 2020-02, the months in force at each 13F deadline.
  2018-10 .. 2020-01 are monthly. This cut the landing time about 3x.
- Bulk ADV Schedule D evidence is a backfill clocked at the bulk file's Last-Modified (`vintage_risk
  adv_bulk_backfill`). The `*_pit` columns exclude it.
- The EDGAR SIC is a 2026-09-19 snapshot (`vintage_risk sic_snapshot`).

**Found.**
- `pyarrow.dataset` scans with `use_threads=False` stalled for over 10 minutes on the 18.6M-row
  `sec_filings/filings.parquet`, while plain row-group reads take 0.1 s each.
  `sec_docs.scan_row_groups` / `read_filtered` replace them (`select_filings` 22 s, 13D/G notices 10 s, pool 0.13 GB).

**Budget.**
- Not fetched: 13G 2026 (23,090 documents), SC 13D/A 2019-2024 (25,120), Form 144 documents (126,291 structured
  notices).
- The stakes trial build published `stakes/` from partial landings; it must be rebuilt (step 5).
- `insider_trades` includes a handful of trades with transaction years 2027-2033 (typos in the source). They are
  kept, labelled by transaction year.

## 11. Ledger candidates

- On a 16 GB host, the guard stops jobs on host low commit (< 0.75 GB free) even under their own cap. N-PORT fetch
  was stopped 3 times and `filer_type values` once. Use a retry wrapper keyed on `stop_reason: low_commit`.
- `pyarrow.dataset(...).to_table(..., use_threads=False)` stalls on the 18.6M-row `sec_filings/filings.parquet`;
  use `ParquetFile.read_row_group` loops (`sec_docs.scan_row_groups`).
- Live EDGAR Schedule 13G XML carries the CUSIP in `<issuerCusip>`, not `<issuerCusipNumber>`.
