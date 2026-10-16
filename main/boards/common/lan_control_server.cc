#include "lan_control_server.h"

#include "application.h"
#include "body_bridge.h"
#include "body_downlink.h"
#include "cjson_utils.h"
#include "device_state_machine.h"
#include "lan_control_util.h"
#include "settings.h"

#include <esp_log.h>
#include <cJSON.h>
#include <wifi_manager.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#if !defined(CONFIG_HTTPD_WS_SUPPORT) || !defined(CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT)
#error "CONFIG_LAN_CONTROL_API requires HTTPD_WS_SUPPORT and HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT"
#endif

namespace {

constexpr const char* TAG = "LanControl";
// Persistent NVS API. Renaming either value needs a migration.
constexpr const char* kNvsNamespace = "lan_api";
constexpr const char* kNvsTokenKey = "token";
constexpr uint16_t kHttpdControlPort = 32770;
constexpr int kMaxHeaderLen = 300;
constexpr int kMaxContentTypeLen = 80;
constexpr int kMaxRecvTimeouts = 3;

enum class AuthResult { kOk, kUnauthorized, kNotConfigured };

enum class BodyError { kOk, kEmpty, kTooLarge, kIncomplete, kInvalidJson };

struct NotifyRequest {
    std::string audio_url;
    std::vector<NotifySubtitle> subtitles;
};

struct AlertRequest {
    std::string status;
    std::string message;
    std::string emotion;
};

std::string LanToken() {
    Settings settings(kNvsNamespace, false);
    std::string stored = settings.GetString(kNvsTokenKey);
    if (stored.size() > lan_control::kMaxTokenLen) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "Ignoring NVS %s/%s longer than %u characters", kNvsNamespace,
                     kNvsTokenKey, static_cast<unsigned>(lan_control::kMaxTokenLen));
        }
        stored.clear();
    }
    if (!stored.empty()) {
        return stored;
    }

    std::string compiled = CONFIG_LAN_CONTROL_API_TOKEN;
    if (compiled.size() > lan_control::kMaxTokenLen) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "Ignoring compiled LAN token longer than %u characters",
                     static_cast<unsigned>(lan_control::kMaxTokenLen));
        }
        return {};
    }
    return compiled;
}

bool ReadHeader(httpd_req_t* req, const char* name, std::string& value, size_t max_len) {
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0 || len > max_len) {
        value.clear();
        return false;
    }
    value.assign(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, value.data(), value.size()) != ESP_OK) {
        value.clear();
        return false;
    }
    value.resize(std::strlen(value.c_str()));
    return true;
}

AuthResult CheckAuth(httpd_req_t* req) {
    const std::string expected = LanToken();
    if (expected.empty()) {
        return AuthResult::kNotConfigured;
    }
    std::string header;
    if (!ReadHeader(req, "Authorization", header, kMaxHeaderLen)) {
        return AuthResult::kUnauthorized;
    }
    std::string_view token;
    if (!lan_control::BearerToken(header, token) || !lan_control::TokenEquals(token, expected)) {
        return AuthResult::kUnauthorized;
    }
    return AuthResult::kOk;
}

esp_err_t SendRaw(httpd_req_t* req, const char* status, const char* json) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

esp_err_t SendJson(httpd_req_t* req, const char* status, cJSON* root) {
    if (root == nullptr) {
        return SendRaw(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"internal\"}");
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root));
    if (!printed) {
        return SendRaw(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"internal\"}");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, printed.get(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* error) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return SendJson(req, status, nullptr);
    }
    cJSON_AddBoolToObject(root.get(), "ok", false);
    cJSON_AddStringToObject(root.get(), "error", error);
    return SendJson(req, status, root.get());
}

bool RejectUnlessAuthorized(httpd_req_t* req, esp_err_t& result) {
    switch (CheckAuth(req)) {
        case AuthResult::kOk:
            return true;
        case AuthResult::kNotConfigured:
            result = SendError(req, "503 Service Unavailable", "token_not_configured");
            return false;
        case AuthResult::kUnauthorized:
            ESP_LOGW(TAG, "Rejected %s", req->uri);
            result = SendError(req, "401 Unauthorized", "unauthorized");
            return false;
    }
    result = SendError(req, "401 Unauthorized", "unauthorized");
    return false;
}

