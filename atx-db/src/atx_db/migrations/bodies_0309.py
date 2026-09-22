"""Retire leaf-module declarations while preserving the published factor meanings."""

from __future__ import annotations

import json

import duckdb

from ._runner import Migration

RETIRED_MODULES = (
    "altman_distress", "beneish_m_score", "net_operating_assets", "rsst_accruals",
    "quarterly_working_capital_accruals", "asset_turnover_change", "annual_margin_change",
    "quarterly_gross_margin_change", "quarterly_profitability_change", "external_financing",
    "net_debt_financing", "net_issuance", "net_payout", "enterprise_yield", "rd_intensity",
    "rd_increase", "tax_expense_momentum", "tax_to_book_income",
)
REPLACEMENT = "atx_db.derived_factor_projection"
PROJECTION_SOURCE = "atx-db derived factor projection v1"

# These four original frame-based research factors were never added to the
# factor registry. Declare their existing formulas; do not overwrite a user's
# or a later migration's declaration if one already exists.
MISSING_DECLARATIONS = (
    ("efficiency_annual_asset_turnover_change", "PIT annual change in total-asset turnover",
     "fundamental_efficiency", "revenue_t/assets_t_1-revenue_t_1/assets_t_2", ("revenue", "total_assets")),
    ("profitability_annual_net_margin_change", "PIT annual change in net profit margin",
     "fundamental_profitability", "net_income_t/revenue_t-net_income_t_1/revenue_t_1", ("net_income", "revenue")),
    ("profitability_annual_operating_margin_change", "PIT annual change in operating margin",
     "fundamental_profitability", "operating_income_t/revenue_t-operating_income_t_1/revenue_t_1", ("operating_income", "revenue")),
    ("profitability_annual_gross_margin_change", "PIT annual change in gross margin",
     "fundamental_profitability", "gross_profit_t/revenue_t-gross_profit_t_1/revenue_t_1", ("gross_profit", "revenue")),
)


def _retire_declarations(conn: duckdb.DuckDBPyConnection) -> None:
    declarations = [f"atx_db.{name}" for name in RETIRED_MODULES]
    placeholders = ",".join("?" for _ in declarations)
    conn.execute(
        f"UPDATE factor_definition SET declared_in=? WHERE declared_in IN ({placeholders})",
        [REPLACEMENT, *declarations],
    )
    for factor_id, name, family, expression, metrics in MISSING_DECLARATIONS:
        conn.execute(
            """
            INSERT INTO factor_definition (
                factor_id,factor_name,family,description,expression,input_ids_json,
                direction,lookback_days,neutralization_spec_json,unit,sign,scale,
                is_point_in_time_safe,available_at_policy,declared_in,owner,source,
                standardization_spec_json,valid_from,valid_to
            ) SELECT ?,?,?,
                'Annual filing observations with positive denominators, 300-430 day fiscal gaps, '
                '550-day reporting age and a governed monthly cohort.',?,?,1,550,
                '{"method":"none","by":[]}','normalized_score','higher_is_better','zscore',true,
                'Published at the 22:00 monthly decision cutoff after all selected inputs and peers are available.',
                ?,'atx-db',?,
                '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"minimum_names_per_date":20}',
                DATE '1900-01-01',NULL
            WHERE NOT EXISTS (SELECT 1 FROM factor_definition WHERE factor_id=?)
            """,
            [factor_id, name, family, expression, json.dumps([f"metric:{metric}" for metric in metrics]),
             REPLACEMENT, PROJECTION_SOURCE, factor_id],
        )


MIGRATIONS = [Migration(version=309, name="retire_per_metric_module_declarations", up=_retire_declarations)]
