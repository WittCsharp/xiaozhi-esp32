#ifndef BODY_DOWNLINK_FORMAT_H_
#define BODY_DOWNLINK_FORMAT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Host-testable wire format for LAN /api/speak. The device plays these bytes
// as they arrive. This header does not depend on ESP-IDF.

namespace body_downlink {

constexpr std::size_t kMaxSpeakJson = 2048;
constexpr std::size_t kMaxSpeakText = 200;
constexpr std::size_t kMaxSpeakEmotion = 32;
constexpr std::size_t kMaxBinaryFrame = 16 * 1024;
constexpr std::size_t kMaxOpusPacket = 1500;

enum class Control { kStart, kEnd, kAbort };
enum class Codec { kOpus, kPcm };
enum class Framing { kPacket, kLengthPrefixed };

struct Params {
    Codec codec = Codec::kOpus;
    int sample_rate = 16000;
    int frame_duration_ms = 60;
    Framing framing = Framing::kPacket;
    std::string text;
    std::string emotion;
};

inline bool IsSupportedSampleRate(int rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

inline bool IsSupportedFrameDuration(int ms) {
    return ms == 5 || ms == 10 || ms == 20 || ms == 40 || ms == 60 || ms == 80 || ms == 100 ||
           ms == 120;
}

// Opus TOC duration in milliseconds. 2.5 ms packets and truncated headers
// return false. Same rules as OggDemuxer::GetOpusPacketDurationMs.
inline bool OpusPacketDurationMs(const uint8_t* data, std::size_t size, int& duration_ms) {
    if (data == nullptr || size == 0 || size > kMaxOpusPacket) {
        return false;
    }
    const uint8_t toc = data[0];
    const uint8_t config = static_cast<uint8_t>(toc >> 3);
    int frame_duration_us = 0;
    if (config < 12) {
        static constexpr int kSilkUs[] = {10000, 20000, 40000, 60000};
        frame_duration_us = kSilkUs[config & 0x03];
    } else if (config < 16) {
        frame_duration_us = (config & 0x01) ? 20000 : 10000;
    } else {
        static constexpr int kCeltUs[] = {2500, 5000, 10000, 20000};
        frame_duration_us = kCeltUs[config & 0x03];
    }

    int frame_count = 0;
    switch (toc & 0x03) {
        case 0:
            frame_count = 1;
            break;
        case 1:
        case 2:
            frame_count = 2;
            break;
        case 3:
            if (size < 2) {
                return false;
            }
            frame_count = data[1] & 0x3f;
            break;
        default:
            return false;
    }
    const int packet_us = frame_duration_us * frame_count;
    if (frame_count == 0 || packet_us <= 0 || packet_us > 120000 || packet_us % 1000 != 0) {
        return false;
    }
    duration_ms = packet_us / 1000;
    return IsSupportedFrameDuration(duration_ms);
}

struct OpusView {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
    int duration_ms = 0;
};

// One length-prefixed Opus packet: uint16 little-endian size, then that many
// bytes. The cursor advances only on success.
inline bool NextLengthPrefixedOpus(const uint8_t*& data, std::size_t& size, OpusView& frame) {
    if (data == nullptr || size < 2) {
        return false;
    }
    const std::size_t packet =
        static_cast<std::size_t>(data[0]) | (static_cast<std::size_t>(data[1]) << 8);
    if (packet == 0 || packet > kMaxOpusPacket || size < packet + 2) {
        return false;
    }
    int duration_ms = 0;
    if (!OpusPacketDurationMs(data + 2, packet, duration_ms)) {
        return false;
    }
    frame.data = data + 2;
    frame.size = packet;
    frame.duration_ms = duration_ms;
    data += packet + 2;
    size -= packet + 2;
    return true;
}

inline bool PcmSampleCount(std::size_t bytes, std::size_t& samples) {
    if (bytes == 0 || (bytes % 2) != 0 || bytes > kMaxBinaryFrame) {
        return false;
    }
    samples = bytes / 2;
    return true;
}

namespace detail {

inline bool IsWhitespace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool SkipWs(std::string_view json, std::size_t& i) {
    while (i < json.size() && IsWhitespace(static_cast<unsigned char>(json[i]))) {
        ++i;
    }
    return i < json.size();
}

inline bool ParseString(std::string_view json, std::size_t& i, std::string& out) {
    if (i >= json.size() || json[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[i]);
        if (c == '"') {
            ++i;
            return true;
        }
        if (c == '\\') {
            if (i + 1 >= json.size()) {
                return false;
            }
            const char esc = json[i + 1];
            char decoded = 0;
            if (esc == '"' || esc == '\\' || esc == '/') {
                decoded = esc;
            } else if (esc == 'n') {
                decoded = '\n';
            } else if (esc == 't') {
                decoded = '\t';
            } else {
                return false;
            }
            if (decoded != '\n' && decoded != '\t' && static_cast<unsigned char>(decoded) < 0x20) {
                return false;
            }
            out.push_back(decoded);
            i += 2;
            continue;
        }
        if (c < 0x20) {
            return false;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return false;
}

inline bool ParseInt(std::string_view json, std::size_t& i, int& out) {
    if (i >= json.size()) {
        return false;
    }
    const bool neg = json[i] == '-';
    if (neg) {
        ++i;
    }
    if (i >= json.size() || json[i] < '0' || json[i] > '9') {
        return false;
    }
    int value = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        const int digit = json[i] - '0';
        if (value > (2147483647 - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        ++i;
    }
    out = neg ? -value : value;
    return true;
}

inline bool SkipValue(std::string_view json, std::size_t& i) {
    if (!SkipWs(json, i)) {
        return false;
    }
    if (json[i] == '"') {
        std::string ignored;
        return ParseString(json, i, ignored);
    }
    if (json[i] == '-' || (json[i] >= '0' && json[i] <= '9')) {
        int ignored = 0;
        return ParseInt(json, i, ignored);
    }
    return false;
}

}  // namespace detail

// Flat JSON object. Unknown keys are ignored. Nested objects and arrays are
// rejected. `error` is "invalid_speak" on failure.
inline bool ParseSpeakJson(std::string_view json, Control& control, Params& params,
                           const char*& error) {
    error = "invalid_speak";
    if (json.size() > kMaxSpeakJson) {
        return false;
    }
    std::size_t i = 0;
    if (!detail::SkipWs(json, i) || json[i] != '{') {
        return false;
    }
    ++i;

    bool saw_event = false;
    bool saw_codec = false;
    bool saw_rate = false;
    bool saw_duration = false;
    bool saw_framing = false;
    bool saw_channels = false;
    bool saw_text = false;
    bool saw_emotion = false;
    Control parsed_control = Control::kStart;
    std::string codec;
    std::string framing;
    std::string text;
    std::string emotion;
    int sample_rate = 16000;
    int frame_duration_ms = 60;
    int channels = 1;

    while (true) {
        if (!detail::SkipWs(json, i)) {
            return false;
        }
        if (json[i] == '}') {
            ++i;
            break;
        }
        std::string key;
        if (!detail::ParseString(json, i, key)) {
            return false;
        }
        if (!detail::SkipWs(json, i) || json[i] != ':') {
            return false;
        }
        ++i;
        if (!detail::SkipWs(json, i)) {
            return false;
        }

        auto take_string = [&](std::string& dest, bool& seen) -> bool {
            if (!detail::ParseString(json, i, dest)) {
                return false;
            }
            seen = true;
            return true;
        };
        bool ok = true;
        if (key == "event") {
            std::string event;
            ok = take_string(event, saw_event);
            if (ok) {
                if (event == "start") {
                    parsed_control = Control::kStart;
                } else if (event == "end") {
                    parsed_control = Control::kEnd;
                } else if (event == "abort") {
                    parsed_control = Control::kAbort;
                } else {
                    return false;
                }
            }
        } else if (key == "codec") {
            ok = take_string(codec, saw_codec);
        } else if (key == "framing") {
            ok = take_string(framing, saw_framing);
        } else if (key == "text") {
            ok = take_string(text, saw_text);
        } else if (key == "emotion") {
            ok = take_string(emotion, saw_emotion);
        } else if (key == "sample_rate") {
            ok = detail::ParseInt(json, i, sample_rate);
            saw_rate = ok;
        } else if (key == "frame_duration_ms") {
            ok = detail::ParseInt(json, i, frame_duration_ms);
            saw_duration = ok;
        } else if (key == "channels") {
            ok = detail::ParseInt(json, i, channels);
            saw_channels = ok;
        } else {
            ok = detail::SkipValue(json, i);
        }
        if (!ok) {
            return false;
        }
        if (!detail::SkipWs(json, i)) {
            return false;
        }
        if (json[i] == ',') {
            ++i;
            continue;
        }
        if (json[i] == '}') {
            ++i;
            break;
        }
        return false;
    }
    if (!detail::SkipWs(json, i) && i != json.size()) {
        return false;
    }
    if (i != json.size() || !saw_event) {
        return false;
    }

    control = parsed_control;
    if (control != Control::kStart) {
        params = Params{};
        return true;
    }
    if (saw_channels && channels != 1) {
        return false;
    }
    if (saw_text && text.size() > kMaxSpeakText) {
        return false;
    }
    if (saw_emotion && emotion.size() > kMaxSpeakEmotion) {
        return false;
    }
    if (!saw_rate) {
        sample_rate = 16000;
    }
    if (!IsSupportedSampleRate(sample_rate)) {
        return false;
    }
    if (!saw_codec) {
        codec = "opus";
    }
    Params out;
    out.sample_rate = sample_rate;
    out.text = saw_text ? std::move(text) : std::string{};
    out.emotion = saw_emotion ? std::move(emotion) : std::string{};
    if (codec == "opus") {
        out.codec = Codec::kOpus;
        if (!saw_duration) {
            frame_duration_ms = 60;
        }
        if (!IsSupportedFrameDuration(frame_duration_ms)) {
            return false;
        }
        out.frame_duration_ms = frame_duration_ms;
        if (!saw_framing || framing == "packet") {
            out.framing = Framing::kPacket;
        } else if (framing == "length-prefixed") {
            out.framing = Framing::kLengthPrefixed;
        } else {
            return false;
        }
    } else if (codec == "pcm") {
        if (saw_framing && framing != "packet") {
            return false;
        }
        out.codec = Codec::kPcm;
        out.framing = Framing::kPacket;
        out.frame_duration_ms = 0;
    } else {
        return false;
    }
    params = std::move(out);
    return true;
}

}  // namespace body_downlink

#endif  // BODY_DOWNLINK_FORMAT_H_
