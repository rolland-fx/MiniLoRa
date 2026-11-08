// MiniLoRa TTN payload decoder — v2 (ICD v3).
//
// Canonical contract: SW/Current/ICD.md · golden vectors: Firmware/host_tests/vectors/*.json
// Verify against the vectors:  node check_vectors.js
//
// Handles:
//   fPort 1 — 28-byte telemetry payload v1 (FROZEN ABI, byte-identical to v1 firmware)
//   fPort 2 — diagnostics: schema 0x03 (24 B), 0x04 (28 B), 0x05 (29 B, configFlags)
//   other   — returned with a warning, data untouched
//
// Downlink helpers live in downlink.js (fPort 3 commands + v1 fPort-1 ABI).

var BAT_MIN_MV = 2500;
var BAT_SPAN_MV = 1800;
var ACC_SPAN = 20000;

function u8(b, o) { return b[o]; }
function u16(b, o) { return b[o] | (b[o + 1] << 8); }
function i32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }
function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

// Li-ion OCV → rough SoC (no load, 1S). Good enough for a dashboard;
// the diag frame's mV values are the precise source.
function batPct(mv) {
  if (mv >= 4150) return 100;
  if (mv <= 3300) return Math.max(0, Math.round(((mv - 3000) / 300) * 5));
  return Math.min(99, Math.round(((mv - 3300) / (4150 - 3300)) * 100));
}

function decodeTelemetry(bytes) {
  if (bytes.length !== 28) {
    return { error: "fport 1 must be 28 bytes, got " + bytes.length };
  }
  var batMv = Math.round(BAT_MIN_MV + bytes[4] * BAT_SPAN_MV / 255);
  var lat = i32(bytes, 10) / 1000000;
  var lon = i32(bytes, 14) / 1000000;
  var gpsValid = !(bytes[10] === 0 && bytes[11] === 0 && bytes[12] === 0 &&
                   bytes[13] === 0 && bytes[14] === 0 && bytes[15] === 0 &&
                   bytes[16] === 0 && bytes[17] === 0 && bytes[20] === 0);
  return {
    temperature: bytes[0] < 128 ? bytes[0] : bytes[0] - 256, // int8 °C
    relativeHumidity: u8(bytes, 1),
    pressure_hPa: u16(bytes, 2) / 10, // payload: 0.1 hPa units
    battery_mV: batMv,
    battery_pct: batPct(batMv),
    light_lux: u16(bytes, 5),
    acceleration: {
      // v1 map: -10000 → 0, +10000 → 255 (raw counts, ±2 g range)
      x_mg: Math.round(-10000 + bytes[7] * ACC_SPAN / 255) / 1000,
      y_mg: Math.round(-10000 + bytes[8] * ACC_SPAN / 255) / 1000,
      z_mg: Math.round(-10000 + bytes[9] * ACC_SPAN / 255) / 1000,
    },
    gps_valid: gpsValid,
    latitude: gpsValid ? lat : null,
    longitude: gpsValid ? lon : null,
    altitude_m: (bytes[18] | (bytes[19] << 8)) < 0x8000
        ? (bytes[18] | (bytes[19] << 8)) - 1000
        : ((bytes[18] | (bytes[19] << 8)) - 0x10000) - 1000,
    // ICD 3.4: tracked-satellite count from the last GGA — populated even
    // when no fix was accepted (0 = window skipped or module silent).
    // Fix validity is gps_valid (position bytes), NOT sats > 0.
    satellites: u8(bytes, 20),
    time: u32(bytes, 21) === 0 ? null : new Date(u32(bytes, 21) * 1000).toISOString(),
    awake_s: u8(bytes, 25),
    sleep_min: u8(bytes, 26),
    gpio: { led_green: (bytes[27] & 1) !== 0, button2: (bytes[27] & 2) !== 0 },
  };
}

var DIAG_FLAGS2 = { 0x01: "gps_valid", 0x02: "ota_pending_verify", 0x04: "recovery_mode" };
var POWER_FLAGS = { 0x01: "degraded", 0x02: "critical" };
var OTA_STATUS = { 0: "none", 1: "staged", 2: "no_server", 3: "download_failed", 4: "bad_image" };
// ICD 3.5 diag byte 28 (schema 0x05): the runtime transport/sensor enables —
// mirrors §3 CFG_SET flag ids (bit = 1 << (id-1)).
var CFG_FLAGS = { 0x01: "wifi", 0x02: "zigbee", 0x04: "ble_beacon",
                  0x08: "gnss", 0x10: "bme280", 0x20: "opt3001",
                  0x40: "lis3dh" };

