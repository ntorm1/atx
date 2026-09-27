"""Audit fixed saved-blend target replays (baseline-v1, monthly-budget-v2); never fits anything.

usage: python audit-target-replay.py --source <ROLE>_planned_targets.csv --baseline DIR --budget DIR --output NEW.json
Schemas: atx-impl/src/strategy_ic_runner.cpp (planned targets), strategy_target_replay.cpp (recipe/daily/summary).
Exit 1 on any mismatch. Rough returns are complete-day sums/means only: NOT period returns, NAV or Sharpe.
"""
import argparse, csv, datetime, hashlib, json, math, struct, sys
from pathlib import Path

SOURCE_HEADER = ('decision_index,session_ns,planned_turnover,planned_gross,planned_net,'
                 'contribution_fraction,eligible_names').split(',')
DAILY_HEADER = ('decision,session_ns,month,entry,endpoint,turnover,forced,discretionary,deployment,'
                'month_turnover,budget_excess,applied_fraction,gross,net,long_weight,short_weight,'
                'max_abs_weight,effective_names,held_names,return_mature,return_complete,'
                'observed_return_component,missing_long,missing_short,missing_gross,missing_names,'
                'guarded_names,modeled_trade_cost,modeled_borrow_cost,complete_gross_return,'
                'complete_net_return').split(',')
INT_COLS = {'decision', 'session_ns', 'month', 'entry', 'endpoint', 'held_names', 'return_mature',
            'return_complete', 'missing_names', 'guarded_names'}
PAIRS = (('planned_turnover', 'turnover'), ('planned_gross', 'gross'), ('planned_net', 'net'))
RULES = {'baseline': 'baseline-target-v1', 'budget': 'monthly-target-budget-v2'}
RECIPE = {'schema': 'atx.dsl-target-replay/v1', 'cadence': 5, 'trade_fraction': 0.25, 'monthly_budget': 0.30,
    'calendar_basis': 'decision-session-month', 'deployment_included': True,
    'forced_exits': 'immediate;charged-before-discretionary;may-breach',
    'target': 'tied-rank;neutral-desired-gross1;partial-no-drift-no-renormalization',
    'rough_returns': True, 'entry_delay': 1, 'return_horizon': 1,
    'missing_return': 'complete-day-undefined;observed-component-and-missing-exposures',
    'return_guard': 'observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill',
    'one_way_bps': 6.0, 'annual_borrow_bps': 300.0,
    'cost_basis': 'target-change-no-drift;short-target/252;not-fills'}
RECIPE_KEYS = set(RECIPE) | {'rule', 'combined_sha256', 'role_sha256', 'max_working_bytes'}
SUMMARY_FIXED = {'schema': 'atx.dsl-target-replay-summary/v1', 'status': 'complete', 'net_sharpe': None,
    'self_financing_nav': False, 'capacity_qualified': False,
    'limitations': 'planned weights without drift; decision-month turnover includes deployment; '
        'rough delayed observed returns only, incomplete days unavailable; no corporate-action accounting, '
        'fills, funding, realized fees, capacity, NAV or realistic Sharpe'}
SUMMARY_KEYS = set(SUMMARY_FIXED) | {'months', 'total_turnover', 'monthly_reconciled_total',
    'mean_monthly_turnover', 'max_monthly_turnover', 'forced_turnover', 'discretionary_turnover',
    'deployment_turnover', 'deployment_date_index', 'mean_gross', 'max_abs_net', 'max_abs_name_weight',
    'mature_return_days', 'complete_return_days', 'incomplete_return_days', 'summed_missing_gross_exposure',
    'observed_component_sum_not_portfolio_return', 'complete_days_gross_sum_not_total_period_return',
    'complete_days_net_sum_not_total_period_return', 'modeled_trade_cost_all_decisions',
    'modeled_borrow_cost_all_decisions', 'recipe_sha256', 'combined_sha256', 'source_bindings', 'daily_csv_sha256'}
DAY_NS, TOL = 86_400 * 10**9, 1e-11  # TOL: float-rounding slack for non-replicable accumulation orders only
NAN_TOKENS = {'nan', '-nan', 'nan(ind)', '-nan(ind)', 'nan(snan)', '-nan(snan)'}
INPUTS = {}


class AuditError(Exception):
    pass


def require(ok, message):
    if not ok:
        raise AuditError(message)


def bits(x): return struct.pack('<d', x)


def same(a, b):  # type-strict identity; floats bit-for-bit (IEEE f64)
    return type(a) is type(b) and (bits(a) == bits(b) if isinstance(a, float) else a == b)


def show(x): return f'{x!r} ({x.hex()})' if isinstance(x, float) else repr(x)


