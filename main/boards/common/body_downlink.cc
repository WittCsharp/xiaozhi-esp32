#include "body_downlink.h"

#include "application.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* TAG = "BodyDownlink";
constexpr uint32_t kWorkerStackBytes = 8192;
constexpr std::size_t kMaxQueuedBytes = body_downlink::kMaxBinaryFrame;
constexpr auto kStartTimeout = std::chrono::milliseconds(2000);

struct Chunk {
    uint32_t id = 0;
    bool pcm = false;
    int sample_rate = 16000;
    int frame_ms = 60;
    std::vector<uint8_t> bytes;
};

const char* StatusError(body_downlink::StartStatus status) {
    switch (status) {
        case body_downlink::StartStatus::kOk:
            return nullptr;
        case body_downlink::StartStatus::kBusy:
            return "busy";
        case body_downlink::StartStatus::kBodyModeOff:
            return "body_mode_off";
        case body_downlink::StartStatus::kTimeout:
            return "timeout";
        case body_downlink::StartStatus::kAborted:
            return "aborted";
    }
    return "busy";
}

class Session {
public:
    static Session& GetInstance() {
        static Session instance;
        return instance;
    }

    body_downlink::StartResult RequestStart(body_downlink::Params params) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ || armed_ || waiting_) {
                return {body_downlink::StartStatus::kBusy, 0, "busy"};
            }
            if (!EnsureWorkerLocked()) {
                return {body_downlink::StartStatus::kBusy, 0, "busy"};
            }
            waiting_ = true;
            armed_ = true;
            cancelled_ = false;
            done_ = false;
            producer_done_ = false;
            finished_ = false;
            accepting_ = false;
            active_ = false;
            params_ = std::move(params);
            id_ = NextIdLocked();
            result_id_ = id_;
            status_ = body_downlink::StartStatus::kBusy;
            error_ = "busy";
        }

        Application::GetInstance().Schedule([]() { Application::GetInstance().BeginBodySpeak(); });

        bool timed_out = false;
        body_downlink::StartResult result;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, kStartTimeout, [this]() { return done_; })) {
                if (!done_) {
                    cancelled_ = true;
                    timed_out = true;
                }
            }
            result.status = done_ ? status_ : body_downlink::StartStatus::kTimeout;
            result.id = result.status == body_downlink::StartStatus::kOk ? result_id_ : 0;
            result.error = done_ ? error_ : "timeout";
            if (timed_out) {
                ESP_LOGW(TAG, "Speak start timed out");
            }
        }
        // BeginBodySpeak may already have passed PrepareBegin. This still
        // reverts Speaking if that transition landed after the timeout.
        if (timed_out) {
            Application::GetInstance().Schedule([]() { body_downlink::OnSpeakingEntered(); });
        }
        return result;
    }

    bool PrepareBegin() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!armed_ || cancelled_ || done_) {
            if (!done_) {
                FinishWaitLocked(body_downlink::StartStatus::kAborted);
            }
            armed_ = false;
            return false;
        }
        return true;
    }

    void CompleteStart(body_downlink::StartStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_) {
            return;
        }
        armed_ = false;
        FinishWaitLocked(status);
    }

    void OnSpeakingEntered() {
        body_downlink::Params params;
        bool revert = false;
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!armed_) {
                return;
            }
            armed_ = false;
            if (cancelled_ || done_) {
                revert = true;
            } else {
                active_ = true;
                accepting_ = true;
                params = params_;
                accepted = true;
                FinishWaitLocked(body_downlink::StartStatus::kOk);
            }
        }
        if (revert) {
            CompleteStart(body_downlink::StartStatus::kAborted);
            if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
                Application::GetInstance().SetDeviceState(kDeviceStateIdle);
            }
            return;
        }
        if (!accepted) {
            return;
        }
        auto* display = Board::GetInstance().GetDisplay();
        if (!params.emotion.empty()) {
            display->SetEmotion(params.emotion.c_str());
        }
        if (!params.text.empty()) {
            display->SetChatMessage("assistant", params.text.c_str());
        }
        ESP_LOGI(TAG, "Speak stream playing (%s %d Hz)",
                 params.codec == body_downlink::Codec::kPcm ? "pcm" : "opus", params.sample_rate);
    }

    bool PushOpus(uint32_t id, const uint8_t* data, std::size_t size, int duration_ms) {
        if (data == nullptr || size == 0 || size > body_downlink::kMaxOpusPacket ||
            !body_downlink::IsSupportedFrameDuration(duration_ms)) {
            return false;
        }
        Chunk chunk;
        chunk.id = id;
        chunk.pcm = false;
        chunk.frame_ms = duration_ms;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chunk.sample_rate = params_.sample_rate;
        }
        chunk.bytes.assign(data, data + size);
        return Enqueue(std::move(chunk));
    }

    bool PushPcm(uint32_t id, const uint8_t* data, std::size_t size) {
        if (data == nullptr || size == 0 || (size % 2) != 0 ||
            size > body_downlink::kMaxBinaryFrame) {
            return false;
        }
        Chunk chunk;
        chunk.id = id;
        chunk.pcm = true;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            chunk.sample_rate = params_.sample_rate;
        }
        chunk.bytes.assign(data, data + size);
        return Enqueue(std::move(chunk));
    }

    void MarkEnded(uint32_t id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_ || id != id_ || finished_) {
                return;
            }
            producer_done_ = true;
        }
        Application::GetInstance().Schedule([]() { body_downlink::OnPlaybackDrained(); });
    }

    bool Cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was = active_ || armed_ || accepting_ || (waiting_ && !done_);
        active_ = false;
        accepting_ = false;
        armed_ = false;
        producer_done_ = false;
        finished_ = true;
        queue_.clear();
        queued_bytes_ = 0;
        cancelled_ = true;
        id_ = NextIdLocked();
        if (waiting_ && !done_) {
            FinishWaitLocked(body_downlink::StartStatus::kAborted);
        }
        cv_.notify_all();
        return was;
    }

    bool IsActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_ || armed_ || accepting_;
    }

    void OnPlaybackDrained() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_ || !producer_done_ || finished_ || !queue_.empty() || worker_busy_) {
                return;
            }
        }
        if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_ || !producer_done_ || finished_ || !queue_.empty() || worker_busy_) {
                return;
            }
            finished_ = true;
            active_ = false;
            accepting_ = false;
        }
        ESP_LOGI(TAG, "Speak stream finished");
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
            Application::GetInstance().SetDeviceState(kDeviceStateIdle);
        }
    }

