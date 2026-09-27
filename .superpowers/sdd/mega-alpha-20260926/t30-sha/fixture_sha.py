"""Hand-derive the pre-change baseline recipe.json bytes of the NAV (and target) replay
for strategy_nav_replay_test.cpp publication_panel() + write_artifact(), by emulating
the fixture writers and nlohmann::json dump(2) (validated byte-exact on 25 committed
C++ outputs). Source of every string: the C++ source at the given worktree."""
import datetime
import hashlib
import math
import re
import struct
import sys

sys.path.insert(0, sys.argv[1])
import nljson  # noqa: E402

ROOT = sys.argv[2]
NAV_SRC = open(ROOT + '/atx-impl/src/strategy_nav_replay.cpp', encoding='utf-8').read()
BS = chr(92)
NAN = float('nan')


def cxx_constant(src, name):
    m = re.search(r'constexpr const char\* ' + name + r' =\s*((?:"(?:[^"' + BS * 2 + r']|' +
                  BS * 2 + r'.)*"\s*)+);', src)
    if not m:
        raise KeyError(name)
    parts = re.findall(r'"((?:[^"' + BS * 2 + r']|' + BS * 2 + r'.)*)"', m.group(1))
    text = ''.join(parts)
    if BS in text:
        raise ValueError('escape in ' + name)
    return text


def sha(b):
    return hashlib.sha256(b).hexdigest()


def weekdays(y, m, d, count):
    out = []
    date = datetime.date(y, m, d)
    while len(out) < count:
        if date.weekday() < 5:
            out.append((date - datetime.date(1970, 1, 1)).days * 86400 * 10**9)
        date += datetime.timedelta(days=1)
    return out


# ---- publication_panel(): Panel(9, 3) ----
D, N = 9, 3
signal = [0.0] * (D * N)
close = [100.0] * (D * N)
raw = [100.0] * (D * N)
volume = [1e9] * (D * N)
member = [1] * (D * N)
present = [1] * (D * N)
sessions = weekdays(2020, 1, 2, D)
ids = [100 + i for i in range(N)]
k = lambda t, i: t * N + i
volume = [1e6] * (D * N)
for t in range(D):
    close[k(t, 2)] = 100.0 + float(t)
    raw[k(t, 2)] = 100.0 + float(t)
c = k(4, 0)
present[c] = 0; member[c] = 0; signal[c] = NAN; close[c] = NAN; raw[c] = NAN; volume[c] = NAN
values = [1.0, 2.0, 3.0]
for t in range(D):
    for i in range(N):
        signal[k(t, i)] = values[i] if member[k(t, i)] else NAN


def f64(v): return struct.pack('<%dd' % len(v), *v)
def u8(v): return bytes(v)
def i64(v): return struct.pack('<%dq' % len(v), *v)
def u64(v): return struct.pack('<%dQ' % len(v), *v)
def receipt(b): return {'bytes': len(b), 'sha256': sha(b)}
def json_file(v): return (nljson.dump(v) + '\n').encode('utf-8')


# ---- write_artifact(dir, p) ----
finite = [1 if math.isfinite(x) else 0 for x in signal]
finite_count, members = sum(finite), sum(member)
files = {
    'train_combined.f64': receipt(f64(signal)),
    'train_combined_member.u8': receipt(u8(member)),
    'train_combined_finite.u8': receipt(u8(finite)),
    'train_combined_sessions.i64': receipt(i64(sessions)),
    'train_combined_ids.u64': receipt(u64(ids)),
}
pin = 'a' * 64
manifest = {
    'schema': 'atx.dsl-combined-signal/v1', 'status': 'complete', 'role': 'train',
    'layout': 'date-major-little-endian', 'dates': D, 'instruments': N,
    'score_begin': 0, 'score_end': D, 'role_manifest_sha256': pin, 'source_sha256': pin,
    'library_sha256': pin, 'train_manifest_sha256': pin, 'run_recipe_sha256': pin,
    'orientation_candidates_sha256': pin, 'orientations_artifact_sha256': None,
    'role_window_required': True,
    'signal_semantics': 'exact-pre-target-composition;equal-family/equal-within;'
                        'missing-or-unoriented-neutral-fixed-denominator',
    'member_semantics': 'decision-member-and-source-present-and-finite-positive-close;'
                        'independent-of-component-coverage',
    'finite_semantics': 'one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal',
    'actual_trades_or_returns': False, 'finite_cells': finite_count, 'member_cells': members,
    'files': files,
}
role_files = {
    'sessions.i64': receipt(i64(sessions)), 'ids.u64': receipt(u64(ids)),
    'close.f64': receipt(f64(close)), 'raw_close.f64': receipt(f64(raw)),
    'present.u8': receipt(u8(present)), 'member.u8': receipt(u8(member)),
    'volume.f64': receipt(f64(volume)),
}
role = {
    'schema': 'atx.recent-research-role/v1', 'status': 'complete', 'source_sha256': pin,
    'instrument_namespace': 'spiderrock.securityID',
    'close_basis': 'f64(raw-f32-close)*f64-cumulReturnFactor',
    'volume_basis': 'raw-share-volume',
    'clock_recipe': 'modeled-session+22h-mark+23h-decision-v1',
    'common_stock_verified': False, 'historical_vintage_verified': False,
    'dates': D, 'instruments': N, 'score_begin': 0, 'score_end': D, 'files': role_files,
}
role_sha256 = sha(json_file(role))
manifest['role_manifest_sha256'] = role_sha256
combined_sha256 = sha(json_file(manifest))

