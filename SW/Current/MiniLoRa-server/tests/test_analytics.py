"""Analytics module tests (summary stats, battery slope, GPS distance)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from minilora_server.analytics import haversine_m, summarize  # noqa: E402

T0 = 1_700_000_000


def _row(seen_at, *, temp=20.0, hum=50, press=1013.0, bat=4000, lux=0,
         lat=None, lon=None, valid=False, awake=5, source="wifi"):
    return {"seen_at": seen_at, "gps_time": None, "source": source,
            "temp_c": temp, "hum_pct": hum, "pressure_hpa": press,
            "bat_mv": bat, "lux": lux, "acc_x_mg": 0, "acc_y_mg": 0,
            "acc_z_mg": 0, "latitude": lat, "longitude": lon,
            "altitude_m": 0.0, "sats": 0, "gps_valid": int(valid),
            "awake_s": awake, "sleep_min": 5, "gpio": 0, "raw_hex": ""}


def test_empty():
    s = summarize([])
    assert s == {"n": 0}


def test_basic_summary():
    rows = [
        _row(T0, temp=10.0, bat=4200),
        _row(T0 + 86400, temp=30.0, bat=4000),
    ]
    s = summarize(rows)
    assert s["n"] == 2
    assert s["span_days"] == 1.0
    assert s["t_min"] == 10.0 and s["t_max"] == 30.0 and s["t_avg"] == 20.0
    assert s["b_first"] == 4200 and s["b_last"] == 4000
    # discharged 200 mV in 1 day
    assert s["bat_slope_mv_day"] == -200.0
    assert s["distance_m"] == 0


def test_sentinels_excluded():
    rows = [
        _row(T0, hum=255, press=1.0),          # no-humidity + garbage pressure
        _row(T0 + 60, hum=50, press=1013.0),
    ]
    s = summarize(rows)
    assert s["h_avg"] == 50 and s["h_max"] == 50      # 255 sentinel ignored
    assert s["p_avg"] == 1013.0 and s["p_min"] == 1013.0


def test_charging_slope_is_negative():
    rows = [_row(T0, bat=4000), _row(T0 + 2 * 86400, bat=4200)]
    assert summarize(rows)["bat_slope_mv_day"] == 100.0


def test_distance_two_points():
    # ~111.2 m per 0.001° latitude; first row is exactly the (0,0)
    # no-fix sentinel and must be ignored
    rows = [
        _row(T0, lat=0.000, lon=0.000, valid=True),
        _row(T0 + 60, lat=1.000, lon=1.000, valid=True),
        _row(T0 + 120, lat=1.001, lon=1.000, valid=True),
    ]
    s = summarize(rows)
    assert 110 < s["distance_m"] < 112


def test_distance_ignores_invalid_and_repeats():
    rows = [
        _row(T0, lat=1.0, lon=1.0, valid=True),
        _row(T0 + 60, lat=1.0, lon=1.0, valid=True),   # same spot
        _row(T0 + 120, lat=2.0, lon=2.0, valid=False),  # invalid: skipped
    ]
    assert summarize(rows)["distance_m"] == 0


def test_haversine_known_distance():
    # Bremen → Helsinki ≈ 1258 km great-circle; loose bound catches
    # formula/sign errors
    d = haversine_m(53.0758, 8.8072, 60.1699, 24.9384)
    assert 1_200_000 < d < 1_300_000
