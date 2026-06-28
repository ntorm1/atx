from __future__ import annotations

from dataclasses import astuple, dataclass

from .connection import DuckDBStore


SOURCE_NAME = "SEC companyfacts"


@dataclass(frozen=True)
class FundamentalStatementMapRow:
    source: str
    taxonomy: str
    concept: str
    statement_type: str
    statement_section: str
    canonical_metric: str
    canonical_label: str
    period_type: str
    normal_balance: str
    unit_type: str
    value_multiplier: float
    concept_priority: int
    is_core_metric: bool
    is_active: bool
    notes: str | None = None


FUNDAMENTAL_STATEMENT_MAP_ROWS: tuple[FundamentalStatementMapRow, ...] = (
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "Assets",
        "balance_sheet",
        "assets",
        "assets",
        "Assets",
        "instant",
        "debit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical balance sheet total assets.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "Liabilities",
        "balance_sheet",
        "liabilities",
        "liabilities",
        "Liabilities",
        "instant",
        "credit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical balance sheet total liabilities.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "StockholdersEquity",
        "balance_sheet",
        "equity",
        "stockholders_equity",
        "Stockholders' equity",
        "instant",
        "credit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical balance sheet stockholders' equity.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "CommonStocksIncludingAdditionalPaidInCapital",
        "balance_sheet",
        "equity",
        "common_stock_and_apic",
        "Common stock and additional paid-in capital",
        "instant",
        "credit",
        "monetary",
        1.0,
        20,
        True,
        True,
        "Equity capital component available from SEC companyfacts.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "RevenueFromContractWithCustomerExcludingAssessedTax",
        "income_statement",
        "revenue",
        "revenue",
        "Revenue",
        "duration",
        "credit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Preferred ASC 606 revenue concept when present.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "Revenues",
        "income_statement",
        "revenue",
        "revenue",
        "Revenue",
        "duration",
        "credit",
        "monetary",
        1.0,
        20,
        True,
        True,
        "Legacy/general revenue concept; lower priority than ASC 606 revenue.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "OperatingIncomeLoss",
        "income_statement",
        "profitability",
        "operating_income",
        "Operating income",
        "duration",
        "credit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical operating income or loss.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "NetIncomeLoss",
        "income_statement",
        "profitability",
        "net_income",
        "Net income",
        "duration",
        "credit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical net income or loss.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "NetCashProvidedByUsedInOperatingActivities",
        "cash_flow",
        "operating",
        "operating_cash_flow",
        "Operating cash flow",
        "duration",
        "debit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical net cash provided by operating activities.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "NetCashProvidedByUsedInInvestingActivities",
        "cash_flow",
        "investing",
        "investing_cash_flow",
        "Investing cash flow",
        "duration",
        "debit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical net cash provided by or used in investing activities.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "NetCashProvidedByUsedInFinancingActivities",
        "cash_flow",
        "financing",
        "financing_cash_flow",
        "Financing cash flow",
        "duration",
        "debit",
        "monetary",
        1.0,
        10,
        True,
        True,
        "Canonical net cash provided by or used in financing activities.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "PaymentsToAcquirePropertyPlantAndEquipment",
        "cash_flow",
        "investing",
        "capital_expenditures",
        "Capital expenditures",
        "duration",
        "credit",
        "monetary",
        -1.0,
        10,
        True,
        True,
        "Capital expenditures as signed cash outflow.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "PaymentsForRepurchaseOfCommonStock",
        "cash_flow",
        "financing",
        "share_repurchases",
        "Share repurchases",
        "duration",
        "credit",
        "monetary",
        -1.0,
        10,
        True,
        True,
        "Common stock repurchases as signed cash outflow.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "PaymentsOfDividends",
        "cash_flow",
        "financing",
        "dividends_paid",
        "Dividends paid",
        "duration",
        "credit",
        "monetary",
        -1.0,
        10,
        True,
        True,
        "Dividends paid as signed cash outflow.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "us-gaap",
        "EarningsPerShareDiluted",
        "per_share",
        "earnings",
        "eps_diluted",
        "Diluted earnings per share",
        "duration",
        "credit",
        "per_share",
        1.0,
        10,
        True,
        True,
        "Canonical diluted EPS.",
    ),
    FundamentalStatementMapRow(
        SOURCE_NAME,
        "dei",
        "EntityCommonStockSharesOutstanding",
        "share_count",
        "shares",
        "shares_outstanding",
        "Common shares outstanding",
        "instant",
        "credit",
        "shares",
        1.0,
        10,
        True,
        True,
        "DEI current common shares outstanding concept.",
    ),
)


