"""Time the first native universe year in a separate scratch database; never publish."""
import hashlib
import json
import time
from pathlib import Path
from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import universe as U

name = 'release-universe-year-diagnostic-v1.duckdb'
path = C.build_root() / '_tmp' / name
if path.exists():
    raise RuntimeError('Preserve prior diagnostic database; choose a new version')
pins = {str(Path(U.__file__)): hashlib.sha256(Path(U.__file__).read_bytes()).hexdigest()}
for stage in ('panel', 'issuer_history', 'issuer_classes', 'universe'):
    manifest = C.build_root() / stage / 'manifest.json'
    if manifest.exists():
        pins[str(manifest)] = hashlib.sha256(manifest.read_bytes()).hexdigest()
print(json.dumps({'event': 'start', 'pins': pins, 'year': 2018}), flush=True)
con = C.connect(memory='380MB', threads=2, db_file=name)
started = time.monotonic()


class TimedConnection:
    def execute(self, sql, *args, **kwargs):
        label = ' '.join(sql.split()[:6])
        begin = time.monotonic()
        print(json.dumps({'event': 'begin', 'statement': label,
                          'sql_sha256': hashlib.sha256(sql.encode()).hexdigest(),
                          'elapsed_s': round(begin - started, 3)}), flush=True)
        result = con.execute(sql, *args, **kwargs)
        print(json.dumps({'event': 'end', 'statement': label,
                          'seconds': round(time.monotonic() - begin, 3)}), flush=True)
        return result


try:
    timed = TimedConnection()
    U.load_issuer_tables(timed)
    U.build_year(timed, 2018)
    rows, keys = con.execute('SELECT count(*), count(DISTINCT (session_date, security_id)) FROM t').fetchone()
    if not rows == keys:
        raise RuntimeError('Unexpected duplicate universe diagnostic keys')
    for filename, digest in pins.items():
        if hashlib.sha256(Path(filename).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'Diagnostic source/code manifest changed: {filename}')
    print(json.dumps({'status': 'YEAR_DIAGNOSTIC_COMPLETE', 'year': 2018,
                      'rows': rows, 'unique_keys': keys,
                      'seconds': round(time.monotonic() - started, 3),
                      'published': False, 'pins': pins}), flush=True)
finally:
    con.close()
