# Lane AL-DATA report (doc-only, Ruling W3-18)

## Outcome

DONE_WITH_CONCERNS. The lane produced the written atx-db request and four builder designs. It built nothing and
registered nothing.
- No gated data has landed: OD-P9-4..6 cannot be executed (ledger :16), and P9-S lapsed at 0 (W3-15).
- Concern 1: the required amendment of the two blocked registrations is given as text but not applied, because this
  lane writes only `al-data/`.
- Concern 2: only one family clears the bar on paper, and only narrowly.

## Branch / SHA / pool

- No pool, no branch and no commit (W3-18). The files are left untracked.
- Everything was read at pool-2 HEAD `f6444417` (`feat/platform-p9-20261003`).
- atx-db source was read only with `git show <ref>:<path>` at `main` = `dec03454` and `feat/tier1-v3-warehouse` =
  `0e2fa077`. The pool-2 tree has no `atx-db/src/atx_db/alpha_panel/` package, and `C:/atx` was not read.

## Files written

1. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md`
   - Part A: ALSIG-7, with file and line citations, the exact point-in-time rule (R1-R6), the planted test, and the
     acceptance checks.
   - Part B: the three OD-P9-6 asks, each with columns, keys, availability stamp, history depth and acceptance check.
     Common rules C1-C6 include partitioning by year so that a TRAIN build never opens a part stamped 2024 or later.
2. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/builder-designs.md`
   - Four one-page designs in the house registry shape: `lazy_prices`, `hf_crowd`, `bm_int`, and option borrow /
     put-call parity (`borrow_fee_implied` and `pcp_spread`).
   - Each has its K-P9-11 keys, planted-leak probe, capacity class and bar verdict.
   - Section 5 holds the blocked-registration amendment text.
3. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/task-AL-DATA-report.md` (this file).

## Required items (wave3-carry "AL-DATA")

| item | result |
|---|---|
| OD-P9-4..6 not executable: build nothing; a one-page design per family | Done: `builder-designs.md` sections 1-4. No code. |
| ALSIG-7: the request to the atx-db owner | Done: request part A. It also covers two more look-ahead channels found in the same `intervals()`: `effective_to` = `last_session` on rows stamped at the rank date, and the `recon_summary` add / delete counts. The existing atx-db test pins the first of these at `test_alpha_panel_indexes.py:72`. |
| ALSIG-4: `hf_crowd` stays blocked and is never queued | Held. The design is marked "not to be built", and ask B2 is deferred to last. |
| Amend `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json`; no duplicate id | **Not applied** (doc-only, W3-18). The exact text is in `builder-designs.md` section 5. No new id is proposed. |
| K-P9-11 keys; `source_sample_end` from the paper; unverifiable rows left out; ids at most 24 characters | See the table below. All ids are 6-18 characters. |

The `source_sample_end` values:

| field | value | source |
|---|---|---|
| `lazy_prices` | 2014 | CMN, checked by the AL-SIG reviewer |
| `hf_crowd` | 2016 | BHL, checked by the AL-SIG reviewer |
| `bm_int` | 2018 | **verified this lane**: NBER w28056 rev. Nov 2021, "The sample is from January 1975 to December 2018" |
| `pcp_spread` | left out | **unverified**: the CW abstract states no years and SSRN returned 403 |

## Evidence

All of the following was read-only.
- Ledger: grep of `progress.md`.
- Sprint files: the carry, the AL-SIG report and re-review, the plan, and the literature review.
- atx-db source via `git show`: `indexes.py`, `security_master.py`, `common.py`, `filing_text.py`, `text_features.py`,
  `tests/test_alpha_panel_indexes.py`, `docs/datasets/{text_native,compustat_std}.md`, and `compustat_std.py` (grep).
- Engine headers in pool-2: `sources/{stage,thirteenf,russell_proxy}.hpp`, `russell_recon_field.cpp`,
  `book/borrow_schedule.hpp`, and the `field_registry.json` rows.
- Web: the EKP NBER PDF (the WebFetch tool saved it to its own session tool-results directory, not the sprint dir) and
  the CW Cambridge abstract.

What was not done:
- no build, no pytest, no git command that writes;
- no atx-db data opened;
- no 2020-2023 return / IC / Sharpe / NAV output read. The only Sharpe ratios read are published literature figures
  (EKP, 1975-2018).

Disk freed: none, because nothing was built or created outside `al-data/`.

## Bar verdicts (detail in `builder-designs.md`)

- **Clears on paper (narrowly): F1 `lazy_prices`.**
  - rho <= .1.
  - SR_c is about .4-.65 net, centre about .5 [est], taken from CMN's value-weighted 34-58 bps/month.
  - It is slow, raises capacity, and its source sample is clean for OD-3.
  - Risk: TRAIN falls entirely after publication, and decay after publication is unverified.
- **Fail and should not be built:**
  - F2 `hf_crowd`: rho .2 (ALSIG-4); its source sample is also contaminated for OD-3.
  - F10 `bm_int`: rho .7 / .76. As a replace, the difference portfolio has SR .32 over 1975-2018 (EKP, before costs),
    and the source is contaminated.
  - F4 / F6 `pcp_spread`: rho .3, and predictability decays even within the authors' own sample.
  - F5 `os_ratio`: rho .2.
- **Outside the bar: `borrow_fee_implied`.** It is a cost-model input. Build it if OD-P9-4 or -5 is bought.

## Blocked on which owner decision

| deliverable | blocked on |
|---|---|
| ALSIG-7 stage fix, and through it a point-in-time `russell_recon` field | the atx-db owner (request A); not an OD |
| `lazy_prices` kind + registration amendment | OD-P9-6 ask B1 (text landing) |
| pinned, year-partitioned `compustat_std` (platform part) | OD-P9-6 ask B3 |
| `hf_crowd` | OD-P9-6 ask B2 **and** a new PM ruling on the rho bar (ALSIG-4) |
| `borrow_fee_implied` (realism) | OD-P9-4 or OD-P9-5 purchase |
| `bm_int`, `pcp_spread`, `os_ratio` | nothing: excluded by the bar |

## Recommended P10 queue

1. **ALSIG-7 stage fix** (atx-db owner). It is small, and it protects every consumer of the Russell / S&P proxies.
   Afterwards the engine re-pins v2 and drops the `russell_recon` aspect; the string stays `dropped`.
2. **B1 text landing**, then the `lazy_prices` kind, then the registration amendment. This is the only family that
   clears on paper, it is clean for OD-3, and it is slow.
3. **Realism data** (OD-P9-5 preferred, else -4), then `borrow_fee_implied` into `BorrowSchedule` as a flagged
   cost-model protocol.
4. **B3 platform part** (pinned, year-partitioned `compustat_std`) for every fundamentals field. `bm_int` itself is
   not queued.
5. **B2 Form ADV**, only after a new PM ruling on the rho bar. Otherwise never.

## Deviations

- The blocked-registration amendment is text only, not applied (see the required-items table).
- The brief asked for line citations "in the pool-2 copy" of the atx-db source. The pool-2 tree lacks the package, so
  the lines are cited at committed refs read from pool-2. Line numbers differ between `main` and the warehouse branch,
  and both are given.

## Open risks

- F1's SR_c half is an estimate (the 8-12% volatility is assumed). A decay after publication of more than about 25%
  fails it.
- CMN's "full-document cosine is the headline measure" is from memory and marked [verify].
- The Form ADV per-filing archive depth, and the CIK item in ADV Item 1, are marked [verify].
- The 85% / 95% acceptance gates in asks B2 and B3 are proposals, not house constants.

## Ledger candidates

- atx-db `indexes.intervals()` has two more look-ahead channels besides ALSIG-7's presence leak:
  - `effective_to` = `last_session` on rows stamped at the rank date (the 2023 reconstitution embeds facts from 2024
    and later);
  - `recon_summary` add / delete counts, through `alive_prev`.
  The atx-db test `test_alpha_panel_indexes.py:72` pins the first.
- EKP 2022 (CFR 11(2)): the sample is January 1975 to December 2018 (NBER w28056 rev. 2021). The baseline is decay
  .2, g .1, INT_0 = SGA_1/(g+decay), 100% of real SG&A, and B_INT = BE - GDWL + INT. HML_INT minus HML_FF has SR .32
  over the full sample and .62 since 2007; the HML_INT / HML_FF correlation is 76%. The CW 2010 abstract states no
  sample years.
- The atx-db text landing (`filing_text` / `text_features`, and `text_native` on the warehouse branch) covers
  10-K / 20-F only, from 2018 (`text_native`: 2019-2026, unqualified). It has no 10-Q and no full-document similarity.
  The legacy scope selects CIKs by 2019-2026 membership.

## Questions for the PM

- **Q1.** Hand the request over as written? Recommend: send A and B1 now and B3 as a platform item; mark B2 deferred
  until the rho bar is re-ruled.
- **Q2.** Lazy Prices measure: full-document cosine (needs a new atx-db column) or MD&A cosine (exists today)?
  Recommend: fix full-document cosine now, before any data; fall back to MD&A only if the owner declines, ruled before
  arrival.
- **Q3.** Apply a decay-after-publication haircut to F1's SR_c half? Recommend: no, for consistency with
  ALSIG-2/4/5. PRE prints the decay risk as the downside of F1's prediction, and lit §8 Q4's replication search runs
  before registration.
- **Q4.** May the engine kind read atx-db's per-filing similarity (a rule-pinned stage feature) instead of recomputing
  it in C++ (PM8-12)? Recommend: yes. The clock, staleness and cross-section stay in C++, and the full documents stay
  in atx-db.
- **Q5.** Record `bm_int` and `pcp_spread` as excluded by the bar and leave them unregistered? Recommend: yes, by the
  ALSIG-2 precedent.
- **Q6.** OD-P9-4 versus -5, now that the option signals fail the bar? Recommend: choose on realism alone.
  - Prefer OD-P9-5, which fills both the fee grid and the locate grid, if it is priced comparably.
  - Buy 2014-2023, or the prereg states that OD-3 runs on the flat fee.
- **Q7.** Who applies the blocked-registration amendment (section 5)? Recommend: root, in its next docs commit, after
  `test_p9_al_sig_registrations.py` passes.
- **Q8.** The two extra index look-ahead channels? Recommend: keep them in the request (done) and add no engine aspect,
  because `russell_recon` reads neither column. Root greps for other house readers of `effective_to` before v2 lands.