BodyError ReadJson(httpd_req_t* req, CJsonUniquePtr& root) {
    if (req->content_len > lan_control::kMaxBodyBytes) {
        return BodyError::kTooLarge;
    }
    if (req->content_len == 0) {
        return BodyError::kEmpty;
    }

    std::string body(req->content_len, '\0');
    size_t received = 0;
    int timeouts = 0;
    while (received < body.size()) {
        const int n = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > kMaxRecvTimeouts) {
                return BodyError::kIncomplete;
            }
            continue;
        }
        if (n <= 0) {
            return BodyError::kIncomplete;
        }
        received += static_cast<size_t>(n);
    }

    body.push_back('\0');
    root.reset(cJSON_Parse(body.c_str()));
    if (!root || !cJSON_IsObject(root.get())) {
        return BodyError::kInvalidJson;
    }
    return BodyError::kOk;
}

bool RequireJson(httpd_req_t* req, CJsonUniquePtr& root, esp_err_t& result) {
    if (!RejectUnlessAuthorized(req, result)) {
        return false;
    }
    std::string content_type;
    if (!ReadHeader(req, "Content-Type", content_type, kMaxContentTypeLen) ||
        !lan_control::IsJsonContentType(content_type)) {
        result = SendError(req, "415 Unsupported Media Type", "unsupported_media_type");
        return false;
    }

    const BodyError error = ReadJson(req, root);
    if (error == BodyError::kOk) {
        return true;
    }
    if (error == BodyError::kTooLarge) {
        result = SendError(req, "413 Payload Too Large", "payload_too_large");
    } else {
        result = SendError(req, "400 Bad Request", "invalid_json");
    }
    return false;
}

bool CopyJsonString(const cJSON* item, std::string& out, size_t max_len, bool allow_empty) {
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return false;
    }
    const size_t len = std::strlen(item->valuestring);
    if (len > max_len || (!allow_empty && len == 0)) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(item->valuestring[i]);
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    out.assign(item->valuestring, len);
    return true;
}

bool ParseNotify(cJSON* root, NotifyRequest& out, const char*& error) {
    if (!CopyJsonString(cJSON_GetObjectItem(root, "audio_url"), out.audio_url,
                        lan_control::kMaxAudioUrlLen, false) ||
        !lan_control::IsAudioUrl(out.audio_url)) {
        error = "invalid_audio_url";
        return false;
    }

    cJSON* subtitles = cJSON_GetObjectItem(root, "subtitles");
    if (subtitles == nullptr) {
        return true;
    }
    if (!cJSON_IsArray(subtitles) ||
        cJSON_GetArraySize(subtitles) > static_cast<int>(lan_control::kMaxSubtitles)) {
        error = "invalid_subtitles";
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach (item, subtitles) {
        if (!cJSON_IsObject(item)) {
            error = "invalid_subtitles";
            return false;
        }
        cJSON* start_ms = cJSON_GetObjectItem(item, "start_ms");
        cJSON* text = cJSON_GetObjectItem(item, "text");
        if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
            start_ms->valuedouble > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
            error = "invalid_subtitles";
            return false;
        }
        std::string subtitle;
        if (!CopyJsonString(text, subtitle, lan_control::kMaxSubtitleText, true)) {
            error = "invalid_subtitles";
            return false;
        }
        out.subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                 .text = std::move(subtitle)});
    }
    return true;
}

bool ParseAlert(cJSON* root, AlertRequest& out, const char*& error) {
    if (!CopyJsonString(cJSON_GetObjectItem(root, "status"), out.status, lan_control::kMaxStatusLen,
                        false) ||
        !CopyJsonString(cJSON_GetObjectItem(root, "message"), out.message,
                        lan_control::kMaxMessageLen, false) ||
        !CopyJsonString(cJSON_GetObjectItem(root, "emotion"), out.emotion,
                        lan_control::kMaxEmotionLen, false)) {
        error = "invalid_alert";
        return false;
    }
    return true;
}

