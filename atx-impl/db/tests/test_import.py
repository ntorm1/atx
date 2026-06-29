"""Regression: import db succeeds and key Dataset classes are importable."""

from __future__ import annotations


def test_import_db():
    import db  # noqa: F401


def test_key_dataset_classes_importable():
    from db import (
        AdjustmentFactorHistoryDataset,
        AlphaResearchDataset,
        CorporateActionsDataset,
        DelistingEventDataset,
        DuckDBStore,
        EquityDailyFeatureDataset,
        FinraShortInterestDataset,
        FredMacroDataset,
        SecCompanyFactsDataset,
        SecSubmissionsDataset,
        SecurityMasterDataset,
        SharesOutstandingHistoryDataset,
        TickerHistoryDataset,
        ThirteenFDataSet,
        UniverseMembershipDataset,
    )
    assert AdjustmentFactorHistoryDataset is not None
    assert AlphaResearchDataset is not None
    assert CorporateActionsDataset is not None
    assert DelistingEventDataset is not None
    assert DuckDBStore is not None
    assert EquityDailyFeatureDataset is not None
    assert FinraShortInterestDataset is not None
    assert FredMacroDataset is not None
    assert SecCompanyFactsDataset is not None
    assert SecSubmissionsDataset is not None
    assert SecurityMasterDataset is not None
    assert SharesOutstandingHistoryDataset is not None
    assert TickerHistoryDataset is not None
    assert ThirteenFDataSet is not None
    assert UniverseMembershipDataset is not None


def test_migration_symbols_exported():
    from db import MIGRATIONS, Migration, apply_pending_migrations

    assert callable(apply_pending_migrations)
    assert isinstance(MIGRATIONS, list)
    assert len(MIGRATIONS) >= 2
    assert all(isinstance(m, Migration) for m in MIGRATIONS)


def test_db_calendar_not_shadowing_stdlib():
    """Ensure db.calendar does not shadow the stdlib calendar module."""
    import calendar as stdlib_calendar

    # stdlib calendar has isleap(); db.calendar does not
    assert hasattr(stdlib_calendar, "isleap"), "stdlib calendar.isleap missing — calendar shadow bug!"
