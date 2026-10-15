#ifndef LAN_CONTROL_UTIL_H_
#define LAN_CONTROL_UTIL_H_

#include <cmath>
#include <cstddef>
#include <string_view>

// Shared checks for the LAN control API. This header is host-testable and does
// not depend on ESP-IDF.

namespace lan_control {

constexpr std::size_t kMaxTokenLen = 128;
constexpr std::size_t kMaxAudioUrlLen = 480;
constexpr std::size_t kMaxBodyBytes = 4096;
constexpr std::size_t kMaxSubtitles = 12;
constexpr std::size_t kMaxSubtitleText = 200;
constexpr std::size_t kMaxStatusLen = 48;
constexpr std::size_t kMaxMessageLen = 200;
constexpr std::size_t kMaxEmotionLen = 32;
constexpr int kServoPanGpio = 8;
constexpr int kServoTiltGpio = 9;
constexpr int kServoMinAngle = -90;
constexpr int kServoMaxAngle = 90;

inline bool TokenEquals(std::string_view provided, std::string_view expected) {
    if (expected.empty()) {
        return false;
    }
    unsigned diff = static_cast<unsigned>(provided.size() ^ expected.size());
    const std::size_t length =
        provided.size() > expected.size() ? provided.size() : expected.size();
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char left =
            i < provided.size() ? static_cast<unsigned char>(provided[i]) : 0;
        const unsigned char right =
            i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0;
        diff |= static_cast<unsigned>(left ^ right);
    }
    return diff == 0;
}

// http:// or https:// with a host. The device HTTP client does not follow
// redirects, and NotifyPlayer accepts only these two schemes.
inline bool IsAudioUrl(std::string_view url) {
    const bool https = url.size() >= 8 && url.compare(0, 8, "https://") == 0;
    const bool http = !https && url.size() >= 7 && url.compare(0, 7, "http://") == 0;
    if (!http && !https) {
        return false;
    }
    if (url.size() > kMaxAudioUrlLen) {
        return false;
    }
    const std::size_t host = https ? 8 : 7;
    if (host >= url.size()) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(url[host]);
    if (first == '/' || first == ':') {
        return false;
    }
    for (char ch : url) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 0x20 || c >= 0x7f) {
            return false;
        }
    }
    return true;
}

// Accepts "Bearer <token>" with a case-insensitive scheme. token aliases
// authorization and is valid only while that string lives.
inline bool BearerToken(std::string_view authorization, std::string_view& token) {
    constexpr std::string_view kPrefix = "bearer ";
    if (authorization.size() <= kPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(authorization[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c != static_cast<unsigned char>(kPrefix[i])) {
            return false;
        }
    }
    token = authorization.substr(kPrefix.size());
    return !token.empty() && token.find(' ') == std::string_view::npos;
}

inline bool IsJsonContentType(std::string_view content_type) {
    constexpr std::string_view kJson = "application/json";
    if (content_type.size() < kJson.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kJson.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(content_type[i]);
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<unsigned char>(c - 'A' + 'a');
        }
        if (c != static_cast<unsigned char>(kJson[i])) {
            return false;
        }
    }
    if (content_type.size() == kJson.size()) {
        return true;
    }
    const unsigned char next = static_cast<unsigned char>(content_type[kJson.size()]);
    return next == ';' || next == ' ' || next == '\t';
}

constexpr std::size_t kMaxBridgeUrlLen = kMaxAudioUrlLen;

// Bridge base URL. Same rules as an audio URL: lowercase http(s), a host, no
// spaces. A path is optional (`http://192.168.1.20:9100`).
inline bool IsBridgeUrl(std::string_view url) { return IsAudioUrl(url); }

// Optional bearer the device sends to the Mac bridge. Empty clears the stored
// token. NVS keys are at most 15 characters, so the key is "bridge_token".
inline bool IsBridgeToken(std::string_view token) {
    if (token.size() > kMaxTokenLen) {
        return false;
    }
    for (unsigned char c : token) {
        if (c <= 0x20 || c >= 0x7f) {
            return false;
        }
    }
    return true;
}

inline bool IsWholeAngle(double value, int& angle) {
    if (!(value >= static_cast<double>(kServoMinAngle) &&
          value <= static_cast<double>(kServoMaxAngle))) {
        return false;
    }
    if (value != std::trunc(value)) {
        return false;
    }
    angle = static_cast<int>(value);
    return true;
}

}  // namespace lan_control

#endif  // LAN_CONTROL_UTIL_H_
