"""Ex-post event-coverage queue; only pinned role axes and two masks are read."""
from pathlib import Path
import datetime as dt
import hashlib
import json
import numpy as np

ROOT = Path('C:/atx-wt/pool-2/build-equity/recent-dev-smoke-v1')
OUT = Path('C:/atx-wt/pool-3/.superpowers/sdd/w2/recent-q1-event-queue.json')
PIN = '900839a1ea8e21edc0f5edd5e9cd8f2bc7884a9295a86d5d6f2de19a79aed36b'
raw = (ROOT / 'manifest.json').read_bytes()
assert hashlib.sha256(raw).hexdigest() == PIN
manifest = json.loads(raw)
D, N = manifest['dates'], manifest['instruments']
assert 0 < D <= 4096 and 0 < N <= 20000 and D * N < 8_000_000
arrays, bindings = {}, {}
for name, dtype, count in [('sessions.i64', '<i8', D), ('ids.u64', '<u8', N),
                            ('present.u8', 'u1', D*N), ('member.u8', 'u1', D*N)]:
    b = (ROOT / name).read_bytes()
    sha = hashlib.sha256(b).hexdigest()
    assert sha == manifest['files'][name]['sha256']
    assert len(b) == manifest['files'][name]['bytes'] == np.dtype(dtype).itemsize * count
    arrays[name] = np.frombuffer(b, dtype=dtype)
    bindings[name] = sha
sessions, ids = arrays['sessions.i64'], arrays['ids.u64']
present = arrays['present.u8'].reshape(D, N).astype(bool)
member = arrays['member.u8'].reshape(D, N).astype(bool)
begin, end = manifest['score_begin'], manifest['score_end']
assert end == D and 0 <= begin < end and (np.diff(sessions) > 0).all()
iso = lambda index: dt.datetime.fromtimestamp(int(sessions[index]) / 1e9, dt.timezone.utc).date().isoformat()
rows = []
for j, security_id in enumerate(ids):
    eligible = np.flatnonzero(member[begin:end, j] & present[begin:end, j]) + begin
    if not len(eligible):
        continue
    missing = np.flatnonzero(~present[int(eligible[0])+1:end, j]) + int(eligible[0]) + 1
    if not len(missing):
        continue
    first_gap = int(missing[0])
    previous_print = int(np.flatnonzero(present[:first_gap, j])[-1])
    following = np.flatnonzero(present[first_gap+1:end, j]) + first_gap + 1
    next_print = int(following[0]) if len(following) else None
    last_print = int(np.flatnonzero(present[:end, j])[-1])
    rows.append({'security_id': int(security_id), 'first_eligible_score_date': iso(int(eligible[0])),
                 'last_eligible_before_gap': iso(int(eligible[eligible < first_gap][-1])),
                 'previous_print': iso(previous_print), 'first_absent_date': iso(first_gap),
                 'next_print_in_role': iso(next_print) if next_print is not None else None,
                 'first_gap_sessions': (next_print if next_print is not None else end) - first_gap,
                 'first_gap_class': 'temporary-with-later-role-print' if next_print is not None else 'no-later-role-print-censored',
                 'member_at_first_absence': bool(member[first_gap, j]),
                 'last_role_print': iso(last_print),
                 'trailing_absence_start': iso(last_print+1) if last_print < end-1 else None,
                 'trailing_absence_sessions': end - last_print - 1})
rows.sort(key=lambda row: (row['first_absent_date'], row['security_id']))
receipt = {'schema': 'atx.recent-role-event-queue/v1', 'manifest_sha256': PIN,
           'bindings': bindings, 'score_first': iso(begin), 'score_last': iso(end-1),
           'definition': 'first missing source row after an earlier scored member AND present observation; membership exit does not erase held-mark risk',
           'qualification': 'EXPOST DIAGNOSTIC ONLY; no terminal inference, price/return reads, or membership/candidate changes',
           'rows': rows, 'affected_ids': len(rows),
           'temporary_first_gap_ids': sum(r['next_print_in_role'] is not None for r in rows),
           'no_later_print_first_gap_ids': sum(r['next_print_in_role'] is None for r in rows)}
OUT.write_text(json.dumps(receipt, sort_keys=True, indent=2) + '\n', encoding='utf8', newline='\n')
print(json.dumps({k:v for k,v in receipt.items() if k not in ('rows','bindings')}, indent=2))
for r in rows:
    print(r['security_id'], r['previous_print'], r['first_absent_date'], r['next_print_in_role'],
          r['first_gap_sessions'], r['member_at_first_absence'], r['trailing_absence_start'])
print('receipt_sha256', hashlib.sha256(OUT.read_bytes()).hexdigest())
