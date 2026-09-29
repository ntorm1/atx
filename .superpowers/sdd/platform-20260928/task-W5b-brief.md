# Task W5b: PIT fields from the holdings/short stages (13F, FTD, Reg SHO) and the identity-bridge-v2 / delisting role

**Pool:** C:/atx-wt/pool-9. `git status` clean, then `git checkout -B feat/platform-v7-w5b-holdfields-20260928 <BASE>`, BASE
= `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules as W5a: never build C++, never run IC/NAV or any returns-conditioned
statistic, never spawn subagents; C:/atx READ-ONLY (docs ALPHA_PANEL*.md; data under C:/atx/atx-db/data/alpha_panel/v1/
{thirteenf,ftd,regsho_threshold,short_volume_ext,delisting,export/identity-bridge-v2-pit,export/identity-bridge-v2-strict,
security_master} for schema and for running your builder; never join to returns). Never read validation/VAL/2023/2024/2025
statistics. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W5b-report.md (<= 40 lines)
in the pool-2 sprint dir; reply < 12 lines. New module `atx-engine/tools/research_fields_holdings.py` + one registry hook
(<= 15 lines) in prepare_research_fields.py placed BELOW lane W5a's hook comment `# W5a registry hook`; if W5a's hook is
not there yet, add a placeholder comment line for it above yours. No other edits to the main builder.

**Read first:** docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md (U1-U4, D1, D3), C:/atx/atx-db/docs/
ALPHA_PANEL_REQUEST_V7_RESPONSE.md, ALPHA_PANEL_SHORTFLOW.md, ALPHA_PANEL_IDENTITY_SECURITY.md; literature-v7.md S6
(13F families: Cohen-Polk-Silli best ideas, Agarwal et al. confidential holdings, institutional demand; FTD/threshold:
Drechsler-Drechsler shorting premium proxies) and v6-literature.md S4; prepare_research_fields.py (PIT rules, manifest,
sv_ratio126 template); atx-engine/tools/prepare_recent_research.py (how a role is restricted: linked-operating-v1 with
identity-bridge-r4-v1 and fundamental-events-v2; universe rule ids).

**Part A, fields (PIT; 13F visible from filing acceptance, FTD from its SEC publication date, Reg SHO from list date):**
1. 13F (D1), quarterly, forward-filled until the next filing: `inst_own_share` (13F shares held / shares_out),
   `inst_breadth_chg` (change in number of holders, Chen-Hong-Stein), `inst_own_chg_q` (share change q/q),
   `inst_best_ideas` (sum over holders of max(0, position weight in the manager's portfolio - the name's market weight),
   Cohen-Polk-Silli), `inst_n_holders`. Handle amendments and the 45-day filing lag by `available_at`.
2. FTD / Reg SHO (D3): `ftd_shares_ratio21` (sum of fails / shares_out over 21 sessions), `regsho_threshold_days63`
   (sessions on a threshold list in 63), `sv_offexchange_share126` (off-exchange short volume share over 126 sessions
   from short_volume_ext, if the split exists).
Manifest, coverage and byte-identity requirements as W5a.

**Part B, role variant (no fields):** a new universe rule id `linked-operating-v2` in prepare_recent_research.py that
restricts with `export/identity-bridge-v2-pit` (instead of identity-bridge-r4-v1) and marks delisting terminations from
`delisting/events.parquet` (delist_code, delist_return where present) as role attributes, plus a `--delisting-returns
<path>` option that applies the imputed delisting return on the termination session (documented rule; default OFF so
existing roles are unchanged). Output must be a normal role manifest with the universe block reporting dropped counts by
reason, so root can compare kept counts against lo1. Byte-identity test: v1 rule output unchanged.

**Tests:** synthetic fixtures for every stage; PIT visibility tests; forward-fill across a missing quarter; best-ideas
arithmetic on a planted 2-manager case; role v2 unchanged-v1 identity; delisting return application on a synthetic role.

**Root acceptance:** fields built on lo1 within the builder caps; role `linked-operating-v2` built from the same base role
with kept-count and dropped-by-reason tables vs lo1 (identity checks only; no returns read). Report as W5a.
