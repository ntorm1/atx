# Task P3 report: library v7 pre-registration draft (read-only)

- **Output.** `library-v7-draft.md` (299 lines). It is not appended to `v7-prereg.md`; root does that.
- **Wave 1** (5 trials): qmj_safety, nincr, q5_eg, smax5, res_mom_ind. Library `fund_industry_ic_v7w1`, 44 = 39 + 5.
- **Wave 2** (5 trials): ins_opp, inst_best_ideas, eap_8k (optional), ftd_fail, ea_overdue. New theme `ownership_flow`;
  library `fund_industry_ic_v7w2`, 49 members.
- **0 trials.** Withdrawn: inst_breadth_chg (published signs conflict). Conditioning only: the 8-K material flag and
  Reg SHO threshold days.
- **Trial accounting.** 10 admission trials, plus 1 composition and 1 construction cell per wave. Cross-cell N goes
  32 -> 33 -> 34.
- **Rules unchanged.** v4-prior-v1 (tau .70, abs(rho) .90, veto t -2.0), ew-theme-v1, sign-only paired gate. Each wave
  is accepted or rejected whole.
- **Root must rule before the freeze:**
  - q5_eg's 6 extra fields (about 52 MB [est]);
  - MAX_ROSTER from 48 to 56;
  - appending `ownership_flow` to the fitter's V4_THEMES.
- **Root must check W2 and W5a outputs:**
  - W2 op spellings and semantics (`ts_topk_mean`, `ts_beta_on`, `ts_mean_mp`, `ts_std_mp`);
  - W5a `ea_days_to_expected` semantics, which `ea_overdue` depends on;
  - W5a `ins_opportunistic_net` units.
- **Not verified in this draft:**
  - the HMXZ slopes come from the NBER w24709 version and must be confirmed against RoF 2021;
  - several t-stats are marked "t n/v", and the nincr t ≈ 1.8 is a secondary report;
  - peak slots are estimated, not checked; no static checker was run.
- **Hygiene.** No VAL, 2023+ or per-candidate return statistic was read. `v6-literature.md` contains TRAIN t for some v5
  members; they were seen there and used for nothing (disclosed in the draft). Pinned SHAs: v6.1 library `db35c276...`,
  recipe `9bf278a6...`.
