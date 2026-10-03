# share_exchange_history (`security_master/share_exchange_history.parquet`)

CRSP-style dated share codes (`shrcd`) and exchange codes (`exchcd`) per line (tier1-v3 S2.5 codes, module
`atx_db.alpha_panel.identity_v3 codes`). Rebuilt 2026-10-03 in tier1-v4 Task 1 (fix round 1: clock restamped,
rows otherwise identical to the 2026-09-30 build). Manifest
`security_master/share_exchange_history_manifest.json` (schema `atx.alpha-panel.share-exchange-history/v1`; no
done criterion is defined for it).

- Path: `data/alpha_panel/v1/security_master/share_exchange_history.parquet` (397,050 rows, 2,291,842 bytes).
- Keys: `(security_id, valid_from)`, unique; runs never overlap within a line (0 overlaps measured).
- Span: 2018-01-02 .. 2026-09-18, 22,704 lines. Delisted lines included up to their last panel session.
- Input: the v2 panel's point-in-time security flags (`panel/`, manifest SHA-256 in the manifest).

## Rule

One row per maximal run of consecutive calendar sessions with the same `(exchange, exchcd, shrcd, security_type)`.

- `exchcd`: 1 NYSE (`XNYS`), 2 NYSE American (`XASE`), 3 Nasdaq (`XNAS`), 4 NYSE Arca (`ARCX`), 5 Cboe BZX (`BATS`),
  6 IEX (`IEXG`) (5 and 6 are our extensions), 0 OTC; NULL when the panel `exchange` is NULL (unknown, not OTC).
- `shrcd` (first matching): 73 ETF / ETP (`is_etf`); 31 ADR (`is_adr` or `is_adr_likely`); 14 closed-end fund
  (`security_type` fund); 71 LP; 18 REIT common; 12 foreign private issuer common (`is_fpi`); 11 US common
  (`security_type` common / common_unverified); NULL otherwise (preferred, warrant, unit, right, note, SPAC, unknown).
  CRSP 10 is not used (11 covers ordinary common).

## PIT rule (rule `share-exchange-runs-v2`)

A run is stamped `available_at` = 22:00 UTC of the calendar session before `valid_from`, minus 1 s (first calendar
session: `valid_from` - 1 day, 21:59:59). It is therefore visible from its first session under the lake rule, and
there is one consumption path:

```sql
SELECT security_id, shrcd, exchcd
FROM share_exchange_history_as_of(lake_cutoff(DATE '2024-03-15'))
WHERE DATE '2024-03-15' BETWEEN valid_from AND valid_to
```

It returns the run holding d, including on the first session of a code change, and never shows a run before its
first session (fixture test `test_share_exchange_history_as_of_returns_new_code_from_its_first_session`). The
result equals the interval join `valid_from <= d <= valid_to`.

Why this stamp: the codes are the panel's session-d flags, and a panel row at session d is read at session d. The
inputs behind those flags were checked in `panel.py`:

- SIC and the SEC filer regime (`is_reit`, `is_fpi`, `is_adr_likely`) are as of `issuer_cutoff` = 22:00 UTC of the
  previous session (strict `<`).
- FINRA issue names (`security_type`, `exchange`) are as of `dissemination_date < d`, with `available_at` =
  dissemination date 22:00 UTC. That clock can equal the cutoff (dissemination on the previous session) or fall
  on a non-session day before d.
- The CIK link behind `is_fpi` uses link `available_at <= d 22:00 UTC` (the identity convention).
- The Nasdaq directory snapshot (`directory_type`, `directory_etf`, 2026-09-18) is a non-PIT fallback for
  `security_type` / `is_etf`.

The codes inherit these panel properties and are not more point in time than the panel row.

## Columns

| name | type | unit | definition | source | clock | staleness |
|---|---|---|---|---|---|---|
| security_id | BIGINT | id | vendor line | panel | - | - |
| valid_from / valid_to | DATE | date | first / last session of the run (inclusive) | panel | - | daily |
| sessions | BIGINT | count | sessions in the run | panel | - | - |
| exchcd | INTEGER | code | CRSP-style exchange code (above) | panel `exchange` | available_at | daily |
| exchange | VARCHAR | MIC | panel exchange (`XNYS`, `XNAS`, `XASE`, `ARCX`, `OTC`, NULL) | panel | available_at | daily |
| shrcd | INTEGER | code | CRSP-style share code (above) | panel flags | available_at | daily |
| security_type | VARCHAR | - | panel security type | panel | available_at | daily |
| available_at | TIMESTAMP | UTC | previous session 22:00 UTC - 1 s (visible from valid_from) | - | - | - |

## Measured

Line-sessions by `shrcd`: 11 10,604,499; 12 905,462; 18 554,816; 31 818,236; 71 176,661; 73 6,891,556; NULL
1,917,612. Runs by `exchcd`: 1 69,311 (6.59 M sessions); 2 12,775 (0.64 M); 3 94,665 (6.08 M); 4 40,968 (4.03 M);
0 6; NULL 179,325 (4.52 M); no BATS / IEX lines occur.

Panel `member_equity` cells 2019-2026: `shrcd` non-NULL 100% every year; `exchcd` in 1-6: 2019 98.4%, 2020 97.4%,
2021 95.1%, 2022 97.4%, 2023 97.6%, 2024 97.6%, 2025 96.4%, 2026 96.2% (the rest has a NULL panel exchange).
Member cells with `shrcd` 11: 82-83%; 12: 4.6-7.4%; 18: 4.5-6.4%.

## Known limits

- Runs are fragmented where the panel flags flicker (e.g. SPAC 92,209 runs averaging 6 sessions, common_unverified
  81,067 runs averaging 44): prefer the session-level join over run counts.
- `exchcd` NULL means the panel has no exchange that session, not OTC.
- The share code is derived from vendor / FINRA / SEC flags, not from CRSP; ADR detection includes the
  `is_adr_likely` heuristic (FPI filer with common-like line).
