# Zigbee & Home Assistant

The tracker can join a Zigbee network as a **sleepy end device** and expose
its dataset as **native Home Assistant clusters** — no custom converter, no
custom Quirk. It works with **Zigbee2MQTT** and **ZHA** out of the box.

Stack: `espressif/esp-zigbee-lib` (ZBOSS) on the ESP32-C6 802.15.4 radio,
declared in `Firmware/main/idf_component.yml`.

## What the device exposes

**End device, endpoint 1**, reporting to the coordinator (`0x0000`, ep 1):

| Cluster | Reported value | Notes |
|---|---|---|
| Basic | — | power source = battery |
| Power Configuration | `batteryVoltage` (100 mV units), `batteryPercentageRemaining` (0–200, 0.5 % steps) | from the same `batMv` the LoRaWAN frame uses |
| Temperature Measurement | °C × 100 | −40…+85 °C range |
| Relative Humidity | % × 100 | 0–100 % |
| Pressure Measurement | hPa | see bounds note below |
| Illuminance Measurement | lux | 0–65535 |
| Occupancy Sensing (PIR) | 0/1 | **motion** — LIS3DH delta vs the previous cycle above a threshold; the anti-theft channel |

Each cycle the device writes the attributes and sends `Report` commands; a
cycle later it is back in deep sleep.

## Enabling it

1. In the app's **Config** tab check **Zigbee** (or send it via any config
   write — `cfgZigbee` rides the 159-byte body) and write.
2. Put your coordinator into pairing mode (Z2M: `Permit join`).
3. The tracker joins on its next wake cycle. Nothing else to do — the app
   will show it as a sensor entity set in Home Assistant:

   ```
   sensor.minilora_<id>_temperature      sensor.minilora_<id>_humidity
   sensor.minilora_<id>_pressure         sensor.minilora_<id>_illuminance
   sensor.minilora_<id>_battery          sensor.minilora_<id>_battery_voltage  (Z2M)
   binary_sensor.minilora_<id>_occupancy
   ```

## How it survives deep sleep

- The ZBOSS **NVRAM dataset** (network context + ZCL reporting config) lives
  in the dedicated `zb_storage` flash partition (`partitions.csv`). Before
  every deep sleep `ZigbeeTransport::persist()` writes it; on wake the stack
  restores and re-joins instantly.
- The **child aging timeout** is set to 8192 minutes so the coordinator does
  not expire the device during 15 min–2 h sleeps.
- Zigbee runs **last** in the cycle (its task holds the 802.15.4 radio) and
  is skipped entirely in low-battery states — LoRaWAN is the lifeline.

## Honest limitations (read before relying on it)

1. **Latency = sleep interval.** A sensor update arrives only when the
   device wakes (15 min…2 h). For live automation use the motion/occupancy
   flag with realistic expectations, or the LoRaWAN path.
2. **Reports, not polling.** The device pushes `Report` commands once per
   wake and sleeps; it does not answer read requests while sleeping. Polling
   entities in HA will show stale values between wakes.
3. **Pressure units** — the ZCL `MeasuredValue` unit is 0.1 kPa (= 1 hPa)
   per spec §4.5.2.2.1.1, and the device sends raw hPa with cluster bounds
   300–1200 — correct.
4. **Zigbee and LoRaWAN coexist** on the C6 but not simultaneously — the
   cycle sequences them; enabling Zigbee adds a few seconds of awake time
   per cycle (energy: see `Firmware/docs/POWER_OPTIMIZATION.md`).

## Commissioning tips

- **Z2M**: the device appears as a custom "esp-zigbee" end device; all
  clusters are standard so entities map automatically. If pairing fails,
  allow joining *while the tracker is awake* (its awake window is seconds —
  press its button once to trigger an immediate cycle, then permit join).
- **ZHA**: same endpoint/cluster layout; add it and HA creates the entity
  set above.
- To **leave/reset the network**, erase the `zb_storage` partition:
  `esptool.py write_flash 0x3C0000 <4 MB of 0xFF>` (or full re-flash of the
  partition) — the device will factory-join on the next cycle.
