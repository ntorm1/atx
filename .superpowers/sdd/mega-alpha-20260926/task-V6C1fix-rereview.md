# V6-C1 fix round 1 re-review

Reviewer: Claude (read-only), 2026-09-27. Commits 2bcfe646, 335c955e (review-V6C1fix.diff). No build/run.

- **I1 (dangling temporary): ADDRESSED.** `strategy_nav_replay_test.cpp:2527-2528` binds
  `const auto exit_summary = read_json(...)` before the range-for over `.at("scenarios")`. Scanned both
  `strategy_target_replay_test.cpp` and `strategy_nav_replay_test.cpp` for the same pattern (range-for over a call
  chain ending in `.at(...)` on an unnamed temporary): none remain; every other `for (const auto&... : X)` iterates a
  named object (`summary`, `base_summary`, `object`, `base`, ...) or a direct temporary as the range-expr itself
  (`directory_iterator(dir)`), which is lifetime-extended and not the P2718 hazard.
- **M3 (key naming / stale text): ADDRESSED.** `exit_rule`→`exit_rate_rule` renamed consistently: constant
  `exit_rate_rule_declaration` (`strategy_target_replay.cpp:207`), JSON key `exit_rate_rule` (`:250`), test pins
  updated (`strategy_nav_replay_test.cpp:2160,2522,2605` all assert the old `exit_rule` key is now absent; no
  production reference to `exit_rule` remains — grep confirms). The stale "nonmembers exit to 0" text is now dynamic
  via `detail::nonmember_exit_clause`/`nonmember_exit(cfg)` (`:679-682`, wired through `strategy_target_replay.hpp`
  and `_detail.hpp`), used by both the target replay's own `aim_partial` (`:240-245`) and the NAV per-name text via
  new `per_name_rate_head`/`_tail` split (`strategy_nav_replay.cpp:1137-1150,1229-1233`).
  - **Default-path byte identity: verified by hand-tracing, not just asserted.** `decaying_exit(cfg)` is
    `exit_rate != 1.0` (`:63`), so at the default `exit_rate=1` `nonmember_exit` returns exactly `"nonmembers exit to
    0"`. Concatenating head+clause+tail (both target and per-name declarations) reproduces the pre-fix literal byte
    for byte in both cases — confirmed by literal string comparison, not just trusting the report. No recipe/summary
    key is added or changed on the default path (target order basis, exit rate 1): `exit_rate`/`exit_rate_rule` are
    still gated on `decaying_exit(c)` only.
- **M5 (`v6_train.sh`): ADDRESSED.** `set -uo pipefail` (:31); `REF_CHECK` validated to `0|1` else exit 2 (:107,
  reached safely since `REF_CHECK=${REF_CHECK:-1}` is set at :85 before any use, so no `set -u` hazard); with
  `REF_CHECK=1` and `REF` absent the script now exits 3 at :154-157, before the nav run (:159) — the old "skipped,
  exit 0" branch at :185 is now unreachable dead code that also exits 3 (correctly documented as unreachable);
  `nav_summ` failure is caught via `PIPESTATUS[0]` (:193-194), which correctly isolates nav_summ's exit code from
  `grep -v financing`'s exit status (grep exits 1 on an all-financing/empty result even when nav_summ succeeded) —
  no regression. `REFA=(); ${REFA[@]+"${REFA[@]}"}` (:191-192) is the correct `set -u`-safe empty-array idiom. No
  other pipeline in the script gates on `$?`/exit status in a way `pipefail` newly disturbs (`set -e` is not enabled,
  so pipefail alone changes no control flow elsewhere).

No new Critical/Important found. `nonmember_exit` has internal linkage (inside the existing anonymous namespace,
`strategy_target_replay.cpp:449-907`) — no ODR risk from the added file-scope function.

ALL ADDRESSED.
