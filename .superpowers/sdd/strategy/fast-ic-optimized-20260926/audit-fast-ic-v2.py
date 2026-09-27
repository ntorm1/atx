from pathlib import Path
import collections, csv, datetime, hashlib, json, math, statistics

root = Path(__file__).resolve().parent
v1, v2 = root / 'recent-fast-ic-v1', root / 'recent-fast-ic-v2'
def rows(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))
def ledger(path):
    return [json.loads(x) for x in path.read_text().splitlines()]
old = {x['id']: x for x in ledger(v1 / 'train_candidates.jsonl') if x['status'] == 'complete'}
new = {x['id']: x for x in ledger(v2 / 'train_candidates.jsonl') if x['status'] == 'complete'}
old_rows = rows(v1 / 'train_daily_ic.csv')
truncated_rows = sum(any(v is None for v in x.values()) for x in old_rows)
old_daily = {(x['id'], x['horizon'], x['decision_index']): x
             for x in old_rows if x['id'] in old and all(v is not None for v in x.values())}
max_delta = 0.0
compared = differences = 0
for row in rows(v2 / 'train_daily_ic.csv'):
    key = row['id'], row['horizon'], row['decision_index']
    if key not in old_daily:
        continue
    for field in ['pearson', 'rank_ic', 'oriented_rank_ic']:
        a, b = old_daily[key][field], row[field]
        if bool(a) != bool(b):
            raise RuntimeError(f'changed availability: {key} {field} before={a!r} after={b!r}; before_row={old_daily[key]!r}')
        if a:
            delta = abs(float(a) - float(b))
            max_delta = max(max_delta, delta)
            differences += delta != 0
            compared += 1
if not compared:
    raise RuntimeError('no comparable completed candidate series')
monthly = collections.defaultdict(float)
gross, net, contributors = [], [], []
for row in rows(v2 / 'train_planned_targets.csv'):
    month = datetime.datetime.fromtimestamp(int(row['session_ns']) / 1e9, datetime.timezone.utc).strftime('%Y-%m')
    monthly[month] += float(row['planned_turnover'])
    gross.append(float(row['planned_gross']))
    net.append(float(row['planned_net']))
    contributors.append(float(row['contribution_fraction']))
summary = json.loads((v2 / 'summary.json').read_text())
train = summary['roles'][0]
val_ledger = ledger(v2 / 'validation_candidates.jsonl')
report = dict(
    train_complete=len(new), validation_started=sum(x['status'] == 'started' for x in val_ledger),
    validation_complete=sum(x['status'] == 'complete' for x in val_ledger),
    common_candidates=len(old), baseline_truncated_csv_rows_excluded=truncated_rows,
    comparable_values=compared, nonzero_differences=differences,
    maximum_absolute_ic_difference=max_delta,
    paired_candidate_seconds=dict(before=sum(x['wall_seconds'] for x in old.values()),
                                  after=sum(new[k]['wall_seconds'] for k in old)),
    train_wall_seconds=train['wall_seconds'], train_stage_seconds=train['stage_seconds'],
    combined_ic=train['combined_ic'],
    planned_target_proxy=dict(monthly_turnover=dict(monthly),
        mean_monthly_turnover=statistics.mean(monthly.values()),
        max_monthly_turnover=max(monthly.values()), initial_deployment_included=True,
        deployment_turnover=train['planned_target_proxy']['deployment_turnover'],
        mean_gross=statistics.mean(gross), minimum_gross=min(gross),
        maximum_absolute_net=max(map(abs, net)), mean_contribution_fraction=statistics.mean(contributors),
        interpretation='planned sum(abs(weight change)); no price drift, executed trades, costs or capacity'),
    orientations_sha256=hashlib.sha256((v2 / 'orientations.json').read_bytes()).hexdigest())
path = root / 'recent-fast-ic-v2-audit.json'
with path.open('x', encoding='utf-8') as f:
    json.dump(report, f, indent=2); f.write('\n')
print(json.dumps({k:v for k,v in report.items() if k not in ['combined_ic', 'planned_target_proxy']}, indent=2))
print(json.dumps({k:v for k,v in report['planned_target_proxy'].items() if k != 'monthly_turnover'}, indent=2))
