"""Deterministic cross-sectional signal-to-position transforms."""

from __future__ import annotations

import polars as pl

from .config import CostModel, PortfolioConfig


def _native_waterfill(
    frame: pl.DataFrame,
    *,
    magnitude_column: str,
    output_column: str,
    target_column: str,
    cap_column: str,
) -> pl.DataFrame:
    """Allocate a side budget with the exact native sorted water-fill solution."""

    prefix = f"_{output_column}"
    ratio_column = f"{prefix}_ratio"
    cumulative_cap_column = f"{prefix}_cumulative_cap"
    cumulative_magnitude_column = f"{prefix}_cumulative_magnitude"
    lambda_candidate_column = f"{prefix}_lambda_candidate"
    lambda_column = f"{prefix}_lambda"
    frame = frame.with_columns(
        pl.when(pl.col(magnitude_column) > 0)
        .then(pl.col(cap_column) / pl.col(magnitude_column))
        .otherwise(float("inf"))
        .alias(ratio_column)
    ).sort("date", ratio_column, "asset_id")
    frame = frame.with_columns(
        (pl.col(cap_column).cum_sum().over("date") - pl.col(cap_column)).alias(
            cumulative_cap_column
        ),
        (
            pl.col(magnitude_column).cum_sum().over("date")
            - pl.col(magnitude_column)
        ).alias(cumulative_magnitude_column),
    ).with_columns(
        (
            (pl.col(target_column) - pl.col(cumulative_cap_column))
            /
            (
                pl.col(magnitude_column).sum().over("date")
                - pl.col(cumulative_magnitude_column)
            )
        ).alias(lambda_candidate_column)
    )
    solved_lambda = (
        pl.when(
            (pl.col(magnitude_column) > 0)
            & (pl.col(lambda_candidate_column) <= pl.col(ratio_column) + 1e-12)
            & (pl.col(lambda_candidate_column) >= -1e-12)
        )
        .then(pl.col(lambda_candidate_column))
        .otherwise(None)
        .max()
        .over("date")
    )
    frame = frame.with_columns(
        pl.when(pl.col(target_column).abs() <= 1e-15)
        .then(0.0)
        .otherwise(solved_lambda)
        .alias(lambda_column)
    )
    if frame.select(pl.col(lambda_column).is_null().any()).item():
        failed = frame.filter(pl.col(lambda_column).is_null())
        failed_dates = failed.get_column("date").unique(maintain_order=True).head(5)
        diagnostics = (
            failed.filter(pl.col("date").is_in(failed_dates))
            .group_by("date")
            .agg(
                pl.col(target_column).first().alias("target"),
                pl.col(magnitude_column).sum().alias("magnitude"),
                pl.when(pl.col(magnitude_column) > 0)
                .then(pl.col(cap_column))
                .otherwise(0.0)
                .sum()
                .alias("capacity"),
                pl.col(lambda_candidate_column).min().alias("min_lambda"),
                pl.col(lambda_candidate_column).max().alias("max_lambda"),
                pl.col(ratio_column).min().alias("min_ratio"),
                pl.col(ratio_column).max().alias("max_ratio"),
            )
            .sort("date")
            .to_dicts()
        )
        raise ValueError(
            "failed to solve capped proportional allocation "
            f"for {magnitude_column}: {diagnostics}"
        )
    frame = frame.with_columns(
        pl.min_horizontal(
            pl.col(cap_column),
            pl.col(lambda_column) * pl.col(magnitude_column),
        ).alias(output_column)
    )
    error = frame.group_by("date").agg(
        (pl.col(output_column).sum() - pl.col(target_column).first())
        .abs()
        .alias("allocation_error")
    ).get_column("allocation_error").max()
    if error is None or error > 1e-10:
        raise ValueError("failed to allocate exact side budget under name cap")
    return frame.drop(
        ratio_column,
        cumulative_cap_column,
        cumulative_magnitude_column,
        lambda_candidate_column,
        lambda_column,
    )


