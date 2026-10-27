#include "application/workers.hpp"
#include "networking/lifecycle.hpp"
#include "networking/network_service.hpp"

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

TEST(WifiResourcePhase, ReleaseWalksServiceOwnedResourcesDownToTheProcess) {
    auto phase = networking::WifiPhase::started;
    std::vector<networking::WifiPhase> released;
    while (networking::service_owned(phase)) {
        released.push_back(phase);
        phase = networking::release_wifi_phase(phase);
    }
    ASSERT_EQ(released.size(), 4U);
    EXPECT_EQ(released[0], networking::WifiPhase::started);
    EXPECT_EQ(released[1], networking::WifiPhase::handlers);
    EXPECT_EQ(released[2], networking::WifiPhase::driver);
    EXPECT_EQ(released[3], networking::WifiPhase::netif);
    EXPECT_EQ(phase, networking::WifiPhase::process_ready);
    EXPECT_EQ(networking::release_wifi_phase(phase), phase);
    EXPECT_EQ(networking::release_wifi_phase(networking::WifiPhase::idle),
              networking::WifiPhase::idle);
}

TEST(WifiResourcePhase, PartialDriverFailureDoesNotPretendTheRadioWasStarted) {
    EXPECT_EQ(networking::release_wifi_phase(networking::WifiPhase::driver),
              networking::WifiPhase::netif);
    EXPECT_TRUE(networking::wifi_phase_advances(networking::WifiPhase::netif,
                                                networking::WifiPhase::driver));
    EXPECT_FALSE(networking::wifi_phase_advances(networking::WifiPhase::netif,
                                                 networking::WifiPhase::started));
    EXPECT_FALSE(networking::wifi_phase_advances(networking::WifiPhase::started,
                                                 networking::WifiPhase::started));
}

TEST(MqttCleanup, MatchesTheSdkStopRules) {
    const auto started = networking::mqtt_cleanup(networking::MqttPhase::started);
    EXPECT_TRUE(started.stop_client);
    EXPECT_TRUE(started.destroy_client);
    const auto created = networking::mqtt_cleanup(networking::MqttPhase::created);
    EXPECT_FALSE(created.stop_client);
    EXPECT_TRUE(created.destroy_client);
    const auto idle = networking::mqtt_cleanup(networking::MqttPhase::idle);
    EXPECT_FALSE(idle.stop_client);
    EXPECT_FALSE(idle.destroy_client);
}

TEST(SntpPhase, StopReturnsToIdleAndBeginIsNotAlreadyStarted) {
    EXPECT_FALSE(networking::sntp_is_started(networking::SntpPhase::idle));
    EXPECT_TRUE(networking::sntp_is_started(networking::SntpPhase::started));
}

TEST(MqttRelease, FailedStopOrDestroyLeavesThatPhase) {
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::started, false, false),
              networking::MqttPhase::started);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::started, false, true),
              networking::MqttPhase::started);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::started, true, false),
              networking::MqttPhase::created);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::started, true, true),
              networking::MqttPhase::idle);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::created, false, false),
              networking::MqttPhase::created);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::created, true, true),
              networking::MqttPhase::idle);
    EXPECT_EQ(networking::mqtt_phase_after_release(networking::MqttPhase::idle, false, false),
              networking::MqttPhase::idle);
}

TEST(SntpRelease, StopReturnsToIdleOnlyWhenTheSessionIsDisabled) {
    EXPECT_EQ(networking::sntp_phase_after_stop(networking::SntpPhase::started, true),
              networking::SntpPhase::started);
    EXPECT_EQ(networking::sntp_phase_after_stop(networking::SntpPhase::started, false),
              networking::SntpPhase::idle);
    EXPECT_EQ(networking::sntp_phase_after_stop(networking::SntpPhase::idle, true),
              networking::SntpPhase::idle);
}

