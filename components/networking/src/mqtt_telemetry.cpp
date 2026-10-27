#include "networking/mqtt_telemetry.hpp"

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "networking/telemetry.hpp"
#include "sdkconfig.h"

#include <array>
#include <cstdio>

namespace networking {

namespace {

constexpr char tag[] = "network";
constexpr unsigned payload_capacity = 512;
constexpr unsigned outbox_limit_bytes = 4096;
constexpr unsigned mqtt_retry_ms = 5000;
constexpr unsigned network_timeout_ms = 3000;

} // namespace

bool MqttTelemetry::start(const configuration::NetworkConfig& config) noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    {
        const std::lock_guard<std::mutex> lock(publish_mutex_);
        if (phase_ == MqttPhase::started) {
            return true;
        }
    }
    stop();
    {
        const std::lock_guard<std::mutex> lock(publish_mutex_);
        // stop() could not finish. Do not create a second client on top of it.
        if (phase_ != MqttPhase::idle) {
            return false;
        }
    }
    std::snprintf(telemetry_topic_, sizeof(telemetry_topic_), "iot/v1/%s/%s/telemetry",
                  config.site_id, config.device_id);
    std::snprintf(availability_topic_, sizeof(availability_topic_), "iot/v1/%s/%s/availability",
                  config.site_id, config.device_id);
    esp_mqtt_client_config_t mqtt{};
    mqtt.broker.address.uri = config.mqtt_uri;
    mqtt.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    mqtt.credentials.client_id = config.device_id;
    mqtt.credentials.username = config.mqtt_username[0] ? config.mqtt_username : nullptr;
    mqtt.credentials.authentication.password =
        config.mqtt_password[0] ? config.mqtt_password : nullptr;
    mqtt.session.last_will.topic = availability_topic_;
    mqtt.session.last_will.msg = "offline";
    mqtt.session.last_will.qos = 1;
    mqtt.session.last_will.retain = 1;
    mqtt.network.reconnect_timeout_ms = mqtt_retry_ms + esp_random() % 1000;
    mqtt.network.timeout_ms = network_timeout_ms;
    mqtt.outbox.limit = outbox_limit_bytes;
    auto client = esp_mqtt_client_init(&mqtt);
    if (client == nullptr) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(publish_mutex_);
        client_.store(client);
        phase_ = MqttPhase::created;
    }
    if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, event, this) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        stop();
        return false;
    }
    const std::lock_guard<std::mutex> lock(publish_mutex_);
    phase_ = MqttPhase::started;
    return true;
#else
    (void)config;
    return false;
#endif
}

void MqttTelemetry::stop() noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    esp_mqtt_client_handle_t client = nullptr;
    MqttPhase phase = MqttPhase::idle;
    {
        const std::lock_guard<std::mutex> lock(publish_mutex_);
        client = client_.exchange(nullptr);
        phase = phase_;
        mqtt_up_.store(false);
    }
    // Unlock before the SDK calls. stop() can wait for the MQTT task, and that
    // task's callback only reads atomics, so it must not need this mutex.
    if (client == nullptr) {
        return;
    }
    const auto cleanup = mqtt_cleanup(phase);
    if (!cleanup.stop_client && !cleanup.destroy_client) {
        const std::lock_guard<std::mutex> lock(publish_mutex_);
        client_.store(client);
        return;
    }
    bool stop_succeeded = false;
    bool destroy_succeeded = false;
    if (cleanup.stop_client) {
        const auto stopped = esp_mqtt_client_stop(client);
        stop_succeeded = stopped == ESP_OK;
        if (!stop_succeeded) {
            ESP_LOGW(tag, "esp_mqtt_client_stop failed: %s", esp_err_to_name(stopped));
        }
    }
    if ((!cleanup.stop_client || stop_succeeded) && cleanup.destroy_client) {
        const auto destroyed = esp_mqtt_client_destroy(client);
        destroy_succeeded = destroyed == ESP_OK;
        if (!destroy_succeeded) {
            ESP_LOGW(tag, "esp_mqtt_client_destroy failed: %s", esp_err_to_name(destroyed));
        }
    }
    const auto next = mqtt_phase_after_release(phase, stop_succeeded, destroy_succeeded);
    const std::lock_guard<std::mutex> lock(publish_mutex_);
    phase_ = next;
    if (next == MqttPhase::idle) {
        availability_epoch_.store(0);
        availability_published_ = 0;
        mqtt_up_.store(false);
        return;
    }
    // The release did not finish. Keep the handle so the next stop can retry.
    client_.store(client);
#endif
}

void MqttTelemetry::publish_availability() noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    const auto epoch = availability_epoch_.load();
    if (epoch == availability_published_) {
        return;
    }
    const std::lock_guard<std::mutex> lock(publish_mutex_);
    const auto client = client_.load();
    const auto current = availability_epoch_.load();
    if (client == nullptr || !mqtt_up_.load() || current == availability_published_) {
        return;
    }
    if (esp_mqtt_client_enqueue(client, availability_topic_, "online", 0, 1, 1, true) < 0) {
        return;
    }
    availability_published_ = current;
#endif
}

void MqttTelemetry::event(void* context, esp_event_base_t, std::int32_t id, void*) {
    auto& self = *static_cast<MqttTelemetry*>(context);
    if (id == MQTT_EVENT_CONNECTED) {
        self.mqtt_up_.store(true);
        self.availability_epoch_.fetch_add(1);
        ESP_LOGI(tag, "MQTT connected");
    } else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        self.mqtt_up_.store(false);
    }
}

void MqttTelemetry::report(const domain::Telemetry& status) noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    const std::lock_guard<std::mutex> lock(publish_mutex_);
    const auto client = client_.load();
    if (client == nullptr || !mqtt_up_.load()) {
        ++dropped_;
        return;
    }
    std::array<char, payload_capacity> payload{};
    const auto length = encode_telemetry(status, payload);
    // QoS 0 + store=false: no offline telemetry backlog or replay after reconnect.
    if (length == 0 || esp_mqtt_client_get_outbox_size(client) >= outbox_limit_bytes ||
        esp_mqtt_client_enqueue(client, telemetry_topic_, payload.data(), static_cast<int>(length),
                                0, 0, false) < 0) {
        ++dropped_;
    }
#else
    (void)status;
#endif
}

} // namespace networking
