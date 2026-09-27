# Task T7 — IC runner: load pinned extra PIT fields into the DSL panel

Owner worktree: `C:/atx-wt/pool-4`, new branch `feat/mega-alpha-runner-fields-20260926` from root
HEAD (root will tell you the SHA in the dispatch). Files you own: `atx-impl/src/strategy_ic_runner.hpp/.cpp`,
`atx-impl/tests/strategy_ic_runner_test.cpp` (and a private helper CPP/HPP in `atx-impl/src/` if cleaner —
report CMake lines for root). Do not change public engine headers (`atx-engine/include/...`) or the
target/NAV replay files.

## Why
New subalpha families need non-price point-in-time fields (short interest, implied/historical vol,
size, a broadcast market return `mkt_ret`). A Python producer (task T6, pool-8 branch
`feat/mega-alpha-fields-20260926`, file `atx-engine/tools/prepare_research_fields.py`) writes them
aligned to the existing role payload axes. The runner must load them next to close/raw_close/volume.

## Producer output contract (from the T6 brief; read the producer in pool-8 read-only if committed and
follow its exact key names; report any mismatch)
Directory `build-equity/<role>-fields-v1/`: one date-major little-endian f64 file per field
(`<name>.f64`, shape role dates x role instruments, NaN where not visible) + `manifest.json` with schema
`atx.research-role-fields/v1`, the role manifest sha256, role sessions/ids sha256, a field list with
name/source/clock/units/coverage, and per-file sha256 and bytes. Field names are plain identifiers
(no dots), e.g. `si_shares`, `si_dtc`, `iv_atm_30`, `hv_30`, `earn_recent`, `shares_out`,
`mktcap_lagged`, `is_common`, `mkt_ret`.

## Requirements
1. CLI/config: `--train-fields DIR --train-fields-sha256 SHA` and `--validation-fields DIR
   --validation-fields-sha256 SHA` (manifest sha). Optional; default = today's behaviour, bit-identical.
2. Before loading payloads (also under `--plan-only`): verify manifest hash, schema, that its role
   manifest sha equals the pinned role sha, and that its sessions/ids hashes match the role's.
3. Load ONLY the extra fields referenced by at least one library candidate's DSL (collect referenced
   field names from the compiled programs or parser AST). A library referencing an unknown field
   (not in role base fields and not in the pinned fields manifest) fails before payload load with a
   clear message. Verify each loaded file's sha256 (streamed) and byte size.
4. Build the DSL panel with base fields + referenced extra fields so the VM resolves them by name. Keep
   the existing presence/cross-section mask semantics: extra fields are NaN where not visible; do not
   change the member mask. Avoid holding two full copies of the base fields longer than necessary.
5. Memory admission: count 8 bytes/cell per loaded extra field in the existing budget.
6. Candidate cache (T1, `--candidate-cache`): the cache key must also cover the pinned fields manifest
   sha when (and only when) a candidate references an extra field, so a changed field payload can
   never be served from a stale cache entry. Candidates referencing only base fields keep their
   existing keys (existing cache entries stay valid).
7. Record the fields manifest sha and the list of loaded fields in recipe.json/summary (canonical
   recipe hash changes only when fields are supplied).

## Constraints
- House style `C:/atx-wt/pool-4/.agents/cpp/agent.md`. Do NOT compile or run; root builds and returns
  errors. Not TDD: implement, then postimplementation fixtures on tiny synthetic roles: (a) library
  using an extra field evaluates correctly vs hand-computed values; (b) unreferenced fields not loaded;
  (c) unknown field / wrong role binding / tampered file refused before payload load; (d) absent option
  -> outputs bit-identical to before; (e) cache key differs for field-referencing candidates when the
  fields manifest changes, unchanged for base-only candidates.
- No subagents. Commit in pool-4 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
Full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T7-report.md`. Return only:
status, commit SHAs, one-line summary, concerns.