bool ParseServo(cJSON* root, int& pan, int& tilt) {
    cJSON* pan_item = cJSON_GetObjectItem(root, "pan");
    cJSON* tilt_item = cJSON_GetObjectItem(root, "tilt");
    if (!cJSON_IsNumber(pan_item) || !cJSON_IsNumber(tilt_item)) {
        return false;
    }
    return lan_control::IsWholeAngle(pan_item->valuedouble, pan) &&
           lan_control::IsWholeAngle(tilt_item->valuedouble, tilt);
}

esp_err_t SendBusy(httpd_req_t* req, DeviceState state) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return SendJson(req, "409 Conflict", nullptr);
    }
    cJSON_AddBoolToObject(root.get(), "ok", false);
    cJSON_AddStringToObject(root.get(), "error", "busy");
    cJSON_AddStringToObject(root.get(), "state", DeviceStateMachine::GetStateName(state));
    return SendJson(req, "409 Conflict", root.get());
}

esp_err_t HealthHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    if (!RejectUnlessAuthorized(req, result)) {
        return result;
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return SendJson(req, "500 Internal Server Error", nullptr);
    }
    const DeviceState state = Application::GetInstance().GetDeviceState();
    const std::string ip = WifiManager::GetInstance().GetIpAddress();
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddStringToObject(root.get(), "state", DeviceStateMachine::GetStateName(state));
    cJSON_AddStringToObject(root.get(), "ip", ip.c_str());
    cJSON_AddNumberToObject(root.get(), "port", CONFIG_LAN_CONTROL_API_PORT);
    cJSON_AddBoolToObject(root.get(), "body_mode", body_bridge::Enabled());
    cJSON_AddStringToObject(root.get(), "bridge",
                            body_bridge::BridgeConfigured() ? "configured" : "missing");
    cJSON_AddBoolToObject(root.get(), "conversation",
                          Application::GetInstance().IsBodyConversationOpen());
    return SendJson(req, "200 OK", root.get());
}

esp_err_t SendModeStatus(httpd_req_t* req) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return SendJson(req, "500 Internal Server Error", nullptr);
    }
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddBoolToObject(root.get(), "body_mode", body_bridge::Enabled());
    cJSON_AddStringToObject(root.get(), "bridge",
                            body_bridge::BridgeConfigured() ? "configured" : "missing");
    return SendJson(req, "200 OK", root.get());
}

esp_err_t SendBridgeStatus(httpd_req_t* req) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (!root) {
        return SendJson(req, "500 Internal Server Error", nullptr);
    }
    const std::string url = body_bridge::BridgeUrl();
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddStringToObject(root.get(), "bridge", url.empty() ? "missing" : "configured");
    cJSON_AddStringToObject(root.get(), "url", url.c_str());
    cJSON_AddBoolToObject(root.get(), "token_set", body_bridge::TokenConfigured());
    return SendJson(req, "200 OK", root.get());
}

esp_err_t ModeGetHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    if (!RejectUnlessAuthorized(req, result)) {
        return result;
    }
    return SendModeStatus(req);
}

esp_err_t ModePostHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    CJsonUniquePtr root;
    if (!RequireJson(req, root, result)) {
        return result;
    }
    cJSON* body = cJSON_GetObjectItem(root.get(), "body");
    if (!cJSON_IsBool(body)) {
        return SendError(req, "400 Bad Request", "invalid_mode");
    }
    const bool enable = cJSON_IsTrue(body);
    const bool capturing = body_bridge::Session::GetInstance().IsCapturing();
    const bool conversation = Application::GetInstance().IsBodyConversationOpen();
    const DeviceState state = Application::GetInstance().GetDeviceState();
    body_bridge::SetEnabled(enable);
    if (!enable &&
        (capturing || conversation || body_downlink::IsActive() || state == kDeviceStateListening ||
         state == kDeviceStateSpeaking || state == kDeviceStateNotifying)) {
        Application::GetInstance().Schedule([]() { Application::GetInstance().StopListening(); });
    }
    return SendModeStatus(req);
}

esp_err_t BridgeGetHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    if (!RejectUnlessAuthorized(req, result)) {
        return result;
    }
    return SendBridgeStatus(req);
}

