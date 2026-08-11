from __future__ import annotations

import datetime as dt

import polars as pl
import pytest

from atx_factor.schema import PanelValidationError, validate_panel


def _panel() -> pl.DataFrame:
    return pl.DataFrame(
        {
            "date": [dt.date(2025, 1, 31), dt.date(2025, 1, 31)],
            "asset_id": ["A", "B"],
            "signal": [1.0, -1.0],
            "forward_return": [0.02, -0.01],
            "available_at": [
                dt.datetime(2025, 1, 31, 20),
                dt.datetime(2025, 1, 31, 21),
            ],
            "forward_end_date": [dt.date(2025, 3, 3), dt.date(2025, 3, 3)],
        }
    )


def test_validate_panel_canonicalizes_and_sorts() -> None:
    result = validate_panel(_panel().reverse())
    assert result.schema["date"] == pl.Date
    assert result.get_column("asset_id").to_list() == ["A", "B"]


def test_validate_panel_rejects_future_knowledge_and_duplicates() -> None:
    future = _panel().with_columns(
        pl.when(pl.col("asset_id") == "A")
        .then(pl.datetime(2025, 2, 1, 1))
        .otherwise(pl.col("available_at"))
        .alias("available_at")
    )
    with pytest.raises(PanelValidationError, match="known after formation"):
        validate_panel(future)
    with pytest.raises(PanelValidationError, match="duplicate"):
        validate_panel(pl.concat([_panel(), _panel().head(1)]))

