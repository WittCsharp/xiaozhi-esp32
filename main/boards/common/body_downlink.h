#ifndef BODY_DOWNLINK_H_
#define BODY_DOWNLINK_H_

#include <cstddef>
#include <cstdint>

#include "body_downlink_format.h"

// Body-mode speaker. The LAN /api/speak WebSocket pushes Opus or PCM here and
// the device plays it before the rest of the utterance exists. Notify stays
// the fallback. Touch the main-loop methods only from the main task; the push
// methods are safe from the HTTP task.

namespace body_downlink {

enum class StartStatus { kOk, kBusy, kBodyModeOff, kTimeout, kAborted };

struct StartResult {
    StartStatus status = StartStatus::kBusy;
    uint32_t id = 0;
    const char* error = "busy";
};

StartResult RequestStart(Params params);
// Main loop. False when the arm was cancelled or already finished.
bool PrepareBegin();
void CompleteStart(StartStatus status);
bool PushOpus(uint32_t id, const uint8_t* data, std::size_t size, int duration_ms);
bool PushPcm(uint32_t id, const uint8_t* data, std::size_t size);
void MarkEnded(uint32_t id);
// Stops an armed or active stream. Safe from any task. Does not change the
// device state; the caller on the main loop does that.
bool Cancel();
bool IsActive();
// Main loop, after Speaking's decoder reset.
void OnSpeakingEntered();
// Main loop, when playback queues have drained.
void OnPlaybackDrained();

}  // namespace body_downlink

#endif  // BODY_DOWNLINK_H_
