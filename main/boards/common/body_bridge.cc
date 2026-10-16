#include "body_bridge.h"

#include "application.h"
#include "board.h"
#include "body_uplink_format.h"
#include "cjson_utils.h"
#include "display.h"
#include "lan_control_util.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <wifi_manager.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* TAG = "BodyBridge";
// Persistent NVS API, same namespace as the LAN bearer token.
constexpr const char* kNvsNamespace = "lan_api";
constexpr const char* kBodyModeKey = "body_mode";
constexpr const char* kBridgeUrlKey = "bot_bridge_url";
constexpr const char* kBridgeTokenKey = "bridge_token";
constexpr int kHttpTimeoutMs = 8000;
constexpr uint32_t kUploadStackBytes = 8192;
constexpr uint64_t kMaxListenUs = 8ULL * 1000ULL * 1000ULL;
constexpr size_t kMaxQueuedJobs = 8;
constexpr size_t kPrerollFrames = 8;

static_assert(sizeof("body_mode") - 1 <= 15, "NVS key too long");
static_assert(sizeof("bot_bridge_url") - 1 <= 15, "NVS key too long");
static_assert(sizeof("bridge_token") - 1 <= 15, "NVS key too long");

// 0 = not loaded, 1 = off, 2 = on.
std::atomic<int> g_body_mode{0};

std::string ReadSetting(const char* key, size_t max_len) {
    Settings settings(kNvsNamespace, false);
    std::string value = settings.GetString(key);
    if (value.size() > max_len) {
        return {};
    }
    return value;
}

bool ReadBodyMode() {
#if CONFIG_BODY_MODE_DEFAULT
    constexpr bool kFallback = true;
#else
    constexpr bool kFallback = false;
#endif
    Settings settings(kNvsNamespace, false);
    return settings.GetBool(kBodyModeKey, kFallback);
}

std::string BridgeToken() {
    std::string token = ReadSetting(kBridgeTokenKey, lan_control::kMaxTokenLen);
    if (!lan_control::IsBridgeToken(token)) {
        return {};
    }
    return token;
}

std::string Endpoint(const std::string& base, const char* path) {
    if (!base.empty() && base.back() == '/') {
        return base.substr(0, base.size() - 1) + path;
    }
    return base + path;
}

void StopTimer(esp_timer_handle_t timer) {
    if (timer != nullptr) {
        (void)esp_timer_stop(timer);
    }
}

struct Job {
    std::string url;
    std::string token;
    std::string content_type;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    bool notify_failure = false;
    uint32_t clip_id = 0;
};

std::mutex g_jobs_mutex;
std::deque<Job> g_jobs;
bool g_worker_running = false;

void ReportUploadFailure(uint32_t clip_id) {
    Application::GetInstance().Schedule([clip_id]() {
        auto& session = body_bridge::Session::GetInstance();
        if (session.ClipId() != clip_id || session.IsCapturing()) {
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            return;
        }
        auto* display = Board::GetInstance().GetDisplay();
        display->SetStatus("Bridge");
        display->SetEmotion("sad");
        display->SetChatMessage("system", "Bridge upload failed");
    });
}

bool PostJob(const Job& job) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "No HTTP client for bridge upload");
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    std::string authorization;
    if (!job.token.empty()) {
        authorization = "Bearer " + job.token;
        http->SetHeader("Authorization", authorization.c_str());
    }
    http->SetHeader("Content-Type", job.content_type.c_str());
    const std::string device_id = SystemInfo::GetMacAddress();
    http->SetHeader("Device-Id", device_id.c_str());
    for (const auto& header : job.headers) {
        http->SetHeader(header.first.c_str(), header.second.c_str());
    }
    std::string body(reinterpret_cast<const char*>(job.body.data()), job.body.size());
    http->SetContent(std::move(body));
    if (auto opened = http->Open("POST", job.url); !opened) {
        ESP_LOGW(TAG, "Bridge POST open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    http->Close();
    if (!status) {
        ESP_LOGW(TAG, "Bridge POST status failed: %s", status.error().ToString().c_str());
        return false;
    }
    if (*status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "Bridge POST %s -> %d", job.url.c_str(), *status);
        return false;
    }
    ESP_LOGI(TAG, "Bridge POST %s -> %d (%u bytes)", job.url.c_str(), *status,
             static_cast<unsigned>(job.body.size()));
    return true;
}

void WorkerEntry(void* arg);

