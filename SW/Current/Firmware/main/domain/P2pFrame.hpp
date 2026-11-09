// P2P star frame — ICD §10.3.
//
//   0      1      2                 6          10        11    12     12+n  16     18
//   +------+------+-----------------+----------+---------+-----+------+-----+------+
//   | ver  | type | origin_id (u32) | seq (u32)| hops(1) | len | body | mic | crc16|
//
// Body is AES-128-CTR encrypted with the mesh PSK (IV = origin || seq ||
// 8×0x00); mic = first 4 bytes of AES-CMAC(key, header || ciphertext);
// crc16-CCITT (ICD-wide) covers everything before it, little-endian.
// The body payloads are the §1/§2/§2.1/§3 frames verbatim — no field is
// re-encoded anywhere in the chain.

#pragma once

#include <cstddef>
#include <cstdint>

namespace minilora {
namespace p2p {

inline constexpr uint8_t kFrameVersion = 0x01;
inline constexpr uint8_t kTypeTelemetry = 0x01; // §1 28-byte body
inline constexpr uint8_t kTypeDiag = 0x02;      // §2 body (24/28/29 B)
inline constexpr uint8_t kTypeHistory = 0x03;   // §2.1 body
inline constexpr uint8_t kTypeCommand = 0x04;   // §3 3-byte body
inline constexpr uint8_t kTypeSlotAssign = 0x05; // v2, gateway → leaf

inline constexpr size_t kHeaderLen = 12;   // ver+type+origin+seq+hops+len
inline constexpr size_t kMicLen = 4;
inline constexpr size_t kCrcLen = 2;
inline constexpr size_t kOverhead = kHeaderLen + kMicLen + kCrcLen;
inline constexpr size_t kMaxBody = 255 - kOverhead;
inline constexpr size_t kKeyLen = 16;

/// Maximum on-air size for a given body size (bounds checks).
inline constexpr size_t frameSize(size_t bodyLen)
{
    return kOverhead + bodyLen;
}

/// Build a frame: encrypts `body` and appends mic + CRC.
/// `out` must hold frameSize(bodyLen). Returns the on-air length,
/// or 0 on invalid input (body too long / zero key).
size_t buildFrame(uint8_t type, uint32_t originId, uint32_t seq,
                  const uint8_t *body, size_t bodyLen,
                  const uint8_t key[kKeyLen], uint8_t *out);

/// Parse + authenticate + decrypt in place. `body` receives the clear
/// body (capacity kMaxBody). Returns body length, 0 on any failure
/// (CRC, mic, version, len).
size_t parseFrame(const uint8_t *in, size_t len,
                  const uint8_t key[kKeyLen],
                  uint8_t *type, uint32_t *originId, uint32_t *seq,
                  uint8_t *body, size_t maxBody);

} // namespace p2p
} // namespace minilora