esp_err_t BridgePostHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    CJsonUniquePtr root;
    if (!RequireJson(req, root, result)) {
        return result;
    }
    cJSON* url_item = cJSON_GetObjectItem(root.get(), "url");
    cJSON* token_item = cJSON_GetObjectItem(root.get(), "token");
    if (url_item == nullptr && token_item == nullptr) {
        return SendError(req, "400 Bad Request", "invalid_bridge");
    }

    std::string url = body_bridge::BridgeUrl();
    if (url_item != nullptr) {
        if (!CopyJsonString(url_item, url, lan_control::kMaxBridgeUrlLen, true) ||
            (!url.empty() && !lan_control::IsBridgeUrl(url))) {
            return SendError(req, "400 Bad Request", "invalid_bridge");
        }
    }

    const std::string* token_ptr = nullptr;
    std::string token;
    if (token_item != nullptr) {
        if (!CopyJsonString(token_item, token, lan_control::kMaxTokenLen, true) ||
            !lan_control::IsBridgeToken(token)) {
            return SendError(req, "400 Bad Request", "invalid_bridge");
        }
        token_ptr = &token;
    }

    if (!body_bridge::SetBridge(url, token_ptr)) {
        return SendError(req, "400 Bad Request", "invalid_bridge");
    }
    return SendBridgeStatus(req);
}

esp_err_t NotifyHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    CJsonUniquePtr root;
    if (!RequireJson(req, root, result)) {
        return result;
    }

    NotifyRequest notify;
    const char* error = "invalid_json";
    if (!ParseNotify(root.get(), notify, error)) {
        return SendError(req, "400 Bad Request", error);
    }

    const DeviceState state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle) {
        return SendBusy(req, state);
    }

    Application::GetInstance().EnqueueNotification(std::move(notify.audio_url),
                                                   std::move(notify.subtitles));
    ESP_LOGI(TAG, "Queued notification");

    CJsonUniquePtr response(cJSON_CreateObject());
    if (!response) {
        return SendRaw(req, "202 Accepted", "{\"ok\":true,\"queued\":true}");
    }
    cJSON_AddBoolToObject(response.get(), "ok", true);
    cJSON_AddBoolToObject(response.get(), "queued", true);
    return SendJson(req, "202 Accepted", response.get());
}

esp_err_t AlertHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    CJsonUniquePtr root;
    if (!RequireJson(req, root, result)) {
        return result;
    }

    AlertRequest alert;
    const char* error = "invalid_alert";
    if (!ParseAlert(root.get(), alert, error)) {
        return SendError(req, "400 Bad Request", error);
    }

    const std::string emotion = alert.emotion;
    Application::GetInstance().Schedule(
        [status = std::move(alert.status), message = std::move(alert.message), emotion]() {
            Application::GetInstance().Alert(status.c_str(), message.c_str(), emotion.c_str());
        });
    ESP_LOGI(TAG, "Queued alert emotion=%s", emotion.c_str());

    CJsonUniquePtr response(cJSON_CreateObject());
    if (!response) {
        return SendJson(req, "200 OK", nullptr);
    }
    cJSON_AddBoolToObject(response.get(), "ok", true);
    return SendJson(req, "200 OK", response.get());
}

esp_err_t ServoHandler(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    CJsonUniquePtr root;
    if (!RequireJson(req, root, result)) {
        return result;
    }

    int pan = 0;
    int tilt = 0;
    if (!ParseServo(root.get(), pan, tilt)) {
        return SendError(req, "400 Bad Request", "invalid_servo");
    }

    // Stub: share this port later. Do not claim GPIO8/GPIO9 here; many boards
    // already use those pins for audio or the display.
    ESP_LOGI(TAG, "Servo stub pan=%d tilt=%d (GPIO %d/%d not driven)", pan, tilt,
             lan_control::kServoPanGpio, lan_control::kServoTiltGpio);

    CJsonUniquePtr response(cJSON_CreateObject());
    if (!response) {
        return SendJson(req, "200 OK", nullptr);
    }
    cJSON_AddBoolToObject(response.get(), "ok", true);
    cJSON_AddBoolToObject(response.get(), "stub", true);
    cJSON_AddNumberToObject(response.get(), "pan", pan);
    cJSON_AddNumberToObject(response.get(), "tilt", tilt);
    cJSON_AddNumberToObject(response.get(), "pan_gpio", lan_control::kServoPanGpio);
    cJSON_AddNumberToObject(response.get(), "tilt_gpio", lan_control::kServoTiltGpio);
    return SendJson(req, "200 OK", response.get());
}