void Worker() {
    for (;;) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(g_jobs_mutex);
            if (g_jobs.empty()) {
                g_worker_running = false;
                return;
            }
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        const bool ok = PostJob(job);
        if (!ok && job.notify_failure) {
            ReportUploadFailure(job.clip_id);
        }
    }
}

void WorkerEntry(void* arg) {
    (void)arg;
    Worker();
    vTaskDelete(nullptr);
}

bool Enqueue(Job job) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    if (g_jobs.size() >= kMaxQueuedJobs) {
        ESP_LOGW(TAG, "Dropping oldest body uplink job");
        g_jobs.pop_front();
    }
    g_jobs.push_back(std::move(job));
    if (g_worker_running) {
        return true;
    }
    g_worker_running = true;
    if (xTaskCreate(WorkerEntry, "body_uplink", kUploadStackBytes, nullptr, 2, nullptr) != pdPASS) {
        g_worker_running = false;
        g_jobs.clear();
        ESP_LOGE(TAG, "Failed to create body uplink task");
        return false;
    }
    return true;
}

std::string EventJson(const char* event, const char* source, const char* reason, int frames,
                      int bytes, int sample_rate, int frame_ms, bool with_stats) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return {};
    }
    cJSON_AddStringToObject(root.get(), "event", event);
    cJSON_AddStringToObject(root.get(), "source", source != nullptr ? source : "button");
    if (reason != nullptr && reason[0] != '\0') {
        cJSON_AddStringToObject(root.get(), "reason", reason);
    }
    const std::string device_id = SystemInfo::GetMacAddress();
    cJSON_AddStringToObject(root.get(), "device_id", device_id.c_str());
    const std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (!ip.empty()) {
        cJSON_AddStringToObject(root.get(), "ip", ip.c_str());
    }
    if (with_stats) {
        cJSON_AddNumberToObject(root.get(), "frames", frames);
        cJSON_AddNumberToObject(root.get(), "bytes", bytes);
        cJSON_AddNumberToObject(root.get(), "sample_rate", sample_rate);
        cJSON_AddNumberToObject(root.get(), "frame_duration_ms", frame_ms);
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root.get()));
    if (!printed) {
        return {};
    }
    return printed.get();
}

bool QueueBody(const char* path, const char* content_type, std::vector<uint8_t> body,
               std::vector<std::pair<std::string, std::string>> headers, bool notify_failure,
               uint32_t clip_id) {
    const std::string base = body_bridge::BridgeUrl();
    if (base.empty() || path == nullptr) {
        return false;
    }
    Job job;
    job.url = Endpoint(base, path);
    job.token = BridgeToken();
    job.content_type = content_type;
    job.headers = std::move(headers);
    job.body = std::move(body);
    job.notify_failure = notify_failure;
    job.clip_id = clip_id;
    return Enqueue(std::move(job));
}

bool QueueEvent(const char* event, const char* source, const char* reason, int frames, int bytes,
                int sample_rate, int frame_ms, bool with_stats, bool notify_failure,
                uint32_t clip_id) {
    const std::string json =
        EventJson(event, source, reason, frames, bytes, sample_rate, frame_ms, with_stats);
    if (json.empty()) {
        return false;
    }
    std::vector<uint8_t> body(json.begin(), json.end());
    return QueueBody("/body/event", "application/json", std::move(body), {}, notify_failure,
                     clip_id);
}

}  // namespace

namespace body_bridge {

bool Enabled() {
    int state = g_body_mode.load(std::memory_order_acquire);
    if (state == 0) {
        state = ReadBodyMode() ? 2 : 1;
        int expected = 0;
        if (!g_body_mode.compare_exchange_strong(expected, state, std::memory_order_release,
                                                 std::memory_order_acquire)) {
            state = expected;
        }
    }
    return state == 2;
}

void SetEnabled(bool on) {
    Settings settings(kNvsNamespace, true);
    settings.SetBool(kBodyModeKey, on);
    g_body_mode.store(on ? 2 : 1, std::memory_order_release);
    ESP_LOGI(TAG, "Body mode %s", on ? "on" : "off");
}

bool BridgeConfigured() { return !BridgeUrl().empty(); }

bool TokenConfigured() { return !BridgeToken().empty(); }

std::string BridgeUrl() {
    std::string url = ReadSetting(kBridgeUrlKey, lan_control::kMaxBridgeUrlLen);
    if (!url.empty() && !lan_control::IsBridgeUrl(url)) {
        return {};
    }
    return url;
}

bool SetBridge(const std::string& url, const std::string* token) {
    if (!url.empty() && !lan_control::IsBridgeUrl(url)) {
        return false;
    }
    if (token != nullptr && !lan_control::IsBridgeToken(*token)) {
        return false;
    }
    Settings settings(kNvsNamespace, true);
    settings.SetString(kBridgeUrlKey, url);
    if (token != nullptr) {
        settings.SetString(kBridgeTokenKey, *token);
    }
    ESP_LOGI(TAG, "Bridge URL %s", url.empty() ? "cleared" : "saved");
    return true;
}

Session& Session::GetInstance() {
    static Session instance;
    return instance;
}

bool Session::IsCapturing() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capturing_;
}

