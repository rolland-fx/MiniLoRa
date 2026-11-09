#include "P2pFrame.hpp"

#include "Config.hpp" // crc16Ccitt — the ICD-wide checksum

#include <esp_log.h>
#include <nvs.h>

#include <mbedtls/aes.h>
#include <cstring>

namespace minilora {
namespace p2p {

namespace {

// AES-128-CTR with the ICD §10.3 IV: origin(BE) || seq(BE) || boot(BE) ||
// 6×0x00. The boot term makes the keystream unique per power cycle.
void ctrCrypt(const uint8_t key[kKeyLen], uint32_t originId, uint32_t seq,
              uint16_t boot, const uint8_t *in, uint8_t *out, size_t len)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);

    uint8_t iv[16] = {0};
    for (int i = 0; i < 4; ++i) {
        iv[i] = static_cast<uint8_t>(originId >> (24 - 8 * i));
        iv[4 + i] = static_cast<uint8_t>(seq >> (24 - 8 * i));
    }
    iv[8] = static_cast<uint8_t>(boot >> 8);
    iv[9] = static_cast<uint8_t>(boot & 0xFF);
    uint8_t streamBlock[16] = {0};
    size_t offset = 0;
    if (len > 0) {
        mbedtls_aes_crypt_ctr(&aes, len, &offset, iv, streamBlock, in, out);
    }
    mbedtls_aes_free(&aes);
}

// Minimal AES-128-CMAC (SP 800-38A) over mbedtls ECB — ~20 lines instead
// of pulling in the cipher-CMAC config surface. Returns the first
// kMicLen bytes in `mic`.
void cmac(const uint8_t key[kKeyLen], const uint8_t *data, size_t len,
          uint8_t *mic)
{
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);

    // Subkeys: L = E(K, 0^16); K1 = L << 1 (+0x87 in the last byte on
    // carry); K2 = K1 << 1 (same rule). Left shift is big-endian: byte 0
    // holds the MSB, its top bit is the carry into the 0x87 constant.
    uint8_t zero[16] = {0}, l[16];
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, zero, l);

    uint8_t k1[16], k2[16];
    auto dbl = [](const uint8_t in[16], uint8_t out[16]) {
        uint8_t msb = static_cast<uint8_t>(in[0] & 0x80);
        for (int i = 0; i < 16; ++i) {
            uint8_t hi = (i < 15) ? static_cast<uint8_t>(in[i + 1] & 0x80) : 0;
            out[i] = static_cast<uint8_t>((in[i] << 1) | (hi ? 1 : 0));
        }
        return msb;
    };
    if (dbl(l, k1)) {
        k1[15] ^= 0x87;
    }
    if (dbl(k1, k2)) {
        k2[15] ^= 0x87;
    }

    // MAC over full blocks + partial (with 0x80 padding and K2 XOR).
    uint8_t y[16] = {0};
    size_t n = len / 16;
    bool lastFull = (len % 16) == 0 && len > 0;
    size_t blocks = lastFull ? n : n + 1;
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t block[16] = {0};
        size_t off = b * 16;
        bool isLast = (b == blocks - 1);
        if (isLast && !lastFull) {
            size_t rem = len - off;
            std::memcpy(block, data + off, rem);
            block[rem] = 0x80;
            for (int i = 0; i < 16; ++i) {
                block[i] ^= k2[i];
            }
        } else {
            std::memcpy(block, data + off, 16);
            if (isLast) {
                for (int i = 0; i < 16; ++i) {
                    block[i] ^= k1[i];
                }
            }
        }
        for (int i = 0; i < 16; ++i) {
            y[i] ^= block[i];
        }
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, y, y);
    }
    std::memcpy(mic, y, kMicLen);
    mbedtls_aes_free(&aes);
}

} // namespace