struct SpeakConn {
    int fd = -1;
    uint32_t id = 0;
    bool started = false;
    bool pcm = false;
    bool length_prefixed = false;
};

SpeakConn g_speak;

void ClearSpeakConn() { g_speak = {}; }

esp_err_t SendWsText(httpd_req_t* req, const char* json) {
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
    frame.len = std::strlen(json);
    return httpd_ws_send_frame(req, &frame) == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t SendWsJson(httpd_req_t* req, cJSON* root) {
    if (root == nullptr) {
        return SendWsText(req, "{\"ok\":false,\"error\":\"internal\"}");
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root));
    if (!printed) {
        return SendWsText(req, "{\"ok\":false,\"error\":\"internal\"}");
    }
    return SendWsText(req, printed.get());
}

esp_err_t SpeakFail(httpd_req_t* req, const char* error, const char* state) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root) {
        cJSON_AddBoolToObject(root.get(), "ok", false);
        cJSON_AddStringToObject(root.get(), "error", error != nullptr ? error : "invalid_speak");
        if (state != nullptr && state[0] != '\0') {
            cJSON_AddStringToObject(root.get(), "state", state);
        }
        SendWsJson(req, root.get());
    }
    httpd_ws_frame_t close_frame = {};
    close_frame.type = HTTPD_WS_TYPE_CLOSE;
    httpd_ws_send_frame(req, &close_frame);
    return ESP_OK;
}

void AbortSpeakConn() {
    ClearSpeakConn();
    Application::GetInstance().Schedule([]() { Application::GetInstance().AbortBodySpeak(); });
}

