# P9-H attempt2: actual seal stop and proposed native continuation

R15-H-LAUNCH2 was committed before measurement at `d5301d33`. The exact released command in the read plan
ran once with its three environment values, no RAM wait/host budget, unchanged caps/floor. Prelaunch RAM3703 MiB,
C:39.4 GB, CRT unchanged. Start2026-10-17T14:40:48.9441014Z; end14:43:00.2566416Z; exit4;
external131.2085962s. Raw console remains unread (`cells/logs/p9h-attempt2-native-console.txt`, SHA
`05b39319cf061186125ba10366fbc6f942e9e2075bc2e8b3d7a9d9a80bf7176a`). No retry or paired/IC read.

Native receipts: preflight0.890s, register0, screen0, spec1.437, run67.828, match59.406, failed verify0.297;
sum129.858s. `07-verify.failed-1.json` reports only22 unallowed date tokens in consoles006/007/011/012.
Native verify constructed mechanics/pin checks but supplied no mechanics/pin error. Mechanics values are still
unread; no own verdict is asserted. No judge, bundle, monitor/summ trial write or wave-result exists.

All sources below are under `build-equity/waves/p9h-cov-hedge-attempt2/consoles/`.
Exact selected lines are preserved in `cells/logs/p9h-attempt2-seal-source-lines.txt`.

| Source | Exact metadata context | Hits |
|---|---|---|
| `006-the-cell-s-inputs----stop-after-w--its-nav-runs-.log:4` | `# dirty outside the code pathspec (listed, not a stop):` followed by the five owner paths below | A1, OTHER4 |
| `011-calibrated-run----stop-after-nav--nav---calibrat.log:4` | Same dirty-list header and exact five owner paths | A1, OTHER4 |
| `007-mechanics-reader.log:66` | First JSON string in `dirty_outside_pathspec` (key at line65) | A1 |
| `007-mechanics-reader.log:67` | `docs/plans/2026-10-03-p9-b0-yf0-equity-curve.png` | OTHER2026-10-03 |
| `007-mechanics-reader.log:68` | `docs/plans/2026-10-03-wq-explore-equity-curve.png` | OTHER2026-10-03 |
| `007-mechanics-reader.log:69` | `docs/plans/2026-10-03-wq-explore-sharpe-vs-turnover.png` | OTHER2026-10-03 |
| `007-mechanics-reader.log:70` | `docs/plans/2026-10-06-p9-f0-yf0-equity-curve.png` | OTHER2026-10-06 |
| `012-mechanics-reader.log:66-70` | Same five exact JSON strings in `dirty_outside_pathspec` (key at line65), in the same order | A1, OTHER4 |
| `007-mechanics-reader.log:7` | `"started_utc": "2026-10-17T14:42:00.805701+00:00"` | B1 |
| `012-mechanics-reader.log:7` | `"started_utc": "2026-10-17T14:42:57.700823+00:00"` | B1 |

The five owner paths are the four exact paths at lines67-70 above plus existing A's
`docs/plans/2026-10-02-x5-equity-curve.png`. Each appears four times: twice in dirty-list headers and twice
as its own receipt JSON string. Thus OTHER2026-10-03 x12 and OTHER2026-10-06 x4; A2026-10-02 x4;
B2026-10-17 x2. R15-SEAL-C additionally classified compact20261003 x6 at line1 of numbered commit logs
004/005/009/010/014/015. Actual source scan:29 logs; A4/B2/C6/OTHER16, exit1. No date in research data was
identified by these hits. The PNG contents were not opened, moved, deleted, staged or changed.

## Narrow source amendment, implemented pending PM launch verification

R15-SEAL-D authorizes class D only for the four explicitly named additional owner PNG paths. Require the exact full path and
exact ISO hit span in a numbered native wave console, in either (a) entries of the exact dirty-list header above
or (b) a JSON string entry of the parsed `dirty_outside_pathspec` array in a bounded-reader receipt. Reject other
fields, free text, data dates, other filenames, path prefixes/suffixes and additional hits even on a qualifying
line. Existing A/B/C predicates are unchanged. The checker now implements this scope. Focused controls:37 PASS
(21 original plus16 D controls); actual29-log rescan A4/B2/C6/D16, OTHER0. Evidence
`cells/logs/p9h-seald-controls.txt` and `p9h-seald-attempt2-rescan.txt`. Checker SHA-256
`fd1aa3e60bf63c0988f74fa3fc0994d4dd9523904bd715079dbf5c7ef3eea551`.

