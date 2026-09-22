"""FC1 effective eligibility for SEC date-only facts; raw stamps stay intact.

This is a conservative date policy, not measured acceptance or source delivery.
Keep v1 stable: governed migration 0319 embeds these projections in an existing
view. A future policy requires a new version and a governed consumer rebuild.
"""

from __future__ import annotations

FUNDAMENTAL_CLOCK_POLICY = "sec_filed_date_plus_46h_v1"


def _effective_relation(table: str, filed_column: str) -> str:
    # Arguments are private, fixed SQL identifiers below. Date-only source data
    # cannot establish exact availability. Unknown/non-finite dates therefore
    # stay in raw storage but are excluded from effective SEC readers. DuckDB's
    # greatest ignores a NULL stored clock, admitting a valid date at its floor.
    filed_date = f"try_cast({filed_column} AS DATE)"
    return f"""(
        SELECT * REPLACE (
            CASE WHEN source = 'SEC companyfacts' THEN
                CASE WHEN isfinite({filed_date}) THEN greatest(
                    available_at, CAST({filed_date} AS TIMESTAMP) + INTERVAL 46 HOUR
                ) END
            ELSE available_at END AS available_at
        )
        FROM {table}
        WHERE source IS DISTINCT FROM 'SEC companyfacts'
           OR isfinite({filed_date})
    )"""


EFFECTIVE_COMPANY_FACTS_SQL = _effective_relation("sec_company_facts", "filed_date")
# Company Facts copies its filed_date into fundamental_points.as_of_date.
EFFECTIVE_FUNDAMENTAL_POINTS_SQL = _effective_relation("fundamental_points", "as_of_date")