#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
esp_err_t SpeakPreHandshake(httpd_req_t* req) {
    esp_err_t result = ESP_OK;
    if (!RejectUnlessAuthorized(req, result)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
#endif

bool PushSpeakBytes(const uint8_t* data, size_t size) {
    if (!g_speak.started || data == nullptr) {
        return false;
    }
    if (g_speak.pcm) {
        return body_downlink::PushPcm(g_speak.id, data, size);
    }
    if (g_speak.length_prefixed) {
        const uint8_t* cursor = data;
        size_t left = size;
        if (left == 0) {
            return false;
        }
        while (left > 0) {
            body_downlink::OpusView frame;
            if (!body_downlink::NextLengthPrefixedOpus(cursor, left, frame)) {
                return false;
            }
            if (!body_downlink::PushOpus(g_speak.id, frame.data, frame.size, frame.duration_ms)) {
                return false;
            }
        }
        return true;
    }
    int duration_ms = 0;
    if (!body_downlink::OpusPacketDurationMs(data, size, duration_ms)) {
        return false;
    }
    return body_downlink::PushOpus(g_speak.id, data, size, duration_ms);
}

esp_err_t SpeakHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        esp_err_t result = ESP_OK;
        if (!RejectUnlessAuthorized(req, result)) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws = {};
    esp_err_t ret = httpd_ws_recv_frame(req, &ws, 0);
    const int fd = httpd_req_to_sockfd(req);
    if (ret != ESP_OK) {
        if (g_speak.started && fd == g_speak.fd) {
            const uint32_t id = g_speak.id;
            ClearSpeakConn();
            body_downlink::MarkEnded(id);
        }
        return ret;
    }
    if (!ws.final || ws.len > body_downlink::kMaxBinaryFrame) {
        if (g_speak.started && fd == g_speak.fd) {
            AbortSpeakConn();
        }
        return SpeakFail(req, "invalid_speak", nullptr);
    }

    std::vector<uint8_t> payload(ws.len);
    if (ws.len > 0) {
        ws.payload = payload.data();
        ret = httpd_ws_recv_frame(req, &ws, ws.len);
        if (ret != ESP_OK) {
            if (g_speak.started && fd == g_speak.fd) {
                const uint32_t id = g_speak.id;
                ClearSpeakConn();
                body_downlink::MarkEnded(id);
            }
            return ret;
        }
    }

    if (ws.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong = {};
        pong.type = HTTPD_WS_TYPE_PONG;
        pong.payload = payload.empty() ? nullptr : payload.data();
        pong.len = ws.len;
        return httpd_ws_send_frame(req, &pong);
    }
    if (ws.type == HTTPD_WS_TYPE_PONG) {
        return ESP_OK;
    }
    if (ws.type == HTTPD_WS_TYPE_CLOSE) {
        if (fd == g_speak.fd && g_speak.started) {
            const uint32_t id = g_speak.id;
            ClearSpeakConn();
            body_downlink::MarkEnded(id);
        } else if (fd == g_speak.fd) {
            AbortSpeakConn();
        }
        return ESP_OK;
    }

    if (ws.type == HTTPD_WS_TYPE_TEXT) {
        const std::string text(
            payload.empty()
                ? std::string()
                : std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        body_downlink::Control control = body_downlink::Control::kStart;
        body_downlink::Params params;
        const char* error = "invalid_speak";
        if (!body_downlink::ParseSpeakJson(text, control, params, error)) {
            if (g_speak.started && fd == g_speak.fd) {
                AbortSpeakConn();
            }
            return SpeakFail(req, "invalid_speak", nullptr);
        }
        if (control == body_downlink::Control::kEnd) {
            if (!g_speak.started || fd != g_speak.fd) {
                return SpeakFail(req, "invalid_speak", nullptr);
            }
            const uint32_t id = g_speak.id;
            ClearSpeakConn();
            body_downlink::MarkEnded(id);
            CJsonUniquePtr root(cJSON_CreateObject());
            if (root) {
                cJSON_AddBoolToObject(root.get(), "ok", true);
                cJSON_AddStringToObject(root.get(), "event", "ended");
            }
            return SendWsJson(req, root.get());
        }
        if (control == body_downlink::Control::kAbort) {
            if (g_speak.started && fd == g_speak.fd) {
                AbortSpeakConn();
            }
            CJsonUniquePtr root(cJSON_CreateObject());
            if (root) {
                cJSON_AddBoolToObject(root.get(), "ok", true);
                cJSON_AddStringToObject(root.get(), "event", "aborted");
            }
            return SendWsJson(req, root.get());
        }

        if (g_speak.started) {
            const char* state =
                DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
            return SpeakFail(req, "busy", state);
        }
        const bool pcm = params.codec == body_downlink::Codec::kPcm;
        const bool length_prefixed = params.framing == body_downlink::Framing::kLengthPrefixed;
        const body_downlink::StartResult started = body_downlink::RequestStart(std::move(params));
        if (started.status != body_downlink::StartStatus::kOk) {
            const char* state = nullptr;
            if (started.status == body_downlink::StartStatus::kBusy) {
                state =
                    DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
            }
            return SpeakFail(req, started.error != nullptr ? started.error : "busy", state);
        }
        g_speak.fd = fd;
        g_speak.id = started.id;
        g_speak.started = true;
        g_speak.pcm = pcm;
        g_speak.length_prefixed = length_prefixed;
        CJsonUniquePtr root(cJSON_CreateObject());
        if (!root) {
            AbortSpeakConn();
            return SpeakFail(req, "internal", nullptr);
        }
        cJSON_AddBoolToObject(root.get(), "ok", true);
        cJSON_AddStringToObject(root.get(), "event", "playing");
        cJSON_AddNumberToObject(root.get(), "playback_id", started.id);
        ESP_LOGI(TAG, "Speak stream %lu accepted", static_cast<unsigned long>(started.id));
        return SendWsJson(req, root.get());
    }

    if (ws.type != HTTPD_WS_TYPE_BINARY) {
        return SpeakFail(req, "invalid_speak", nullptr);
    }
    if (!g_speak.started || fd != g_speak.fd) {
        if (!g_speak.started) {
            return SpeakFail(req, "invalid_speak", nullptr);
        }
        const char* state =
            DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState());
        return SpeakFail(req, "busy", state);
    }
    if (!PushSpeakBytes(payload.empty() ? nullptr : payload.data(), payload.size())) {
        ESP_LOGW(TAG, "Speak stream %lu rejected a frame", static_cast<unsigned long>(g_speak.id));
        AbortSpeakConn();
        return SpeakFail(req, "invalid_speak", nullptr);
    }
    return ESP_OK;
}

bool RegisterRoute(httpd_handle_t server, const char* uri, httpd_method_t method,
                   esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t route = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = nullptr,
    };
    return httpd_register_uri_handler(server, &route) == ESP_OK;
}

}  // namespace

