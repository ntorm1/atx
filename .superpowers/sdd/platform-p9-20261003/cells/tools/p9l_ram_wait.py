"""P9L-WBUDGET: timestamped, bounded 60-second host polling; never launches a run."""
import ctypes
import datetime as dt
import shutil
import time
from pathlib import Path

from start_state import MEMSTAT

log = Path('.superpowers/sdd/platform-p9-20261003/cells/logs/p9l-ram-wait-2.txt')
begin = time.monotonic()
while True:
    m = MEMSTAT()
    m.dwLength = ctypes.sizeof(MEMSTAT)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    ram = m.ullAvailPhys // 2**20
    disk = shutil.disk_usage('C:\\').free / 1e9
    elapsed = time.monotonic() - begin
    state = 'READY' if ram >= 4608 and disk >= 15 else 'WAIT'
    if disk < 15 or elapsed >= 180 * 60:
        state = 'BLOCKED-HOST'
    line = f'{dt.datetime.now(dt.timezone.utc).isoformat()} RAM {ram} MiB C: {disk:.1f} GB elapsed {elapsed:.0f}s {state}'
    with log.open('a', encoding='utf-8') as f:
        f.write(line + '\n')
    print(line, flush=True)
    if state != 'WAIT':
        raise SystemExit(0 if state == 'READY' else 2)
    time.sleep(60)
