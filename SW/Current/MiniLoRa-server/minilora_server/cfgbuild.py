"""Device .cfg envelope builder (ICD §4, schemas 4 and 5).

Builds the CRC-protected envelope the Android app writes over BLE GATT
('C' | schema | body | CRC16-CCITT LE) and the firmware stores in NVS
(schema byte + struct + CRC — the NVS blob wraps the SAME body). A .cfg
generated here is byte-compatible with the app's "Import .cfg" feature:
import → review → Write to device.

Schema 4 (168 B) is emitted unless P2P fields are requested — old apps
and old flows are untouched. Schema 5 (192 B, ICD §10) adds the P2P star
fields; the app needs schema-5 support to import those.

Field layout (must stay in lockstep with the app's
BLEDeviceConfigurator.buildBody() and the firmware's DeviceConfig):

  0..31    globalName (null-padded, UTF-8)
  32..63   wifiSSID
  64..95   wifiPassword
  96..99   wifiServerIP (OCTET order: 192.168.1.10 -> C0 A8 02 28)
  100..101 wifiServerPort (u16 LE)
  102..109 joinEUI (u64 LE)
  110..117 devEUI (u64 LE)
  118..133 appKey (16 B)
  134..149 nwkKey (16 B)
  150..156 cfgLoRaWAN, cfgWiFi, cfgGNSS, cfgBME280, cfgOPT30, cfgLIS3DH,
           cfgBLE (each 0x00/0x01)
  157      cfgRange (index into kSleepTableMs: 1,2,5,10,15,20,30,60,120 min)
  158      cfgZigbee
  159      cfgAssetMode
  160      cfgMotionSens (1..3)
  161      cfgAssetHeartbeatH (0 = runtime default 24)
  162      cfgBurstIntervalMin (0 = runtime default 5)
  163      cfgBurstMaxPerDay (0 = runtime default 20)
  --- schema 5 only (ICD §10) ---
  164      cfgLoRaMode (0 = LoRaWAN, 1 = P2P star)
  165      cfgP2PRole (0 = leaf, 1 = gateway)
  166      cfgP2PProfile (0 = long-range SF12, 1 = SF10, 2 = SF7)
  167      cfgP2PRxAfterTx (0/1)
  168..171 cfgP2PShortId (u32 BE, 0 = derive from DevEUI)
  172..187 cfgMeshKey (16 B; all-zero = P2P disabled)
  188      cfgP2PTxPower (0 = 14 dBm default; 14-22 honored)
"""
import struct

BODY_V4 = 164
ENVELOPE_V4 = BODY_V4 + 4  # magic + version + body + CRC16
BODY_V5 = 189
ENVELOPE_V5 = BODY_V5 + 4

#: kSleepTableMs index -> minutes (Telemetry.hpp; payload byte 26 semantics)
SLEEP_MINUTES = [1, 2, 5, 10, 15, 20, 30, 60, 120]


def crc16_ccitt(data: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF) — the ICD §4 checksum."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_envelope(*, name: str, ssid: str, password: str, ip: str,
                   port: int, join_eui: int, dev_eui: int,
                   app_key: bytes, nwk_key: bytes,
                   cfg_lorawan: bool, cfg_wifi: bool, cfg_gnss: bool,
                   cfg_bme280: bool, cfg_opt30: bool, cfg_lis3dh: bool,
                   cfg_ble: bool, cfg_zigbee: bool, cfg_range: int,
                   cfg_asset_mode: bool, cfg_motion_sens: int,
                   cfg_asset_heartbeat_h: int, cfg_burst_interval_min: int,
                   cfg_burst_max_per_day: int,
                   # P2P star (ICD §10) — schema 5 is emitted only when
                   # any of these deviates from the LoRaWAN defaults
                   cfg_lora_mode: int = 0,  # 0 = LoRaWAN, 1 = P2P star, 2 = trek
                   cfg_p2p_role: bool = False,
                   cfg_p2p_profile: int = 0, cfg_p2p_rx_after_tx: bool = True,
                   cfg_p2p_short_id: int = 0,
                   cfg_mesh_key: bytes = b"",
                   cfg_p2p_tx_power: int = 0) -> bytes:
    p2p_wanted = (cfg_lora_mode != 0 or cfg_p2p_role or cfg_p2p_profile or
                  cfg_p2p_short_id or len(cfg_mesh_key) == 16 or
                  cfg_p2p_tx_power != 0)
    body = bytearray(BODY_V5 if p2p_wanted else BODY_V4)

    def put_str(off: int, s: str) -> None:
        b = s.encode()[:32]
        body[off:off + len(b)] = b

    put_str(0, name)
    put_str(32, ssid)
    put_str(64, password)
    body[96:100] = bytes(int(x) for x in ip.split("."))  # octet order
    body[100:102] = struct.pack("<H", port)
    body[102:110] = struct.pack("<Q", join_eui)
    body[110:118] = struct.pack("<Q", dev_eui)
    body[118:134] = app_key
    body[134:150] = nwk_key
    for i, v in enumerate((cfg_lorawan, cfg_wifi, cfg_gnss, cfg_bme280,
                           cfg_opt30, cfg_lis3dh, cfg_ble)):
        body[150 + i] = 1 if v else 0
    body[157] = cfg_range
    body[158] = 1 if cfg_zigbee else 0
    body[159] = 1 if cfg_asset_mode else 0
    body[160] = cfg_motion_sens
    body[161] = cfg_asset_heartbeat_h
    body[162] = cfg_burst_interval_min
    body[163] = cfg_burst_max_per_day

    if not p2p_wanted:
        crc = crc16_ccitt(body)
        return b"C" + b"\x04" + bytes(body) + struct.pack("<H", crc)

    body[164] = 1 if cfg_lora_mode == 1 else (2 if cfg_lora_mode == 2 else 0)
    body[165] = 1 if cfg_p2p_role else 0
    body[166] = cfg_p2p_profile if 0 <= cfg_p2p_profile <= 2 else 0
    body[167] = 1 if cfg_p2p_rx_after_tx else 0
    body[168:172] = struct.pack(">I", cfg_p2p_short_id)
    if len(cfg_mesh_key) == 16:
        body[172:188] = cfg_mesh_key
    body[188] = max(0, min(22, cfg_p2p_tx_power))

    crc = crc16_ccitt(body)
    return b"C" + b"\x05" + bytes(body) + struct.pack("<H", crc)
