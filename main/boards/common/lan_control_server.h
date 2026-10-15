#ifndef LAN_CONTROL_SERVER_H_
#define LAN_CONTROL_SERVER_H_

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

// LAN-only HTTP control server. Started by WifiBoard after STA connects.
// There is no TLS. Every route requires Authorization: Bearer <token>.
class LanControlServer {
public:
    static LanControlServer& GetInstance();

    // Safe to call from the Wi-Fi event callback. Work runs on a short task.
    void Start();
    void Stop();

private:
    enum class Command { kNone, kStart, kStop };

    LanControlServer() = default;

    void Post(Command command);
    static void WorkerEntry(void* arg);
    void Worker();
    void Listen();
    void Shutdown();

    std::mutex mutex_;
    Command pending_ = Command::kNone;
    bool worker_running_ = false;
    httpd_handle_t server_ = nullptr;
};

#endif  // LAN_CONTROL_SERVER_H_
