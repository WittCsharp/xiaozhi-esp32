#include "lan_control_util.h"
#include "body_downlink_format.h"
#include "body_uplink_format.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, line, text);
        ++g_failures;
    }
}

#define EXPECT(cond) Expect((cond), #cond, __LINE__)

}  // namespace

int main() {
    using lan_control::BearerToken;
    using lan_control::IsAudioUrl;
    using lan_control::IsJsonContentType;
    using lan_control::IsWholeAngle;
    using lan_control::TokenEquals;

    EXPECT(TokenEquals("secret", "secret"));
    EXPECT(!TokenEquals("secret", "secreT"));
    EXPECT(!TokenEquals("secret", "secret-extra"));
    EXPECT(!TokenEquals("sec", "secret"));
    EXPECT(!TokenEquals("", ""));
    EXPECT(!TokenEquals("secret", ""));

    EXPECT(IsAudioUrl("http://192.168.1.20:8090/notify.ogg"));
    EXPECT(IsAudioUrl("https://cdn.example.com/audio/task.ogg"));
    EXPECT(!IsAudioUrl("http://"));
    EXPECT(!IsAudioUrl("http:///notify.ogg"));
    EXPECT(!IsAudioUrl("http://:80/notify.ogg"));
    EXPECT(!IsAudioUrl("HTTP://192.168.1.20/notify.ogg"));
    EXPECT(!IsAudioUrl("ftp://192.168.1.20/notify.ogg"));
    EXPECT(!IsAudioUrl("http://192.168.1.20/notify ogg"));
    EXPECT(!IsAudioUrl(std::string(lan_control::kMaxAudioUrlLen + 1, 'a')));
    EXPECT(IsAudioUrl(std::string("http://") + std::string(20, 'a')));

    std::string_view token;
    EXPECT(BearerToken("Bearer secret", token) && token == "secret");
    EXPECT(BearerToken("bearer secret", token) && token == "secret");
    EXPECT(BearerToken("BEARER secret", token) && token == "secret");
    EXPECT(!BearerToken("Bearer ", token));
    EXPECT(!BearerToken("Bearer", token));
    EXPECT(!BearerToken("Basic secret", token));
    EXPECT(!BearerToken("Bearer secret extra", token));

    EXPECT(IsJsonContentType("application/json"));
    EXPECT(IsJsonContentType("Application/JSON"));
    EXPECT(IsJsonContentType("application/json; charset=utf-8"));
    EXPECT(IsJsonContentType("application/json;charset=utf-8"));
    EXPECT(!IsJsonContentType("text/plain"));
    EXPECT(!IsJsonContentType("application/json-patch"));
    EXPECT(!IsJsonContentType(""));

    EXPECT(lan_control::IsBridgeUrl("http://192.168.1.20:9100"));
    EXPECT(lan_control::IsBridgeUrl("http://192.168.1.20:9100/"));
    EXPECT(!lan_control::IsBridgeUrl(""));
    EXPECT(!lan_control::IsBridgeUrl("http://"));
    EXPECT(lan_control::IsBridgeToken(""));
    EXPECT(lan_control::IsBridgeToken("bridge-secret"));
    EXPECT(!lan_control::IsBridgeToken("has space"));
    EXPECT(!lan_control::IsBridgeToken(std::string(lan_control::kMaxTokenLen + 1, 'a')));

    std::vector<uint8_t> clip;
    const uint8_t first[] = {'a', 'b'};
    const uint8_t second[] = {'x', 'y', 'z'};
    EXPECT(body_bridge::AppendOpusFrame(clip, first, 2, 32));
    EXPECT(body_bridge::AppendOpusFrame(clip, second, 3, 32));
    const uint8_t expected[] = {0x02, 0x00, 'a', 'b', 0x03, 0x00, 'x', 'y', 'z'};
    EXPECT(clip.size() == sizeof(expected));
    EXPECT(clip.size() == sizeof(expected) && std::equal(clip.begin(), clip.end(), expected));
    EXPECT(!body_bridge::AppendOpusFrame(clip, first, 2, clip.size()));
    EXPECT(body_bridge::AppendOpusFrame(clip, nullptr, 0, clip.size() + 2));
    EXPECT(body_bridge::kPostSpeechSilenceUs == 600000);
    EXPECT(body_bridge::kPostSpeechSilenceUs >= 400000);
    EXPECT(body_bridge::kPostSpeechSilenceUs < 1000000);

    int duration_ms = 0;
    const uint8_t silk20[] = {0x08, 0x00};
    EXPECT(body_downlink::OpusPacketDurationMs(silk20, sizeof(silk20), duration_ms));
    EXPECT(duration_ms == 20);
    const uint8_t celt25[] = {static_cast<uint8_t>(16 << 3)};
    EXPECT(!body_downlink::OpusPacketDurationMs(celt25, sizeof(celt25), duration_ms));
    EXPECT(!body_downlink::OpusPacketDurationMs(nullptr, 0, duration_ms));

    const uint8_t prefixed[] = {0x02, 0x00, 0x08, 0x00, 0x02, 0x00, 0x08, 0x00};
    const uint8_t* cursor = prefixed;
    std::size_t left = sizeof(prefixed);
    body_downlink::OpusView frame;
    EXPECT(body_downlink::NextLengthPrefixedOpus(cursor, left, frame));
    EXPECT(frame.size == 2 && frame.duration_ms == 20);
    EXPECT(body_downlink::NextLengthPrefixedOpus(cursor, left, frame));
    EXPECT(left == 0);
    cursor = prefixed;
    left = 3;
    EXPECT(!body_downlink::NextLengthPrefixedOpus(cursor, left, frame));

    std::size_t samples = 0;
    EXPECT(body_downlink::PcmSampleCount(640, samples) && samples == 320);
    EXPECT(!body_downlink::PcmSampleCount(1, samples));
    EXPECT(!body_downlink::PcmSampleCount(0, samples));

    body_downlink::Control control = body_downlink::Control::kEnd;
    body_downlink::Params params;
    const char* error = nullptr;
    EXPECT(body_downlink::ParseSpeakJson(
        R"({"event":"start","codec":"opus","sample_rate":16000,"frame_duration_ms":20,"text":"Hi \"there\"","emotion":"happy"})",
        control, params, error));
    EXPECT(control == body_downlink::Control::kStart);
    EXPECT(params.codec == body_downlink::Codec::kOpus);
    EXPECT(params.sample_rate == 16000);
    EXPECT(params.frame_duration_ms == 20);
    EXPECT(params.framing == body_downlink::Framing::kPacket);
    EXPECT(params.text == "Hi \"there\"");
    EXPECT(params.emotion == "happy");

    EXPECT(body_downlink::ParseSpeakJson(
        R"({"event":"start","codec":"pcm","sample_rate":24000,"channels":1})", control, params,
        error));
    EXPECT(params.codec == body_downlink::Codec::kPcm);
    EXPECT(params.sample_rate == 24000);
    EXPECT(params.frame_duration_ms == 0);

    EXPECT(body_downlink::ParseSpeakJson(
        R"({"codec":"opus","event":"start","framing":"length-prefixed"})", control, params, error));
    EXPECT(params.framing == body_downlink::Framing::kLengthPrefixed);
    EXPECT(params.sample_rate == 16000);
    EXPECT(params.frame_duration_ms == 60);

    EXPECT(body_downlink::ParseSpeakJson(R"({"event":"end","ignored":1})", control, params, error));
    EXPECT(control == body_downlink::Control::kEnd);
    EXPECT(body_downlink::ParseSpeakJson(R"({"event":"abort"})", control, params, error));
    EXPECT(control == body_downlink::Control::kAbort);

    EXPECT(!body_downlink::ParseSpeakJson(R"({"event":"start","channels":2})", control, params,
                                          error));
    EXPECT(!body_downlink::ParseSpeakJson(R"({"event":"start","codec":"mp3"})", control, params,
                                          error));
    EXPECT(!body_downlink::ParseSpeakJson(R"({"event":"start","sample_rate":22050})", control,
                                          params, error));
    EXPECT(!body_downlink::ParseSpeakJson(R"({"event":"nope"})", control, params, error));
    EXPECT(!body_downlink::ParseSpeakJson(R"({"event":"start","text":{"a":1}})", control, params,
                                          error));
    EXPECT(!body_downlink::ParseSpeakJson("[]", control, params, error));

    int angle = 123;
    EXPECT(IsWholeAngle(0, angle) && angle == 0);
    EXPECT(IsWholeAngle(-90, angle) && angle == -90);
    EXPECT(IsWholeAngle(90, angle) && angle == 90);
    EXPECT(!IsWholeAngle(90.5, angle));
    EXPECT(!IsWholeAngle(91, angle));
    EXPECT(!IsWholeAngle(-91, angle));

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    return 0;
}
