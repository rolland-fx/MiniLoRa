// LoRaWAN transport — RadioLib SX1262 + LoRaWANNode, OTAA EU868.
// Session/nonces persist in RTC RAM across deep sleep (v1 pattern) so an
// already-joined device does NOT rejoin on every cycle.

#pragma once

#include <cstddef>
#include <cstdint>

#include "../../domain/Payload.hpp"

namespace minilora {

// ---- Remote commands (downlink, fport 3) ---------------------------------
// Frame: [0x4D]['opcode'][arg] — the ONLY downlink ABI since FW 2.4.0
// (the v1 fPort-1 sleepIdx/gpio6 bytes are gone; sleep now changes via the
// config envelope OR the SET_RANGE command below). LoRaWAN provides
// integrity; no secrets are ever accepted over downlink — key material
// only via the BLE config service.
inline constexpr uint8_t kDlCmdNone = 0x00;
inline constexpr uint8_t kDlCmdLocateNow = 0x01; // arg unused: next cycle in 60 s, forced GPS
inline constexpr uint8_t kDlCmdSendDiag = 0x02;  // arg unused: diagnostics uplink next cycle
inline constexpr uint8_t kDlCmdResetDiag = 0x03; // arg unused: clear failure counters
inline constexpr uint8_t kDlCmdBleWindow = 0x04; // arg unused: BLE config window next cycle
inline constexpr uint8_t kDlCmdOtaUpdate = 0x05; // arg unused: pull FW from the server next cycle
inline constexpr uint8_t kDlCmdConfigSet = 0x06; // arg: bit7=value, bits0-3=flag id (ICD §3 CFG_SET)
inline constexpr uint8_t kDlCmdSetRange = 0x07;  // arg: cfgRange index into kSleepTableMs (ICD §3 SET_RANGE)
inline constexpr uint8_t kDlMagic = 0x4D;        // 'M'

class LoRaWanTransport {
public:
    struct Downlink {
        uint8_t cmd = kDlCmdNone;
        uint8_t cmdArg = 0;
        /// UTC seconds from DeviceTimeAns (0 = not requested / no answer).
        uint32_t epochSec = 0;
    };

    struct Callbacks {
        // Called when a downlink changes the sleep index; store via NVS.
        void (*onSleepIdx)(uint8_t idx) = nullptr;
        // Called when a downlink changes the GPIO6 override byte.
        void (*onGpio6Control)(uint8_t ctrl) = nullptr;
    };

    bool begin(uint64_t joinEUI, uint64_t devEUI,
               const uint8_t *nwkKey16, const uint8_t *appKey16);

    /// Blocking join + uplink + downlink wait. Returns true if the frame was
    /// sent (join state restored or fresh). `fport` selects the payload type.
    /// `wantTime` queues a DeviceTimeReq MAC command; the answer (if the
    /// network sent one) is returned in `dl.epochSec`.
    bool sendReceive(const uint8_t *payload, size_t len, Downlink &dl,
                     uint8_t fport = kFportTelemetry, bool wantTime = false);

    void shutdown(); // SPI pins to pulldown, radio powered down

    /// True if a LoRaWAN session exists in RTC RAM (joined or restored
    /// since the last power cycle). Used by the BLE status frame.
    static bool hasSession();

    /// Queue a LinkCheckReq MAC command on the next uplink (diag v4).
    static void requestLinkCheck();

    /// Read the LinkCheck answer from the LAST uplink's RX window.
    /// Returns false if no answer (then the out-params get 255).
    static bool readLinkCheck(uint8_t *marginDb, uint8_t *gwCnt);

private:
    Callbacks cb_;
};

} // namespace minilora
