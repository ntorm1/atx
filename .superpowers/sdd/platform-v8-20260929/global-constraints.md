# Global constraints, review focus, contracts (cut from the plan)
## 3. Global Constraints

Every task inherits these. They restate the v7 standing rules with the OD-1 change.

- Root worktree is `C:/atx-wt/pool-2`, rebased on `C:/atx` main. Branch `feat/platform-v8-20260929`. Never build, switch or
  commit in `C:/atx`; the atx-db session owns it. Never touch `atx-db/` or kill its processes.
- Root alone builds and runs real data. Children own one pool worktree each (pools 3, 4, 7, 8, 9, 10, 11; never 1 or 6),
  leased with `scripts/lease-worktree.ps1`. Children never build, never run real data, never spawn subagents.
- Real data runs only through `scripts/run_bounded_research.py` or `scripts/research_cycle.py run`. Caps are 180 s and
  1,536 MiB unless OD-2 rules otherwise.
- TRAIN is `[2020-01-01, 2024-01-01)`. No tool, test, report or agent may open a session, file or statistic dated
  2024-01-01 or later. No validation statistic is quoted anywhere.
- Every ruling is written as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias.
- Every new cell, library and field is pre-registered in `v8-prereg.md` before it runs. Every result carries the
  Appendix A trial accounting block.
- Track P tasks must reproduce named accepted outputs byte for byte. A task that cannot is not merged.
- C++ follows `.agents/cpp/agent.md`. Build through the tracked research build script (task E-1). `/W4 /WX` stays on.
- No pushes, warehouse writes or broker actions.
- The sprint directory is `.superpowers/sdd/platform-v8-20260929/`, committed with `git add -f` before every real-data run.
- One variant per hypothesis. Windows come from the house set {5, 21, 63, 126, 252}. No window, sign or parameter search.

---

## 4. Review Focus

Failure modes the inputs imply and that a single task's happy-path test would miss. Each line names the owning task's test.

| # | condition | expected behaviour | test |
|---|---|---|---|
| 1 | A role, field, cache sidecar or report path carries a session on or after 2024-01-01 | every tool refuses with a seal error; nothing is scored or rendered | W0-1 `test_seal_refuses_2024` (Python), `ResearchWindow.RefusesSealedSession` (gtest) |
| 2 | Extending the role to 2023 changes the 2020-2022 values (larger instrument union, summation order) | per (date, instrument id) values of 2020-2022 equal the 3-year run; any difference is reported and stops the re-base | W0-2 `compare_window_overlap` |
| 3 | The 4-year role exceeds the memory admission | `--plan-only` prints required bytes; the run refuses before loading; the driver hard-stops | W0-2 step 3; B-2 `AdmissionReportsRequiredBytes` |
| 4 | Two roles or two windows share one cache root | a payload from another role or window is a miss, never a hit | C-1 `test_fit_store_keyed_by_role_and_window`; B-1 `CacheMissOnRoleChange` |
| 5 | A child report lands in the sprint directory during a cycle; a second cell is ledgered between plan and run | the cycle continues; `dsr_n` resolves from the ledger at scoring time | A-3 `test_clean_check_pathspec`, `test_dsr_n_from_ledger` |
| 6 | A candidate has signal on fewer than 80% of eligible names, or an event field is empty for a whole year | it is scored with a coverage flag, never silently unscored | C-1 `test_card_low_coverage_reported` |
| 7 | A hex cache folder name contains "2024" | the report reads it; a date-shaped path part is still refused | E-4 `test_seal_regex_hex_vs_date` |

---
### 6.3 Cross-lane contracts (declared before dispatch)

| contract | writer | reader | content |
|---|---|---|---|
| K1 plan rows | B-3 | A-1 | `atx-equity-strategy-ic --plan-only` prints `candidates[]`: `{id, dsl_sha256, num_slots, required_lookback, extra_fields[], node_count}` |
| K2 exposures layout | D-2 | C (follow-up) | `atx-equity-strategy-targets exposures --role R --output DIR` writes `basis.f64` (decisions x names x 4) and `forward_returns.f64` plus `manifest.json` |
| K3 `--no-git` | A-3 | E-3 | accepted by `research_cycle.py` and `run_bounded_research.py` only when `--root` is outside the repository |
| K4 window | W0-1 | all | `research_window.json` schema `atx.research-window/v2`; nobody hard-codes a TRAIN or seal date again |
| K5 origin class | A-1 | V-1 | registry field `origin` in {`prior`, `grid`, `mined`}; copied into every ledger line |
| K6 marginal IC | F-2 | C-2, G-1 | `marginal_ic.json` per candidate: `{id, ic21, ic21_hac_t, marginal_ic21, marginal_hac_t, max_abs_rho, max_rho_member}` |

### 6.4 Merge order

W0-1 first. Then E-3 (fixture) and B-3 (move-only split). Then B -> C -> A -> F -> D -> V -> G -> E-2.