TEST(TimeWait, ExpiryRestartsTheSessionThenFailsBringup) {
    constexpr auto wait = networking::network_init_retry_ms;
    constexpr auto budget = networking::network_time_session_budget;
    EXPECT_EQ(networking::time_wait_decision(wait - 1, wait, 0, budget),
              networking::TimeWait::wait);
    EXPECT_EQ(networking::time_wait_decision(wait, wait, 0, budget),
              networking::TimeWait::restart_session);
    EXPECT_EQ(networking::time_wait_decision(wait * 2, wait * 2, 1, budget),
              networking::TimeWait::fail_bringup);
    EXPECT_EQ(networking::time_wait_decision(0, wait, budget, budget),
              networking::TimeWait::fail_bringup);
}

TEST(ConnectionStopOrder, ReleasesMqttThenTimeThenWifi) {
    ASSERT_EQ(std::size(networking::connection_stop_order), 3U);
    EXPECT_EQ(networking::connection_stop_order[0], networking::ConnectionRole::mqtt);
    EXPECT_EQ(networking::connection_stop_order[1], networking::ConnectionRole::time);
    EXPECT_EQ(networking::connection_stop_order[2], networking::ConnectionRole::wifi);
}

class CountingOutput final : public application::OutputSink {
    public:
    int calls{0};
    bool enabled{false};

    domain::OutputError set_enabled(bool requested) noexcept override {
        ++calls;
        enabled = requested;
        return domain::OutputError::none;
    }
};

class MemoryReading final : public application::SnapshotReader<domain::Reading>,
                            public application::SnapshotWriter<domain::Reading> {
    public:
    bool latest(domain::Reading& destination) const noexcept override {
        destination = value;
        return present;
    }

    void publish(const domain::Reading& source) noexcept override {
        value = source;
        present = true;
    }

    bool present{false};
    domain::Reading value{};
};

class MemoryTelemetry final : public application::SnapshotWriter<domain::Telemetry> {
    public:
    void publish(const domain::Telemetry&) noexcept override {
    }
};

class AlwaysOn final : public application::RequestSource {
    public:
    bool enable_requested(domain::Milliseconds) noexcept override {
        return true;
    }
};

class IdleClock final : public application::Clock {
    public:
    domain::Milliseconds now() const noexcept override {
        return 1000;
    }
};

class IdleWifi final : public networking::WifiConnection {
    public:
    bool start(const configuration::NetworkConfig&) noexcept override {
        return false;
    }

    void stop() noexcept override {
        ++stops;
    }

    void step() noexcept override {
    }

    bool connected() const noexcept override {
        return false;
    }

    int stops{0};
};

class IdleMqtt final : public networking::MqttConnection {
    public:
    bool start(const configuration::NetworkConfig&) noexcept override {
        return false;
    }

    void stop() noexcept override {
    }

    void report(const domain::Telemetry&) noexcept override {
    }

    void drop_report() noexcept override {
    }

    void publish_availability() noexcept override {
    }
};

class IdleTime final : public networking::NetworkTime {
    public:
    void begin() noexcept override {
    }

    void stop() noexcept override {
    }

    bool ready() const noexcept override {
        return false;
    }
};

TEST(ServiceLifecycle, NetworkFailureDoesNotChangeAnIndependentControlOutput) {
    IdleClock clock;
    MemoryReading readings;
    readings.present = true;
    readings.value = {22.0F, 1000, true};
    MemoryTelemetry telemetry;
    AlwaysOn requests;
    CountingOutput output;
    domain::TimedOutput policy({});
    application::ControlWorker control(policy, clock, readings, requests, output, telemetry);
    control.step();
    ASSERT_TRUE(output.enabled);
    ASSERT_EQ(output.calls, 1);

    IdleWifi wifi;
    IdleMqtt mqtt;
    IdleTime time;
    networking::NetworkService network(wifi, mqtt, time, clock);
    configuration::NetworkConfig config{};
    std::strcpy(config.wifi_ssid, "lab");
    std::strcpy(config.mqtt_uri, "mqtts://broker.example");
    network.configure(config);
    network.step();
    network.stop();
    network.step();
    EXPECT_EQ(network.state(), networking::NetworkState::stopped);
    EXPECT_EQ(wifi.stops, 2);
    EXPECT_EQ(output.calls, 1);
    EXPECT_TRUE(output.enabled);

    control.step();
    EXPECT_EQ(output.calls, 2);
    EXPECT_TRUE(output.enabled);
    EXPECT_FALSE(policy.fault());
}

} // namespace