def seed_fundamental_statement_map(store: DuckDBStore) -> int:
    """Seed canonical statement mappings for public SEC companyfacts concepts."""

    for row in FUNDAMENTAL_STATEMENT_MAP_ROWS:
        store.con.execute(
            """
            INSERT OR REPLACE INTO fundamental_statement_map (
                source,
                taxonomy,
                concept,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                period_type,
                normal_balance,
                unit_type,
                value_multiplier,
                concept_priority,
                is_core_metric,
                is_active,
                notes,
                updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, now())
            """,
            list(astuple(row)),
        )
    return len(FUNDAMENTAL_STATEMENT_MAP_ROWS)


def refresh_fundamental_statement_points(store: DuckDBStore) -> int:
    """Refresh normalized statement facts from mapped SEC fact revisions."""

    seed_fundamental_statement_map(store)
    with store.transaction():
        store.con.execute("DELETE FROM fundamental_statement_points")
        store.con.execute(
            """
            INSERT INTO fundamental_statement_points (
                statement_point_id,
                fact_revision_id,
                revision_group_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                taxonomy,
                concept,
                unit,
                unit_type,
                period_type,
                normal_balance,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                raw_value,
                value,
                previous_raw_value,
                previous_value,
                value_delta,
                value_delta_percent,
                run_id,
                source_url,
                source_loaded_at
            )
            WITH mapped AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            coalesce(CAST(r.period_start AS VARCHAR), ''),
                            CAST(r.period_end AS VARCHAR),
                            r.accession_number
                        )
                    ) AS statement_point_id,
                    sha256(
                        concat_ws(
                            '|',
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            coalesce(CAST(r.period_start AS VARCHAR), ''),
                            CAST(r.period_end AS VARCHAR)
                        )
                    ) AS statement_revision_group_id,
                    r.fact_revision_id,
                    r.source,
                    r.security_id,
                    s.primary_symbol AS symbol,
                    r.cik,
                    m.statement_type,
                    m.statement_section,
                    m.canonical_metric,
                    m.canonical_label,
                    r.taxonomy,
                    r.concept,
                    r.unit,
                    m.unit_type,
                    m.period_type,
                    m.normal_balance,
                    r.period_start,
                    r.period_end,
                    r.filed_date AS as_of_date,
                    r.available_at,
                    r.fiscal_year,
                    r.fiscal_period,
                    r.form,
                    r.accession_number,
                    r.value AS raw_value,
                    r.value * m.value_multiplier AS value,
                    r.run_id,
                    r.source_url,
                    r.source_loaded_at,
                    row_number() OVER (
                        PARTITION BY
                            r.source,
                            r.security_id,
                            m.canonical_metric,
                            r.unit,
                            r.period_start,
                            r.period_end,
                            r.accession_number
                        ORDER BY
                            m.concept_priority,
                            r.available_at DESC NULLS LAST,
                            r.source_loaded_at DESC NULLS LAST,
                            r.taxonomy,
                            r.concept,
                            r.fact_revision_id
                    ) AS canonical_rank
                FROM fundamental_fact_revisions r
                JOIN fundamental_statement_map m
                  ON m.source = r.source
                 AND m.taxonomy = r.taxonomy
                 AND m.concept = r.concept
                 AND m.is_active
                LEFT JOIN securities s
                  ON s.security_id = r.security_id
            ),
            chosen AS (
                SELECT *
                FROM mapped
                WHERE canonical_rank = 1
            ),
            sequenced AS (
                SELECT
                    chosen.*,
                    row_number() OVER statement_window AS revision_sequence,
                    count(*) OVER statement_window AS revision_count,
                    lag(raw_value) OVER statement_window AS previous_raw_value,
                    lag(value) OVER statement_window AS previous_value
                FROM chosen
                WINDOW statement_window AS (
                    PARTITION BY statement_revision_group_id
                    ORDER BY
                        coalesce(available_at, CAST(as_of_date AS TIMESTAMP)),
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        accession_number,
                        statement_point_id
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                statement_point_id,
                fact_revision_id,
                statement_revision_group_id AS revision_group_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                taxonomy,
                concept,
                unit,
                unit_type,
                period_type,
                normal_balance,
                period_start,
                period_end,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE value IS DISTINCT FROM previous_value
                END AS is_value_changed,
                raw_value,
                value,
                previous_raw_value,
                previous_value,
                CASE
                    WHEN previous_value IS NULL OR value IS NULL THEN NULL
                    ELSE value - previous_value
                END AS value_delta,
                CASE
                    WHEN previous_value IS NULL OR previous_value = 0 OR value IS NULL THEN NULL
                    ELSE (value - previous_value) / abs(previous_value)
                END AS value_delta_percent,
                run_id,
                source_url,
                source_loaded_at
            FROM sequenced
            """
        )
    return int(store.con.execute("SELECT count(*) FROM fundamental_statement_points").fetchone()[0])


