#ifndef BODY_UPLINK_FORMAT_H_
#define BODY_UPLINK_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#include <vector>

// Length-prefixed Opus packets posted to {bridge}/body/audio.
// Each frame is uint16 little-endian payload length, then that many bytes.
// The bytes are one raw Opus packet (not an Ogg page). Sample rate, channel
// count, and frame duration travel in HTTP headers.

namespace body_bridge {

constexpr size_t kMaxBodyClipBytes = 32 * 1024;
// Silence after local VAD has already gone quiet. 600 ms is shorter than the
// previous 1 s endpoint and still longer than a brief pause between words.
constexpr uint64_t kPostSpeechSilenceUs = 600000;

inline bool AppendOpusFrame(std::vector<uint8_t>& out, const uint8_t* data, size_t size,
                            size_t max_bytes) {
    if (data == nullptr && size != 0) {
        return false;
    }
    if (size > 0xffff) {
        return false;
    }
    const size_t needed = size + 2;
    if (out.size() > max_bytes || needed > max_bytes - out.size()) {
        return false;
    }
    out.push_back(static_cast<uint8_t>(size & 0xff));
    out.push_back(static_cast<uint8_t>((size >> 8) & 0xff));
    if (size != 0) {
        out.insert(out.end(), data, data + size);
    }
    return true;
}

}  // namespace body_bridge

#endif  // BODY_UPLINK_FORMAT_H_
