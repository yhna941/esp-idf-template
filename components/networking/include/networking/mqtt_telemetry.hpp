#pragma once

#include "application/ports/device.hpp"
#include "configuration/network_config.hpp"
#include "mqtt_client.h"
#include "networking/connections.hpp"
#include "networking/lifecycle.hpp"

#include <atomic>
#include <mutex>

namespace networking {

// Boot-lifetime MQTT adapter. The configuration passed to start() must outlive
// the client. stop() destroys that client. No command subscription and no
// offline telemetry backlog. report() may run on the reporter task; start(),
// stop(), and publish_availability() run on the network worker. The mutex
// covers the client pointer. The event callback only stores atomics.
// A future adapter calls application::CommandIngress::submit and does not write outputs.
class MqttTelemetry final : public MqttConnection {
    public:
    MqttTelemetry() = default;
    MqttTelemetry(const MqttTelemetry&) = delete;
    MqttTelemetry& operator=(const MqttTelemetry&) = delete;
    bool start(const configuration::NetworkConfig& config) noexcept override;
    void stop() noexcept override;
    void report(const domain::Telemetry& status) noexcept override;

    void drop_report() noexcept override {
        ++dropped_;
    }

    void publish_availability() noexcept override;

    std::uint32_t dropped_reports() const noexcept {
        return dropped_.load();
    }

    private:
    static void event(void* context, esp_event_base_t base, std::int32_t id, void* data);
    std::mutex publish_mutex_{};
    std::atomic<esp_mqtt_client_handle_t> client_{nullptr};
    std::atomic<bool> mqtt_up_{false};
    std::atomic<std::uint32_t> dropped_{0};
    // Bumped by the MQTT task on connect. The network worker publishes once
    // per value and is the only writer of availability_published_.
    std::atomic<std::uint32_t> availability_epoch_{0};
    std::uint32_t availability_published_{0};
    MqttPhase phase_{MqttPhase::idle};
    char telemetry_topic_[160]{};
    char availability_topic_[160]{};
};

} // namespace networking
