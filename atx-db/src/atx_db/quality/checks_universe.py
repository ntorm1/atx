"""Tier1-S4 T1 fix round 1: real ``SqlQualityCheck`` bodies for the two
``quality_check_registry`` rows migration 0304 seeded as data-only (review finding: a
registry row alone is override metadata -- ``_resolve_spec`` merges onto an *existing*
spec, it never synthesizes one, so the rows were inert until a matching ``check_name``
existed here).

Two checks, matching migration 0304's registry rows exactly by ``check_name``:

* ``universe_us_listed_overlapping_intervals`` -- **critical**. Counts distinct
  ``(universe_id, security_id)`` interval pairs that overlap under half-open
  ``[valid_from, valid_to)`` semantics (``valid_to IS NULL`` means open-ended, i.e. the
  interval is still live). A self-join on the same key, with a total order over
  ``membership_id`` (an arbitrary but total order over the table's own primary key) so each
  unordered overlapping pair is counted once, not twice.
* ``universe_us_listed_missing_decile`` -- **warning**. Counts membership rows whose
  ``market_cap_decile`` is NULL even though the security had a priced market cap on the
  interval's own ``valid_from`` date. ``market_cap_decile`` is attached only at
  ``valid_from`` (see this table's ``table_catalog.pit_notes``, migration 0304), so
  "missing" is scoped to rows where a decile was actually computable that day
  (``market_daily_metrics.market_cap IS NOT NULL``) -- a security with no market-data
  coverage yet is not a violation.

This module is a LEAF of ``atx_db.quality`` (imports only ``._types`` at module level),
following ``checks_identities.py`` / ``checks_survivorship.py``'s precedent, so it cannot
introduce an import cycle inside the package (enforced by
``test_decomposed_package_import_graphs_are_acyclic``). Wired into the standard production
sweep via ``quality/_checks.py::_check_specs`` (the ``checks_identities.py`` shape), not
``checks_survivorship.py``'s separate leaf-hook pattern -- neither check here needs anything
beyond a single SQL scalar.
"""

from __future__ import annotations

from ._types import SqlQualityCheck

UNIVERSE_DATASET_ID = "universe_us_listed"
UNIVERSE_TABLE_NAME = "universe_us_listed_membership"

OVERLAPPING_INTERVALS_CHECK_NAME = "universe_us_listed_overlapping_intervals"
MISSING_DECILE_CHECK_NAME = "universe_us_listed_missing_decile"

# Half-open [valid_from, valid_to) overlap: two intervals on the same (universe_id,
# security_id) overlap iff a.valid_from < b.valid_to_or_open AND b.valid_from <
# a.valid_to_or_open (strict '<' -- adjacent, touching intervals such as
# [Jan2, Jan4) / [Jan4, Jan8) are NOT an overlap under half-open semantics). NULL valid_to
# (still open/live) is treated as +infinity via a coalesce to a date far past any real
# trading session. `a.membership_id < b.membership_id` is an arbitrary but total order over
# the table's own primary key, used only so each overlapping pair is counted once.
_OVERLAPPING_INTERVALS_SQL = f"""
SELECT count(*)::DOUBLE
FROM {UNIVERSE_TABLE_NAME} a
JOIN {UNIVERSE_TABLE_NAME} b
  ON a.universe_id = b.universe_id
 AND a.security_id = b.security_id
 AND a.membership_id < b.membership_id
WHERE a.valid_from < coalesce(b.valid_to, DATE '9999-12-31')
  AND b.valid_from < coalesce(a.valid_to, DATE '9999-12-31')
"""

_MISSING_DECILE_SQL = f"""
SELECT count(*)::DOUBLE
FROM {UNIVERSE_TABLE_NAME} m
JOIN market_daily_metrics d
  ON d.security_id = m.security_id
 AND d.trade_date = m.valid_from
 AND d.is_latest_revision
WHERE m.market_cap_decile IS NULL
  AND d.market_cap IS NOT NULL
"""


def _overlapping_intervals_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id=UNIVERSE_DATASET_ID,
        table_name=UNIVERSE_TABLE_NAME,
        check_name=OVERLAPPING_INTERVALS_CHECK_NAME,
        sql=_OVERLAPPING_INTERVALS_SQL,
        threshold=0.0,
        comparator="eq",
        required_tables=(UNIVERSE_TABLE_NAME,),
        warn_if_missing=True,
        failure_status="failed",
        severity="critical",
    )


def _missing_decile_spec() -> SqlQualityCheck:
    return SqlQualityCheck(
        dataset_id=UNIVERSE_DATASET_ID,
        table_name=UNIVERSE_TABLE_NAME,
        check_name=MISSING_DECILE_CHECK_NAME,
        sql=_MISSING_DECILE_SQL,
        threshold=0.0,
        comparator="eq",
        required_tables=(UNIVERSE_TABLE_NAME, "market_daily_metrics"),
        warn_if_missing=True,
        failure_status="warning",
        severity="warning",
    )


def universe_check_specs(**_ignored: object) -> tuple[SqlQualityCheck, ...]:
    """The two migration-0304 universe checks.

    Accepts and ignores the ``daily_macro_stale_days`` / ``monthly_macro_stale_days`` /
    ``valuation_stale_gap_days`` common kwargs so the factory is interchangeable with the
    other ``*_check_specs`` factories in ``quality/_checks.py::_check_specs``.
    """

    return (_overlapping_intervals_spec(), _missing_decile_spec())
