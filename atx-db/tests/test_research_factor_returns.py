"""P4 factor-return layer: one end-to-end fixture run (planted market, betas, sorts, delisting).

The fixture plants daily returns r = beta * M + eps with eps demeaned by the verified
caps' weights and sum(w * beta) = 1, so the value-weighted market over verified linked
lines equals the planted M exactly when (and only when) the prior-session caps weight it.
Monthly labels are the real R3a publisher's; the R2a panel and R2b version are written in
their own schemas.
"""

from __future__ import annotations

import datetime as dt
import json
import math

import duckdb
import numpy as np
import pandas as pd

from atx_db.connection import DuckDBStore
from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
from atx_db.research import evaluation as ev
from atx_db.research import factor_returns as fr
from atx_db.research import features as rf
from atx_db.research.labels import MONTHLY_LABEL_SOURCE, MonthlyForwardLabelOptions, refresh_monthly_forward_labels
from atx_db.research.store import ResearchStore
from atx_db.universe_us_listed import UNIVERSE_SOURCE_NAME

SESSIONS = [day.date() for day in pd.bdate_range("2021-01-04", "2023-06-30")]
N = 48
NYSE = {i for i in range(N) if i % 3 == 0}
UNVERIFIED = {40, 41, 42, 43, 44}          # vendor/archive share basis: never a weight
UNLINKED = {45, 46}                        # eligible lines without an owner link (DEI caps in market_daily)
VERIFIED_LINKED = [i for i in range(N) if i not in UNVERIFIED | UNLINKED]
DELISTED, LAST_TRADE = 41, dt.date(2022, 8, 15)
SPIKE_NAME, SPIKE_DAY = 3, dt.date(2022, 3, 15)
MEMBERSHIP_FROM = dt.date(2021, 7, 1)      # point-in-time venue evidence starts here
NEGATIVE_BOOK = 9                          # out of the B/M domain at every formation
INDUSTRIES = ("NoDur", "BusEq", "Hlth")
CHARACTERISTICS = {"book_to_market": 1, "operating_profitability": 1, "asset_growth": -1, "momentum_12_1": 1}


def _sid(i: int) -> str:
    return f"S{i:03d}"


def _bulk(con, sql, frame):
    con.register("_rows", frame)
    try:
        con.execute(sql)
    finally:
        con.unregister("_rows")


def _plant():
    rng = np.random.default_rng(7)
    t = len(SESSIONS)
    market = rng.normal(0.0004, 0.01, t)
    caps = 1e8 * (1 + np.arange(N)) * (1.2 + np.sin(np.arange(N)))
    weights = caps[VERIFIED_LINKED] / caps[VERIFIED_LINKED].sum()
    beta = 0.5 + np.arange(N) / (N - 1)
    beta = beta / float(weights @ beta[VERIFIED_LINKED])
    eps = rng.normal(0.0, 0.015, (t, N))
    eps -= (eps[:, VERIFIED_LINKED] @ weights)[:, None]
    returns = beta[None, :] * market[:, None] + eps
    returns[0] = 0.0
    prices = (20.0 + np.arange(N))[None, :] * np.cumprod(1.0 + returns, axis=0)
    return market, caps, beta, returns, prices, rng