size_t buildFrame(uint8_t type, uint32_t originId, uint32_t seq,
                  uint16_t boot, const uint8_t *body, size_t bodyLen,
                  const uint8_t key[kKeyLen], uint8_t *out)
{
    if (!body || bodyLen == 0 || bodyLen > kMaxBody) {
        return 0;
    }
    static const uint8_t kZero[kKeyLen] = {0};
    if (std::memcmp(key, kZero, kKeyLen) == 0) {
        return 0; // ICD §10.6: no P2P without a provisioned key
    }

    out[0] = kFrameVersion;
    out[1] = type;
    for (int i = 0; i < 4; ++i) {
        out[2 + i] = static_cast<uint8_t>(originId >> (24 - 8 * i));
        out[6 + i] = static_cast<uint8_t>(seq >> (24 - 8 * i));
    }
    out[10] = 0; // hops — star topology
    out[11] = static_cast<uint8_t>(boot >> 8);
    out[12] = static_cast<uint8_t>(boot & 0xFF);
    out[13] = static_cast<uint8_t>(bodyLen);

    ctrCrypt(key, originId, seq, boot, body, out + kHeaderLen, bodyLen);

    cmac(key, out, kHeaderLen + bodyLen, out + kHeaderLen + bodyLen);

    const size_t total = kHeaderLen + bodyLen + kMicLen;
    const uint16_t crc = crc16Ccitt(out, total);
    out[total] = static_cast<uint8_t>(crc & 0xFF);
    out[total + 1] = static_cast<uint8_t>(crc >> 8);
    return total + kCrcLen;
}

size_t parseFrame(const uint8_t *in, size_t len, const uint8_t key[kKeyLen],
                  uint8_t *type, uint32_t *originId, uint32_t *seq,
                  uint8_t *body, size_t maxBody)
{
    if (len < kOverhead || len > kOverhead + kMaxBody) {
        return 0;
    }
    const uint16_t crc = static_cast<uint16_t>(in[len - 2]) |
                         static_cast<uint16_t>(in[len - 1]) << 8;
    if (crc != crc16Ccitt(in, len - kCrcLen)) {
        return 0;
    }
    if (in[0] != kFrameVersion || in[10] != 0) {
        return 0;
    }
    const uint8_t t = in[1];
    const uint32_t origin = static_cast<uint32_t>(in[2]) << 24 |
                            static_cast<uint32_t>(in[3]) << 16 |
                            static_cast<uint32_t>(in[4]) << 8 |
                            static_cast<uint32_t>(in[5]);
    const uint32_t sq = static_cast<uint32_t>(in[6]) << 24 |
                        static_cast<uint32_t>(in[7]) << 16 |
                        static_cast<uint32_t>(in[8]) << 8 |
                        static_cast<uint32_t>(in[9]);
    const uint16_t boot = static_cast<uint16_t>(in[11]) << 8 | in[12];
    const size_t bodyLen = in[13];
    if (kHeaderLen + bodyLen + kMicLen + kCrcLen != len ||
        bodyLen > maxBody) {
        return 0;
    }

    static const uint8_t kZero[kKeyLen] = {0};
    if (std::memcmp(key, kZero, kKeyLen) == 0) {
        return 0;
    }

    // MIC over header + ciphertext.
    uint8_t want[kMicLen];
    cmac(key, in, kHeaderLen + bodyLen, want);
    if (std::memcmp(want, in + kHeaderLen + bodyLen, kMicLen) != 0) {
        return 0;
    }

    ctrCrypt(key, origin, sq, boot, in + kHeaderLen, body, bodyLen);
    if (type) {
        *type = t;
    }
    if (originId) {
        *originId = origin;
    }
    if (seq) {
        *seq = sq;
    }
    return bodyLen;
}

uint16_t bootCount()
{
    // RTC cache: valid across deep sleep, invalidated by a real power
    // cycle (the magic dies with the RTC domain). Only a power cycle
    // increments the NVS counter — one flash write per battery swap.
    constexpr uint32_t kMagic = 0x424F4F54; // "BOOT"
    static RTC_DATA_ATTR uint32_t magic = 0;
    static RTC_DATA_ATTR uint16_t cache = 0;
    if (magic == kMagic) {
        return cache;
    }
    uint16_t boot = 0;
    nvs_handle_t h;
    if (nvs_open("minilora", NVS_READONLY, &h) == ESP_OK) {
        uint32_t stored = 0;
        if (nvs_get_u32(h, "p2p_boot", &stored) == ESP_OK) {
            boot = static_cast<uint16_t>(stored & 0xFFFF);
        }
        nvs_close(h);
    }
    boot = static_cast<uint16_t>((boot + 1) & 0xFFFF);
    if (nvs_open("minilora", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "p2p_boot", boot);
        nvs_commit(h);
        nvs_close(h);
    }
    magic = kMagic;
    cache = boot;
    return boot;
}

} // namespace p2p
} // namespace minilora
