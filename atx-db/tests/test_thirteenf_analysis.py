from __future__ import annotations

from atx_db.thirteenf_analysis import render_thirteenf_analysis_report


def test_report_separates_amendment_semantics_and_rejects_pooled_signal() -> None:
    summary = {
        "source": {
            "archive_count": 1,
            "submission_rows": 2,
            "holding_rows": 3,
            "minimum_report_period": "2020-03-31",
            "maximum_report_period": "2020-06-30",
            "minimum_filing_date": "2020-05-15",
            "maximum_filing_date": "2020-08-15",
        },
        "amendments": {
            "manager_quarter_rows": 2,
            "amended_manager_quarters": 1,
            "position_corrections": 1,
            "spike_manager_quarters": 1,
        },
        "signals": {
            "candidate_count": 2,
            "signal_count": 1,
            "years_with_signals": 1,
            "signals_per_year": 1.0,
            "mapped_signals": 1,
            "maximum_rank_per_quarter": 20,
            "stress_quarters_excluded": True,
            "amendment_type_cohorts": [
                {
                    "cohort": "RESTATEMENT_ONLY",
                    "selected_signals": 1,
                    "completed_47d_trades": 1,
                    "mean_47d_net_short_return": 0.01,
                    "median_47d_net_short_return": 0.01,
                    "win_rate_47d": 1.0,
                }
            ],
        },
        "disclosed_exits": {
            "followup_manager_positions": 1,
            "exited_manager_positions": 0,
            "disclosed_exit_rate": 0.0,
            "exits_disclosed_within_47_trading_days": 0,
        },
        "backtest_47d": {
            "completed_trades": 1,
            "mean_net_short_return": -0.01,
            "median_net_short_return": -0.01,
            "short_win_rate": 0.0,
            "one_way_slippage_bps": 5.0,
            "minimum_market_cap_usd": 2_000_000_000.0,
            "maximum_market_cap_usd": 10_000_000_000.0,
            "by_stress_regime": [
                {
                    "is_stress_quarter": False,
                    "completed_trades": 1,
                    "mean_net_short_return": -0.01,
                }
            ],
            "horizons": [
                {
                    "trading_days": 47,
                    "completed_trades": 1,
                    "mean_net_short_return": -0.01,
                    "median_net_short_return": -0.01,
                    "short_win_rate": 0.0,
                }
            ],
        },
    }

    report = render_thirteenf_analysis_report(summary)

    assert "RESTATEMENT corrects and supersedes" in report
    assert "RESTATEMENT_ONLY" in report
    assert "**Reject.**" in report
