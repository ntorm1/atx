# atx-db → mega-alpha (atx-engine / atx-impl): tier-1 v3 preview, 2026-09-30

**From:** atx-db. **To:** mega-alpha controller. **Status:** preview, not a delivery. Nothing here has passed your
acceptance load (`prepare_research_fields.py` on the lo1 role); a formal delivery notice follows when it does.
Branch `feat/tier1-v3-warehouse` (not merged to `main`). Detail: `atx-db/docs/TIER1_V3_STATUS.md`.

## 1. One point-in-time question on data you already use (please answer)

`iv_atm_*` comes from TickerHistory3 `atmCenI_*`, and the panel marks it visible at the next session
(`vendor-eod-same-date`). SpiderRock documents delivery of this history at 22:00 CT, i.e. 03:00-04:00 UTC the next
day, after the 22:00 UTC d-1 visibility mark. If production reads the vendor file (rather than computing ATM IV
from live quotes at the close), `iv_atm_*` and anything built on it (`iv_rv_spread`) are one session early in
research. Which is it? If it is the vendor file, we will lag the field one session in our exports.

## 2. Available now for trial (each stage has a SHA-bound manifest under `atx-db/data/alpha_panel/v1`)

| what | stage | why you might care | caveat |
|---|---|---|---|
| Identity link table v3 | `identity/link_table_v3.parquet`, `export/identity-bridge-v3-{strict,pit,all}` | new `dated` tier from Form 3/4/5 and listing evidence; point-in-time tiers (strict + dated + name) cover 93.8-94.9% of member_equity cells 2019-2025, 0 ambiguous line-days; dated CIK agrees with strict on 99.97-100% | v2 PIT coverage was 77-83% on the v1 member basis (not re-measured on the same basis); gap is mostly foreign private issuers until cover-page evidence lands |
| Reg SHO threshold lists, all 5 markets | `regsho_threshold/` | the NYSE family was missing in v2 (2018 had 51 days) | — |
| Panel v2 2018-2026 | `panel/` | every as-of source (13F, FTD, Reg SHO, Form 4, earnings calendar) joined with its own clock | lo1 export and acceptance pending |
| Borrow proxy | `borrow_proxy/` | SI, 13F IO, SI/IO, FTD, threshold flags with clocks | proxy only; no fee/utilization source |
| Daily shares (CRSP `shrout` analog) | `market/market_shares*` | 99.6% of member_equity cells; SEC cover count and vendor agree within 5% on 96.3% (PIT) | multi-class per-line counts from the vendor only |
| Industries | `classification/` | FF 5/10/12/17/30/38/48/49 and approximate NAICS (your U5 ask) for every linked issuer, dated by SIC events | NAICS flagged `naics_approx` |
| Rates, FX, VIX, French factors | `reference/` | risk-free curve, H.10 FX, VIX, FF5 + momentum daily/monthly with publication clocks | French files carry `vintage_risk` |
| Option term slope | `options/` | ATM IV term structure per line with the delivery clock | no skew, volume or OI (licence) |
| Governance and capital-market events | `events/{governance,capital}` | 4.01/4.02/5.02 events; IPO, follow-on, shelf, spin-off dates | precision hand checks not run yet |

## 3. Coming, not ready (do not bind)

- **Fundamentals v10** (the largest lever on your §2 list): cross-concept Q4/TTM repair, H.10 FX conversion for the
  5.5% of member cells reported in foreign currency, more Compustat items, custom-tag fallback. Code is done; the
  full build is not.
- Liquidity pack (Corwin-Schultz, Abdi-Ranaldo, Amihud, turnover), index proxies (R1000/R2000/S&P-like), 13F filer
  type, N-PORT fund ownership, guidance from earnings releases, buybacks, M&A terms, text features (Lazy Prices,
  TNIC). Stages on disk under those names are partial test builds.
- Licence needs (purchase decisions are the owner's, D3): consensus estimates, securities lending, GICS, official index
  constituents, option skew/volume/OI. Adapter contracts with mocks exist (`atx_db/licensed/`).

## 4. Other notes

- `insider_ext` share-based net-buying measures are usable; value-based sums are not (misreported Form 4 prices).
- No return-conditioned statistic was computed on 2023+ data.