def check(label, actual, expected):
    require(same(actual, expected), f'{label}: got {show(actual)}, expected {show(expected)}')


def close(label, a, b, rel=1e-9):
    require(math.isclose(a, b, rel_tol=rel, abs_tol=TOL), f'{label}: {a!r} vs {b!r}')


def load(path):
    data = path.read_bytes()
    INPUTS[str(path)] = dict(sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
    return data


def parse_json(path):
    def bad(token): raise AuditError(f'{path}: non-standard JSON constant {token}')
    return json.loads(load(path).decode('utf-8'), parse_constant=bad)


def f64(text, label):
    if text.lower() in NAN_TOKENS:
        return math.nan
    require(text in ('inf', '-inf') or (text and set(text) <= set('0123456789+-.eE')), f'{label}: bad f64 {text!r}')
    return float(text)  # correctly rounded: equal doubles iff equal values, whatever the text form


def integer(text, label):
    require(text.isascii() and text.isdigit(), f'{label}: bad integer {text!r}')
    return int(text)


def table(path, header):
    text = load(path).decode('ascii')
    require(text.endswith('\n') and '\r' not in text, f'{path}: not a complete LF-terminated CSV')
    rows = list(csv.reader(text[:-1].split('\n')))
    require(rows[0] == header, f'{path}: header {rows[0]} != {header}')
    for n, row in enumerate(rows[1:], 2):
        require(len(row) == len(header), f'{path}:{n}: {len(row)} fields, expected {len(header)}')
    require(len(rows) > 1, f'{path}: no data rows')
    return [dict(zip(header, row)) for row in rows[1:]]


def month_of(ns, label):
    require(0 < ns and ns % DAY_NS == 0, f'{label}: session {ns} is not a UTC midnight')
    date = datetime.date(1970, 1, 1) + datetime.timedelta(days=ns // DAY_NS)
    return date.year * 100 + date.month


def is_sha(s): return isinstance(s, str) and len(s) == 64 and set(s) <= set('0123456789abcdef')


def mean(total, count): return total / count if count else None


def audit_policy(policy, directory):
    recipe_path, daily_path, summary_path = (directory / n for n in ('recipe.json', 'daily.csv', 'summary.json'))
    recipe = parse_json(recipe_path)
    raw = table(daily_path, DAILY_HEADER)
    summary = parse_json(summary_path)
    days = [{c: (integer if c in INT_COLS else f64)(r[c], f'{daily_path}:{k + 2}:{c}') for c in DAILY_HEADER}
            for k, r in enumerate(raw)]
    # Declared recipe and summary identity.
    require(set(recipe) == RECIPE_KEYS, f'{recipe_path}: key drift {sorted(set(recipe) ^ RECIPE_KEYS)}')
    check(f'{recipe_path}:rule', recipe['rule'], RULES[policy])
    for key, value in RECIPE.items():
        check(f'{recipe_path}:{key}', recipe[key], value)
    require(is_sha(recipe['combined_sha256']) and is_sha(recipe['role_sha256']), f'{recipe_path}: pins')
    require(type(recipe['max_working_bytes']) is int, f'{recipe_path}: max_working_bytes')
    require(set(summary) == SUMMARY_KEYS, f'{summary_path}: key drift {sorted(set(summary) ^ SUMMARY_KEYS)}')
    for key, value in SUMMARY_FIXED.items():
        check(f'{summary_path}:{key}', summary[key], value)
    check(f'{summary_path}:daily_csv_sha256', summary['daily_csv_sha256'], INPUTS[str(daily_path)]['sha256'])
    compact = json.dumps(recipe, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    check(f'{summary_path}:recipe_sha256', summary['recipe_sha256'], hashlib.sha256(compact).hexdigest())
    check(f'{summary_path}:combined_sha256', summary['combined_sha256'], recipe['combined_sha256'])
    b = summary['source_bindings']
    check(f'{summary_path}:source_bindings.schema', b['schema'], 'atx.dsl-combined-signal/v1')
    check(f'{summary_path}:source_bindings.status', b['status'], 'complete')
    check(f'{summary_path}:source_bindings.role_manifest_sha256', b['role_manifest_sha256'], recipe['role_sha256'])
    begin, end, dates = b['score_begin'], b['score_end'], b['dates']
    require(0 <= begin < end <= dates and len(days) == end - begin,
            f'{daily_path}: {len(days)} rows for score window [{begin},{end}) of {dates} dates')
    cadence, fraction, budget = RECIPE['cadence'], RECIPE['trade_fraction'], RECIPE['monthly_budget']
    one_way, borrow = recipe['one_way_bps'], recipe['annual_borrow_bps']
    month, spent, deploy, prev_session = 0, 0.0, dates, 0
    for k, d in enumerate(days):
        dec = d['decision']; at = f'{daily_path} decision {dec}'
        check(f'{at} index', dec, begin + k)
        require(d['session_ns'] > prev_session, f'{at}: sessions not strictly increasing')
        prev_session = d['session_ns']
        check(f'{at} month', d['month'], month_of(d['session_ns'], at))
        check(f'{at} entry', d['entry'], min(dates, dec + 1))
        check(f'{at} endpoint', d['endpoint'], min(dates, dec + 2))
        if d['month'] != month:
            month, spent = d['month'], 0.0
        spent += d['turnover']
        check(f'{at} month_turnover', d['month_turnover'], spent)
        check(f'{at} budget_excess', d['budget_excess'], max(0.0, spent - budget) if policy == 'budget' else 0.0)
        for c in ('turnover', 'forced', 'discretionary', 'gross', 'long_weight', 'short_weight', 'max_abs_weight'):
            require(math.isfinite(d[c]) and d[c] >= 0, f'{at}: {c}={d[c]!r}')
        require(math.isfinite(d['net']), f'{at}: net')
        close(f'{at} turnover=forced+discretionary', d['turnover'], d['forced'] + d['discretionary'])
        close(f'{at} gross=long+short', d['gross'], d['long_weight'] + d['short_weight'])
        close(f'{at} net=long-short', d['net'], d['long_weight'] - d['short_weight'])
        af = d['applied_fraction']
        if k % cadence:
            check(f'{at} off-cycle applied_fraction', af, 0.0)
        elif policy == 'baseline':
            check(f'{at} applied_fraction', af, fraction)
        else:
            require(0.0 <= af <= fraction, f'{at}: applied_fraction {af!r}')
        if af == 0.0:
            check(f'{at} discretionary without fraction', d['discretionary'], 0.0)
        if policy == 'budget' and d['discretionary'] > 0:
            require(spent <= budget + TOL, f'{at}: discretionary trade pushed month turnover to {spent!r} > {budget}')
        if deploy == dates and d['gross'] > 0:
            deploy = dec
            check(f'{at} deployment', d['deployment'], d['turnover'])
        else:
            check(f'{at} deployment', d['deployment'], 0.0)
        check(f'{at} modeled_trade_cost', d['modeled_trade_cost'], d['turnover'] * one_way / 10000)
        check(f'{at} modeled_borrow_cost', d['modeled_borrow_cost'], d['short_weight'] * borrow / 2520000)
        check(f'{at} missing_gross', d['missing_gross'], d['missing_long'] + d['missing_short'])
        mature, complete = d['return_mature'], d['return_complete']
        require(mature in (0, 1) and complete in (0, 1), f'{at}: flags')
        check(f'{at} return_mature', mature, int(dec + 2 < end))
        require(math.isfinite(d['observed_return_component']), f'{at}: observed component')
        if complete:
            require(mature and d['missing_names'] == 0 and d['guarded_names'] == 0 and d['missing_gross'] == 0.0,
                    f'{at}: complete day with missing exposure')
            check(f'{at} complete_gross_return', d['complete_gross_return'], d['observed_return_component'])
            check(f'{at} complete_net_return', d['complete_net_return'],
                  d['complete_gross_return'] - d['modeled_trade_cost'] - d['modeled_borrow_cost'])
        else:
            require(math.isnan(d['complete_gross_return']) and math.isnan(d['complete_net_return']),
                    f'{at}: incomplete day must carry NaN complete returns')
            if mature:
                require(d['missing_names'] >= 1 and d['guarded_names'] <= d['missing_names'] and d['missing_gross'] > 0,
                        f'{at}: mature incomplete day without disclosed missing exposure')
            else:
                require(d['missing_names'] == d['guarded_names'] == 0 and d['missing_gross'] == 0.0
                        and d['observed_return_component'] == 0.0, f'{at}: immature day carries return data')
    # Summary reconciliation: same accumulation order as summarize(), so exact.
    months = {}
    total = forced = discretionary = gross = max_net = max_name = missing = observed = 0.0
    cgross = cnet = trade = borrow_cost = 0.0
    mature_n = complete_n = 0
    for d in days:
        m = months.setdefault(d['month'], dict(days=0, turnover=0.0, forced=0.0, discretionary=0.0,
                                                deployment=0.0, excess=0.0))
        m['days'] += 1; m['turnover'] += d['turnover']; m['forced'] += d['forced']
        m['discretionary'] += d['discretionary']; m['deployment'] += d['deployment']
        m['excess'] = max(m['excess'], d['budget_excess'])
        total += d['turnover']; forced += d['forced']; discretionary += d['discretionary']
        gross += d['gross']; max_net = max(max_net, abs(d['net'])); max_name = max(max_name, d['max_abs_weight'])
        missing += d['missing_gross']; mature_n += d['return_mature']; complete_n += d['return_complete']
        if d['return_mature']: observed += d['observed_return_component']
        if d['return_complete']: cgross += d['complete_gross_return']; cnet += d['complete_net_return']
        trade += d['modeled_trade_cost']; borrow_cost += d['modeled_borrow_cost']
    require(list(months) == sorted(months), f'{daily_path}: months not chronological')
    require(len(summary['months']) == len(months), f'{summary_path}: month count')
    reconciled = max_month = 0.0
    above, breaches, rounding = [], [], []
    for row, (mid, m) in zip(summary['months'], months.items()):
        at = f'{summary_path} month {mid}'
        require(set(row) == {'month', 'observed_decision_sessions', 'turnover', 'forced', 'discretionary',
                             'deployment', 'budget_excess'}, f'{at}: key drift')
        check(f'{at} month', row['month'], mid)
        check(f'{at} observed_decision_sessions', row['observed_decision_sessions'], m['days'])
        for key in ('turnover', 'forced', 'discretionary', 'deployment'):
            check(f'{at} {key}', row[key], m[key])
        check(f'{at} budget_excess', row['budget_excess'], m['excess'])
        reconciled += m['turnover']; max_month = max(max_month, m['turnover'])
        if m['turnover'] > budget:
            item = dict(month=mid, turnover=m['turnover'], excess=m['turnover'] - budget, forced=m['forced'],
                        disclosed_budget_excess=m['excess'])
            above.append(item)
            if policy == 'budget':
                require(m['excess'] > 0.0, f'{at}: breach not disclosed')
                if item['excess'] <= TOL:
                    rounding.append(item)
                else:
                    require(item['excess'] <= m['forced'] + TOL, f'{at}: breach exceeds forced exits: {item}')
                    breaches.append(item)
    expect = {'total_turnover': total, 'monthly_reconciled_total': reconciled,
        'mean_monthly_turnover': reconciled / len(months), 'max_monthly_turnover': max_month,
        'forced_turnover': forced, 'discretionary_turnover': discretionary,
        'deployment_turnover': days[deploy - begin]['turnover'] if deploy < dates else 0.0,
        'deployment_date_index': deploy, 'mean_gross': gross / len(days), 'max_abs_net': max_net,
        'max_abs_name_weight': max_name, 'mature_return_days': mature_n, 'complete_return_days': complete_n,
        'incomplete_return_days': mature_n - complete_n, 'summed_missing_gross_exposure': missing,
        'observed_component_sum_not_portfolio_return': observed,
        'complete_days_gross_sum_not_total_period_return': cgross,
        'complete_days_net_sum_not_total_period_return': cnet,
        'modeled_trade_cost_all_decisions': trade, 'modeled_borrow_cost_all_decisions': borrow_cost}
    for key, value in expect.items():
        check(f'{summary_path}:{key}', summary[key], value)
    close(f'{summary_path}: daily vs monthly total', total, reconciled, rel=1e-12)
    incomplete = [d for d in days if d['return_mature'] and not d['return_complete']]
    result = dict(policy=policy, rule=recipe['rule'], directory=str(directory), decisions=len(days),
        first_decision=days[0]['decision'], last_decision=days[-1]['decision'], months=len(months),
        total_turnover=total, mean_monthly_turnover=reconciled / len(months), max_monthly_turnover=max_month,
        forced_turnover=forced, discretionary_turnover=discretionary, deployment_decision=deploy,
        deployment_turnover=expect['deployment_turnover'], months_above_monthly_budget=above,
        mean_gross=gross / len(days), max_abs_net=max_net, max_abs_name_weight=max_name,
        min_held_names=min(d['held_names'] for d in days),
        mean_held_names=sum(d['held_names'] for d in days) / len(days),
        mean_effective_names=sum(d['effective_names'] for d in days) / len(days),
        mature_days=mature_n, complete_days=complete_n, incomplete_nan_days=len(incomplete),
        incomplete_days_with_guarded_names=sum(1 for d in incomplete if d['guarded_names']),
        incomplete_missing_long_sum=sum(d['missing_long'] for d in incomplete),
        incomplete_missing_short_sum=sum(d['missing_short'] for d in incomplete),
        incomplete_missing_gross_max=max((d['missing_gross'] for d in incomplete), default=0.0),
        trade_cost_all_decisions=trade, borrow_cost_all_decisions=borrow_cost,
        complete_day_gross_return_sum_not_period_return=cgross,
        complete_day_net_return_sum_not_period_return=cnet,
        complete_day_gross_return_mean_per_day=mean(cgross, complete_n),
        complete_day_net_return_mean_per_day=mean(cnet, complete_n))
    if policy == 'budget':
        result.update(forced_exit_breach_months=breaches, rounding_level_breach_months=rounding)
    return result, recipe, summary, raw, days


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for flag in ('--source', '--baseline', '--budget', '--output'):
        parser.add_argument(flag, type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), f'{args.output}: refusing to overwrite an existing file')
    source = table(args.source, SOURCE_HEADER)
    base, base_recipe, base_summary, base_raw, base_days = audit_policy('baseline', args.baseline)
    budg, budg_recipe, budg_summary, _, budg_days = audit_policy('budget', args.budget)
    # The two policies replay one pinned blend over one window.
    for key in RECIPE_KEYS - {'rule', 'max_working_bytes'}:
        check(f'recipes:{key}', budg_recipe[key], base_recipe[key])
    require(budg_summary['source_bindings'] == base_summary['source_bindings'], 'summaries: source_bindings differ')
    require(len(base_days) == len(budg_days), 'budget/baseline row counts differ')
    for a, b in zip(base_days, budg_days):
        for c in ('decision', 'session_ns', 'month', 'entry', 'endpoint', 'return_mature'):
            check(f'budget vs baseline decision {a["decision"]} {c}', b[c], a[c])
    # Baseline must reproduce the IC runner's planned-target arithmetic bit-for-bit.
    bind = base_summary['source_bindings']
    require(args.source.name == f"{bind['role']}_planned_targets.csv",
            f'{args.source}: not the {bind["role"]} role planned-target CSV of the saved blend')
    require(len(source) == len(base_days), f'{args.source}: {len(source)} rows vs {len(base_days)} replay decisions')
    mismatches, textual, exact = [], 0, 0
    for n, (s, r, d) in enumerate(zip(source, base_raw, base_days), 2):
        at = f'{args.source}:{n}'
        check(f'{at} decision_index', integer(s['decision_index'], at), d['decision'])
        check(f'{at} session_ns', integer(s['session_ns'], at), d['session_ns'])
        f64(s['contribution_fraction'], at); integer(s['eligible_names'], at)
        for src, col in PAIRS:
            want = f64(s[src], f'{at}:{src}')
            if not (math.isfinite(want) and same(d[col], want)):
                mismatches.append(f'decision {d["decision"]} {col}: replay {show(d[col])} source {show(want)}')
            else:
                exact += 1
            textual += s[src] != r[col]
    require(not mismatches, f'baseline != source planned targets ({len(mismatches)}): ' + '; '.join(mismatches[:8]))
    common = [(a, b) for a, b in zip(base_days, budg_days) if a['return_complete'] and b['return_complete']]
    for k, result in enumerate((base, budg)):
        g = sum(pair[k]['complete_gross_return'] for pair in common)
        n = sum(pair[k]['complete_net_return'] for pair in common)
        result.update(common_complete_days=len(common), common_complete_gross_return_sum_not_period_return=g,
                      common_complete_net_return_sum_not_period_return=n,
                      common_complete_gross_return_mean_per_day=mean(g, len(common)),
                      common_complete_net_return_mean_per_day=mean(n, len(common)))
    out = dict(schema='atx.fixed-target-comparison-audit/v2', status='pass',
        audit_script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        inputs=[dict(path=p, **v) for p, v in INPUTS.items()],
        combined_sha256=base_recipe['combined_sha256'], role_sha256=base_recipe['role_sha256'],
        score_window=[bind['score_begin'], bind['score_end']],
        exact_baseline_f64_comparisons=exact, baseline_textual_format_differences=textual,
        results=[base, budg],
        limitation='Planned-target proxy without drift, fills or cash book; rough returns are constant-weight '
                   'd+1->d+2 approximations; incomplete (missing/guarded) days are NaN and excluded, their '
                   'exposure disclosed. Sums/means are over complete days only: not period returns, NAV, '
                   'Sharpe, impact or capacity estimates.')
    text = json.dumps(out, indent=2, allow_nan=False) + '\n'
    with open(args.output, 'x', encoding='ascii', newline='\n') as handle:
        handle.write(text)
    print(text, end='')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:  # every failure, including schema KeyErrors, is a loud non-zero exit
        print(f'AUDIT FAIL: {type(error).__name__}: {error}', file=sys.stderr)
        sys.exit(1)
