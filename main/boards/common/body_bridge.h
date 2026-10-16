#ifndef BODY_BRIDGE_H_
#define BODY_BRIDGE_H_

#include <esp_timer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "body_uplink_format.h"
#include "protocol.h"

// Body mode: the device is a mic/speaker/face, and a Mac bridge is the brain.
// Compiled only with CONFIG_LAN_CONTROL_API. Cloud chat is unchanged when the
// runtime flag is off.

namespace body_bridge {

bool Enabled();
void SetEnabled(bool on);
bool BridgeConfigured();
bool TokenConfigured();
std::string BridgeUrl();

// Empty url clears the stored URL. token == nullptr leaves the token as it is.
// Returns false when a non-empty URL or token fails validation.
bool SetBridge(const std::string& url, const std::string* token);

enum class ClipResult { kNotCapturing, kNoBridge, kQueued, kUploadFailed };

// Mic capture for one clip. Touch this from the main loop only, except
// IsCapturing/ClipId/HeardVoice which are safe from the HTTP task and esp_timer.
class Session {
public:
    static Session& GetInstance();

    using StopCallback = std::function<void(const char* reason)>;

    bool IsCapturing() const;
    uint32_t ClipId() const;

    // source is "button", "wake", or "followup". on_stop runs on the esp_timer task.
    // pre_speech_us > 0 waits that long for speech, then stops with "session".
    // The 8 s cap starts when speech begins. 0 keeps the 8 s cap from Begin.
    bool Begin(const char* source, StopCallback on_stop, uint64_t pre_speech_us = 0);
    bool HeardVoice() const;
    void OnVad(bool speaking);
    // False when the clip is full and the caller should finish it.
    bool Accept(std::unique_ptr<AudioStreamPacket> packet);
    ClipResult Finish(const char* reason);
    void Cancel();

private:
    Session() = default;

    void EnsureTimers();
    void StopTimers();
    void ArmSilenceTimer();
    void DisarmSilenceTimer();
    void ArmPreSpeechTimer(uint64_t us);
    void ArmMaxTimer();
    void Fire(const char* reason);

    mutable std::mutex mutex_;
    bool capturing_ = false;
    bool heard_voice_ = false;
    bool pause_notified_ = false;
    bool defer_max_ = false;
    StopCallback on_stop_;
    std::string source_;
    std::vector<uint8_t> audio_;
    std::vector<std::vector<uint8_t>> preroll_;
    int frames_ = 0;
    int sample_rate_ = 16000;
    int frame_ms_ = 60;
    uint32_t clip_id_ = 0;
    esp_timer_handle_t max_timer_ = nullptr;
    esp_timer_handle_t silence_timer_ = nullptr;
    esp_timer_handle_t prespeech_timer_ = nullptr;
};

}  // namespace body_bridge

#endif  // BODY_BRIDGE_H_
