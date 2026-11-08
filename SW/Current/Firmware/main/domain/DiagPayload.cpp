#include "DiagPayload.hpp"

namespace minilora {

size_t encodeDiagnostics(const Diagnostics &d, uint8_t *out)
{
    size_t i = 0;
    out[i++] = kDiagSchema; // decoders switch on this byte, never the fw version
    out[i++] = d.fwMajor;
    out[i++] = d.fwMinor;
    out[i++] = d.fwPatch;
    const auto put16 = [&](uint16_t v) {
        out[i++] = static_cast<uint8_t>(v & 0xFF);
        out[i++] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    put16(d.bootCount);
    put16(d.unexpectedResets);
    out[i++] = d.lastResetReason;
    put16(d.gpsTimeouts);
    put16(d.loraJoinFailures);
    put16(d.loraSendFailures);
    put16(d.wifiFailures);
    put16(d.zigbeeFailures);
    put16(d.sensorReadFailures);
    out[i++] = d.gpsConsecFails;
    out[i++] = d.powerFlags;
    out[i++] = static_cast<uint8_t>((d.gpsValid ? kDiagFlagGpsValid : 0) |
                                    d.imageFlags);
    out[i++] = d.lastTtff;
    out[i++] = d.linkMargin;
    out[i++] = d.gwCnt;
    out[i++] = d.otaStatus;
    // Schema 0x05: remote-config state (ICD §2) — the transport/sensor
    // enables as set in the runtime config.
    out[i++] = d.configFlags;
    return i;
}

} // namespace minilora