# ---- nav_recipe(cfg, base, fixed_nav_scenarios(), NavTurnoverLimits{}, null) ----
legacy_financing = {
    'id': 'flat-300-v0', 'rule': 'flat-short-v0', 'flat_short_bps': 300.0,
    'long_spread_bps': 0.0, 'short_spread_bps': 0.0,
    'tier_fee_bps': {'gc': 0.0, 'warm': 0.0, 'special': 0.0},
    'day_count': 'ACT/365', 'block_special_shorts': False,
}


def scenario(sid, primary, cost, flat_bps, half, comm, y, delta, part, stale, adverse):
    return {'id': sid, 'trading_scenario': sid, 'financing_scenario': 'flat-300-v0',
            'primary': primary, 'cost_rule': cost, 'flat_bps': flat_bps,
            'half_spread_bps': half, 'commission_bps': comm, 'impact_y': y,
            'impact_delta': delta, 'max_participation': part,
            'financing': legacy_financing, 'fallback_daily_vol': 0.05,
            'stale_exit_sessions': stale,
            'terminal_haircut_long': -0.55 if adverse else 0.0,
            'terminal_haircut_short': 0.30 if adverse else 0.0}


scenarios = [
    scenario('linear-6bps-stale5-v1', False, 'flat-bps-v1', 6.0, 0.0, 0.0, 0.0, 0.5,
             'uncapped', 5, False),
    scenario('modeled-1bn-stale5-v1', True, 'sqrt-impact-v1', 0.0, 5.0, 1.0, 0.6, 0.5,
             0.01, 5, False),
    scenario('modeled-1bn-terminal-adverse-v1', False, 'sqrt-impact-v1', 0.0, 5.0, 1.0, 0.6,
             0.5, 0.01, 1, True),
]
C = lambda name: cxx_constant(NAV_SRC, name)
nav = {
    'schema': 'atx.dsl-nav-replay/v1', 'combined_sha256': combined_sha256,
    'role_sha256': role_sha256, 'rule': 'baseline-target-v1', 'cadence': 5,
    'trade_fraction': 0.25, 'monthly_budget': 0.30, 'initial_nav': 1e9,
    'liquidity_window': 63, 'min_vol_pairs': 20, 'max_events': 262144,
    'max_working_bytes': 512 << 20, 'scenarios': scenarios,
    'primary_scenario': 'modeled-1bn-stale5-v1', 'sharpe_target': 1.0,
    'monthly_turnover_target': 0.30, 'timing': C('timing_declaration'),
    'accounting': C('accounting_declaration'), 'target': C('target_declaration'),
    'turnover': C('turnover_definition'), 'missing': C('missing_declaration'),
    'guard': C('guard_declaration'), 'liquidity': C('liquidity_declaration'),
    'desired_target_postprocess': 'none',
    'cost_input_status': 'declared-unfitted-scenario-no-locate',
    'monthly_turnover_target_status': 'legacy-reporting-only;retired-2026-09-27',
    'daily_turnover_mean_max': 0.20, 'daily_turnover_p95_max': 0.30,
    'daily_turnover_definition': C('daily_turnover_definition'),
    'financing': C('financing_declaration'), 'primary_financing_available': False,
    'financing_fields': None,
}
target = {
    'schema': 'atx.dsl-target-replay/v1', 'rule': 'baseline-target-v1',
    'combined_sha256': combined_sha256, 'role_sha256': role_sha256, 'cadence': 5,
    'trade_fraction': 0.25, 'monthly_budget': 0.30, 'calendar_basis': 'decision-session-month',
    'deployment_included': True,
    'forced_exits': 'immediate;charged-before-discretionary;may-breach',
    'target': 'tied-rank;neutral-desired-gross1;partial-no-drift-no-renormalization',
    'rough_returns': True, 'entry_delay': 1, 'return_horizon': 1,
    'missing_return': 'complete-day-undefined;observed-component-and-missing-exposures',
    'return_guard': 'observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill',
    'one_way_bps': 0.0, 'annual_borrow_bps': 0.0,
    'cost_basis': 'target-change-no-drift;short-target/252;not-fills',
    'max_working_bytes': 512 << 20,
}
print('role_sha256', role_sha256)
print('combined_sha256', combined_sha256)
print('nav_recipe_file_sha256', sha(json_file(nav)))
print('nav_recipe_compact_sha256', sha(nljson.dump_compact(nav).encode('utf-8')))
print('target_recipe_file_sha256', sha(json_file(target)))
print('nav keys', len(nav), sorted(nav))
if len(sys.argv) > 3:
    open(sys.argv[3], 'wb').write(json_file(nav))
