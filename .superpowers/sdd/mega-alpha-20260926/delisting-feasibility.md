# T33a: delisting-return feasibility (read-only explorer)

**Verdict: NO-GO for T33b/T32 as briefed.** The pre-registered gate is ≥ 80% of TRAIN true terminations classifiable as
mna+performance. Measured: **59.5%** under the brief's literal rule and **59.9%** under the recommended rule R (§3). The
filing rule works: **96.6% / 97.2% of bridge-linked terminations** are classified. The gap is identity. **203 of 529 TRAIN
true terminations (38%) have no CIK in the pinned bridge** `identity-bridge-r4-v1`. Recommendation: park the lane and raise
U3 with the two options in §8.

Conduct: every input was read-only. The warehouse was opened once per script with
`duckdb.connect(path, read_only=True, config={"memory_limit":"384MB","threads":2})` and it **opened without a lock error**,
so no parquet fallback was needed. No retries, no writes, no builds, no bounded runner, no subagents. VAL was used for
**counts only**. VAL TickerHistory rows were read only for ticker continuity. No VAL price or return was computed or
reported. Scratch scripts: `C:/Users/natha/AppData/Local/Temp/claude/t33a/` (`stage_a.py`, `stage_c.py`, `stage_d.py`,
`classify.py`, `rule_r.py`, `fix_lc.py`, `final.py`, `pit.py`). Pool-2 HEAD at read time: `c8192c46`.

## 1. Inputs (pinned)

| input | identity |
|---|---|
| TRAIN role | `build-equity/recent-fast-train-2020-2022-v2`, manifest sha256 `210fff9687aa…` (1155 sessions × 5627 lines; score 2020-01-02..2022-12-30) |
| VAL role (counts only) | `build-equity/recent-fast-validation-2023-2024-v1`, manifest `0c757c41a363…` (903 × 5048) |
| bridge | `build-equity/identity-bridge-r4-v1`, manifest `ddf9716459a1…`, `links.parquet` sha `baca826e…` (sr_id, cik int64, start, end_incl, primary P/J) |
| filings | `C:/atx/atx-db/data/warehouse.duckdb` `main.sec_submissions`: 10,297,910 rows, 72,810 CIKs, run_ids `04cf947d…` / `d4baf9b1…`, loaded 2026-09-20 (`acceptance_datetime` = naive UTC) |
| ticker continuity | `C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet` (read with an in-memory DuckDB, 384 MB / 2 threads) |
| NAV holding check | `build-equity/mega-nav-train-v6-c1/events_modeled-1bn-stale5-v1.csv` (`kind=write-off` rows) |

**Role layout.** Each role is read as follows:
- `sessions.i64` holds midnight-ns labels and `ids.u64` holds ascending securityIDs.
- `present.u8` and `member.u8` are **date-major** u8 arrays of shape `[dates, instruments]`. `prepare_research_fields.py:538`
  reshapes them `(n_dates, n)`.
- `L_j` is the last present session: the largest `t` with `present[t,j] = 1`.
- A line counts as **terminated** when all three hold: it is a member somewhere in `[score_begin, score_end)`, `L_j` is before
  the role's last session, and `L_j >= score_begin`.

## 2. Exact queries

Terminations and CIKs are computed in pandas (`stage_a.py`). The CIK is the bridge row with `sr_id = j` and
`start <= L_j <= end_incl`. The r4 bridge is already point-in-time: `available_at <= start 22:00`. No covering row means the
line is **unlinked**.

The filings query (`stage_c.py`) runs against the read-only warehouse, with `term(role, sr_id, cik10, last_present)`
registered as a pandas relation:

```sql
SELECT DISTINCT t.role, t.sr_id, t.cik10, CAST(t.last_present AS DATE) AS last_present,
       s.accession_number, s.form, s.items, s.filing_date, s.acceptance_datetime,
       date_diff('day', CAST(t.last_present AS DATE), s.filing_date) AS dd
FROM sec_submissions s
JOIN term t ON lpad(trim(s.cik), 10, '0') = t.cik10
WHERE s.filing_date BETWEEN CAST(t.last_present AS DATE) - INTERVAL 365 DAY
                        AND CAST(t.last_present AS DATE) + INTERVAL 400 DAY;
-- venue evidence: Form 25-NSE filer agent = first 10 digits of the accession number
SELECT left(accession_number,10) AS filer, count(DISTINCT accession_number) FROM sec_submissions
WHERE form='25-NSE' AND filing_date BETWEEN DATE '2019-01-01' AND DATE '2025-12-31' GROUP BY 1 ORDER BY 2 DESC;
```

Notes on the query:
- `DISTINCT` is needed because `sec_submissions` repeats an accession across co-registrant CIKs.
- 8-K item tests match whole item tokens: `re.search(r"(^|[^0-9.])" + re.escape(item) + r"([^0-9]|$)", items)`.
- The window `dd` is measured in calendar days on `filing_date`.

