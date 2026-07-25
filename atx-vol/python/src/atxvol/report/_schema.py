"""RunArchive (ATXRUN01) column registry.

Carries the format identity and the column registry as plain data, plus a
byte-for-byte port of the C++ ``ra_schema_hash()`` FNV-1a-64 fold so the
pure-Python reader can pin a file's schema at open. No third-party imports.

PROVENANCE (corrected, FIX-5/I5). This file previously declared itself
``GENERATED, do not edit by hand``, named
``atx-vol/include/atx/vol/run_archive_schema.hpp`` as its source of truth and
``atx-vol/tools/gen_runarchive_schema.py`` as the regeneration path. **Neither
file exists in this repository**, and neither does any other ATXRUN01 producer:
``git ls-files`` matching ``run_archive`` returns only this reader, its two
fixtures and its two test files. The header/generator belong to the out-of-tree
C++ writer. So in THIS repo this module is hand-maintained and is itself the
authority for the pure-Python reader, and the header at the top said the
opposite — which is why ``test_runarchive.py``'s anti-drift guard skipped in
silence against its own docstring. That guard now asserts in both worlds; see
``test_schema_py_not_stale_vs_cpp_header``.

The real drift detector is ``RA_SCHEMA_HASH``: the writer stamps its own fold
into every file and ``RunArchive.open`` rejects a mismatch, so a registry that
diverges from the producer fails loudly at open rather than silently mis-reading
columns.

WHY THE UNIT STRINGS CANNOT SIMPLY BE CORRECTED. ``ra_schema_hash`` folds the
unit string of every column (see the ``fbytes(h, unit)`` line below), so the unit
annotations are part of the persisted format identity, not documentation.
Relabelling the three blank ``gross_vega`` units moves the hash from
``0xdcce47781ac8390d`` to ``0xd173d8c005a70291`` (measured), which would reject
every existing ``.atxrun`` — including both committed fixtures — at open. Unit
corrections therefore belong to a coordinated format-version bump with the C++
writer, not to this file alone. Until then the semantics that the blank
annotations fail to carry are recorded in ``COLUMN_NOTES`` below, which is
deliberately NOT folded into the hash.
"""

from __future__ import annotations

RA_MAGIC = b'ATXRUN01'
RA_MAJOR = 1
RA_MINOR = 0
RA_SCHEMA_SALT = 0x41545852554E3031

# RaDType numeric codes (run_archive_schema.hpp).
F64, I64, U32, U8ENUM, DICTSTR = 0, 1, 2, 3, 4
# RaSectionKind numeric codes.
SCALARKV, TIMESERIES, SUBTABLE = 0, 1, 2

