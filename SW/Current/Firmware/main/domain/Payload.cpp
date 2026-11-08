#include "Payload.hpp"

#include <algorithm>

namespace minilora {

uint8_t mapToByte(int32_t v, int32_t lo, int32_t hi)
{
    if (v <= lo) {
        return 0;
    }
    if (v >= hi) {
        return 255;
    }
    return static_cast<uint8_t>((v - lo) * 255 / (hi - lo));
}

uint8_t batteryToByte(uint16_t mv)
{
    return mapToByte(mv, 2500, 4300);
}

uint8_t accelToByte(int32_t raw)
{
    return mapToByte(raw, -10000, 10000);
}

uint16_t batteryFromByte(uint8_t b)
{
    return static_cast<uint16_t>(2500 + static_cast<int32_t>(b) * 1800 / 255);
}

int32_t accelFromByte(uint8_t b)
{
    return -10000 + static_cast<int32_t>(b) * 20000 / 255;
}

size_t encodePayload(const TelemetryData &t, uint8_t *out)
{
    size_t i = 0;

    out[i++] = static_cast<uint8_t>(t.tempC);
    out[i++] = t.humPct;
    out[i++] = static_cast<uint8_t>(t.presDeciHpa & 0xFF);
    out[i++] = static_cast<uint8_t>((t.presDeciHpa >> 8) & 0xFF);
    out[i++] = batteryToByte(t.batMv);
    out[i++] = static_cast<uint8_t>(t.lux & 0xFF);
    out[i++] = static_cast<uint8_t>((t.lux >> 8) & 0xFF);
    for (int axis = 0; axis < 3; ++axis) {
        out[i++] = accelToByte(t.acc[axis]);
    }

    const int32_t lat = t.latMicro;
    const int32_t lon = t.lonMicro;
    for (int shift = 0; shift < 32; shift += 8) {
        out[i++] = static_cast<uint8_t>((lat >> shift) & 0xFF);
    }
    for (int shift = 0; shift < 32; shift += 8) {
        out[i++] = static_cast<uint8_t>((lon >> shift) & 0xFF);
    }

    const int16_t alt = static_cast<int16_t>(t.altM + 1000);
    out[i++] = static_cast<uint8_t>(alt & 0xFF);
    out[i++] = static_cast<uint8_t>((alt >> 8) & 0xFF);

    out[i++] = t.sats;

    const uint32_t ts = t.epochSec;
    for (int shift = 0; shift < 32; shift += 8) {
        out[i++] = static_cast<uint8_t>((ts >> shift) & 0xFF);
    }

    out[i++] = t.awakeSec;
    out[i++] = sleepMinutes(t.sleepIdx);
    out[i++] = t.gpioStatus;

    return i;
}

bool decodePayload(const uint8_t *in, size_t len, TelemetryData &t)
{
    if (len != kPayloadSize) {
        return false;
    }
    size_t i = 0;
    t.tempC = static_cast<int8_t>(in[i++]);
    t.humPct = in[i++];
    t.presDeciHpa = static_cast<uint16_t>(in[i] | (in[i + 1] << 8));
    i += 2;
    t.batMv = batteryFromByte(in[i++]);
    t.lux = static_cast<uint16_t>(in[i] | (in[i + 1] << 8));
    i += 2;
    for (int axis = 0; axis < 3; ++axis) {
        t.acc[axis] = static_cast<int16_t>(accelFromByte(in[i++]));
    }
    int32_t lat = 0, lon = 0;
    for (int shift = 0; shift < 32; shift += 8, ++i) {
        lat |= static_cast<int32_t>(in[i]) << shift;
    }
    for (int shift = 0; shift < 32; shift += 8, ++i) {
        lon |= static_cast<int32_t>(in[i]) << shift;
    }
    t.latMicro = lat;
    t.lonMicro = lon;
    const int16_t altRaw = static_cast<int16_t>(in[i] | (in[i + 1] << 8));
    i += 2;
    t.altM = altRaw - 1000;
    t.sats = in[i++];
    uint32_t ts = 0;
    for (int shift = 0; shift < 32; shift += 8, ++i) {
        ts |= static_cast<uint32_t>(in[i]) << shift;
    }
    t.epochSec = ts;
    t.awakeSec = in[i++];
    t.sleepIdx = sleepIndexFromMinutes(in[i++]);
    t.gpioStatus = in[i];
    return true;
}

} // namespace minilora