Ticker continuity (`stage_d.py`) runs over the TickerHistory3 parquet. `tk` holds each line's last `ticker_tk` on or before
`L`:

```sql
SELECT tk.sr_id, min(p.securityID) other_sid, min(p.tradingDate) other_first
FROM read_parquet('…/TickerHistory3.parquet') p JOIN tk ON p.ticker_tk = tk.th_last_ticker
WHERE p.tradingDate > tk.last_present AND p.tradingDate <= tk.last_present + INTERVAL 30 DAY
  AND p.securityID <> tk.sr_id AND p.securityID <> 0 GROUP BY 1;
```

`securityID = 0` rows are vendor ticker placeholders, so they are excluded.

## 3. Classification rules

**Rule S (the brief, literal).** Window `[L−60, L+30]`. Precedence, first match wins:
1. 8-K Item 1.03 → performance (bankruptcy overlay, same precedence as `atx_db/delisting_evidence.py`).
2. 8-K Item 2.01 → mna.
3. 8-K 3.01, Form 25 or Form 25-NSE → performance.
4. Otherwise → unknown.

**Rule R (recommended).** Same precedence, with these changes:
- **mna** also accepts 8-K Item **5.01** (change in control). This is how "acquirer-named 8-K" is implemented.
- **mna** also accepts a target-side merger form (`DEFM14A, DEFM14C, PREM14A, PREM14C, SC 14D9, SC TO-T, SC 13E3, 425`) in
  `[L−365, L+30]`.
- **A bare 25-NSE is unknown, not performance.**

Why the change: a 25-NSE and Item 3.01 (usually with 3.03 and 5.01) are the mechanics of *every* exit, M&A and holdco
reorganizations included. Rule S labels these M&A cases as performance: LDL, OSB (Norbord → West Fraser), WAIR, GCAP, AKCA,
FRT. atx-db reached the same conclusion: "a bare Form 25 … explains that the security left, not why". A 20-name spot check
of rule-R mna found 20/20 real acquisitions (TWTR, NUAN, PFPT, LM, TCO, CUB, …).

**Line continuation (link-end, not a delisting).** Any one of these marks it:
- the same ticker appears under a new nonzero securityID within 30 days;
- vendor rows for the same securityID resume within 30 days;
- a same-CIK bridge successor line is present after `L`.

A continued line is not a terminal event unless an 8-K 1.03 falls in the window (WLL, ALF).

## 4. TRAIN results (585 member lines whose presence ends in 2020-01-02..2022-12-29)

| class | rule S | rule R | R, held subset¹ |
|---|---:|---:|---:|
| link-end-not-delisting (line continues) | 56 | 56 | 40 |
| mna | 283 | 292 | 255 |
| performance | 32 | 25 | 3 |
| unknown, linked (no qualifying filing) | 11 | 9 | 7 |
| unknown, unlinked (no bridge CIK) | 203 | 203 | 109 |
| **true terminations** (all minus link-end) | 529 | 529 | 374 |
| **classifiable share (mna+perf)/true** | **59.5%** | **59.9%** | **69.0%** |
| classifiable share, bridge-linked only | 96.6% (315/326) | 97.2% (317/326) | 97.4% (258/265²) |

¹ "Held" means the line has a `write-off` event in the v6-c1 stale5 NAV reference: 414 of the 585 lines (across all 419
write-off rows, 290 long and 129 short). This is the population T32 actually changes.
² 265 = 374 − 109.

Other breakdowns:
- By year: mna 80 / 97 / 115 and performance 11 / 4 / 10 for 2020 / 2021 / 2022.
- Rule-R performance is almost all bankruptcy (1.03): BGG, CHK, SSI, VAL, I, MNK, CRC, HTZ, WPG, CLVS, CORZ, ENDP, GTX, …
  The rest are 3.01 notices (REV, PEI, ODT, SCPS, EVK, BPYU; EVK and BPYU are doubtful).
- Linked unknowns (9) are mostly foreign private issuers that file no 8-K: TRQ, SINA, CBPO, TGP, JE, GLOG, BPY, OSB. Seven of
  them have **no or truncated `sec_submissions` coverage**: TRQ, TGP, GLOG and BPY have 0 rows; SINA's rows stop in 2007 and
  CBPO's in 2017; JE's start only in 2022-08. This is a coverage defect in the live table (322/326 linked true-termination
  CIKs have any rows). The ninth is ISNS (Nasdaq → OTC, still filing 10-Qs).

**VAL (counts only, rule R):** 323 lines break down as link-end 35, mna 152, performance 29, linked-unknown 13 and unlinked
94. That is 288 true terminations with a 62.8% classifiable share (93.3% of linked lines).