function decodeDiagnostics(bytes) {
  var schema = bytes[0];
  var len = { 3: 24, 4: 28, 5: 29 }[schema];
  if (len === undefined) {
    return { error: "unknown diagnostics schema 0x" + schema.toString(16) };
  }
  if (bytes.length !== len) {
    return { error: "fport 2 schema " + schema + " must be " + len + " bytes, got " + bytes.length };
  }
  var flags = function (map, v) {
    var out = [];
    for (var k in map) { if (v & Number(k)) { out.push(map[k]); } }
    return out;
  };
  var d = {
    schema: schema,
    fw_version: bytes[1] + "." + bytes[2] + "." + bytes[3],
    boot_count: u16(bytes, 4),
    unexpected_resets: u16(bytes, 6),
    last_reset_reason: u8(bytes, 8),
    failures: {
      gps_timeouts: u16(bytes, 9),
      lora_join: u16(bytes, 11),
      lora_send: u16(bytes, 13),
      wifi: u16(bytes, 15),
      zigbee: u16(bytes, 17),
      sensor_read: u16(bytes, 19),
    },
    gps_consecutive_fails: u8(bytes, 21),
    power_flags: flags(POWER_FLAGS, bytes[22]),
    flags: flags(DIAG_FLAGS2, bytes[23]),
  };
  if (schema >= 4) {
    d.last_ttff_s = bytes[24] === 255 ? null : bytes[24];
    d.link_margin_db = bytes[25] === 255 ? null : bytes[25];
    d.gateways = bytes[26] === 255 ? null : bytes[26];
    d.ota_status = OTA_STATUS[bytes[27]] || bytes[27];
  }
  if (schema >= 5) {
    var cf = bytes[28];
    d.config_flags = cf;
    d.config = flags(CFG_FLAGS, cf); // e.g. ["wifi","gnss","ble_beacon"]
  }
  return d;
}

function decodeHistory(bytes) {
  // ICD: fPort 4 — store-and-forward position history.
  // [schema 0x01][count 1-2][{lat i32, lon i32, time u32, sats u8} × count]
  if (bytes.length < 2 || bytes[0] !== 0x01) {
    return { error: "not a history frame" };
  }
  var count = bytes[1];
  if (count < 1 || count > 2 || bytes.length !== 2 + count * 13) {
    return { error: "bad history frame" };
  }
  var out = [];
  for (var i = 0; i < count; i++) {
    var o = 2 + i * 13;
    var t = u32(bytes, o + 8);
    out.push({
      latitude: i32(bytes, o) / 1000000,
      longitude: i32(bytes, o + 4) / 1000000,
      time: t === 0 ? null : new Date(t * 1000).toISOString(),
      satellites: bytes[o + 12],
    });
  }
  return { schema: bytes[0], history: out };
}

function decodeUplink(input) {
  switch (input.fPort) {
    case 1:
      return { data: decodeTelemetry(input.bytes) };
    case 2:
      return { data: decodeDiagnostics(input.bytes) };
    case 4:
      return { data: decodeHistory(input.bytes) };
    default:
      return {
        data: { info: "unhandled fPort " + input.fPort, bytes_hex: input.bytes },
      };
  }
}

/** TTN validates the normalizer output against the Device Repository's
 * Normalized Payload Schema (lib/payload.json, additionalProperties:false)
 * — unknown fields make every uplink log a decode failure. Mapping per
 * that schema: position takes latitude/longitude ONLY (no altitude field
 * exists), air.pressure is hPa, battery is volts. Diagnostics (fPort 2)
 * and history (fPort 4) have no normalized representation — an empty
 * measurement validates and keeps the console clean. */