def _warehouse(path, caps, prices):
    store = DuckDBStore(path)
    store.connection = duckdb.connect(str(path), config={"threads": 1, "memory_limit": "256MB"})
    store._configure_session(store.con)
    store.analytical_memory_limit = "256MB"
    store.analytical_threads = 1
    store._initialized = True
    con = store.con
    con.execute("""
        CREATE TABLE equity_daily_bars (source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP, vendor_security_id VARCHAR,
            source_loaded_at TIMESTAMP);
        CREATE TABLE trading_calendar (calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR);
        CREATE TABLE delisting_terminal_returns (terminal_return_id VARCHAR, security_id VARCHAR, delist_date DATE,
            terminal_return DOUBLE, terminal_return_source VARCHAR, return_observation_id VARCHAR,
            available_at TIMESTAMP, source_loaded_at TIMESTAMP);
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, security_id VARCHAR NOT NULL,
            symbol VARCHAR, as_of_date DATE NOT NULL, horizon_days INTEGER NOT NULL, forward_end_date DATE,
            raw_forward_return DOUBLE, terminal_return DOUBLE, forward_return DOUBLE NOT NULL,
            is_delisted_in_horizon BOOLEAN NOT NULL, is_stitched BOOLEAN NOT NULL, delist_date DATE,
            terminal_return_source VARCHAR, return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true, available_at TIMESTAMP NOT NULL, run_id VARCHAR,
            price_basis VARCHAR, calculation_version VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now());
        CREATE TABLE universe_us_listed_membership (universe_id VARCHAR, security_id VARCHAR, valid_from DATE,
            valid_to DATE, available_at TIMESTAMP, as_of_date DATE, exchange_code VARCHAR, source VARCHAR);
        CREATE TABLE market_daily_metrics (market_daily_id VARCHAR, source VARCHAR, security_id VARCHAR,
            trade_date DATE, market_cap DOUBLE, shares_source VARCHAR, available_at TIMESTAMP, as_of_date DATE);
    """)
    _bulk(con, "INSERT INTO trading_calendar SELECT 'XNYS', day, true, 'equity_daily_bars calendar' FROM _rows",
          pd.DataFrame({"day": pd.to_datetime(SESSIONS)}))
    bars, caps_rows = [], []
    for t, day in enumerate(SESSIONS):
        for i in range(N):
            if i == DELISTED and day > LAST_TRADE:
                continue
            bars.append((_sid(i), day, prices[t, i]))
            cap = caps[i] * (50.0 if (i, day) == (SPIKE_NAME, SPIKE_DAY) else 1.0)
            caps_rows.append((_sid(i), day, cap, "archive" if i in UNVERIFIED else "dei"))
    frame = pd.DataFrame(bars, columns=["security_id", "trade_date", "price"])
    frame["trade_date"] = pd.to_datetime(frame["trade_date"])
    _bulk(con, "INSERT INTO equity_daily_bars SELECT 'prices', security_id, security_id, trade_date, price, price, "
               "trade_date + INTERVAL 22 HOUR, 'v', TIMESTAMP '2023-07-02' FROM _rows", frame)
    frame = pd.DataFrame(caps_rows, columns=["security_id", "trade_date", "cap", "src"])
    frame["trade_date"] = pd.to_datetime(frame["trade_date"])
    _bulk(con, f"INSERT INTO market_daily_metrics SELECT security_id || '-' || trade_date, '{MARKET_DAILY_SOURCE_NAME}', "
               "security_id, trade_date, cap, src, trade_date + INTERVAL 21 HOUR, trade_date FROM _rows", frame)
    delist = SESSIONS[SESSIONS.index(LAST_TRADE) + 1]
    con.execute("INSERT INTO delisting_terminal_returns VALUES ('t41', ?, ?, -0.4, 'observed', 'obs-41', ?, ?)",
                [_sid(DELISTED), delist, dt.datetime.combine(delist, dt.time(12)) + dt.timedelta(days=2),
                 dt.datetime(2022, 8, 20)])
    _bulk(con, f"INSERT INTO universe_us_listed_membership SELECT 'us_listed_v1', security_id, DATE "
               f"'{MEMBERSHIP_FROM}', NULL, TIMESTAMP '{MEMBERSHIP_FROM} 22:00:00', DATE '{MEMBERSHIP_FROM}', "
               f"exchange_code, '{UNIVERSE_SOURCE_NAME}' FROM _rows",
          pd.DataFrame({"security_id": [_sid(i) for i in range(N)],
                        "exchange_code": ["XNYS" if i in NYSE else "XNAS" for i in range(N)]}))
    refresh_monthly_forward_labels(store, MonthlyForwardLabelOptions(run_id="p4_fixture"))
    store.connection.close()