def refresh_fundamental_periods(store: DuckDBStore) -> int:
    """Refresh normalized reporting-period windows from SEC statement points."""

    with store.transaction():
        store.con.execute("DELETE FROM fundamental_periods")
        store.con.execute(
            """
            INSERT INTO fundamental_periods (
                fundamental_period_id,
                period_group_id,
                source,
                security_id,
                symbol,
                cik,
                period_start,
                period_end,
                period_days,
                normalized_period_type,
                calendar_year,
                calendar_quarter,
                calendar_period,
                as_of_date,
                available_at,
                form,
                accession_number,
                reported_fiscal_years_json,
                reported_fiscal_periods_json,
                statement_types_json,
                canonical_metrics_json,
                input_statement_point_ids_json,
                statement_point_count,
                canonical_metric_count,
                concept_count,
                value_changed_statement_count,
                has_balance_sheet,
                has_income_statement,
                has_cash_flow,
                has_per_share,
                revision_sequence,
                revision_count,
                is_latest_revision,
                first_available_at,
                latest_available_at,
                source_loaded_at
            )
            WITH grouped AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR)
                        )
                    ) AS period_group_id,
                    sha256(
                        concat_ws(
                            '|',
                            source,
                            security_id,
                            coalesce(CAST(period_start AS VARCHAR), ''),
                            CAST(period_end AS VARCHAR),
                            accession_number
                        )
                    ) AS fundamental_period_id,
                    source,
                    security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    period_start,
                    period_end,
                    CASE
                        WHEN period_start IS NULL THEN NULL
                        ELSE date_diff('day', period_start, period_end) + 1
                    END AS period_days,
                    CASE
                        WHEN period_start IS NULL THEN 'instant'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 70 AND 120 THEN 'quarter'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 121 AND 220 THEN 'semiannual_ytd'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 221 AND 329 THEN 'multi_quarter_ytd'
                        WHEN date_diff('day', period_start, period_end) + 1 BETWEEN 330 AND 380 THEN 'annual'
                        WHEN date_diff('day', period_start, period_end) + 1 > 380 THEN 'multi_year_comparative'
                        ELSE 'other'
                    END AS normalized_period_type,
                    CAST(EXTRACT(YEAR FROM period_end) AS INTEGER) AS calendar_year,
                    CAST(EXTRACT(QUARTER FROM period_end) AS INTEGER) AS calendar_quarter,
                    CAST(EXTRACT(YEAR FROM period_end) AS VARCHAR)
                        || 'Q'
                        || CAST(EXTRACT(QUARTER FROM period_end) AS VARCHAR) AS calendar_period,
                    max(as_of_date) AS as_of_date,
                    max(available_at) AS available_at,
                    any_value(form) AS form,
                    accession_number,
                    CAST(to_json(list(DISTINCT CAST(fiscal_year AS VARCHAR) ORDER BY CAST(fiscal_year AS VARCHAR))) AS VARCHAR) AS reported_fiscal_years_json,
                    CAST(to_json(list(DISTINCT fiscal_period ORDER BY fiscal_period)) AS VARCHAR) AS reported_fiscal_periods_json,
                    CAST(to_json(list(DISTINCT statement_type ORDER BY statement_type)) AS VARCHAR) AS statement_types_json,
                    CAST(to_json(list(DISTINCT canonical_metric ORDER BY canonical_metric)) AS VARCHAR) AS canonical_metrics_json,
                    CAST(to_json(list(statement_point_id ORDER BY statement_type, canonical_metric, statement_point_id)) AS VARCHAR) AS input_statement_point_ids_json,
                    count(*) AS statement_point_count,
                    count(DISTINCT canonical_metric) AS canonical_metric_count,
                    count(DISTINCT concat_ws('|', taxonomy, concept)) AS concept_count,
                    sum(CASE WHEN is_value_changed THEN 1 ELSE 0 END) AS value_changed_statement_count,
                    bool_or(statement_type = 'balance_sheet') AS has_balance_sheet,
                    bool_or(statement_type = 'income_statement') AS has_income_statement,
                    bool_or(statement_type = 'cash_flow') AS has_cash_flow,
                    bool_or(statement_type = 'per_share') AS has_per_share,
                    max(source_loaded_at) AS source_loaded_at
                FROM fundamental_statement_points
                WHERE source IS NOT NULL
                  AND source <> ''
                  AND security_id IS NOT NULL
                  AND security_id <> ''
                  AND cik IS NOT NULL
                  AND cik <> ''
                  AND period_end IS NOT NULL
                  AND as_of_date IS NOT NULL
                  AND accession_number IS NOT NULL
                  AND accession_number <> ''
                GROUP BY source, security_id, period_start, period_end, accession_number
            ),
            sequenced AS (
                SELECT
                    grouped.*,
                    row_number() OVER period_window AS revision_sequence,
                    count(*) OVER period_window AS revision_count,
                    min(available_at) OVER period_window AS first_available_at,
                    max(available_at) OVER period_window AS latest_available_at
                FROM grouped
                WINDOW period_window AS (
                    PARTITION BY period_group_id
                    ORDER BY
                        available_at,
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        accession_number
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                fundamental_period_id,
                period_group_id,
                source,
                security_id,
                symbol,
                cik,
                period_start,
                period_end,
                period_days,
                normalized_period_type,
                calendar_year,
                calendar_quarter,
                calendar_period,
                as_of_date,
                available_at,
                form,
                accession_number,
                coalesce(reported_fiscal_years_json, '[]') AS reported_fiscal_years_json,
                coalesce(reported_fiscal_periods_json, '[]') AS reported_fiscal_periods_json,
                statement_types_json,
                canonical_metrics_json,
                input_statement_point_ids_json,
                statement_point_count,
                canonical_metric_count,
                concept_count,
                value_changed_statement_count,
                has_balance_sheet,
                has_income_statement,
                has_cash_flow,
                has_per_share,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                first_available_at,
                latest_available_at,
                source_loaded_at
            FROM sequenced
            """
        )
    return int(store.con.execute("SELECT count(*) FROM fundamental_periods").fetchone()[0])


