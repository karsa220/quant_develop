#include "feed/CtpLikeCodec.h"

#include <cstring>

namespace md {

static inline void putLE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
static inline uint16_t getLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static inline uint8_t checksum(const uint8_t* p, uint32_t n) {
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; ++i) c ^= p[i];
    return c;
}

uint32_t CtpLikeCodec_encodeTick(const MarketTick& t, uint8_t* buf) {
    putLE16(buf + 0, kMagic);                       // magic
    buf[2] = static_cast<uint8_t>(MSG_TICK);        // type
    putLE16(buf + 3, sizeof(TickPayload));          // len
    std::memcpy(buf + 5, &t, sizeof(TickPayload)); // payload
    buf[5 + sizeof(TickPayload)] = checksum(buf, 5 + sizeof(TickPayload));
    return kTickFrameSize;
}

bool CtpLikeCodec_decodeTick(const uint8_t* frame, uint32_t frameLen, MarketTick& out) {
    if (frameLen != kTickFrameSize) return false;
    if (getLE16(frame) != kMagic) return false;
    if (frame[2] != static_cast<uint8_t>(MSG_TICK)) return false;
    if (getLE16(frame + 3) != sizeof(TickPayload)) return false;
    const uint8_t cs = checksum(frame, 5 + sizeof(TickPayload));
    if (frame[5 + sizeof(TickPayload)] != cs) return false;
    std::memcpy(&out, frame + 5, sizeof(TickPayload));
    return true;
}

}  // namespace md
