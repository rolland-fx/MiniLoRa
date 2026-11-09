"""Derived health insights — pure functions over narrow DB projections.

Three answers the analytics page now shows:

1. delivery_ratio — LoRaWAN link health from TTN f_cnt gaps (Next-Improv
   item 1): frames the gateway confirmed are counter-adjacent; every gap
   is a frame the device sent but nobody heard (or never sent — LoRaWAN
   can't tell, which is exactly why this is "link health", not
   "payload loss").
2. battery_projection — days-to-critical from the stored bat_mv trend
   (item 2a): least-squares slope over the window, projected to the
   ICD's critical threshold (3300 mV; degraded is 3500).
3. missed_cycles — expected cadence from each frame's own sleep_min +
   awake_s, gaps above 1.5x that count as missed uplinks (item 2b).

Everything here is deterministic and unit-tested (tests/test_insights.py);
the endpoint only feeds rows in and ships the dicts out.
"""

# ICD §7 powerFlags thresholds — bit0 degraded (<3500 mV), bit1 critical
# (<3300 mV). The projection answers "when does bit1 flip on?".
CRITICAL_MV = 3300
DEGRADED_MV = 3500


def delivery_ratio(fcnts: list) -> dict:
    """Lost-frame count + delivery ratio from ordered TTN f_cnts.

    A counter reset (device rejoin) starts a new span — nothing can be
    inferred across it, so it is not counted as a loss burst. Duplicate
    counters (retransmission) neither add nor lose.
    """
    out = {"sent": len(fcnts), "lost": 0, "expected": len(fcnts),
           "ratio": None, "first": None, "last": None, "resets": 0}
    if not fcnts:
        return out
    out["first"], out["last"] = fcnts[0], fcnts[-1]
    prev = None
    for c in fcnts:
        if prev is not None:
            d = c - prev
            if d > 1:
                out["lost"] += d - 1
            elif d <= 0:
                # 0 = duplicate; <0 = counter reset (rejoin) — new span.
                if d < 0:
                    out["resets"] += 1
        prev = c
    out["expected"] = out["sent"] + out["lost"]
    if out["expected"] > 1:
        out["ratio"] = round(out["sent"] / out["expected"], 4)
    return out


def battery_projection(rows: list, cutoff_mv: int = CRITICAL_MV,
                       warn_mv: int = DEGRADED_MV) -> dict:
    """Days until the battery crosses cutoff_mv, from (seen_at, bat_mv).

    Needs >= 4 points spanning >= 2 days for a slope worth trusting —
    below that the projection is omitted (slope None) and only the
    instantaneous flags are returned. A non-negative slope (charging,
    solar, or fresh cells) means "no projection": days_to_cutoff None.
    """
    out = {"n": len(rows), "bat_mv_now": None, "last_seen": None,
           "slope_mv_day": None, "r2": None, "days_to_cutoff": None,
           "critical_now": False, "degraded_now": False, "warn_soon": False}
    if not rows:
        return out
    last_ts, last_mv = rows[-1]
    out["last_seen"] = last_ts
    out["bat_mv_now"] = last_mv
    out["critical_now"] = last_mv < cutoff_mv
    out["degraded_now"] = cutoff_mv <= last_mv < warn_mv
    if len(rows) < 4:
        return out
    span_s = last_ts - rows[0][0]
    if span_s < 2 * 86400:
        return out
    # Least squares over days; ts/86400 keeps the numbers tame.
    xs = [ts / 86400.0 for ts, _ in rows]
    ys = [mv for _, mv in rows]
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    slope = sxy / sxx
    intercept = my - slope * mx
    ss_res = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(xs, ys))
    ss_tot = sum((y - my) ** 2 for y in ys)
    out["slope_mv_day"] = round(slope, 1)
    out["r2"] = round(1 - ss_res / ss_tot, 3) if ss_tot > 0 else None
    if slope < 0:
        out["days_to_cutoff"] = round((cutoff_mv - last_mv) / slope, 1)
        out["warn_soon"] = 0 <= out["days_to_cutoff"] < 7
    return out


def missed_cycles(rows: list, now_s: int = None,
                  stale_after_s: int = 86400) -> dict:
    """Missed uplink cycles from (seen_at, sleep_min, awake_s).

    Each frame declares its own next-cycle length (sleep_min + awake_s);
    a gap of k*expected + tolerance between consecutive received frames
    means k-1 cycles never made it. The open gap (now - last frame) is
    scored the same way, and `stale` flags 24 h of silence outright.
    """
    now_s = now_s if now_s is not None else 0
    out = {"missed": 0, "missed_since_last": 0, "stale": False,
           "last_seen": None, "expected_interval_s": None}
    if not rows:
        return out
    last_ts, last_sleep, last_awake = rows[-1]
    out["last_seen"] = last_ts
    out["expected_interval_s"] = max(60, (last_sleep or 0) * 60 +
                                     (last_awake or 0))
    for i in range(1, len(rows)):
        ts, sleep_min, awake_s = rows[i - 1]
        expected = max(60, (sleep_min or 0) * 60 + (awake_s or 0))
        gap = rows[i][0] - ts
        if gap > 1.5 * expected:
            out["missed"] += max(0, round(gap / expected) - 1)
    open_gap = now_s - last_ts
    if open_gap > 1.5 * out["expected_interval_s"]:
        out["missed_since_last"] = max(
            0, round(open_gap / out["expected_interval_s"]) - 1)
    out["stale"] = open_gap > stale_after_s
    return out