function normalizeUplink(input) {
  var d = input.data;
  if (d.schema !== undefined || d.history !== undefined) {
    return { data: {} };
  }
  var out = {};
  if (d.time) {
    out.time = d.time; // ISO 8601 from decodeTelemetry — schema wants RFC 3339
  }
  if (d.battery_mV !== undefined) {
    out.battery = d.battery_mV / 1000;
  }
  var air = {};
  if (d.temperature !== undefined) air.temperature = d.temperature;
  // schema range 900..1100 hPa: skip out-of-range instead of failing
  // validation (legacy pre-ICD-3.3 rows read 10x low)
  if (d.pressure_hPa !== undefined && d.pressure_hPa >= 900 &&
      d.pressure_hPa <= 1100) {
    air.pressure = d.pressure_hPa;
  }
  if (d.relativeHumidity !== undefined) air.relativeHumidity = d.relativeHumidity;
  if (d.light_lux !== undefined) air.lightIntensity = d.light_lux;
  if (Object.keys(air).length) out.air = air;
  if (d.gps_valid && d.latitude !== undefined) {
    out.position = { latitude: d.latitude, longitude: d.longitude };
  }
  return { data: out };
}

// ---- downlink (fPort 3 commands): [0x4D][opcode][arg], ICD section 3 ----

var DL_CMDS = {
  0x01: "LOCATE_NOW",
  0x02: "SEND_DIAG",
  0x03: "RESET_DIAG",
  0x04: "BLE_WINDOW",
  0x05: "OTA_UPDATE",
  0x06: "CFG_SET",
  0x07: "SET_RANGE",
};

// ICD 3.5 §3 CFG_SET flag ids (arg bits0-3) and ICD 3.6 §3 SET_RANGE table.
var DL_FLAG_NAMES = { 1: "wifi", 2: "zigbee", 3: "ble", 4: "gnss",
                      5: "bme280", 6: "opt3001", 7: "lis3dh" };
var DL_SLEEP_MIN = [1, 2, 5, 10, 15, 20, 30, 60, 120];

/** Human-readable detail for the two arg-bearing commands. */
function decodeCmdArg(opcode, arg) {
  if (opcode === 0x06) { // CFG_SET: bit7 = value, bits0-3 = flag id
    var flag = DL_FLAG_NAMES[arg & 0x0F];
    return {
      flag: flag || ("unknown id " + (arg & 0x0F)),
      value: (arg & 0x80) !== 0,
    };
  }
  if (opcode === 0x07) { // SET_RANGE: sleep-table index
    var idx = arg;
    return { index: idx,
             sleepMinutes: idx < DL_SLEEP_MIN.length ? DL_SLEEP_MIN[idx] : null };
  }
  return {};
}

/** TTN calls this on "Receive downlink data message"; without it the
 * console logs a decode failure for every queued command (harmless, but
 * noisy and hides the payload). Frames here are what the app's Remote tab
 * sends: 4D 01 00 ... 4D 05 00. */
function decodeDownlink(input) {
  var b = input.bytes;
  if (input.fPort !== 3) {
    return { errors: ["downlink commands use fPort 3, got " + input.fPort] };
  }
  if (b.length !== 3) {
    return { errors: ["expected 3 bytes [0x4D][opcode][arg], got " + b.length] };
  }
  if (b[0] !== 0x4d) {
    return { errors: ["bad magic 0x" + b[0].toString(16) + ", expected 0x4D"] };
  }
  var name = DL_CMDS[b[1]];
  var out = {
    data: Object.assign(
      { command: name || "UNKNOWN", opcode: b[1], arg: b[2] },
      name ? decodeCmdArg(b[1], b[2]) : {}),
  };
  if (!name) {
    out.warnings = ["opcode 0x" + b[1].toString(16) +
      " is reserved - device ignores it (forward-compatible)"];
  }
  return out;
}

/** Inverse, for TTN's downlink payload-formatter test bench / API callers:
 * input = { fPort: 3, data: { command: "SEND_DIAG" } } or
 * { fPort: 3, data: { opcode: 2, arg: 0 } }. */
function encodeDownlink(input) {
  if (input.fPort !== 3) {
    return { errors: ["downlink commands use fPort 3, got " + input.fPort] };
  }
  var op = input.data.command !== undefined ? null : input.data.opcode;
  for (var k in DL_CMDS) {
    if (DL_CMDS[k] === input.data.command) { op = Number(k); break; }
  }
  if (op === null || op === undefined) {
    return { errors: ["unknown command " + JSON.stringify(input.data.command) +
      " - expected one of " + Object.keys(DL_CMDS).map(function (k) {
        return DL_CMDS[k]; }).join(", ")] };
  }
  var arg = input.data.arg || 0;
  if (arg < 0 || arg > 255) {
    return { errors: ["arg must fit one byte, got " + arg] };
  }
  return { fPort: 3, bytes: [0x4D, op, arg] };
}