def _research(tmp_path, caps, rng):
    warehouse = tmp_path / "warehouse.duckdb"
    research = ResearchStore(tmp_path / "research.duckdb", warehouse_path=warehouse, memory_limit="256MB")
    research.open()
    con = research.con
    spec = json.dumps({"size_policy": {"verified_status": "verified_dei_shares",
                                       "unverified_status": "unverified_vendor_shares",
                                       "size_features": ["market_cap"]}})
    con.execute("""
        INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
            fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
            definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
            end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
        VALUES ('panel_recon', 'complete', 'reconstructed', 'u', 'x', 'u', 'f', 'm', 'q', ?, 'x', '[]', 'x', 'x',
                '{}', 'x', '2021-01-01', '2023-06-01', '2023-07-01', '2023-07-02', 'w', '[]', ?, '2023-07-02')
    """, [spec, "a" * 64])
    months = sorted({(d.year, d.month) for d in SESSIONS})
    formed = []
    for year, month in months:
        end = max(d for d in SESSIONS if (d.year, d.month) == (year, month))
        later = [d for d in SESSIONS if d > end]
        con.execute("""INSERT INTO research_panel_calendar (run_id, month_start, expected_session,
                           last_observed_session, formation_date, cutoff, entry_date, status, eligible_members)
                       VALUES ('panel_recon', ?, ?, ?, ?, ?, ?, ?, ?)""",
                    [dt.date(year, month, 1), end, end, end if later else None,
                     dt.datetime.combine(end, dt.time(22)) if later else None, later[0] if later else None,
                     "formed" if later else "missing_next_session", N])
        if later:
            formed.append(end)
    cohort, values, matrix, context = [], [], [], []
    chars: dict[tuple[dt.date, int], dict[str, float]] = {}
    for day in formed:
        cutoff = dt.datetime.combine(day, dt.time(22))
        for i in range(N):
            if i == DELISTED and day >= LAST_TRADE:
                continue
            linked = i not in UNLINKED
            cohort.append((day, _sid(i), f"{i:010d}" if linked else None,
                           "valid" if linked else "missing_owner_link", True if linked else None))
            if not linked:
                continue
            values.append((day, _sid(i), caps[i],
                           "unverified_vendor_shares" if i in UNVERIFIED else "verified_dei_shares"))
            draw = {"book_to_market": rng.uniform(0.1, 2.0), "operating_profitability": rng.normal(0.1, 0.05),
                    "asset_growth": rng.normal(0.05, 0.1), "momentum_12_1": rng.normal(0.1, 0.3)}
            if i == NEGATIVE_BOOK:
                draw["book_to_market"] = -0.2
            chars[(day, i)] = draw
            for feature, sign in CHARACTERISTICS.items():
                domain = "negative_book" if feature == "book_to_market" and i == NEGATIVE_BOOK else rf.IN_DOMAIN
                matrix.append(("fv_recon", day, _sid(i), feature, rf.OWNER_BASIS_LINKED, sign, cutoff,
                               draw[feature], domain))
            context.append(("fv_recon", day, _sid(i), rf.OWNER_BASIS_LINKED, f"{i:010d}",
                            INDUSTRIES[(i // 3) % 3]))
    _bulk(con, """INSERT INTO research_panel_cohort (run_id, formation_date, security_id, owner_cik, identity_basis,
                                                     cohort_reason, eligible, primary_line)
                  SELECT 'panel_recon', day, sid, cik, 'x', reason, true, primary_line FROM _rows""",
          pd.DataFrame(cohort, columns=["day", "sid", "cik", "reason", "primary_line"]).astype({"day": "datetime64[ns]"}))
    _bulk(con, """INSERT INTO research_panel_values (run_id, formation_date, security_id, feature_id, metric_code,
                      metric_window, raw_value, reason, available_at, identity_basis, universe_basis,
                      availability_basis, size_status)
                  SELECT 'panel_recon', day, sid, 'market_cap', 'market_cap', 'daily', cap, 'valid',
                         day + INTERVAL 21 HOUR, 'x', 'u', 'm', status FROM _rows""",
          pd.DataFrame(values, columns=["day", "sid", "cap", "status"]).astype({"day": "datetime64[ns]"}))
    rf.ensure_feature_schema(con)
    con.execute(f"""
        INSERT INTO research_feature_versions (feature_version, status, basis, panel_run_id, panel_sha256,
            classification_basis, values_sha256, query_version, universe_rule, spec_json, spec_sha256, code_sha256,
            catalog_sha256, inputs_json, inputs_sha256, blockers_json, created_at, finished_at)
        VALUES ('fv_recon', 'sealed', 'reconstructed', 'panel_recon', ?, 'current_sic_backcast:FAMA_FRENCH_12', 'v',
                '{rf.QUERY_VERSION}', '{rf.UNIVERSE_RULE}', ?, 'x', 'x', ?, '{{}}', 'x', '[]', ?, ?)
    """, ["a" * 64, json.dumps({"store_schema": ev.MIN_FEATURE_STORE_SCHEMA}), "c" * 64, dt.datetime(2023, 7, 2),
          dt.datetime(2023, 7, 2)])
    _bulk(con, """INSERT INTO research_feature_matrix (feature_version, formation_date, security_id, feature_id,
                      owner_basis, expected_sign, available_at, raw_value, domain_status)
                  SELECT * FROM _rows""",
          pd.DataFrame(matrix, columns=["v", "day", "sid", "f", "ob", "sign", "at", "raw", "dom"]).astype(
              {"day": "datetime64[ns]"}))
    _bulk(con, "INSERT INTO research_feature_context (feature_version, formation_date, security_id, owner_basis, "
               "owner_cik, industry_group) SELECT * FROM _rows",
          pd.DataFrame(context, columns=["v", "day", "sid", "ob", "cik", "ind"]).astype({"day": "datetime64[ns]"}))
    return research, formed, chars


def _vw(weights, returns, members):
    members = np.asarray(members)
    return float(weights[members] @ returns[members] / weights[members].sum())


def test_factor_layer_end_to_end_on_a_planted_fixture(tmp_path):
    market, caps, beta, returns, prices, rng = _plant()
    _warehouse(tmp_path / "warehouse.duckdb", caps, prices)
    research, formed, chars = _research(tmp_path, caps, rng)
    risk_free = fr.RiskFreeSeries.from_observations([(dt.date(2020, 12, 31), 5.0), (dt.date(2022, 6, 1), 2.0)],
                                                    source="fixture")
    spec = fr.FactorSpec(run_id="p4_fixture", feature_version="fv_recon",
                         label_cutoff=dt.datetime(2023, 7, 10, tzinfo=dt.UTC), nyse_min_names=5,
                         min_portfolio_names=1, verify_panels=False, formation_chunk=5)
    result = fr.build_factor_returns(research, spec, risk_free=risk_free)
    con = research.con
    assert result.status == "complete" and result.results_sha256
    assert "ff49_industry_returns_not_built_needs_ff49_classification_version" in result.blockers

    # Every row is labeled with its basis, venue basis and risk-free basis.
    for table in ("research_factor_returns", "research_factor_exposures", "research_factor_segments",
                  "research_factor_breakpoints"):
        unlabeled = con.execute(f"SELECT count(*) FROM {table} WHERE run_id='p4_fixture' AND (basis IS NULL "
                                "OR venue_basis IS NULL OR rf_basis IS NULL)").fetchone()[0]
        assert unlabeled == 0, table

    # Daily VW market: prior-session verified caps only (planted identity VW == M).
    daily = con.execute("""SELECT period_date, window_start, factor_id, value, n_held, n_names, n_weighted,
                                  n_excluded_unverified, n_excluded_no_weight, excluded_cap_share, rf_basis
                           FROM research_factor_returns WHERE run_id='p4_fixture' AND frequency='daily'""").df()
    daily["period_date"] = pd.to_datetime(daily["period_date"]).dt.date
    vw = daily[daily["factor_id"] == "mkt_vw"].set_index("period_date")
    after_spike = SESSIONS[SESSIONS.index(SPIKE_DAY) + 1]
    planted = pd.Series(market, index=SESSIONS)
    exact = vw.drop(index=after_spike)["value"]
    assert len(exact) > 500 and float((exact - planted[exact.index]).abs().max()) < 1e-12
    spiked = caps.copy()
    spiked[SPIKE_NAME] *= 50.0
    t = SESSIONS.index(after_spike)
    assert abs(vw.loc[after_spike, "value"] - _vw(spiked, returns[t], VERIFIED_LINKED)) < 1e-12
    same_day = _vw(spiked, returns[SESSIONS.index(SPIKE_DAY)], VERIFIED_LINKED)
    assert abs(vw.loc[SPIKE_DAY, "value"] - same_day) > 1e-5      # same-session weights would have differed
    before = vw.loc[dt.date(2022, 5, 2)]
    assert (before["n_held"], before["n_names"], before["n_weighted"], before["n_excluded_unverified"],
            before["n_excluded_no_weight"]) == (48, 48, 41, 5, 2)
    linked_names = [i for i in range(N) if i not in UNLINKED]
    assert abs(before["excluded_cap_share"] - caps[sorted(UNVERIFIED)].sum() / caps[linked_names].sum()) < 1e-12

    # The delisting's terminal return is the name's return on its effective session (EW includes it).
    ew = daily[daily["factor_id"] == "mkt_ew"].set_index("period_date")
    t_star = SESSIONS.index(LAST_TRADE) + 1
    alive = [i for i in range(N) if i != DELISTED]
    assert abs(ew.loc[SESSIONS[t_star], "value"] - (returns[t_star, alive].sum() - 0.4) / N) < 1e-12
    assert ew.loc[SESSIONS[t_star + 1], "n_names"] == N - 1
    assert vw.loc[SESSIONS[t_star + 1], "n_excluded_unverified"] == 4

    # Risk-free: the latest DTB3 discount rate dated before the period start, as a 91-day
    # bond-equivalent yield y = 365 d / (360 - 91 d); mkt_rf = VW - rf.
    rfd = daily[daily["factor_id"] == "rf"].set_index("window_start")
    rfd.index = pd.to_datetime(rfd.index).date
    for start, discount in ((dt.date(2022, 6, 1), 0.05), (dt.date(2022, 6, 2), 0.02)):
        bey = 365 * discount / (360 - 91 * discount)
        assert abs(rfd.loc[start, "value"] - ((1 + bey) ** (1 / 252) - 1)) < 1e-15
    excess = daily[daily["factor_id"] == "mkt_rf"].set_index("period_date")["value"]
    rf_by_day = daily[daily["factor_id"] == "rf"].set_index("period_date")["value"]
    assert float((excess - (vw["value"] - rf_by_day)).abs().max()) < 1e-15
    assert set(daily["rf_basis"]) == {fr.RF_DTB3}

    # Planted 2x3 sort reproduced exactly (NYSE point-in-time breakpoints; negative book out of HML).
    monthly = con.execute("""SELECT period_date, factor_id, value, venue_basis, status FROM research_factor_returns
                             WHERE run_id='p4_fixture' AND frequency='monthly'""").df()
    monthly["period_date"] = pd.to_datetime(monthly["period_date"]).dt.date
    day = dt.date(2022, 6, 30)
    entry = SESSIONS[SESSIONS.index(day) + 1]
    label = dict(con.execute("""SELECT security_id, forward_return FROM forward_returns_survivorship_safe
                                WHERE source=? AND as_of_date=? AND horizon_days=21""",
                             [MONTHLY_LABEL_SOURCE, entry]).fetchall())
    names = np.array(VERIFIED_LINKED)
    cap = caps[names]
    ret = np.array([label[_sid(i)] for i in names])
    nyse = np.array([i in NYSE for i in names])
    size_cut = np.percentile(cap[nyse], 50)
    small = cap < size_cut        # a cap equal to the NYSE median is big (R3b size-bucket rule)

    def factor(feature, long_group, short_group):
        x = np.array([chars[(day, i)][feature] if not (feature == "book_to_market" and i == NEGATIVE_BOOK)
                      else np.nan for i in names])
        lo, hi = np.percentile(x[nyse & np.isfinite(x)], [30, 70])
        group = np.where(x <= lo, 1, np.where(x <= hi, 2, 3))
        leg = {(s, g): _vw(cap, ret, np.flatnonzero((small == s) & (group == g) & np.isfinite(x)))
               for s in (True, False) for g in (1, 2, 3)}
        value = 0.5 * (leg[(True, long_group)] + leg[(False, long_group)]) \
            - 0.5 * (leg[(True, short_group)] + leg[(False, short_group)])
        return value, np.mean([leg[(True, g)] for g in (1, 2, 3)]) - np.mean([leg[(False, g)] for g in (1, 2, 3)])

    at_day = monthly[monthly["period_date"] == day].set_index("factor_id")
    hml, smb_bm = factor("book_to_market", 3, 1)
    rmw, smb_op = factor("operating_profitability", 3, 1)
    cma, smb_inv = factor("asset_growth", 1, 3)
    umd, _ = factor("momentum_12_1", 3, 1)
    for name, expected in (("hml", hml), ("rmw", rmw), ("cma", cma), ("umd", umd), ("smb_ff3", smb_bm),
                           ("smb", (smb_bm + smb_op + smb_inv) / 3)):
        assert abs(at_day.loc[name, "value"] - expected) < 1e-12, name
        assert at_day.loc[name, "venue_basis"] == fr.VENUE_NYSE_PIT
    early = monthly[(monthly["period_date"] == dt.date(2021, 3, 31)) & (monthly["factor_id"] == "hml")]
    assert early["venue_basis"].tolist() == [fr.VENUE_ALL_NAMES] and early["status"].tolist() == ["computed"]
    hlth = [i for i in VERIFIED_LINKED if INDUSTRIES[(i // 3) % 3] == "Hlth"]
    assert abs(at_day.loc["ind_fama_french_12_hlth", "value"]
               - _vw(caps, np.array([label.get(_sid(i), np.nan) for i in range(N)]), hlth)) < 1e-12
    assert abs(at_day.loc["mkt_vw", "value"] - _vw(cap, ret, np.arange(len(names)))) < 1e-12

    # Exposures: planted 252-session market betas recovered within 2 SE; nothing after the formation.
    last = formed[-1]
    exposures = con.execute("""SELECT e.*, s.size_segment FROM research_factor_exposures e
                               JOIN research_factor_segments s USING (run_id, formation_date, security_id)
                               WHERE e.run_id='p4_fixture' AND e.formation_date=?""", [last]).df()
    index = exposures["security_id"].str[1:].astype(int).to_numpy()
    z = (exposures["beta_mkt_252d"].to_numpy() - beta[index]) / exposures["se_beta_mkt_252d"].to_numpy()
    assert (exposures["status_252d"] == fr.EXPOSURE_ESTIMATED).all() and len(exposures) == N - 1
    assert np.mean(np.abs(z) <= 2.0) >= 0.9 and np.max(np.abs(z)) < 4.0
    assert abs(np.median(exposures["ivol_252d"]) / (0.015 * math.sqrt(252)) - 1) < 0.1
    assert (pd.to_datetime(exposures["available_at"]) <= pd.Timestamp(dt.datetime.combine(last, dt.time(22)))).all()
    linked36 = exposures[exposures["owner_basis"] == rf.OWNER_BASIS_LINKED]
    assert (linked36["n_monthly_obs"] >= 24).all() and (linked36["status_36m"] == fr.EXPOSURE_ESTIMATED).all()
    assert set(exposures.loc[exposures["owner_basis"] == rf.OWNER_BASIS_UNLINKED, "size_segment"]) == {"unknown"}
    burn_in = con.execute("SELECT DISTINCT status_252d FROM research_factor_exposures WHERE run_id='p4_fixture' "
                          "AND formation_date=?", [dt.date(2021, 6, 30)]).fetchall()
    assert burn_in == [("insufficient_obs",)]

    # span_test: a spanned series has no alpha; a planted alpha is significant (EWC robust).
    gen = np.random.default_rng(11)
    index = pd.date_range("1999-01-31", periods=300, freq="ME")
    factors = pd.DataFrame({"mkt_rf": gen.normal(0.006, 0.045, 300), "hml": gen.normal(0.003, 0.03, 300)},
                           index=index)
    spanned = pd.Series(0.5 * factors["hml"] + gen.normal(0.0, 0.01, 300), index=index)
    null = fr.span_test(spanned, factors)
    alpha = fr.span_test(spanned + 0.004, factors)
    assert null.alpha_robust_p > 0.05 and abs(null.betas["hml"] - 0.5) < 0.05
    assert alpha.alpha_robust_p < 1e-3 and abs(alpha.alpha - null.alpha - 0.004) < 1e-12
    wide = fr.load_factor_returns(research, "p4_fixture")
    run_span = fr.span_test_against_run(research, "p4_fixture", wide["umd"], factors=("mkt_rf", "hml"))
    assert run_span.n_obs == int(monthly[(monthly["factor_id"] == "umd") & monthly["value"].notna()].shape[0])

    # h > 1: a 3-month series spanned by UMD is tested on the run's exact 3-month label-window
    # rows (monthly_h3): no alpha (store path; a PeriodIndex series is accepted). On 1-month
    # factors (the old path) a spanned series shows a spurious alpha, on h-month windows it
    # does not (the reviewer's T=600 case).
    umd3 = fr.load_factor_returns(research, "p4_fixture", frequency="monthly_h3", factors=("umd",))["umd"].dropna()
    spanned3 = umd3 + np.random.default_rng(5).normal(0.0, 0.002, len(umd3))
    spanned3.index = spanned3.index.to_period("M")
    res3 = fr.span_test_against_run(research, "p4_fixture", spanned3, factors=("umd",), horizon_periods=3)
    assert res3.n_obs == len(spanned3) >= 24
    assert res3.factor_window_basis == fr.WINDOW_EXACT and res3.significance_claimable
    assert abs(res3.betas["umd"] - 1.0) < 0.05 and res3.alpha_robust_p > 0.05
    gen3 = np.random.default_rng(13)
    umd = pd.DataFrame({"umd": gen3.normal(0.008, 0.04, 600)}, index=pd.date_range("1970-01-31", periods=600, freq="ME"))
    target = fr.compound_factor_windows(umd, 3)["umd"]
    target = target + gen3.normal(0.0, 0.01, len(target))
    aligned = fr.span_test(target, fr.compound_factor_windows(umd, 3), horizon_periods=3)
    misaligned = fr.span_test(target, fr.compound_factor_windows(umd, 1), horizon_periods=3)
    assert aligned.alpha_robust_p > 0.05 and misaligned.alpha_robust_p < 1e-3
    research.close()