## 5. Link-end vs true delisting (how T33b tells them apart)

The 56 continued lines are almost all reorganizations or redomiciles that keep the ticker under a new securityID:
- holdco reorganizations and redomiciles: APA, AZPN, CR, FCFS, WELL, J, SPXC, STX, MRVL, AON, KKR (+ KKR.PRC), DKNG, APO, TPL,
  IAC, ZI ×2, BNL, BRBR, …
- SPAC unit → share separations: AJAX.U.

Of the 56, **15 carry mna evidence**. Some are holdco mergers (MRVL, AZPN, NCNO, NE, …). In others the acquirer took the
ticker: CADE, PRMW, HR, GRUB, BHVN (spin plus Pfizer). For the old holders, these are acquisitions.

For η purposes both kinds pay **η = 0**:
- in a reorganization, holders receive the successor shares one for one;
- in an acquisition, the last close stands.

So T33b needs no finer split. Rule: continued line → no terminal event, unless 8-K 1.03 → performance (WLL: old equity
wiped).

**The CIK-only test the controller suggested does not work.** "The company keeps filing 10-Q/10-K after `L`" cannot separate
a line change from a performance delisting to OTC, because OTC names keep filing (ISNS). Continuity must come from a
ticker/security source, not from EDGAR alone. The bridge-successor test found 0 cases: the r4 bridge carries almost no
same-CIK successor lines.

## 6. Venue availability

`exchange_listings` has 45,820 rows (sources `tbltickerhistory3_10y`, `SEC company_tickers`); **all have
`exchange_code IS NULL AND mic IS NULL`: 0 venue rows (confirmed).** TickerHistory3 has no exchange column;
`sec_company_tickers` and the submissions JSON `exchanges` are current-only and empty for delisted CIKs.

**The only historical venue source found is the Form 25-NSE filer agent** (the accession prefix):

| prefix | agent | venue |
|---|---|---|
| `0000876661` | NEW YORK STOCK EXCHANGE LLC | nyse |
| `0001354457` | Nasdaq Stock Market LLC | nasdaq |
| `0001143313` | NYSE AMERICAN LLC | amex |
| `0001143362` | NYSE ARCA | unknown for η |
| `0001417835` | Cboe BZX | unknown for η |

Coverage among TRAIN true terminations: nyse 43, nasdaq 37, none 246. Among the 25 rule-R performance cases it is nyse 6,
unknown 19.

**So venue = unknown (η −0.35) for most performance and unknown cases.** The 25-NSE venue is an optional refinement.
Caution: across 2019-2025 Nasdaq-agent 25-NSE rows are only 284 against NYSE's 2,773, so Nasdaq venue coverage looks
incomplete in this table.

## 7. OTC continuation evidence (TickerHistory3)

**None usable.** TickerHistory3 is SpiderRock's optionable listed universe, so OTC trading after delisting does not appear.
6 of the 585 lines have vendor rows after `L`. Only FIHD and RGC resume within 30 days (role-edge gaps in late December 2022,
counted as link-end). LTM (LATAM, NYSE → OTC in bankruptcy) resumes only on its 2024 relisting; YNDX (Nasdaq halt, February
2022) only as NBIS in October 2024, the only `todayTicker ≠ ticker_tk` case; ALF has a single row in 2025.

Last close vs later vendor price, TRAIN only: LTM 2.91 → 3.21 (2024), YNDX 20.32 → 18.94 (2024). These are relistings years
later and say nothing about the OTC terminal return. **OTC continuation cannot be measured from local data.** The η
convention stands.

## 8. GO / NO-GO and U3 options

**NO-GO**: 59.9% (rule R) is below 80% on all 529 TRAIN true terminations. The held subset is also below: 69.0%.

The cause is entirely the 203 unlinked lines. The pinned r4 bridge links only reconstructed high/medium "common" company
lines (`scope_complete=false`). The unlinked lines, by ticker pattern and eyeball:
- 21 warrants, units or rights and 6 preferred-like lines;
- the remaining 176 are a mix of:
  - ETPs (BulletShares BSC*/BSJ*/IBD*, TVIX, UGAZ/DGAZ, USLV/DSLV, RUSL, ERUS, …);
  - pre-deal SPACs (PSTH, IPOE/IPOF, FTOC, CCIV.U, …);
  - ADRs and foreign filers (DIDI, CHL, PTR, SNP, LFC, GWPH, YNDX, RDS.A/B, …);
  - US common acquisitions the bridge misses (EV, FIT, HDS, MANT, WORK, PS, QTS, CATM, MCFE, TLND, INOV, ANAT, PE, EQOS, …).

A diagnostic fallback did not rescue them. Candidate CIKs from the r4 rehearsal `link_evidence` exist for 64/203 lines (only
34 unique), and classify just 14 of them. There is no local, point-in-time ticker → CIK history.