The native CLI requires token/ruling pairs; it cannot express source predicates. Only after PM's source ruling
and checker evidence, the same-manifest continuation needs ISO2026-10-02 for the actual A4 sources,
ISO2026-10-17 for actual B2 timestamps, ISO2026-10-03 and ISO2026-10-06 for the exact D sources, plus the existing
compact20261003=R15-SEAL-C. R15-SEAL-D is the ruling id for the four ISO tokens. Source auditing must still stop any other occurrence of
these tokens. This is not a generic date allowance, and future calendar dates receive no prospective allowance.

## Supported native reuse

`research_wave.py:53-66` parses repeatable seal allowances into `w.seal_allow`, without editing the manifest.
`wave_seal.py:91-95` merges them with verify's carried rulings. `wave_stage_preflight.py:29-38` fingerprints the
manifest, parent, fields and template, not seal allowances. `wave_stage_cell.py:428-443` fingerprints the cell
spec and combined w payload for completed run/match; `wave_stage_record.py:391-400` fingerprints verify inputs.
`atx-engine/tools/stage_chain.py:252-294` rechecks each ok receipt's bound inputs and skips its run function;
only pending stages execute. `read` at148-157 reads the normal ok filename, leaving failed receipts alone;
`failed_path` at159-165 allocates a new suffix; `write` at300-304 uses exclusive-create, never overwrite.

Actual read-only `research_cycle.py wave status scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json`:
preflight/register/screen/spec/run/match all done, verify pending, judge/record blocked. Evidence
`cells/logs/p9h-attempt2-native-status.txt`. Thus a PM-released same-manifest wave invocation can reuse all six
completed stages and continue verify, then judge/record, with no fit/w/card/NAV rerun or spec amendment.
Stale bindings would refuse, not silently rerun. The failed verify receipt and all current32 wave files remain.
The judge's first monitor/summ/bundle computations would be new registered work, not reruns. Raw console must
use a distinct resumed log; all-source/pin/void/mechanics, then rho-first, then paired read order stays unchanged.

Native generated one-file commits, each with prescribed trailer/exclusion: `6f19b1e5` initial spec,
`b1a9f925` calibrated spec, `bd837403` plain gm recipe. Frozen executable source remains5f48a25d; runtime
driver is the reviewed native commit support plus committed metadata, without an executable rebuild. Release27/27
and Debug27/27 hashes match; CRT matches. The first Debug check accidentally targeted unused build-equity-dbg
and emitted no rows; corrected verification explicitly targets receipt BuildDir build-equity and checks27.
All32 wave-file hashes are in `p9h-attempt2-stopped-files-sha256.txt`; native commit scope in
`p9h-attempt2-generated-commits.txt`. Cache686, no index/research-store. Ledger134, unchanged SHA
`b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`.
R15-H-VERIFY-RESUME preservation: all32 wave files rehashed unchanged, including first six successful receipts,
the failed verify and both mechanics reader outputs. Additional197 output/run files across fit, card, actual
keyed w output (`mega-p9w-train-theme-cov-hedge-1`) and NAV are hashed in
`cells/logs/p9h-attempt2-output-files-sha256.txt`. An initial directory list used the unkeyed w base and stopped
before writing a list; corrected to the actual -1 directory. No output was modified. Native status still shows
the same six done stages and pending verify; no new run was launched. PM final-report.md remains untouched.

Receipt phase metadata (`p9h-attempt2-phases-final.json`): fresh fit3.078s, card24.781s, w35.610s, NAV52.859s,
sum116.328s. The u51.578s receipt is reused from2026-10-16, not measured by this invocation. No engine-fit,
cache-index or engine-timing flag in those commands. The earlier stage/phase metadata extract requested the
wrong wall_seconds key and returned null; the final extract correctly reads phase_rows.seconds. No stats rerun.
No complete wave/scoreboard result exists; no throughput pass claim. G-P1 zero-spec-edits-on-retry remains UNMET,
and interruptions remain disclosed. This is a seal-verification driver stop, not REJECTED/UNDEFINED/VOID.
The full void checklist and H verdict are pending; predictions/printed statistics remain unread. P9-M unreleased.
