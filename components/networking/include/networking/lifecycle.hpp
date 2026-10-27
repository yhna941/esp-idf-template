#pragma once

#include "domain/types.hpp"

#include <cstdint>

namespace networking {

// Resources acquired by WifiStation. idle and process_ready are not released:
// the TCP/IP stack and the default event loop outlive one station restart.
enum class WifiPhase : std::uint8_t {
    idle,
    process_ready,
    netif,
    driver,
    handlers,
    started
};

enum class MqttPhase : std::uint8_t {
    idle,
    created,
    started
};

enum class SntpPhase : std::uint8_t {
    idle,
    started
};

// Stop walks this list. Start is not the reverse loop: Wi-Fi and SNTP have to
// become ready before MQTT, so NetworkService orders those waits itself.
enum class ConnectionRole : std::uint8_t {
    wifi,
    time,
    mqtt
};

struct MqttCleanup {
    bool stop_client;
    bool destroy_client;
};

inline constexpr ConnectionRole connection_stop_order[] = {
    ConnectionRole::mqtt,
    ConnectionRole::time,
    ConnectionRole::wifi,
};

constexpr bool service_owned(WifiPhase phase) noexcept {
    return phase == WifiPhase::netif || phase == WifiPhase::driver ||
           phase == WifiPhase::handlers || phase == WifiPhase::started;
}

constexpr bool wifi_phase_advances(WifiPhase current, WifiPhase next) noexcept {
    return (current == WifiPhase::idle && next == WifiPhase::process_ready) ||
           (current == WifiPhase::process_ready && next == WifiPhase::netif) ||
           (current == WifiPhase::netif && next == WifiPhase::driver) ||
           (current == WifiPhase::driver && next == WifiPhase::handlers) ||
           (current == WifiPhase::handlers && next == WifiPhase::started);
}

// One step of a reverse teardown. idle and process_ready stay where they are.
constexpr WifiPhase release_wifi_phase(WifiPhase phase) noexcept {
    switch (phase) {
    case WifiPhase::started:
        return WifiPhase::handlers;
    case WifiPhase::handlers:
        return WifiPhase::driver;
    case WifiPhase::driver:
        return WifiPhase::netif;
    case WifiPhase::netif:
        return WifiPhase::process_ready;
    case WifiPhase::process_ready:
    case WifiPhase::idle:
        return phase;
    }
    return WifiPhase::idle;
}

// A started client must be stopped before destroy. A client that was only
// created must be destroyed without stop: the SDK rejects stop in that state.
constexpr MqttCleanup mqtt_cleanup(MqttPhase phase) noexcept {
    switch (phase) {
    case MqttPhase::started:
        return MqttCleanup{true, true};
    case MqttPhase::created:
        return MqttCleanup{false, true};
    case MqttPhase::idle:
        return MqttCleanup{false, false};
    }
    return MqttCleanup{false, false};
}

constexpr bool sntp_is_started(SntpPhase phase) noexcept {
    return phase == SntpPhase::started;
}

// A failed call leaves its phase. stop has to succeed before destroy is
// attempted; a started client whose stop succeeded but whose destroy failed is
// created, so the next stop does not call stop again.
constexpr MqttPhase mqtt_phase_after_release(MqttPhase phase, bool stop_succeeded,
                                             bool destroy_succeeded) noexcept {
    switch (phase) {
    case MqttPhase::started:
        if (!stop_succeeded) {
            return MqttPhase::started;
        }
        if (!destroy_succeeded) {
            return MqttPhase::created;
        }
        return MqttPhase::idle;
    case MqttPhase::created:
        if (!destroy_succeeded) {
            return MqttPhase::created;
        }
        return MqttPhase::idle;
    case MqttPhase::idle:
        return MqttPhase::idle;
    }
    return phase;
}

// idle only after the SDK session is actually disabled. A stop that leaves the
// session enabled stays started so begin() will not open another one.
constexpr SntpPhase sntp_phase_after_stop(SntpPhase phase, bool session_enabled) noexcept {
    if (phase == SntpPhase::started && !session_enabled) {
        return SntpPhase::idle;
    }
    return phase;
}

// waiting_for_time on the monotonic clock. sessions_completed counts sessions
// whose wait already expired. The session still running is not included.
enum class TimeWait : std::uint8_t {
    wait,
    restart_session,
    fail_bringup
};

constexpr TimeWait time_wait_decision(domain::Milliseconds now, domain::Milliseconds deadline,
                                      int sessions_completed, int session_budget) noexcept {
    if (session_budget <= sessions_completed) {
        return TimeWait::fail_bringup;
    }
    if (now < deadline) {
        return TimeWait::wait;
    }
    if (sessions_completed + 1 >= session_budget) {
        return TimeWait::fail_bringup;
    }
    return TimeWait::restart_session;
}

} // namespace networking
