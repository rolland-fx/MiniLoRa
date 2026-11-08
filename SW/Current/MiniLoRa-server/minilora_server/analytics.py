"""Readings-level analytics: summary stats, battery slope, GPS distance.

Pure functions over db.readings_all() rows (ASC by seen_at) — no I/O, so
they're unit-testable and shared by the analytics page and the JSON API.
"""
import math

# Sentinel/implausible filters (payload semantics, see ICD §1):
_HUM_SENTINEL = 255            # 0xFF = "no humidity"
_PRESSURE_RANGE = (300, 1200)  # hPa; old-firmware garbage read ~1


def haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6_371_000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = (math.sin(dphi / 2) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2)
    return 2 * r * math.asin(math.sqrt(a))


def summarize(rows: list) -> dict:
    """Aggregate stats over one device's readings (ASC by seen_at)."""
    out = {"n": len(rows)}
    if not rows:
        return out

    first, last = rows[0], rows[-1]
    out["first_seen"] = first["seen_at"]
    out["last_seen"] = last["seen_at"]
    out["span_days"] = max((last["seen_at"] - first["seen_at"]) / 86400.0, 0.0)

    def vals(key, lo=None, hi=None):
        return [r[key] for r in rows
                if r[key] is not None
                and (lo is None or r[key] >= lo)
                and (hi is None or r[key] <= hi)]

    temps = vals("temp_c")
    if temps:
        out.update(t_avg=round(sum(temps) / len(temps), 1),
                   t_min=min(temps), t_max=max(temps))
    hums = vals("hum_pct", hi=_HUM_SENTINEL - 1)
    if hums:
        out.update(h_avg=round(sum(hums) / len(hums), 0),
                   h_min=min(hums), h_max=max(hums))
    press = vals("pressure_hpa", *_PRESSURE_RANGE)
    if press:
        out.update(p_avg=round(sum(press) / len(press), 1),
                   p_min=min(press), p_max=max(press))
    lux = vals("lux")
    if lux:
        out["lux_max"] = max(lux)

    bats = [r["bat_mv"] for r in rows if r["bat_mv"]]
    if bats:
        out.update(b_min=min(bats), b_max=max(bats),
                   b_first=bats[0], b_last=bats[-1])
        # mV/day slope: positive = discharging, negative = net charging
        if out["span_days"] >= 0.02:
            out["bat_slope_mv_day"] = round(
                (bats[-1] - bats[0]) / out["span_days"], 1)

    out["awake_total_s"] = sum(r["awake_s"] or 0 for r in rows)
    out["gps_n"] = sum(1 for r in rows if r["gps_valid"])
    out["sources"] = sorted({r["source"] for r in rows})

    # GPS track: haversine sum over consecutive valid fixes. (0,0) is the
    # firmware's no-fix sentinel, but any single nonzero coordinate is a
    # real position — don't drop fixes on the equator/meridian.
    dist = 0.0
    prev = None
    for r in rows:
        if r["gps_valid"] and (r["latitude"] or r["longitude"]):
            cur = (r["latitude"] or 0.0, r["longitude"] or 0.0)
            if prev is not None and cur != prev:
                dist += haversine_m(prev[0], prev[1], cur[0], cur[1])
            prev = cur
    out["distance_m"] = round(dist)

    return out