# Registry: (section_name, kind_code, ((col_name, dtype_code, unit), ...)).
# Order and contents mirror kRaSections exactly and are load-bearing for the
# schema hash.
SECTIONS = (
    ('meta', 0, (
        ('key', 4, ''),
        ('value', 4, ''),
    )),
    ('backtest', 1, (
        ('date', 4, ''),
        ('ts_ns', 1, 'ns'),
        ('pnl_total', 0, 'usd'),
        ('pnl_delta', 0, 'usd'),
        ('pnl_gamma', 0, 'usd'),
        ('pnl_vega', 0, 'usd'),
        ('pnl_vanna', 0, 'usd'),
        ('pnl_volga', 0, 'usd'),
        ('pnl_theta', 0, 'usd'),
        ('pnl_rho', 0, 'usd'),
        ('pnl_charm', 0, 'usd'),
        ('pnl_unexplained', 0, 'usd'),
        ('pnl_settlement', 0, 'usd'),
        ('pnl_shares', 0, 'usd'),
        ('financing', 0, 'usd'),
        ('cost', 0, 'usd'),
        ('nav', 0, 'usd'),
        ('cash', 0, 'usd'),
        ('gross_delta', 0, ''),
        ('gross_gamma', 0, ''),
        ('gross_vega', 0, ''),
        ('gross_theta', 0, ''),
        ('turnover_notional', 0, 'usd'),
        ('turnover_vega', 0, ''),
        ('n_open_lots', 0, 'count'),
        ('n_unpriced_lots', 0, 'count'),
        ('n_unpriced_greeks', 0, 'count'),
    )),
    ('projected_cold', 1, (
        ('date', 4, ''),
        ('ts_ns', 1, 'ns'),
        ('pnl_total', 0, 'usd'),
        ('pnl_delta', 0, 'usd'),
        ('pnl_gamma', 0, 'usd'),
        ('pnl_vega', 0, 'usd'),
        ('pnl_vanna', 0, 'usd'),
        ('pnl_volga', 0, 'usd'),
        ('pnl_theta', 0, 'usd'),
        ('pnl_rho', 0, 'usd'),
        ('pnl_charm', 0, 'usd'),
        ('pnl_unexplained', 0, 'usd'),
        ('pnl_settlement', 0, 'usd'),
        ('pnl_shares', 0, 'usd'),
        ('financing', 0, 'usd'),
        ('cost', 0, 'usd'),
        ('nav', 0, 'usd'),
        ('cash', 0, 'usd'),
        ('gross_delta', 0, ''),
        ('gross_gamma', 0, ''),
        ('gross_vega', 0, ''),
        ('gross_theta', 0, ''),
        ('turnover_notional', 0, 'usd'),
        ('turnover_vega', 0, ''),
        ('n_open_lots', 0, 'count'),
        ('n_unpriced_lots', 0, 'count'),
        ('n_unpriced_greeks', 0, 'count'),
    )),
    ('projected_nodiv', 1, (
        ('date', 4, ''),
        ('ts_ns', 1, 'ns'),
        ('pnl_total', 0, 'usd'),
        ('pnl_delta', 0, 'usd'),
        ('pnl_gamma', 0, 'usd'),
        ('pnl_vega', 0, 'usd'),
        ('pnl_vanna', 0, 'usd'),
        ('pnl_volga', 0, 'usd'),
        ('pnl_theta', 0, 'usd'),
        ('pnl_rho', 0, 'usd'),
        ('pnl_charm', 0, 'usd'),
        ('pnl_unexplained', 0, 'usd'),
        ('pnl_settlement', 0, 'usd'),
        ('pnl_shares', 0, 'usd'),
        ('financing', 0, 'usd'),
        ('cost', 0, 'usd'),
        ('nav', 0, 'usd'),
        ('cash', 0, 'usd'),
        ('gross_delta', 0, ''),
        ('gross_gamma', 0, ''),
        ('gross_vega', 0, ''),
        ('gross_theta', 0, ''),
        ('turnover_notional', 0, 'usd'),
        ('turnover_vega', 0, ''),
        ('n_open_lots', 0, 'count'),
        ('n_unpriced_lots', 0, 'count'),
        ('n_unpriced_greeks', 0, 'count'),
    )),
    ('reconciliation', 1, (
        ('date', 4, ''),
        ('valuation_ts_ns', 1, 'ns'),
        ('held_cohort', 2, ''),
        ('model_option_pnl', 0, 'usd'),
        ('quote_mid_pnl', 0, 'usd'),
        ('model_minus_quote_pnl', 0, 'usd'),
        ('model_nav', 0, 'usd'),
        ('quote_mid_nav', 0, 'usd'),
        ('quote_mid_coverage', 0, ''),
        ('n_held_lots', 2, 'count'),
        ('n_quote_mid_lots', 2, 'count'),
    )),
    ('trade_schedule', 2, (
        ('roll_date', 4, ''),
        ('valuation_ts_ns', 1, 'ns'),
        ('cohort', 2, ''),
        ('expiry_ts_ns', 1, 'ns'),
        ('gross_index_vega_target', 0, 'usd_per_volpt'),
        ('net_vega', 0, 'usd_per_volpt'),
        ('gross_vega', 0, 'usd_per_volpt'),
        ('n_names', 2, 'count'),
        ('is_index', 3, ''),
        ('symbol', 4, ''),
        ('uid', 2, ''),
        ('instrument_id', 2, ''),
        ('raw_symbol', 4, ''),
        ('strike', 0, 'usd'),
        ('side', 3, ''),
        ('quantity', 0, 'contracts'),
        ('multiplier', 0, ''),
        ('raw_bid', 0, 'usd'),
        ('raw_ask', 0, 'usd'),
        ('raw_mid', 0, 'usd'),
        ('model_mark', 0, 'usd'),
        ('delta_per_share', 0, ''),
        ('vega_per_unit_vol', 0, 'usd_per_unitvol'),
        ('vega_per_contract_per_vol_point', 0, 'usd_per_volpt'),
        ('normalized_weight', 0, ''),
        ('target_straddle_vega', 0, 'usd_per_volpt'),
        ('achieved_leg_vega', 0, 'usd_per_volpt'),
        ('source_fingerprint', 1, ''),
        ('surface_fingerprint', 1, ''),
    )),
    ('projected_schedule', 2, (
        ('roll_date', 4, ''),
        ('valuation_ts_ns', 1, 'ns'),
        ('cohort', 2, ''),
        ('expiry_ts_ns', 1, 'ns'),
        ('gross_index_vega_target', 0, 'usd_per_volpt'),
        ('net_vega', 0, 'usd_per_volpt'),
        ('gross_vega', 0, 'usd_per_volpt'),
        ('n_names', 2, 'count'),
        ('is_index', 3, ''),
        ('symbol', 4, ''),
        ('uid', 2, ''),
        ('instrument_id', 2, ''),
        ('raw_symbol', 4, ''),
        ('strike', 0, 'usd'),
        ('side', 3, ''),
        ('quantity', 0, 'contracts'),
        ('multiplier', 0, ''),
        ('raw_bid', 0, 'usd'),
        ('raw_ask', 0, 'usd'),
        ('raw_mid', 0, 'usd'),
        ('model_mark', 0, 'usd'),
        ('delta_per_share', 0, ''),
        ('vega_per_unit_vol', 0, 'usd_per_unitvol'),
        ('vega_per_contract_per_vol_point', 0, 'usd_per_volpt'),
        ('normalized_weight', 0, ''),
        ('target_straddle_vega', 0, 'usd_per_volpt'),
        ('achieved_leg_vega', 0, 'usd_per_volpt'),
        ('source_fingerprint', 1, ''),
        ('surface_fingerprint', 1, ''),
    )),
    ('contract_marks', 2, (
        ('date', 4, ''),
        ('valuation_ts_ns', 1, 'ns'),
        ('role', 3, ''),
        ('cohort', 2, ''),
        ('symbol', 4, ''),
        ('uid', 2, ''),
        ('instrument_id', 2, ''),
        ('raw_symbol', 4, ''),
        ('expiry_ts_ns', 1, 'ns'),
        ('strike', 0, 'usd'),
        ('side', 3, ''),
        ('quantity', 0, 'contracts'),
        ('multiplier', 0, ''),
        ('status', 3, ''),
        ('raw_bid', 0, 'usd'),
        ('raw_ask', 0, 'usd'),
        ('raw_mid', 0, 'usd'),
        ('model_mark', 0, 'usd'),
        ('model_in_spread', 3, ''),
    )),
    ('mark_divergence', 2, (
        ('date', 4, ''),
        ('symbol', 4, ''),
        ('raw_symbol', 4, ''),
        ('strike', 0, 'usd'),
        ('expiry_ts_ns', 1, 'ns'),
        ('side', 3, ''),
        ('schedule_mark', 0, 'usd'),
        ('live_mark', 0, 'usd'),
        ('diff', 0, 'usd'),
        ('abs_diff_bps_of_mark', 0, 'bps'),
    )),
    ('diagnostics', 2, (
        ('subcommand', 4, ''),
        ('phase', 4, ''),
        ('wall_ms', 0, 'ms'),
        ('count', 1, 'count'),
    )),
)