def refresh_fundamental_ttm_points(store: DuckDBStore) -> int:
    """Refresh PIT-safe trailing-twelve-month statement values."""

    with store.transaction():
        store.con.execute("DELETE FROM fundamental_ttm_points")
        store.con.execute(
            """
            INSERT INTO fundamental_ttm_points (
                ttm_point_id,
                ttm_revision_group_id,
                anchor_statement_point_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                quarter_count,
                coverage_days,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_period_ends_json,
                ttm_value,
                previous_ttm_value,
                ttm_value_delta,
                ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                is_latest_revision,
                is_value_changed,
                calculation_method,
                source_loaded_at
            )
            WITH statement_points AS (
                SELECT
                    statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    date_diff('day', period_start, period_end) + 1 AS period_days,
                    coalesce(available_at, CAST(as_of_date AS TIMESTAMP)) AS availability_ts
                FROM fundamental_statement_points
                WHERE period_type = 'duration'
                  AND period_start IS NOT NULL
                  AND period_end IS NOT NULL
                  AND value IS NOT NULL
            ),
            reported_quarter_points AS (
                SELECT
                    statement_point_id,
                    statement_point_id AS anchor_statement_point_id,
                    revision_group_id,
                    source,
                    security_id,
                    symbol,
                    cik,
                    statement_type,
                    statement_section,
                    canonical_metric,
                    canonical_label,
                    unit,
                    unit_type,
                    period_start,
                    period_end,
                    as_of_date,
                    available_at,
                    fiscal_year,
                    fiscal_period,
                    form,
                    accession_number,
                    value,
                    revision_sequence,
                    source_loaded_at,
                    period_days,
                    availability_ts,
                    1 AS quarter_source_priority
                FROM statement_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 160 AND 380
            ),
            prior_ytd_points AS (
                SELECT *
                FROM statement_points
                WHERE unit_type = 'monetary'
                  AND period_days BETWEEN 70 AND 290
            ),
            derived_ytd_quarter_points AS (
                SELECT
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.statement_point_id, prior_ytd.statement_point_id)) AS statement_point_id,
                    current_ytd.statement_point_id AS anchor_statement_point_id,
                    sha256(concat_ws('|', 'derived_ytd_quarter', current_ytd.revision_group_id, prior_ytd.revision_group_id)) AS revision_group_id,
                    current_ytd.source,
                    current_ytd.security_id,
                    current_ytd.symbol,
                    current_ytd.cik,
                    current_ytd.statement_type,
                    current_ytd.statement_section,
                    current_ytd.canonical_metric,
                    current_ytd.canonical_label,
                    current_ytd.unit,
                    current_ytd.unit_type,
                    CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE) AS period_start,
                    current_ytd.period_end,
                    greatest(current_ytd.as_of_date, prior_ytd.as_of_date) AS as_of_date,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS available_at,
                    current_ytd.fiscal_year,
                    CASE
                        WHEN current_ytd.period_days BETWEEN 160 AND 205 THEN 'Q2_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 250 AND 290 THEN 'Q3_DERIVED'
                        WHEN current_ytd.period_days BETWEEN 330 AND 380 THEN 'Q4_DERIVED'
                        ELSE 'Q_DERIVED'
                    END AS fiscal_period,
                    current_ytd.form,
                    current_ytd.accession_number,
                    current_ytd.value - prior_ytd.value AS value,
                    current_ytd.revision_sequence,
                    greatest(
                        coalesce(current_ytd.source_loaded_at, TIMESTAMP '1970-01-01'),
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01')
                    ) AS source_loaded_at,
                    date_diff('day', CAST(prior_ytd.period_end + INTERVAL 1 DAY AS DATE), current_ytd.period_end) + 1 AS period_days,
                    greatest(current_ytd.availability_ts, prior_ytd.availability_ts) AS availability_ts,
                    2 AS quarter_source_priority
                FROM ytd_points current_ytd
                JOIN prior_ytd_points prior_ytd
                  ON prior_ytd.source = current_ytd.source
                 AND prior_ytd.security_id = current_ytd.security_id
                 AND prior_ytd.canonical_metric = current_ytd.canonical_metric
                 AND prior_ytd.unit = current_ytd.unit
                 AND prior_ytd.period_start = current_ytd.period_start
                 AND prior_ytd.period_end < current_ytd.period_end
                 AND prior_ytd.as_of_date <= current_ytd.as_of_date
                 AND prior_ytd.availability_ts <= current_ytd.availability_ts
                QUALIFY row_number() OVER (
                    PARTITION BY current_ytd.statement_point_id
                    ORDER BY
                        prior_ytd.period_end DESC,
                        prior_ytd.availability_ts DESC,
                        prior_ytd.as_of_date DESC,
                        coalesce(prior_ytd.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                        prior_ytd.revision_sequence DESC,
                        prior_ytd.statement_point_id DESC
                ) = 1
            ),
            quarter_points AS (
                SELECT *
                FROM reported_quarter_points
                UNION ALL
                SELECT *
                FROM derived_ytd_quarter_points
                WHERE period_days BETWEEN 70 AND 115
            ),
            visible AS (
                SELECT
                    a.anchor_statement_point_id AS anchor_statement_point_id,
                    a.as_of_date AS anchor_as_of_date,
                    a.available_at AS anchor_available_at,
                    a.fiscal_year AS anchor_fiscal_year,
                    a.fiscal_period AS anchor_fiscal_period,
                    a.form AS anchor_form,
                    a.accession_number AS anchor_accession_number,
                    a.source_loaded_at AS anchor_source_loaded_at,
                    q.*,
                    row_number() OVER (
                        PARTITION BY a.anchor_statement_point_id, q.period_start, q.period_end
                        ORDER BY
                            q.quarter_source_priority,
                            q.availability_ts DESC,
                            q.as_of_date DESC,
                            coalesce(q.source_loaded_at, TIMESTAMP '1970-01-01') DESC,
                            q.revision_sequence DESC,
                            q.statement_point_id DESC
                    ) AS visible_rank
                FROM quarter_points a
                JOIN quarter_points q
                  ON q.source = a.source
                 AND q.security_id = a.security_id
                 AND q.canonical_metric = a.canonical_metric
                 AND q.unit = a.unit
                 AND q.period_end <= a.period_end
                 AND q.as_of_date <= a.as_of_date
                 AND q.availability_ts <= a.availability_ts
            ),
            latest_visible AS (
                SELECT *
                FROM visible
                WHERE visible_rank = 1
            ),
            trailing_windows AS (
                SELECT
                    *,
                    row_number() OVER (
                        PARTITION BY anchor_statement_point_id
                        ORDER BY period_end DESC, period_start DESC, statement_point_id DESC
                    ) AS trailing_rank
                FROM latest_visible
            ),
            aggregated AS (
                SELECT
                    sha256(
                        concat_ws(
                            '|',
                            any_value(source),
                            any_value(security_id),
                            any_value(canonical_metric),
                            any_value(unit),
                            CAST(max(period_end) AS VARCHAR)
                        )
                    ) AS ttm_revision_group_id,
                    any_value(anchor_statement_point_id) AS anchor_statement_point_id,
                    any_value(source) AS source,
                    any_value(security_id) AS security_id,
                    any_value(symbol) AS symbol,
                    any_value(cik) AS cik,
                    any_value(statement_type) AS statement_type,
                    any_value(statement_section) AS statement_section,
                    any_value(canonical_metric) AS canonical_metric,
                    any_value(canonical_label) AS canonical_label,
                    any_value(unit) AS unit,
                    any_value(unit_type) AS unit_type,
                    min(period_start) AS ttm_start_date,
                    max(period_end) AS ttm_end_date,
                    max(as_of_date) AS as_of_date,
                    max(availability_ts) AS available_at,
                    any_value(anchor_fiscal_year) AS fiscal_year,
                    any_value(anchor_fiscal_period) AS fiscal_period,
                    any_value(anchor_form) AS form,
                    any_value(anchor_accession_number) AS accession_number,
                    count(*) AS quarter_count,
                    date_diff('day', min(period_start), max(period_end)) + 1 AS coverage_days,
                    min(availability_ts) AS min_input_available_at,
                    max(availability_ts) AS max_input_available_at,
                    CAST(to_json(list(statement_point_id ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_statement_point_ids_json,
                    CAST(to_json(list(accession_number ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_accessions_json,
                    CAST(to_json(list(CAST(period_end AS VARCHAR) ORDER BY period_end, statement_point_id)) AS VARCHAR) AS input_period_ends_json,
                    sum(value) AS ttm_value,
                    max(coalesce(source_loaded_at, anchor_source_loaded_at)) AS source_loaded_at
                FROM trailing_windows
                WHERE trailing_rank <= 4
                GROUP BY anchor_statement_point_id
                HAVING count(*) = 4
                   AND date_diff('day', min(period_start), max(period_end)) + 1 BETWEEN 330 AND 380
            ),
            keyed AS (
                SELECT
                    sha256(concat_ws('|', ttm_revision_group_id, anchor_statement_point_id)) AS ttm_point_id,
                    *
                FROM aggregated
            ),
            sequenced AS (
                SELECT
                    keyed.*,
                    row_number() OVER ttm_window AS revision_sequence,
                    count(*) OVER ttm_window AS revision_count,
                    lag(ttm_value) OVER ttm_window AS previous_ttm_value
                FROM keyed
                WINDOW ttm_window AS (
                    PARTITION BY ttm_revision_group_id
                    ORDER BY
                        available_at,
                        as_of_date,
                        coalesce(source_loaded_at, TIMESTAMP '1970-01-01'),
                        anchor_statement_point_id
                    ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING
                )
            )
            SELECT
                ttm_point_id,
                ttm_revision_group_id,
                anchor_statement_point_id,
                source,
                security_id,
                symbol,
                cik,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                unit,
                unit_type,
                ttm_start_date,
                ttm_end_date,
                as_of_date,
                available_at,
                fiscal_year,
                fiscal_period,
                form,
                accession_number,
                CAST(quarter_count AS INTEGER) AS quarter_count,
                CAST(coverage_days AS INTEGER) AS coverage_days,
                min_input_available_at,
                max_input_available_at,
                input_statement_point_ids_json,
                input_accessions_json,
                input_period_ends_json,
                ttm_value,
                previous_ttm_value,
                CASE
                    WHEN previous_ttm_value IS NULL OR ttm_value IS NULL THEN NULL
                    ELSE ttm_value - previous_ttm_value
                END AS ttm_value_delta,
                CASE
                    WHEN previous_ttm_value IS NULL OR previous_ttm_value = 0 OR ttm_value IS NULL THEN NULL
                    ELSE (ttm_value - previous_ttm_value) / abs(previous_ttm_value)
                END AS ttm_value_delta_percent,
                revision_sequence,
                revision_count,
                revision_sequence = revision_count AS is_latest_revision,
                CASE
                    WHEN revision_sequence = 1 THEN false
                    ELSE ttm_value IS DISTINCT FROM previous_ttm_value
                END AS is_value_changed,
                'sum_four_visible_quarter_like_statement_points_with_ytd_quarter_derivations' AS calculation_method,
                source_loaded_at
            FROM sequenced
            """
        )
    return int(store.con.execute("SELECT count(*) FROM fundamental_ttm_points").fetchone()[0])
