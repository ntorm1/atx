# Task W1b: spo-v2 corrections after the R2 review (no runs)

**Pool:** C:/atx-wt/pool-3. `git status` clean, then `git checkout -B feat/platform-v7-w1b-spo-20260929 c8503bb3`
(pool-2 HEAD; it contains your b63829f0).
**Rules (binding):** never build C++ (root builds; write code that compiles under clang-cl 18 /W4 /WX, C++20); never run
real data; never spawn subagents; never touch C:/atx; never read validation / 2023+ statistics; no pushes. Read
.agents/cpp/agent.md before writing C++. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write files
with the Write tool (heredoc apostrophes break).
**Report location:** task-W1b-report.md in YOUR pool at C:/atx-wt/pool-3/.superpowers/sdd/platform-20260928/, committed
with `git add -f`. Do NOT write into C:/atx-wt/pool-2 (a real-data run is in progress there and any write voids it).
Do NOT edit atx-impl/src/strategy_risk_model.* (lane F3 owns the risk model).

**Read first:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-R2-review.md (all findings and the proposed
pre-registration), task-W1-report.md, v7-prereg.md "Construction trial: spo-v1".

**Work (spo-v1 planned weights and daily / events CSVs must stay bit for bit under the spo-v1 flags)**
1. I-1: budget flag `--spo-gross G` = the hard cap on planned gross used by the optimiser; spo-v2 default 1.0; spo-v1
   default = --aim-leverage (unchanged semantics). The aim-partial-v5 shadow book keeps --aim-leverage. Refuse
   G <= 0 or G > --aim-leverage x 1.5 (sanity), and record G in recipe / extras / summary.
2. M-2: spo-v2 default `--alpha-horizon 21` (the IC's measurement horizon), independent of --spo-horizon; spo-v1 stays 1.
   m-6: state in the recipe declaration that a uniform 1/sqrt(h) is used for all sleeves.
3. M-1: the specific ceiling is a tripwire in spo-v2: summary carries `capped_specific_decisions` and
   `capped_specific_names_max`; with `--specific-ceiling-void` (spo-v2 default on) a nonzero count makes the run exit
   non-zero after writing diagnostics and BEFORE any NAV / return file is written (so no return statistic exists).
4. m-2: spo-v2 recipe / extras blocks keyed `spo_v2`; m-3: a failure of the report-only gamma_bind root is reported
   (NaN + reason), never aborts.
5. M-3 ledger correction: document in the diagnostics header / report that `exante_vol` is annualised.
6. Tests (M-4): a digest pin of spo-v1 planned weights on a fixed synthetic replay (so "bit for bit" is tested, not
   read); a spo-v2 replay test over >= 30 decisions asserting gross_binding on >= 90% of post-ramp decisions and planned
   gross == G to 1e-10 when binding; the clamp changes alpha (and the tripwire voids); parse tests for the new flags and
   refusals.

**Report** (<= 40 lines): flags and defaults table (v1 vs v2), files, tests, exact root command lines: build targets,
test filter, identity run (b) of the R2 proposal (`--rule spo-v1` on pin 897ffdf2: which files / columns must match),
and the spo-v2 cell line with `<RISK_DIR>` / `<RISK_SHA>` placeholders for the F3 pin. Reply to the parent in < 15 lines
with the commit sha.
