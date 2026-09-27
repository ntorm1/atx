from pathlib import Path
import collections, csv, datetime, hashlib, io, json, statistics

build = Path(__file__).resolve().parent
old_dir, new_dir = build / 'recent-fast-ic-v2', build / 'recent-fast-ic-validation-v3'
def read_json(path):
    return json.loads(path.read_text())
def ledger(path):
    return [json.loads(x) for x in path.read_text().splitlines()]
def csv_rows(path):
    with path.open(newline='') as f:
        return list(csv.DictReader(f))
def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for part in iter(lambda: f.read(1 << 20), b''):
            h.update(part)
    return h.hexdigest()
summary = read_json(new_dir / 'summary.json')
assert summary['status'] == 'complete' and summary['train_candidates_planned'] == 0
assert len(summary['roles']) == 1 and summary['roles'][0]['role'] == 'validation'
role = summary['roles'][0]
original = read_json(old_dir / 'orientations.json')
receipt = read_json(new_dir / 'frozen_train_receipt.json')
assert receipt['artifact'] == original
assert digest(old_dir / 'orientations.json') == summary['orientations_artifact_sha256']
old = {x['id']: x for x in ledger(old_dir / 'validation_candidates.jsonl') if x['status'] == 'complete'}
new = {x['id']: x for x in role['candidates']}
assert len(new) == 48
for name in old:
    for key in ['horizons', 'reject', 'enough_evidence', 'reason', 'frozen_train_sign']:
        assert old[name][key] == new[name][key], (name, key)
signs = {x['id']: x['sign'] for x in original['candidates']}
assert all(x['frozen_train_sign'] == signs[x['id']] for x in new.values())
old_text = (old_dir / 'validation_daily_ic.csv').read_text()
truncated = int(bool(old_text) and not old_text.endswith('\n'))
if truncated:
    old_text = old_text[:old_text.rfind('\n') + 1]
old_rows = list(csv.DictReader(io.StringIO(old_text)))
truncated += sum(any(v is None for v in x.values()) for x in old_rows)
known = {(x['id'], x['horizon'], x['decision_index']): x for x in old_rows
         if x['id'] in old and all(v is not None for v in x.values())}
compared = differences = 0
examples = []
for row in csv_rows(new_dir / 'validation_daily_ic.csv'):
    key = row['id'], row['horizon'], row['decision_index']
    if key not in known:
        continue
    for field in ['pearson', 'rank_ic', 'oriented_rank_ic']:
        a, b = known[key][field], row[field]
        assert bool(a) == bool(b), (key, field)
        if a:
            compared += 1
            differences += float(a) != float(b)
            if float(a) != float(b) and len(examples) < 3:
                examples.append((key, field, a, b))
assert compared and differences == 0, (compared, differences, examples)
monthly = collections.defaultdict(float)
gross, net, contribution, names = [], [], [], []
for row in csv_rows(new_dir / 'validation_planned_targets.csv'):
    month = datetime.datetime.fromtimestamp(int(row['session_ns']) / 1e9, datetime.timezone.utc).strftime('%Y-%m')
    monthly[month] += float(row['planned_turnover'])
    gross.append(float(row['planned_gross'])); net.append(float(row['planned_net']))
    contribution.append(float(row['contribution_fraction'])); names.append(int(row['eligible_names']))
saved = role['combined_artifact']
manifest_path = new_dir / saved['manifest']
assert digest(manifest_path) == saved['manifest_sha256']
manifest = read_json(manifest_path)
assert manifest['orientations_artifact_sha256'] == summary['orientations_artifact_sha256']
assert manifest['orientation_candidates_sha256'] == saved['orientation_candidates_sha256']
payload_bytes = 0
for name, item in manifest['files'].items():
    path = new_dir / name
    assert path.parent == new_dir and path.stat().st_size == item['bytes']
    assert digest(path) == item['sha256']
    payload_bytes += item['bytes']
report = dict(status='verified', train_evaluations=0, validation_candidates=len(new),
    unchanged_train_signs=len(signs), paired_old_candidates=len(old),
    truncated_old_csv_rows_excluded=truncated, compared_daily_values=compared, numeric_differences=differences,
    paired_stage_seconds={key:dict(before=sum(x['stage_seconds'][key] for x in old.values()),
                                  after=sum(new[k]['stage_seconds'][key] for k in old))
                          for key in ['vm', 'ic', 'composition']},
    combined_rank_ic=[dict(horizon=h['horizon'], **h['rank']) for h in role['combined_ic']['horizons']],
    planned_target_proxy=dict(monthly_turnover=dict(monthly), mean_monthly_turnover=statistics.mean(monthly.values()),
        maximum_monthly_turnover=max(monthly.values()), mean_gross=statistics.mean(gross),
        maximum_absolute_net=max(map(abs, net)), mean_contribution_fraction=statistics.mean(contribution),
        minimum_names=min(names), maximum_names=max(names), initial_deployment_included=True,
        interpretation='planned sum(abs(weight change)); no drift, executed fills, costs or capacity'),
    combined_manifest_sha256=saved['manifest_sha256'], verified_combined_payload_bytes=payload_bytes)
with (build / 'recent-fast-ic-validation-v3-audit.json').open('x') as f:
    json.dump(report, f, indent=2); f.write('\n')
brief = dict(report)
brief['planned_target_proxy'] = {k:v for k,v in report['planned_target_proxy'].items() if k != 'monthly_turnover'}
print(json.dumps(brief, indent=2))
