#ifndef BODY_UPLINK_FORMAT_H_
#define BODY_UPLINK_FORMAT_H_

#include <stddef.h>
#include <stdint.h>

#include <string_view>
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
// Idle window for one body conversation. The device waits this long for a
// reply, and again for the next utterance after playback. Wake is required
// once it expires. Kconfig BODY_CONVERSATION_IDLE_MS overrides the default.
#if defined(CONFIG_BODY_CONVERSATION_IDLE_MS)
constexpr uint64_t kConversationIdleUs =
    static_cast<uint64_t>(CONFIG_BODY_CONVERSATION_IDLE_MS) * 1000ULL;
#else
constexpr uint64_t kConversationIdleUs = 20000000ULL;
#endif

// A finished utterance keeps the conversation open. BOOT ("button") and an
// idle follow-up ("session") close it. Timeout with no speech closes it too.
inline bool KeepsConversation(const char* reason, bool heard_voice) {
    if (reason == nullptr || !heard_voice) {
        return false;
    }
    const std::string_view why(reason);
    return why == "silence" || why == "full" || why == "timeout";
}

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
