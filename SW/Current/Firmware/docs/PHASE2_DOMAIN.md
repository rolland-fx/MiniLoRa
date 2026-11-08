# Phase 2 — Domain Layer

**Status:** ✅ complete — 69 host checks green (`host_tests/build/minilora_tests`)
**Everything here compiles with plain g++ — no ESP-IDF headers allowed.**

## What was built

| File | Role |
|---|---|
| `main/domain/Telemetry.hpp` | `TelemetryData` — the one logical dataset; sleep table + minutes/idx conversions |
| `main/domain/Payload.{hpp,cpp}` | 28-byte v1 codec (encode/decode) |
| `main/domain/Config.{hpp,cpp}` | `DeviceConfig` (BLE ABI), versioned NVS blob, legacy parser |
| `main/domain/Diagnostics.hpp` | counters + boot-loop-guard threshold (data only; wiring in Phase 6) |
| `host_tests/` | zero-dep test harness + tests, `vectors.hpp` from Phase 0 |

## Technical decisions

### 1. `TelemetryData` is the single source of truth
Every transport (LoRaWAN, Zigbee, BLE beacon, WiFi) serializes this struct —
never raw sensor structs. Adding Zigbee later means *one more serializer*, not
a parallel data model.

### 2. Payload codec: byte-exact with v1, one documented divergence
- In-range inputs: **byte-identical to v1** `buildUplinkPayload` (verified
  against 5 golden vectors, including the reconciled `doc_example`).
- Divergence: **saturation**. v1's Arduino `map()` extrapolates and wraps for
  out-of-range accel/battery (e.g. raw 15000 → byte 62). v2 clamps to 0/255.
  Golden vector `acc_saturation` locks the v2 behavior; v1 behavior is
  preserved nowhere on purpose — wrapping was always a bug.
- `sleepMinutes(idx)` writes byte 26 (v1 semantics); `decodePayload` maps the
  minutes back to an index (`sleepIndexFromMinutes`).

### 3. Config ABI: discovered size is 158, not 150
The v1 `ConfigSensitive` packed struct is **158 bytes** (3×32 strings + IP +
port + 2×EUI + 2×16 keys + 8 flag/range bytes) — confirmed against the Android
`BLEDeviceConfigurator.buildPayload` (writes a 158-byte buffer). Consequences:
- `DeviceConfig` = 158 + 1 new `cfgZigbee` byte = **159 bytes**, packed, same
  field order (`static_assert` guards it).
- The BLE write handler (Phase 4) accepts **158 (legacy: Zigbee off) or 159**.
- NVS blob = 1 schema-version byte + payload; schema 1 = legacy 158, schema 2 =
  current 159. Migration = parse, default the new fields.
- Quirk preserved: Android writes `wifiServerIP` big-endian; after memcpy on
  the little-endian CPU the octets land LSB-first — `makeDefaults()` and
  `sendWifiPayload` (Phase 4) treat octet 0 as LSB, matching v1.

### 4. Zero-dependency test harness instead of GoogleTest
`testfw.hpp` is ~60 lines (CHECK/CHECK_EQ/RUN + failure counting). Rationale:
no fetch step, identical CI value for value-level tests, trivially portable.
If property-based/fuzzy tests arrive later, swap to GTest then.

### 5. Host tests double as an architecture rule
`host_tests/CMakeLists.txt` compiles `domain/*.cpp` with plain g++ +
`-Wall -Wextra -Werror`. If anyone leaks an ESP-IDF include into the domain,
CI fails. The `-Werror` gate also caught two real bugs during development
(missing field, wrong constant).

## Test results (local run)

```
test_encode_doc_example ........... PASS
test_encode_negative_coords_night . PASS
test_encode_no_fix ................ PASS
test_encode_edges ................. PASS
test_encode_deep_winter ........... PASS
test_saturation_clamps ............ PASS (v2 clamp ≠ v1 wrap, documented)
test_decode_roundtrip ............. PASS
test_decode_rejects_wrong_length .. PASS
test_decode_doc_example_values .... PASS
test_sleep_index_roundtrip ........ PASS
test_size_contract ................ PASS (158/159 BLE ABI)
test_serialize_deserialize_*. ..... PASS
test_legacy_v1_blob_parses ........ PASS
test_make_defaults_parses_ip ...... PASS
TOTAL: 69 checks, 0 failures
```
