# Task R2: adversarial review of spo-v2 (read-only) + proposed pre-registration defaults

**Scope:** read-only. You write exactly one file:
C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-R2-review.md (<= 80 lines). Never build, never run real data,
never spawn subagents, never read validation / 2023+ statistics. You may read the already-disclosed spo-v1 TRAIN outputs
(C:/atx-wt/pool-2/build-equity/mega-nav-v61u-spo-v1-L1.247/: summary.json, v7_extras.json, spo_diagnostics.csv head) since
that cell is rejected and in the ledger.

**Subject:** commit b63829f0 in C:/atx-wt/pool-3 (`git -C C:/atx-wt/pool-3 show b63829f0`, diff vs 934bb4cd): spo-v2
after the spo-v1 root cause. Context: task-W1-brief.md, task-W1-report.md, progress.md entry "spo-v1 cell REJECTED",
v7-prereg.md "Construction trial: spo-v1". Files: atx-impl/src/strategy_spo.{hpp,cpp}, strategy_nav_v7.{hpp,cpp},
atx-impl/tests/strategy_spo_test.cpp; risk model code atx-impl/src/strategy_risk*.cpp.

**Questions to answer (severity I / M / m per finding, file:line, one line each)**
1. Do the three claimed root causes explain the spo-v1 numbers (net SR -0.94, gross .65 on every row, tau 12%/day, a'w
   3.6x realized)? Is the arithmetic of a_i = IC sigma z / sqrt(h) with h = H = 20 right under the GK / Boyd et al.
   convention? Anything still inconsistent between alpha horizon, cost amortisation and the risk term's horizon?
2. gamma = gamma_vol with target vol .05 and gross budget L 1.247: will the gross constraint bind on (nearly) every date,
   and is the refusal "vol target out of reach" reachable on real data? What does the book look like when gross binds
   (lasso-like concentration, number of names, w_max .01 binding share)? Is R6' mechanics (gross in [.90, 1.05] x L? read
   the prereg for the exact definition, |net| <= .02, tau mean <= .20, p95 <= .30) plausibly met?
3. `--specific-ceiling 1.0`: is clamping right, or does it hide a risk-model defect? Locate the source of the daily
   specific variance 1e0..9e12 on 177-179 names, 2020-05-12..06-09, in the risk code (read-only diagnosis: which
   estimator, which input; e.g. a price/return outlier, a split, a Bayesian-shrinkage division). State whether the
   factor bias statistics of F2 could be affected and whether a risk-model fix (new pin) should precede spo trial #2.
4. spo-v1 semantics preserved bit for bit with the old flags? Flag-off (aim-partial-v5) path untouched?
5. Test gaps.

**Then propose** the pre-registration paragraph for "Construction trial: spo-v2 (spo trial #2)": every flag with its
default and literature reason, none tuned on returns; the identity requirement; the acceptance rule (mirror spo-v1);
what counts as a further trial. Mark each default you would change versus the commit, with the reason.

Reply to the parent in < 15 lines: count of I / M / m, the top three findings, go / no-go for running the cell on the
commit as-is.
