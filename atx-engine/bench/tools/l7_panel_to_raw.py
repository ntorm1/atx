"""L7 risk-model real-data harness: APNL panel (context.bin) -> raw little-endian arrays.

Reads an atx::impl APNL v1 panel (see atx-impl/src/serialize_panel.cpp: header, field
names, F date-major f64 columns, D*I universe-mask bytes, fnv1a64 trailer) and writes
the inputs the `BM_L7RealScorecard` bench consumes (env ATX_L7_REAL_DIR):

  meta.txt     "T N" (dates ascending, instruments)
  close.f64    T*N  adjusted close, NaN outside the universe mask
  volume.f64   T*N  volume, NaN outside the universe mask
  cap_tn.f64   T*N  market cap as of each row (NaN outside the universe mask or <= 0)
  sector_tn.f64 T*N sector id as of each row (NaN outside the mask / missing)
  dates.i64    T    day number (unix days) of each row

Point-in-time: every per-name field is written PER ROW. There are no "last value"
summaries, which would carry information from later dates into earlier rows. The
bench picks each row's cap and a PIT-safe static sector from these matrices.
Read-only on the input. The digest trailer is verified only with L7_VERIFY_DIGEST=1.
Usage: python l7_panel_to_raw.py <context.bin> <out_dir>
"""
import os
import struct
import sys

import numpy as np

MASK64 = (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    # Pure-python loop (slow on ~60MB); enabled only with L7_VERIFY_DIGEST=1.
    for b in data:
        h ^= b
        h = (h * prime) & MASK64
    return h


def main() -> None:
    src, out = sys.argv[1], sys.argv[2]
    verify = os.environ.get("L7_VERIFY_DIGEST", "0") == "1"
    buf = open(src, "rb").read()
    payload, trailer = buf[:-8], buf[-8:]
    if verify and fnv1a64(payload) != struct.unpack("<Q", trailer)[0]:
        raise SystemExit("digest mismatch")
    magic, version, d, n, f = struct.unpack_from("<IIQQQ", payload, 0)
    assert magic == 0x4C4E5041 and version == 1, (hex(magic), version)
    pos = 32
    names = []
    for _ in range(f):
        (ln,) = struct.unpack_from("<I", payload, pos)
        pos += 4
        names.append(payload[pos : pos + ln].decode())
        pos += ln
    cells = d * n
    cols = {}
    for name in names:
        cols[name] = np.frombuffer(payload, dtype="<f8", count=cells, offset=pos).reshape(d, n)
        pos += cells * 8
    mask = np.frombuffer(payload, dtype=np.uint8, count=cells, offset=pos).reshape(d, n) != 0
    pos += cells
    assert pos == len(payload), (pos, len(payload))

    import json

    man = json.load(open(src + ".manifest.json"))
    date_ns = man["axes"].get("session_keys")
    os.makedirs(out, exist_ok=True)
    close = np.where(mask, cols["close"], np.nan)
    vol = np.where(mask, cols["volume"], np.nan) if "volume" in cols else np.full((d, n), np.nan)
    capm = np.where(mask, cols["market_cap"], np.nan)
    capm = np.where(np.isfinite(capm) & (capm > 0), capm, np.nan)
    sec = np.where(mask & np.isfinite(cols["sector"]), cols["sector"], np.nan)
    if date_ns is not None and len(date_ns) == d:
        days = np.array([int(x) // 86_400_000_000_000 for x in date_ns], dtype="<i8")
    else:
        days = np.arange(d, dtype="<i8")
    open(os.path.join(out, "meta.txt"), "w").write(f"{d} {n}\n")
    close.astype("<f8").tofile(os.path.join(out, "close.f64"))
    vol.astype("<f8").tofile(os.path.join(out, "volume.f64"))
    capm.astype("<f8").tofile(os.path.join(out, "cap_tn.f64"))
    sec.astype("<f8").tofile(os.path.join(out, "sector_tn.f64"))
    days.tofile(os.path.join(out, "dates.i64"))
    print(f"T={d} N={n} fields={names} in_universe={mask.mean():.3f} "
          f"cap_ok={np.isfinite(capm[mask]).mean():.3f} "
          f"sectors={len(np.unique(sec[np.isfinite(sec)]))}")


if __name__ == "__main__":
    main()