uint32_t Session::ClipId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clip_id_;
}

void Session::EnsureTimers() {
    if (max_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) { static_cast<Session*>(arg)->Fire("timeout"); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "body_max",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &max_timer_));
    }
    if (silence_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) { static_cast<Session*>(arg)->Fire("silence"); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "body_sil",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &silence_timer_));
    }
    if (prespeech_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) { static_cast<Session*>(arg)->Fire("session"); },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "body_pre",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&args, &prespeech_timer_));
    }
}

void Session::StopTimers() {
    StopTimer(max_timer_);
    StopTimer(silence_timer_);
    StopTimer(prespeech_timer_);
}

void Session::ArmSilenceTimer() {
    EnsureTimers();
    (void)esp_timer_stop(silence_timer_);
    if (esp_timer_start_once(silence_timer_, kPostSpeechSilenceUs) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm body silence timer");
    }
}

void Session::DisarmSilenceTimer() { StopTimer(silence_timer_); }

void Session::ArmPreSpeechTimer(uint64_t us) {
    EnsureTimers();
    (void)esp_timer_stop(prespeech_timer_);
    if (esp_timer_start_once(prespeech_timer_, us) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm body pre-speech timer");
    }
}

void Session::ArmMaxTimer() {
    EnsureTimers();
    (void)esp_timer_stop(max_timer_);
    if (esp_timer_start_once(max_timer_, kMaxListenUs) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm body listen timer");
    }
}

void Session::Fire(const char* reason) {
    StopCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) {
            return;
        }
        // Speech already started; a stale pre-speech timeout must not drop it.
        if (reason != nullptr && std::strcmp(reason, "session") == 0 && heard_voice_) {
            return;
        }
        callback = on_stop_;
    }
    if (callback) {
        callback(reason);
    }
}

bool Session::HeardVoice() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capturing_ && heard_voice_;
}

bool Session::Begin(const char* source, StopCallback on_stop, uint64_t pre_speech_us) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (capturing_) {
            return false;
        }
        capturing_ = true;
        heard_voice_ = false;
        pause_notified_ = false;
        defer_max_ = pre_speech_us > 0;
        source_ = source != nullptr ? source : "button";
        on_stop_ = std::move(on_stop);
        audio_.clear();
        preroll_.clear();
        frames_ = 0;
        sample_rate_ = 16000;
        frame_ms_ = 60;
    }
    EnsureTimers();
    StopTimers();
    if (pre_speech_us > 0) {
        ArmPreSpeechTimer(pre_speech_us);
    } else {
        ArmMaxTimer();
    }
    std::string source_copy;
    uint32_t clip_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_copy = source_;
        clip_id = clip_id_;
    }
    QueueEvent("listen_start", source_copy.c_str(), nullptr, 0, 0, 0, 0, false, false, clip_id);
    return true;
}

void Session::OnVad(bool speaking) {
    bool arm = false;
    bool disarm = false;
    bool notify_pause = false;
    bool start_max = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) {
            return;
        }
        if (speaking) {
            heard_voice_ = true;
            disarm = true;
            if (defer_max_) {
                defer_max_ = false;
                start_max = true;
            }
        } else if (heard_voice_) {
            arm = true;
            if (!pause_notified_) {
                pause_notified_ = true;
                notify_pause = true;
            }
        }
    }
    if (disarm) {
        DisarmSilenceTimer();
        if (start_max) {
            StopTimer(prespeech_timer_);
            ArmMaxTimer();
        }
    } else if (arm) {
        ArmSilenceTimer();
        if (notify_pause) {
            std::string source;
            int frames = 0;
            int bytes = 0;
            int sample_rate = 16000;
            int frame_ms = 60;
            uint32_t clip_id = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                source = source_;
                frames = frames_;
                bytes = static_cast<int>(audio_.size());
                sample_rate = sample_rate_;
                frame_ms = frame_ms_;
                clip_id = clip_id_;
            }
            // Hint only. The clip is still open until the silence timer fires.
            QueueEvent("speech_pause", source.c_str(), "silence", frames, bytes, sample_rate,
                       frame_ms, true, false, clip_id);
        }
    }
}