def ra_schema_hash() -> int:
    """FNV-1a-64 fold over the registry, salted, mirroring the C++ pin."""
    mask = 0xFFFFFFFFFFFFFFFF
    prime = 0x100000001B3

    def fb(h: int, b: int) -> int:
        return ((h ^ b) * prime) & mask

    def fbytes(h: int, s: str) -> int:
        for c in s.encode("ascii"):
            h = fb(h, c)
        return h

    h = 0xCBF29CE484222325
    for i in range(8):  # fnv1a_u64(RA_SCHEMA_SALT), little-endian
        h = fb(h, (RA_SCHEMA_SALT >> (8 * i)) & 0xFF)
    for name, kind, cols in SECTIONS:
        h = fbytes(h, name)
        h = fb(h, 0x1F)
        h = fb(h, kind)
        for cname, dtype, unit in cols:
            h = fbytes(h, cname)
            h = fb(h, 0x1F)
            h = fb(h, dtype)
            h = fbytes(h, unit)
            h = fb(h, 0x1E)
        h = fb(h, 0x1D)
    return h


RA_SCHEMA_HASH = ra_schema_hash()


# ── Column semantics the registry's unit strings do not carry (FIX-5/I5) ─────
#
# NOT folded into ra_schema_hash() — this is documentation, and adding it to the
# fold would change the format identity (see the module docstring). Keyed by
# (section, column). Consult it before comparing a column across sections.
#
# The live collision: ``gross_vega`` is one name over two units 100x apart AND a
# gross/net flip, with the unit annotation blank exactly where it is needed.
COLUMN_NOTES = {
    # Producer: src/backtest.cpp:1603,1862 — `out.gross_vega.push_back(g.vega)`,
    # where `g` is the summed book bundle. So despite the name this is the NET
    # (signed, summed) book vega, and it is the pricer's dP/dsigma per UNIT vol.
    # A vega-neutral book drives it to ~0 by construction. Divide by 100 to
    # compare against the schedule sections' per-vol-point figures.
    ('backtest', 'gross_vega'): (
        'usd_per_unitvol; NET signed book vega (dP/dsigma per unit vol), not gross'),
    ('projected_cold', 'gross_vega'): (
        'usd_per_unitvol; NET signed book vega (dP/dsigma per unit vol), not gross'),
    ('projected_nodiv', 'gross_vega'): (
        'usd_per_unitvol; NET signed book vega (dP/dsigma per unit vol), not gross'),
    # Producer: src/listed_dispersion_schedule.cpp:310,360 —
    # `roll.gross_vega_per_vol_point += fabs(leg.achieved_leg_vega_per_vol_point)`.
    # Genuinely GROSS (absolute values summed) and genuinely per VOL POINT, i.e.
    # 100x the units of the `backtest` column of the same name.
    ('trade_schedule', 'gross_vega'): (
        'usd_per_volpt; GROSS (sum of |achieved leg vega|) — 100x the backtest column'),
    ('projected_schedule', 'gross_vega'): (
        'usd_per_volpt; GROSS (sum of |achieved leg vega|) — 100x the backtest column'),
    # Same blank-unit class, same producer family, recorded for completeness.
    ('backtest', 'turnover_vega'): 'usd_per_unitvol',
    ('backtest', 'gross_delta'): 'per share',
    ('backtest', 'gross_gamma'): 'per share^2',
    ('backtest', 'gross_theta'): 'usd_per_year',
}


def column_note(section: str, column: str) -> str:
    """The recorded semantics of a column whose registry unit is blank or
    misleading, or '' when the registry annotation is already sufficient."""
    return COLUMN_NOTES.get((section, column), '')