private:
    Session() = default;

    uint32_t NextIdLocked() {
        uint32_t id = ++next_id_;
        if (id == 0) {
            id = ++next_id_;
        }
        return id;
    }

    void FinishWaitLocked(body_downlink::StartStatus status) {
        if (done_) {
            return;
        }
        done_ = true;
        waiting_ = false;
        status_ = status;
        error_ = StatusError(status);
        cv_.notify_all();
    }

    bool EnsureWorkerLocked() {
        if (worker_running_) {
            return true;
        }
        worker_running_ = true;
        if (xTaskCreate(WorkerEntry, "body_down", kWorkerStackBytes, this, 2, nullptr) != pdPASS) {
            worker_running_ = false;
            ESP_LOGE(TAG, "Failed to create speak worker");
            return false;
        }
        return true;
    }

    bool Enqueue(Chunk chunk) {
        const std::size_t nbytes = chunk.bytes.size();
        if (nbytes == 0 || nbytes > kMaxQueuedBytes) {
            return false;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, &chunk, nbytes]() {
            return cancelled_ || !accepting_ || chunk.id != id_ || finished_ || producer_done_ ||
                   queued_bytes_ + nbytes <= kMaxQueuedBytes;
        });
        if (cancelled_ || !accepting_ || chunk.id != id_ || finished_ || producer_done_) {
            return false;
        }
        queued_bytes_ += nbytes;
        queue_.push_back(std::move(chunk));
        cv_.notify_all();
        return true;
    }

    static void WorkerEntry(void* arg) {
        static_cast<Session*>(arg)->Worker();
        vTaskDelete(nullptr);
    }

    void Worker() {
        for (;;) {
            Chunk chunk;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return !queue_.empty(); });
                chunk = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= chunk.bytes.size();
                worker_busy_ = true;
                cv_.notify_all();
            }

            const bool played = Play(chunk);

            bool schedule_drain = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                worker_busy_ = false;
                schedule_drain = producer_done_ && queue_.empty() && active_ && !finished_;
                cv_.notify_all();
            }
            if (!played) {
                ESP_LOGW(TAG, "Speak chunk dropped");
            }
            if (schedule_drain) {
                Application::GetInstance().Schedule([]() { body_downlink::OnPlaybackDrained(); });
            }
        }
    }

    bool Play(const Chunk& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!accepting_ || chunk.id != id_ || finished_) {
                return false;
            }
        }
        auto& audio = Application::GetInstance().GetAudioService();
        bool played = false;
        if (chunk.pcm) {
            if (chunk.bytes.size() % sizeof(int16_t) != 0) {
                return false;
            }
            const std::size_t count = chunk.bytes.size() / sizeof(int16_t);
            std::vector<int16_t> samples(count);
            std::memcpy(samples.data(), chunk.bytes.data(), chunk.bytes.size());
            played =
                audio.PushPcmToPlaybackQueue(std::move(samples), chunk.sample_rate, 0, 0, true);
        } else {
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = chunk.sample_rate;
            packet->frame_duration = chunk.frame_ms;
            packet->payload = chunk.bytes;
            played = audio.PushPacketToDecodeQueue(std::move(packet), true);
        }
        // Abort bumps the id and resets the decoder before this push returns.
        // If the push landed after that reset, drop it. A newer stream's own
        // speaking reset already cleared this packet, so leave that buffer.
        bool drop = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool stale = chunk.id != id_ || !accepting_ || finished_;
            drop = played && stale && !active_;
        }
        if (drop) {
            audio.ResetDecoder();
            return false;
        }
        return played;
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Chunk> queue_;
    body_downlink::Params params_;
    std::size_t queued_bytes_ = 0;
    uint32_t next_id_ = 0;
    uint32_t id_ = 0;
    uint32_t result_id_ = 0;
    body_downlink::StartStatus status_ = body_downlink::StartStatus::kBusy;
    const char* error_ = "busy";
    bool waiting_ = false;
    bool armed_ = false;
    bool cancelled_ = false;
    bool done_ = false;
    bool active_ = false;
    bool accepting_ = false;
    bool producer_done_ = false;
    bool finished_ = false;
    bool worker_busy_ = false;
    bool worker_running_ = false;
};

}  // namespace

namespace body_downlink {

StartResult RequestStart(Params params) {
    return Session::GetInstance().RequestStart(std::move(params));
}

bool PrepareBegin() { return Session::GetInstance().PrepareBegin(); }

void CompleteStart(StartStatus status) { Session::GetInstance().CompleteStart(status); }

bool PushOpus(uint32_t id, const uint8_t* data, std::size_t size, int duration_ms) {
    return Session::GetInstance().PushOpus(id, data, size, duration_ms);
}

bool PushPcm(uint32_t id, const uint8_t* data, std::size_t size) {
    return Session::GetInstance().PushPcm(id, data, size);
}

void MarkEnded(uint32_t id) { Session::GetInstance().MarkEnded(id); }

bool Cancel() { return Session::GetInstance().Cancel(); }

bool IsActive() { return Session::GetInstance().IsActive(); }

void OnSpeakingEntered() { Session::GetInstance().OnSpeakingEntered(); }

void OnPlaybackDrained() { Session::GetInstance().OnPlaybackDrained(); }

}  // namespace body_downlink
