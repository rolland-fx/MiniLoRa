// MiniLoRa downlink helpers — build frames the firmware accepts (ICD.md §3).
//
//   fPort 3, [0x4D][opcode][arg] — remote commands (the only downlink ABI;
//   the v1 fPort-1 sleepIdx/gpio6 ABI was removed in FW 2.4.0 — sleep is
//   now changed via the BLE config envelope).
//
// Usage in the TTN console: Messaging → Downlink, set FPort and paste the
// byte array (e.g. `4D 01 00`). Programmatically, call these in
// `encodeDownlink(input)` on the device profile.

var CMD = { LOCATE_NOW: 0x01, SEND_DIAG: 0x02, RESET_DIAG: 0x03, BLE_WINDOW: 0x04, OTA_UPDATE: 0x05, CONFIG_SET: 0x06, SET_RANGE: 0x07 };
var MAGIC = 0x4d;
var FPORT = 3;

// The device's sleep table (Firmware kSleepTableMs) — arg = index into it,
// the same scale the config envelope and the app's config tab use.
var SLEEP_TABLE_MIN = [1, 2, 5, 10, 15, 20, 30, 60, 120];

// ICD 3.5 §3 CFG_SET flag ids (arg = (value << 7) | id). LoRaWAN itself is
// deliberately not addressable — it is the lifeline.
var CFG_FLAG = {
  WIFI: 1, ZIGBEE: 2, BLE: 3, GNSS: 4,
  BME280: 5, OPT3001: 6, LIS3DH: 7,
};

function command(opcode, arg) {
  arg = arg === undefined ? 0 : arg;
  return { fPort: FPORT, bytes: [MAGIC, opcode, arg & 0xff] };
}

// CFG_SET: arg bit7 = value, bits0-3 = flag id (ICD 3.5 §3).
function configSet(flagId, on) {
  if (flagId < 1 || flagId > 7) {
    throw new Error("CFG_SET flag id must be 1..7");
  }
  return command(CMD.CONFIG_SET, ((on ? 1 : 0) << 7) | flagId);
}

// SET_RANGE: arg = the sleep-table index (0..8). The confirm rides the
// next telemetry frame's sleep-minutes byte (ICD 3.6 §3).
function setRange(index) {
  if (index < 0 || index >= SLEEP_TABLE_MIN.length) {
    throw new Error("setRange index must be 0.." + (SLEEP_TABLE_MIN.length - 1));
  }
  return command(CMD.SET_RANGE, index);
}

var downlink = {
  locateNow: function () { return command(CMD.LOCATE_NOW); },
  sendDiag: function () { return command(CMD.SEND_DIAG); },
  resetDiag: function () { return command(CMD.RESET_DIAG); },
  bleWindow: function () { return command(CMD.BLE_WINDOW); },
  otaUpdate: function () { return command(CMD.OTA_UPDATE); },
  configSet: configSet,
  setRange: setRange,
  sleepMinutes: function (min) {
    var idx = SLEEP_TABLE_MIN.indexOf(min);
    if (idx < 0) throw new Error("sleep minutes must be one of " + SLEEP_TABLE_MIN);
    return setRange(idx);
  },
  // Per-flag convenience (state confirm rides the next diag's configFlags):
  wifiOn:     function () { return configSet(CFG_FLAG.WIFI, true); },
  wifiOff:    function () { return configSet(CFG_FLAG.WIFI, false); },
  zigbeeOn:   function () { return configSet(CFG_FLAG.ZIGBEE, true); },
  zigbeeOff:  function () { return configSet(CFG_FLAG.ZIGBEE, false); },
  bleOn:      function () { return configSet(CFG_FLAG.BLE, true); },
  bleOff:     function () { return configSet(CFG_FLAG.BLE, false); },
  gnssOn:     function () { return configSet(CFG_FLAG.GNSS, true); },
  gnssOff:    function () { return configSet(CFG_FLAG.GNSS, false); },
  bme280On:   function () { return configSet(CFG_FLAG.BME280, true); },
  bme280Off:  function () { return configSet(CFG_FLAG.BME280, false); },
  opt3001On:  function () { return configSet(CFG_FLAG.OPT3001, true); },
  opt3001Off: function () { return configSet(CFG_FLAG.OPT3001, false); },
  lis3dhOn:   function () { return configSet(CFG_FLAG.LIS3DH, true); },
  lis3dhOff:  function () { return configSet(CFG_FLAG.LIS3DH, false); },
};

// TTN device-profile hook (optional): enables `Downlink switch` payloads like
// { "cmd": "locateNow" } or { "cmd": "configSet", "flag": "wifi", "on": false }
// in the console. Unknown commands are rejected.
var CFG_FLAG_BY_NAME = {
  wifi: CFG_FLAG.WIFI, zigbee: CFG_FLAG.ZIGBEE, ble: CFG_FLAG.BLE,
  gnss: CFG_FLAG.GNSS, bme280: CFG_FLAG.BME280, opt3001: CFG_FLAG.OPT3001,
  lis3dh: CFG_FLAG.LIS3DH,
};

function encodeDownlink(input) {
  var fns = {
    locateNow: downlink.locateNow, sendDiag: downlink.sendDiag,
    resetDiag: downlink.resetDiag, bleWindow: downlink.bleWindow,
    otaUpdate: downlink.otaUpdate, configSet: downlink.configSet,
    wifiOn: downlink.wifiOn, wifiOff: downlink.wifiOff,
    zigbeeOn: downlink.zigbeeOn, zigbeeOff: downlink.zigbeeOff,
    bleOn: downlink.bleOn, bleOff: downlink.bleOff,
    gnssOn: downlink.gnssOn, gnssOff: downlink.gnssOff,
    bme280On: downlink.bme280On, bme280Off: downlink.bme280Off,
    opt3001On: downlink.opt3001On, opt3001Off: downlink.opt3001Off,
    lis3dhOn: downlink.lis3dhOn, lis3dhOff: downlink.lis3dhOff,
  };
  if (input.data && input.data.cmd && fns[input.data.cmd]) {
    return fns[input.data.cmd]();
  }
  if (input.data && input.data.cmd === "configSet" && input.data.flag) {
    var id = typeof input.data.flag === "number"
      ? input.data.flag : CFG_FLAG_BY_NAME[input.data.flag];
    return configSet(id, input.data.on !== false);
  }
  if (input.data && (input.data.cmd === "setRange" ||
                     input.data.cmd === "sleep")) {
    var idx = input.data.cmd === "setRange"
      ? input.data.index : SLEEP_TABLE_MIN.indexOf(input.data.minutes);
    return setRange(idx);
  }
  throw new Error("unknown command (expected locateNow/sendDiag/resetDiag/" +
                  "bleWindow/otaUpdate/configSet/setRange)");
}