def _position_frame(frame: pl.DataFrame, config: PortfolioConfig) -> pl.DataFrame:
    work = frame.with_columns(
        (pl.col("_score") - pl.col("_score").mean().over("date")).alias("_score")
    ).with_columns(
        pl.when(
            pl.col("_score").abs()
            <= pl.col("_score").abs().max().over("date") * 1e-14
        )
        .then(0.0)
        .otherwise(pl.col("_score"))
        .alias("_score")
    )
    if config.dollar_neutral:
        work = work.with_columns(
            pl.col("_score").clip(lower_bound=0.0).alias("_positive_score"),
            (-pl.col("_score")).clip(lower_bound=0.0).alias("_negative_score"),
        )
        half_gross = config.gross_leverage / 2.0
        work = work.with_columns(
            pl.when(pl.col("_positive_score") > 0)
            .then(pl.col("position_cap"))
            .otherwise(0.0)
            .sum()
            .over("date")
            .alias("_long_capacity"),
            pl.when(pl.col("_negative_score") > 0)
            .then(pl.col("position_cap"))
            .otherwise(0.0)
            .sum()
            .over("date")
            .alias("_short_capacity"),
        ).with_columns(
            pl.min_horizontal(
                pl.lit(half_gross),
                pl.col("_long_capacity"),
                pl.col("_short_capacity"),
            ).alias("_side_target")
        )
        work = _native_waterfill(
            work,
            magnitude_column="_positive_score",
            output_column="_long_weight",
            target_column="_side_target",
            cap_column="position_cap",
        )
        work = _native_waterfill(
            work,
            magnitude_column="_negative_score",
            output_column="_short_weight",
            target_column="_side_target",
            cap_column="position_cap",
        )
        return work.with_columns(
            (pl.col("_long_weight") - pl.col("_short_weight")).alias("target_weight"),
            (2.0 * pl.col("_side_target") / config.gross_leverage).alias(
                "capacity_scale"
            ),
        ).drop(
            "_positive_score",
            "_negative_score",
            "_long_weight",
            "_short_weight",
            "_long_capacity",
            "_short_capacity",
            "_side_target",
        )
    work = work.with_columns(pl.col("_score").abs().alias("_absolute_score"))
    work = work.with_columns(
        pl.when(pl.col("_absolute_score") > 0)
        .then(pl.col("position_cap"))
        .otherwise(0.0)
        .sum()
        .over("date")
        .clip(upper_bound=config.gross_leverage)
        .alias("_gross_target")
    )
    work = _native_waterfill(
        work,
        magnitude_column="_absolute_score",
        output_column="_absolute_weight",
        target_column="_gross_target",
        cap_column="position_cap",
    )
    return work.with_columns(
        (pl.col("_score").sign() * pl.col("_absolute_weight")).alias("target_weight"),
        (pl.col("_gross_target") / config.gross_leverage).alias("capacity_scale"),
    ).drop("_absolute_score", "_absolute_weight", "_gross_target")


def normalize_weight_scores(
    frame: pl.DataFrame,
    config: PortfolioConfig | None = None,
    *,
    score_column: str = "weight_score",
    costs: CostModel | None = None,
) -> pl.DataFrame:
    """Normalize arbitrary signed scores into capped target weights."""

    config = config or PortfolioConfig(rank_signal=False)
    required = {"date", "asset_id", score_column}
    missing = sorted(required.difference(frame.columns))
    if missing:
        raise ValueError(f"weight-score frame is missing columns: {missing}")
    work = frame.with_columns(
        pl.col("date").cast(pl.Date),
        pl.col("asset_id").cast(pl.String),
        pl.col(score_column).cast(pl.Float64).alias("_score"),
    )
    work = _attach_position_cap(work, config, costs)
    counts = pl.len().over("date")
    work = work.with_columns(counts.alias("_n_names")).filter(
        pl.col("_n_names") >= config.minimum_names
    )
    if work.is_empty():
        raise ValueError("no dates meet minimum_names")
    return _position_frame(work, config).drop("_score", "_n_names").sort(
        "date", "asset_id"
    )


def build_target_weights(
    panel: pl.DataFrame,
    config: PortfolioConfig | None = None,
    *,
    costs: CostModel | None = None,
) -> pl.DataFrame:
    """Build continuous rank positions from a validated long signal panel."""

    config = config or PortfolioConfig()
    required = {"date", "asset_id", "signal"}
    missing = sorted(required.difference(panel.columns))
    if missing:
        raise ValueError(f"panel is missing portfolio columns: {missing}")
    if config.neutralize_column and config.neutralize_column not in panel.columns:
        raise ValueError(
            f"neutralize column {config.neutralize_column!r} is missing from panel"
        )
    work = panel.sort("date", "asset_id")
    work = _attach_position_cap(work, config, costs)
    count_expr = pl.len().over("date")
    if config.rank_signal:
        score_expr = (
            pl.col("signal").rank(method="average").over("date") / (count_expr + 1.0)
            - 0.5
        )
    else:
        score_expr = pl.col("signal")
    work = work.with_columns(score_expr.alias("_score"), count_expr.alias("_n_names"))
    work = work.filter(pl.col("_n_names") >= config.minimum_names)
    if config.neutralize_column:
        group_keys = ["date", config.neutralize_column]
        work = work.with_columns(
            (pl.col("_score") - pl.col("_score").mean().over(group_keys)).alias("_score")
        )
    work = work.with_columns(
        (pl.col("_score") - pl.col("_score").mean().over("date")).alias("_score")
    )
    if work.is_empty():
        raise ValueError("no dates meet minimum_names")
    return _position_frame(work, config).drop("_score", "_n_names").sort(
        "date", "asset_id"
    )


def _attach_position_cap(
    frame: pl.DataFrame,
    config: PortfolioConfig,
    costs: CostModel | None,
) -> pl.DataFrame:
    if costs is None:
        cap = pl.lit(config.name_cap)
    else:
        adv = (
            pl.col("adv_usd").fill_null(costs.default_adv_usd)
            if "adv_usd" in frame.columns
            else pl.lit(costs.default_adv_usd)
        )
        # The default quarter-ceiling position supports a full long-to-short flip plus a
        # roughly 2x deterioration in ADV before breaching the trade participation limit.
        liquidity_cap = (
            costs.position_participation_fraction
            * costs.max_participation
            * adv
            / costs.aum_usd
        )
        cap = pl.min_horizontal(pl.lit(config.name_cap), liquidity_cap)
    return frame.with_columns(cap.cast(pl.Float64).alias("position_cap"))
