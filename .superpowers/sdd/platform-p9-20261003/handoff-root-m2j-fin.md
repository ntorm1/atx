# Handoff: root after M2j finish (RESUME 7, dispatch R7-1), 2026-10-05

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Resumed from `handoff-root-m2j.md` (its DONE rows were not
redone). Rows: `integration-log.md` "M2j finish"; detail: `root-wave2-merge-report.md` "M2j finish" and "For PRE (round
2b facts from M2j)"; evidence `m2j-logs/` (tools `m2j-logs/tools/`).

- **Status: BLOCKED at item 8 only** (the C3-5 probe found a duplicate `--rule` on the NAV argv: a stop for the PM by
  the dispatch's own words). Every other item of the section is DONE and holds. Stopped at a clean point: no merge open,
  no run, build or test of mine in flight.
- **Code head: `ebc918c3`** = `327c86b2` (C3 part A merge) + `ebc918c3` (SQL2-CLS: `atx.nav-spec/v1` in `classes.json`).
- **Docs head:** the commit that adds this file. Earlier docs commits of this resume: `a15ad887` (housekeeping:
  `progress.md`, `status-7.md`, `next-goal-prompt.md`, `resume7-addendum.md`, `r7-dispatch.md` as they stood).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Next free build tag: `p9-2r`** (no build in this resume).
- **Known-red set: EMPTY.** The classes guard cleared at `ebc918c3`. Release
  `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` stays a named skip.
- No lane merged; item 7 (W3-8 flip) is lane D3's (W3-8a): `scripts/tests/test_wave_kinds.py` not touched. No lease
  touched. No permission refusal.

## Exes (EXE-CUR: all current at `ebc918c3`; `git diff --name-only 327c86b2..HEAD` over C++ / CMake / presets = 0 files)

Source `327c86b2` for both builds (receipts `m2j-logs/mega-p9-2{p,q}-receipt.json`).

| exe | Debug p9-2p (`build-equity/bin`) | Release p9-2q (`build-equity-rel/bin`) |
|---|---|---|
| atx-equity-strategy-targets (NAV) | `637f706987da395533342f51ede8cf894249a0fa1cfae9a1dc96217741338827` | `adcf72d4821de049f2e83db43759f3d28df4493504956739e1ed40f580656368` |
| atx-equity-strategy-ic | `919569fb9983b8591bd4d345187f582da20dedecbcffd916ba57a6417741b6b0` | `1d24336f9de8c6337df3984693d94a8a02b9f8524238077c1ce085916e3b593e` |
| atx-equity-strategy | `3dc162d9532ab5f0118325a7cbc2350bc79f1173475f4eb515ba94a84c2b86ba` | `257d4b30d0ed6c0a843c54358db19e1ec5b29fbbfad20feb53a510c918eb5cf1` |
| atx-equity-strategy-risk | `a50bb015073d09de10c04eeb94766edd6b27aff4dc2b65b8c99ab22781b1465b` | `0afaffcc8bce6c3c5bbd1965f27ef7c91655a1c53d03a3767c8554c95e209a8b` |
| atx-equity-strategy-mine | `1e9e0cda395f665f5ff48c5d3e48e64f6687503f540fbbff4e354f5a115c83bb` | `cca753127741c3c0ae42da54a7ed7f237f5da37e7f8c3571d3c7d8e100b64e3d` |
| atx-research-eval | `bd8b5e08a984ebb60f6f1d22dd002452a82b9ec555d242e2ea84ab2f2c52128e` | `a57a2090999c0f71557ebf93bd952337395bc8cd9cd107d641f4b8e4452e38d9` |
| atx-impl | `fbdcf2c2fd8af5e0fada807e53bfccc9c66f32145620f69edf4380cd99dac818` | `099e68e53eb329eddad566b1d6dcd4f179213cb7b4c00e8f7b891ac2972bc194` |
| atx-impl-strategy-target-tests | `975e0a018b967b86eabfe9758569aa2d1703a2c06b69cb1d372ca52aff9ef03b` | `2a4913aa2ebb454f68f2f29369c46873e6e033332ca58fbf96ca8022a8e9cbe3` |
| atx-impl-tests | `3d5f7efe2d91c899276214370d44292dc19c3d75a40561bf774ba6c618603675` | `dd753dcf9bfd5cddd1089de94951b6aba76ea1d0a7cadddc1f691b9f1e386141` |

Research-store `build-equity/bin/atx-research-store.exe` p9-1u `3b955ba1...` (source `1300fd6a`): current (no store /
catalog source, catalog CMake block, `atx-core` or top-level CMake change since).

## Items of this resume (order M2J-1)

| item | state | result (evidence in `m2j-logs/`) |
|---|---|---|
| 0 housekeeping | DONE | `a15ad887` |
| 4 opt-in (PRE-7a, C3-9) | DONE, **holds** | only `summary.json` differs, by `/scenarios/0..4/ann_mean_gross` (opt-in side only); strip = flag-absent bytes; `capacity/summary.json` no key (`item4-cmp.txt`) |
| 5 Release = Debug, walls | DONE, **holds** | X-5 Debug vs Release: only `/producer/build_type`; vs `p9-c1-s2-x5-nav`: only `/producer/engine_git_sha` (`item5-cmp.txt`); walls in `item5-walls.txt` |
| 1 rest | DONE, 0 failed | Release whole target-tests 390 / 0 / 1 DISABLED; `atx-impl-tests` Release and Debug 1,206 ran, 1,201 passed, 5 named skips (`gt-exits.txt`) |
| 6 SQL2-CLS | DONE | `ebc918c3`; guard 1 failed -> 5 passed; store suite 43 x seeds 0 / 1; fixture chain `d32f7655...4e69`, 12 / 0, rel = abs |
| 9 ROOT-1 | DONE, 0 failed | `scripts/tests` seed 0 512 / 4 skip; stage chain 9 (`pyt-exits.txt`) |
| 8 C3-5 probe | **STOP for the PM** | duplicate `--rule aim-partial-v5` on the calibrated NAV argv; NAV exe refused, exit 2; wave exit 4 at match (`item8-probe.txt`) |
| M2I-HK | DONE | `build-equity/p9-m2i-cov-y1-nav` deleted by literal path (12 MB) |

## Item 8: what the PM must rule (C3-5)

- The tiny-world wave `c3r` (kind rule, `pm6-6-engine`, rules `[aim-partial-v5 {trade_fraction .1}]`) puts two `--rule`
  tokens on every NAV argv of the rule cell (calibrated and cell of record):
  1. `scripts/cycle/phases.py:829`: `nav_step` always emits `--rule nav["rule"]`; the parent's `nav.rule` is
     `aim-partial-v5` (tiny world, and the real X-5 / Y-F0 argvs too).
  2. `scripts/wave_kinds.py:385-399` (lane D3's file): the row's x-flag `--rule aim-partial-v5` goes into
     `change.flags.nav["--rule"]`; `scripts/research_spec.py:128` `apply_flags` appends options absent from `nav.flags`
     (it replaced `--trade-fraction` .05 -> 0.1 in place; `--rule` is `nav.rule`, not in `nav.flags`).
- The NAV exe fails closed (`nav replay: duplicate/missing flag`, exit 2): no silent double application. A row other
  than the parent's would be a conflicting pair (same refusal expected; not run).
- Not fixed, not routed around. The fix touches either the kind derivation (D3's file) or the cycle's `nav.rule`
  handling; the PM chooses the owner. After the fix, root re-runs `m2j_c3_probe.py wave c3r` / `report c3r` on a fresh
  world (delete `build-equity/p9-m2j-tw-c3r` by name first; the base `p9-m2j-tw` can be reused only if HEAD's code dirs
  are unchanged, else rebuild it) and repeats it at AL-CLOCK's merge with `info-clock-rate-v1` (C3-5).

## What the next root agent must know

- **Interpreter.** In Git Bash the first `python` on PATH is `C:/atx/atx-db/.venv` (another session's; no `psutil`).
  Use `C:/Program Files/Python312/python.exe` explicitly (as `m2j-logs/tools/pyt.sh` does). Item 4's attempt 1 was void
  for this reason (logged).
- Real-data runs: `m2j-logs/tools/nav_run.py` (bounded runner, `--seconds 600 --max-rss-mib 2048`); comparisons
  `m2g-logs/tools/nav_cmp.py` and `m2j-logs/tools/optin_cmp.py` (paths and SHAs only).
- Kept outputs (regenerable): `build-equity/p9-m2j-{x5,yf0}-{argv,spec}`, `p9-m2j-x5-{optin,dbg}` + `-run` dirs (11 MB
  each); probe worlds `build-equity/p9-m2j-tw` (23 MB), `p9-m2j-tw-c3r` (24 MB, the stop's evidence). Delete by name
  when the PM has ruled item 8. Never delete `p9-m2i-dcov-v11`, `p9-m2i-dcov-covv1`, `p9-m2d-cov-risk-container`, the
  Debug / Release object trees, or the pre-merge Release exes (session scratchpad of the earlier M2j agent).
- Lanes waiting to merge (R7-1) merge onto `ebc918c3`'s code; their trees should see no classes-guard red from
  `atx.nav-spec/v1` once they merge root again.

## Uncommitted state at the end

- None of mine. `progress.md` committed as it stood at the final docs commit. Four untracked `docs/plans/*.png`: not
  mine, left. `m2j-logs/tools/.mypy_cache/` (not mine, ignored) not committed.

## Open questions for the PM (one line each)

1. C3-5: which owner fixes the duplicate `--rule` (kind derivation in `wave_kinds.py` vs the cycle's `nav.rule` emission), and is the fix "the row sets `nav.rule`" or "a row flag replaces `nav.rule`"?
2. Should the probe also run a conflicting pair (a trade-rate row other than `aim-partial-v5`) once the fix lands, as a negative test?