bool Session::Accept(std::unique_ptr<AudioStreamPacket> packet) {
    if (!packet) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capturing_) {
        return true;
    }
    if (packet->sample_rate > 0) {
        sample_rate_ = packet->sample_rate;
    }
    if (packet->frame_duration > 0) {
        frame_ms_ = packet->frame_duration;
    }
    // A follow-up listen can sit open for the whole conversation window.
    // Keep a short preroll and drop the rest until VAD hears speech, or the
    // 32 KB cap would upload silence and end the turn early.
    if (!heard_voice_ && defer_max_) {
        if (!packet->payload.empty()) {
            preroll_.push_back(packet->payload);
            if (preroll_.size() > kPrerollFrames) {
                preroll_.erase(preroll_.begin());
            }
        }
        return true;
    }
    if (!preroll_.empty()) {
        for (const auto& frame : preroll_) {
            if (!AppendOpusFrame(audio_, frame.data(), frame.size(), kMaxBodyClipBytes)) {
                preroll_.clear();
                return false;
            }
            frames_ += 1;
        }
        preroll_.clear();
    }
    const uint8_t* data = packet->payload.empty() ? nullptr : packet->payload.data();
    if (!AppendOpusFrame(audio_, data, packet->payload.size(), kMaxBodyClipBytes)) {
        return false;
    }
    frames_ += 1;
    return true;
}

ClipResult Session::Finish(const char* reason) {
    std::vector<uint8_t> audio;
    std::string source;
    int frames = 0;
    int sample_rate = 16000;
    int frame_ms = 60;
    uint32_t clip_id = 0;
    bool session_idle = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) {
            return ClipResult::kNotCapturing;
        }
        capturing_ = false;
        on_stop_ = nullptr;
        defer_max_ = false;
        source = source_;
        // Idle follow-up: listen_stop closes listen_start, and there is no
        // utterance for the bridge to answer.
        session_idle = !heard_voice_ && reason != nullptr && std::strcmp(reason, "session") == 0;
        heard_voice_ = false;
        if (session_idle) {
            audio_.clear();
            preroll_.clear();
            frames_ = 0;
        }
        audio = std::move(audio_);
        frames = frames_;
        sample_rate = sample_rate_;
        frame_ms = frame_ms_;
        audio_.clear();
        preroll_.clear();
        frames_ = 0;
        clip_id = ++clip_id_;
    }
    StopTimers();
    const int byte_count = static_cast<int>(audio.size());
    ESP_LOGI(TAG, "Body clip %s frames=%d bytes=%d reason=%s", source.c_str(), frames, byte_count,
             reason != nullptr ? reason : "");
    if (!BridgeConfigured()) {
        ESP_LOGW(TAG, "Body clip kept local; bridge URL is missing");
        return ClipResult::kNoBridge;
    }
    if (session_idle) {
        QueueEvent("listen_stop", source.c_str(), reason, frames, byte_count, sample_rate, frame_ms,
                   true, false, clip_id);
        return ClipResult::kQueued;
    }

    bool queued = false;
    if (!audio.empty()) {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"X-Audio-Codec", "opus"},
            {"X-Sample-Rate", std::to_string(sample_rate)},
            {"X-Channels", "1"},
            {"X-Frame-Duration-Ms", std::to_string(frame_ms)},
            {"X-Frame-Count", std::to_string(frames)},
            {"X-Source", source},
        };
        queued = QueueBody("/body/audio", "application/octet-stream", std::move(audio),
                           std::move(headers), true, clip_id);
    }
    if (QueueEvent("utterance", source.c_str(), reason, frames, byte_count, sample_rate, frame_ms,
                   true, !queued, clip_id)) {
        queued = true;
    }
    QueueEvent("listen_stop", source.c_str(), reason, frames, byte_count, sample_rate, frame_ms,
               true, false, clip_id);
    return queued ? ClipResult::kQueued : ClipResult::kUploadFailed;
}

void Session::Cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capturing_) {
            return;
        }
        capturing_ = false;
        on_stop_ = nullptr;
        heard_voice_ = false;
        defer_max_ = false;
        audio_.clear();
        preroll_.clear();
        frames_ = 0;
    }
    StopTimers();
    ESP_LOGI(TAG, "Body listen cancelled");
}

}  // namespace body_bridge