Options for the owner (U3):
- **(a) Scoped GO:** T33b emits rule-R events for bridge-linked lines only; unlinked lines keep the scenario η. This covers
  97% of linked lines and 69% of held lines, and must be disclosed. Its weakness: 109 held unlinked write-offs stay at
  S2 η = 0 (optimistic).
- **(b) Identity first:** extend the bridge, or add a point-in-time ticker → CIK map, for the 203 lines, then re-run T33a.
- In either case, rule on **non-common lines**: ETP liquidation at NAV, SPAC trust redemption, unit separation and preferred
  redemption all make "last close stands" (η = 0) economically right. The literal "unknown = performance −0.35" would wrongly
  penalize them. Holding them in the book at all is questionable (`common_stock_verified=false`).

## 9. Producer recipe for T33b (if U3 rules GO or scoped GO)

**Inputs:** role dir (sessions, ids, present, member); bridge `links.parquet`; plus
- a **sealed parquet export** of `sec_submissions` — do not bind the live 12 GB warehouse, which another session is
  rebuilding. Columns: `cik, accession_number, form, items, filing_date, acceptance_datetime`. Keep the §3 forms plus 10-K/10-Q.
  TRAIN filter: `filing_date` in [2019-01-01, 2023-01-31] and `acceptance_datetime < seal (2023-01-01)`. Record the SHA and
  per-CIK coverage;
- **a ticker-continuity input**, which the current CLI lacks. Add
  `--ticker-continuity <parquet>` with `(sr_id, last_ticker, successor_sid, successor_first)` derived by the §2 query.

**Steps:**
1. Compute `L_j` and the terminated set as in §1.
2. Look up the CIK: bridge row with `sr_id=j`, `start<=L<=end_incl`, `primary ∈ {P,J}`. More than one CIK → unknown.
3. Continued line (§3) → no row, unless 8-K 1.03 → performance.
4. Filings: DISTINCT `(cik, accession)` with `dd ∈ [−365,+30]`. Apply rule R.
5. Venue from the 25-NSE agent in `[L−60,L+30]` (table in §6). Otherwise venue = unknown.
6. η: mna 0; performance/unknown −0.30 (nyse, amex), −0.55 (nasdaq), −0.35 (unknown, arca, cboe, multiple).

**Output columns:** `cik` (NULL if unlinked), `instrument_id = sr_id`, `terminal_session = L`, `kind`, `venue`,
`replacement_return = η`, `source_accession`, `source_accepted_utc`. The source fields hold the **earliest-accepted filing of
the winning class** (ties by accession), NULL for unknown. `source_accepted_utc` = `sec_submissions.acceptance_datetime`,
already UTC (normalised from EDGAR's offset stamp).

**PIT rule:**
- Known session = the first role session whose 22:00-UTC mark is ≥ `source_accepted_utc`, then **+1 session**.
- Emit the value at `max(L, known)`, never earlier.
- Measured on TRAIN rule-R classifications, the lag is median +2 sessions, range −250 to +11. The negative end is merger
  proxies filed months ahead.
- 90 are known on or before `L`, 224 within 1-5 sessions, and 3 after `L+5` (SES +11, JCAP +6, AIMT +6). Those 3 miss the
  stale5 write-off, which falls at `L+5` (median), and T32 falls back to the scenario η. None fall after the TRAIN end.
- Of 258 held names classified under rule R, 255 were known by their own write-off session.

**Expected counts (rule R, TRAIN):** classifiable (mna+performance) = 292 + 25 = **317**; rows emitted for linked lines,
unknowns included = 326; non-NaN `terminal_return` = 326, or 529 if the 203 unlinked unknowns are also emitted.

## 10. Concerns

- **Lookahead in "presence ends for good".** The terminated set uses the full-sample future: the line never returns. The
  point-in-time equivalent is to classify at the write-off session using only filings accepted by then, with "no evidence" →
  unknown η. T32 should prefer that framing over a precomputed terminal flag, or disclose the difference: 7 names reappeared
  after write-off in v6-c1.
- **Identity reliability.** Bridge CIKs are rehearsal identity. The r4 source parquet under `identity_rehearsal/…/export-001`
  shows `created_at` 2026-09-27, so it may be under rewrite by the live session. It was used for diagnostics only.
- **Filing coverage.** `sec_submissions` coverage is incomplete for some foreign or large filers (§4). Nasdaq 25-NSE rows look
  sparse (§6). Items come from EDGAR's `items` metadata, with no document text.
- **Mechanical 3.01 rule.** 3.01-only "performance" still includes likely M&A or reorganization cases (EVK, BPYU). Item 3.03
  with 3.01 is a merger or reorganization marker worth adding if the lane resumes.
