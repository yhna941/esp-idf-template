#pragma once

#include "domain/types.hpp"
#include "networking/connections.hpp"

namespace networking {

// How long step() waits before repeating a failed start.
// Link loss after a successful start uses WifiStation's own backoff instead.
// waiting_for_time also uses this as the length of one SNTP session.
inline constexpr domain::Milliseconds network_init_retry_ms = 5000;

// SNTP sessions waiting_for_time may start before the bring-up fails like a
// Wi-Fi start. The first expiry stops SNTP and begins again. The next expiry
// releases the stack and enters failed.
inline constexpr int network_time_session_budget = 2;

// disabled has no configuration. stopped keeps the owned copy and does not
// retry. failed keeps that copy, has released resources, and step() retries
// after network_init_retry_ms. A stalled wall clock reaches failed the same
// way. Link loss while running is not failed: the station and the MQTT client
// reconnect on their own.
enum class NetworkState {
    disabled,
    starting,
    waiting_for_wifi,
    waiting_for_time,
    waiting_for_mqtt,
    running,
    stopped,
    failed
};

// The composition root owns this object and the injected adapters for the boot.
// It does not destroy them on stop. The network worker is the only caller of
// configure, start, stop, step, and state. configure runs once, on the startup
// thread, before that worker starts. stop() does not read sensors or change
// outputs; control keeps running on its own task.
class NetworkService final : public application::Work, public application::TelemetrySink {
    public:
    NetworkService(WifiConnection& wifi, MqttConnection& mqtt, NetworkTime& time,
                   application::Clock& clock)
        : wifi_(wifi), mqtt_(mqtt), time_(time), clock_(clock) {
    }

    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;
    // Ignored unless the service is disabled. Restart reuses the owned copy.
    void configure(const configuration::NetworkConfig& config) noexcept;
    // From stopped or failed, ask the next step() to bring the stack up again.
    void start() noexcept;
    // Synchronous release in connection_stop_order. Repeated calls are safe.
    void stop() noexcept;
    void step() noexcept override;
    void report(const domain::Telemetry& status) noexcept override;

    // Network-worker inspection only.
    NetworkState state() const noexcept {
        return state_;
    }

    private:
    void release() noexcept;
    void fail_bringup() noexcept;
    void clear_time_wait() noexcept;
    // Bounds waiting_for_time. Returns after arming, restarting SNTP, or failing.
    void poll_time() noexcept;

    WifiConnection& wifi_;
    MqttConnection& mqtt_;
    NetworkTime& time_;
    application::Clock& clock_;
    configuration::NetworkConfig config_{};
    NetworkState state_{NetworkState::disabled};
    bool needs_time_{false};
    domain::Milliseconds retry_not_before_{0};
    bool time_wait_armed_{false};
    int time_sessions_completed_{0};
    domain::Milliseconds time_deadline_{0};
};

} // namespace networking