LanControlServer& LanControlServer::GetInstance() {
    static LanControlServer instance;
    return instance;
}

void LanControlServer::Start() { Post(Command::kStart); }

void LanControlServer::Stop() { Post(Command::kStop); }

void LanControlServer::Post(Command command) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = command;
    if (worker_running_) {
        return;
    }
    worker_running_ = true;
    if (xTaskCreate(WorkerEntry, "lan_http", 8192, this, 2, nullptr) != pdPASS) {
        worker_running_ = false;
        pending_ = Command::kNone;
        ESP_LOGE(TAG, "Failed to create LAN control task");
    }
}

void LanControlServer::WorkerEntry(void* arg) {
    static_cast<LanControlServer*>(arg)->Worker();
    vTaskDelete(nullptr);
}

void LanControlServer::Worker() {
    for (;;) {
        Command command = Command::kNone;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            command = pending_;
            pending_ = Command::kNone;
            if (command == Command::kNone) {
                worker_running_ = false;
                return;
            }
        }
        if (command == Command::kStart) {
            Listen();
        } else if (command == Command::kStop) {
            Shutdown();
        }
    }
}

void LanControlServer::Listen() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_ != nullptr) {
            return;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_LAN_CONTROL_API_PORT;
    config.ctrl_port = CONFIG_LAN_CONTROL_API_PORT == kHttpdControlPort ? 32771 : kHttpdControlPort;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.max_uri_handlers = 12;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to listen on port %d", CONFIG_LAN_CONTROL_API_PORT);
        return;
    }

    httpd_uri_t speak = {};
    speak.uri = "/api/speak";
    speak.method = HTTP_GET;
    speak.handler = SpeakHandler;
    speak.user_ctx = nullptr;
#if CONFIG_HTTPD_WS_SUPPORT
    speak.is_websocket = true;
    speak.handle_ws_control_frames = true;
#endif
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
    speak.ws_pre_handshake_cb = SpeakPreHandshake;
#endif

    const bool registered = RegisterRoute(server, "/api/health", HTTP_GET, HealthHandler) &&
                            RegisterRoute(server, "/api/notify", HTTP_POST, NotifyHandler) &&
                            RegisterRoute(server, "/api/alert", HTTP_POST, AlertHandler) &&
                            RegisterRoute(server, "/api/servo", HTTP_POST, ServoHandler) &&
                            RegisterRoute(server, "/api/mode", HTTP_GET, ModeGetHandler) &&
                            RegisterRoute(server, "/api/mode", HTTP_POST, ModePostHandler) &&
                            RegisterRoute(server, "/api/bridge", HTTP_GET, BridgeGetHandler) &&
                            RegisterRoute(server, "/api/bridge", HTTP_POST, BridgePostHandler) &&
                            httpd_register_uri_handler(server, &speak) == ESP_OK;
    if (!registered) {
        httpd_stop(server);
        ESP_LOGE(TAG, "Failed to register LAN control routes");
        return;
    }

    bool stopped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ == Command::kStop) {
            stopped = true;
        } else {
            server_ = server;
        }
    }
    if (stopped) {
        httpd_stop(server);
        return;
    }

    const std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        ESP_LOGI(TAG, "LAN control API listening on port %d", CONFIG_LAN_CONTROL_API_PORT);
    } else {
        ESP_LOGI(TAG, "LAN control API at http://%s:%d", ip.c_str(), CONFIG_LAN_CONTROL_API_PORT);
    }
    ESP_LOGI(TAG, "LAN only, no TLS; Authorization: Bearer <token> is required");
    if (LanToken().empty()) {
        ESP_LOGW(TAG, "No LAN token yet; set Kconfig or NVS %s/%s", kNvsNamespace, kNvsTokenKey);
    }
}

void LanControlServer::Shutdown() {
    httpd_handle_t server = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server = server_;
        server_ = nullptr;
    }
    if (server != nullptr) {
        httpd_stop(server);
        ESP_LOGI(TAG, "LAN control API stopped");
    }
}
