import json
from pathlib import Path
import duckdb

connection = duckdb.connect('C:/atx/atx-db/data/warehouse.duckdb', read_only=True,
                            config={'memory_limit': '128MiB', 'threads': 1})
connection.execute('BEGIN TRANSACTION')
queries = {
    'terminal_observation_sources': """SELECT source,provider,count(*) AS rows,
        min(delist_date) AS first_date,max(delist_date) AS last_date,
        count(*) FILTER(WHERE delist_date>=DATE '2020-01-01' AND delist_date<DATE '2025-01-01') AS recent_rows
        FROM delisting_return_observations GROUP BY source,provider ORDER BY rows DESC LIMIT 30""",
    'terminal_return_sources': """SELECT source,terminal_return_source,terminal_return_policy,count(*) AS rows,
        min(delist_date) AS first_date,max(delist_date) AS last_date,
        count(*) FILTER(WHERE delist_date>=DATE '2020-01-01' AND delist_date<DATE '2025-01-01') AS recent_rows
        FROM delisting_terminal_returns GROUP BY source,terminal_return_source,terminal_return_policy ORDER BY rows DESC LIMIT 30""",
    'event_sources': """SELECT source,method,inferred_from_absence,count(*) AS rows,
        min(delist_date) AS first_date,max(delist_date) AS last_date,
        count(*) FILTER(WHERE delist_date>=DATE '2020-01-01' AND delist_date<DATE '2025-01-01') AS recent_rows
        FROM delisting_events GROUP BY source,method,inferred_from_absence ORDER BY rows DESC LIMIT 30""",
    'type_sources': """SELECT source,security_type,count(*) AS rows,
        min(valid_from) AS first_date,max(valid_to) AS last_date,min(available_at) AS first_available,
        max(available_at) AS last_available
        FROM universe_us_listed_membership GROUP BY source,security_type ORDER BY rows DESC LIMIT 30""",
}
result = {'mode': 'read-only-counts-and-source-coverage;no-event-values-or-strategy-performance', 'queries': {}}
for name, sql in queries.items():
    cursor = connection.execute(sql)
    names = [column[0] for column in cursor.description]
    result['queries'][name] = [dict(zip(names, row)) for row in cursor.fetchall()]
connection.execute('COMMIT')
connection.close()
encoded = json.dumps(result, default=str, indent=2) + '\n'
with Path('build-equity/existing-event-coverage.json').open('x', encoding='utf-8') as out:
    out.write(encoded)
print(encoded)
