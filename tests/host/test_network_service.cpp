#include "networking/network_service.hpp"

#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

class FakeClock final : public application::Clock {
    public:
    domain::Milliseconds time{0};

    domain::Milliseconds now() const noexcept override {
        return time;
    }
};

class FakeWifi final : public networking::WifiConnection {
    public:
    bool start(const configuration::NetworkConfig& config) noexcept override {
        trace->push_back("wifi.start");
        ++starts;
        ssid = config.wifi_ssid;
        if (!succeeds) {
            holding = true;
            return false;
        }
        holding = true;
        return true;
    }

    void stop() noexcept override {
        trace->push_back("wifi.stop");
        ++stops;
        holding = false;
        online = false;
    }

    void step() noexcept override {
        ++steps;
    }

    bool connected() const noexcept override {
        return online;
    }

    std::vector<std::string>* trace{nullptr};
    int starts{0};
    int stops{0};
    int steps{0};
    bool succeeds{true};
    bool online{false};
    bool holding{false};
    std::string ssid;
};

class FakeMqtt final : public networking::MqttConnection {
    public:
    bool start(const configuration::NetworkConfig&) noexcept override {
        trace->push_back("mqtt.start");
        ++starts;
        if (!succeeds) {
            holding = true;
            return false;
        }
        holding = true;
        return true;
    }

    void stop() noexcept override {
        trace->push_back("mqtt.stop");
        ++stops;
        holding = false;
    }

    void report(const domain::Telemetry&) noexcept override {
        ++reports;
    }

    void drop_report() noexcept override {
        ++drops;
    }

    void publish_availability() noexcept override {
        ++availability_publishes;
    }

    std::vector<std::string>* trace{nullptr};
    int starts{0};
    int stops{0};
    int reports{0};
    int drops{0};
    int availability_publishes{0};
    bool succeeds{true};
    bool holding{false};
};

class FakeTime final : public networking::NetworkTime {
    public:
    void begin() noexcept override {
        trace->push_back("time.begin");
        ++begins;
        running = true;
    }

    void stop() noexcept override {
        trace->push_back("time.stop");
        ++stops;
        running = false;
    }

    bool ready() const noexcept override {
        return synchronized;
    }

    std::vector<std::string>* trace{nullptr};
    int begins{0};
    int stops{0};
    bool synchronized{false};
    bool running{false};
};

class NetworkServiceTest : public ::testing::Test {
    protected:
    std::vector<std::string> trace;
    FakeWifi wifi;
    FakeMqtt mqtt;
    FakeTime time;
    FakeClock clock;
    networking::NetworkService service{wifi, mqtt, time, clock};
    configuration::NetworkConfig config{};

    void SetUp() override {
        wifi.trace = &trace;
        mqtt.trace = &trace;
        time.trace = &trace;
        std::strcpy(config.wifi_ssid, "test-network");
        std::strcpy(config.mqtt_uri, "mqtts://broker.example");
    }

    void reach_running() {
        service.configure(config);
        service.step();
        wifi.online = true;
        time.synchronized = true;
        service.step();
        service.step();
        ASSERT_EQ(service.state(), networking::NetworkState::running);
        trace.clear();
    }
};

TEST_F(NetworkServiceTest, UnconfiguredServiceDoesNotStartAdapters) {
    service.step();
    service.start();
    service.stop();
    service.stop();
    EXPECT_EQ(service.state(), networking::NetworkState::disabled);
    EXPECT_TRUE(trace.empty());
}

TEST_F(NetworkServiceTest, TlsWaitsForWifiAndTimeAndKeepsServicingReconnects) {
    service.configure(config);
    config.wifi_ssid[0] = 'X';
    service.step();
    EXPECT_EQ(wifi.ssid, "test-network");
    EXPECT_EQ(time.begins, 1);
    EXPECT_EQ(mqtt.starts, 0);
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_wifi);
    wifi.online = true;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(mqtt.starts, 0);
    time.synchronized = true;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::running);
    EXPECT_EQ(mqtt.starts, 1);
    wifi.online = false;
    service.step();
    wifi.online = true;
    service.step();
    EXPECT_EQ(wifi.steps, 5);
    EXPECT_EQ(wifi.starts, 1);
    EXPECT_EQ(mqtt.starts, 1);
    EXPECT_EQ(wifi.stops, 0);
    EXPECT_EQ(time.begins, 1);
    EXPECT_EQ(time.stops, 0);
}

TEST_F(NetworkServiceTest, TimeSyncStallRestartsSntpThenFailsLikeWifiBringup) {
    service.configure(config);
    service.step();
    wifi.online = true;
    service.step();
    ASSERT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(time.begins, 1);
    EXPECT_EQ(time.stops, 0);
    EXPECT_EQ(mqtt.starts, 0);

    clock.time = networking::network_init_retry_ms - 1;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(time.begins, 1);
    EXPECT_TRUE(wifi.holding);

    clock.time = networking::network_init_retry_ms;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(time.stops, 1);
    EXPECT_EQ(time.begins, 2);
    EXPECT_TRUE(time.running);
    EXPECT_EQ(wifi.stops, 0);
    EXPECT_EQ(mqtt.starts, 0);

    clock.time = networking::network_init_retry_ms * 2 - 1;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(time.begins, 2);

    clock.time = networking::network_init_retry_ms * 2;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::failed);
    EXPECT_FALSE(wifi.holding);
    EXPECT_FALSE(time.running);
    EXPECT_FALSE(mqtt.holding);
    EXPECT_EQ(trace.back(), "wifi.stop");

    service.step();
    EXPECT_EQ(wifi.starts, 1);
    clock.time = networking::network_init_retry_ms * 3;
    service.step();
    EXPECT_EQ(wifi.starts, 2);
    EXPECT_EQ(time.begins, 3);
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_wifi);
}

TEST_F(NetworkServiceTest, TimeSyncBudgetDoesNotResetWhenTheLinkDrops) {
    service.configure(config);
    service.step();
    wifi.online = true;
    service.step();
    ASSERT_EQ(service.state(), networking::NetworkState::waiting_for_time);

    clock.time = networking::network_init_retry_ms - 1;
    wifi.online = false;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_wifi);
    EXPECT_EQ(time.stops, 0);

    clock.time = networking::network_init_retry_ms;
    wifi.online = true;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_time);
    EXPECT_EQ(time.stops, 1);
    EXPECT_EQ(time.begins, 2);
    EXPECT_EQ(wifi.stops, 0);
    EXPECT_EQ(mqtt.starts, 0);
}

TEST_F(NetworkServiceTest, WorkerPublishesAvailabilityUntilStop) {
    reach_running();
    const auto published = mqtt.availability_publishes;
    EXPECT_GT(published, 0);
    service.step();
    EXPECT_EQ(mqtt.availability_publishes, published + 1);
    service.stop();
    service.step();
    EXPECT_EQ(mqtt.availability_publishes, published + 1);
}

TEST_F(NetworkServiceTest, PlaintextDoesNotRequireTimeSynchronization) {
    std::strcpy(config.mqtt_uri, "mqtt://broker.example");
    wifi.online = true;
    service.configure(config);
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::running);
    EXPECT_EQ(time.begins, 0);
    EXPECT_EQ(mqtt.starts, 1);
}

TEST_F(NetworkServiceTest, WifiInitializationFailureReleasesResourcesAndRetriesAfterBackoff) {
    wifi.succeeds = false;
    service.configure(config);
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::failed);
    EXPECT_FALSE(wifi.holding);
    EXPECT_FALSE(time.running);
    EXPECT_FALSE(mqtt.holding);
    EXPECT_EQ(mqtt.starts, 0);
    EXPECT_EQ(trace,
              (std::vector<std::string>{"wifi.start", "mqtt.stop", "time.stop", "wifi.stop"}));
    service.step();
    EXPECT_EQ(wifi.starts, 1);
    clock.time = networking::network_init_retry_ms;
    service.step();
    EXPECT_EQ(wifi.starts, 2);
    EXPECT_EQ(service.state(), networking::NetworkState::failed);
}

TEST_F(NetworkServiceTest, MqttInitializationFailureKeepsTheStationAndRetriesOnlyMqtt) {
    wifi.online = true;
    time.synchronized = true;
    mqtt.succeeds = false;
    service.configure(config);
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_mqtt);
    EXPECT_TRUE(wifi.holding);
    EXPECT_TRUE(time.running);
    EXPECT_FALSE(mqtt.holding);
    EXPECT_EQ(wifi.stops, 0);
    EXPECT_EQ(time.stops, 0);
    EXPECT_EQ(mqtt.stops, 1);
    service.step();
    EXPECT_EQ(mqtt.starts, 1);
    clock.time = networking::network_init_retry_ms;
    service.step();
    EXPECT_EQ(wifi.starts, 1);
    EXPECT_EQ(time.begins, 1);
    EXPECT_EQ(mqtt.starts, 2);
    EXPECT_EQ(service.state(), networking::NetworkState::waiting_for_mqtt);
    mqtt.succeeds = true;
    clock.time = networking::network_init_retry_ms * 2;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::running);
    EXPECT_EQ(wifi.starts, 1);
}

TEST_F(NetworkServiceTest, StopReleasesInReverseOrderAndRepeatedStopDoesNotRestart) {
    reach_running();
    service.stop();
    EXPECT_EQ(service.state(), networking::NetworkState::stopped);
    EXPECT_EQ(trace, (std::vector<std::string>{"mqtt.stop", "time.stop", "wifi.stop"}));
    EXPECT_FALSE(wifi.holding);
    EXPECT_FALSE(time.running);
    EXPECT_FALSE(mqtt.holding);
    service.stop();
    EXPECT_EQ(service.state(), networking::NetworkState::stopped);
    EXPECT_EQ(mqtt.stops, 2);
    EXPECT_EQ(time.stops, 2);
    EXPECT_EQ(wifi.stops, 2);
    clock.time = networking::network_init_retry_ms * 10;
    service.step();
    EXPECT_EQ(wifi.starts, 1);
    EXPECT_EQ(mqtt.starts, 1);
    EXPECT_EQ(service.state(), networking::NetworkState::stopped);
}

TEST_F(NetworkServiceTest, StartAfterStopBringsTheStackUpAgain) {
    reach_running();
    service.stop();
    service.start();
    EXPECT_EQ(service.state(), networking::NetworkState::starting);
    service.start();
    wifi.online = true;
    time.synchronized = true;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::running);
    EXPECT_EQ(wifi.starts, 2);
    EXPECT_EQ(time.begins, 2);
    EXPECT_EQ(mqtt.starts, 2);
}

TEST_F(NetworkServiceTest, StopAfterFailureCancelsTheAutomaticRetry) {
    wifi.succeeds = false;
    service.configure(config);
    service.step();
    ASSERT_EQ(service.state(), networking::NetworkState::failed);
    service.stop();
    clock.time = networking::network_init_retry_ms;
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::stopped);
    EXPECT_EQ(wifi.starts, 1);
}

TEST_F(NetworkServiceTest, StartWhileRunningDoesNotBringTheStackUpTwice) {
    reach_running();
    service.start();
    service.step();
    EXPECT_EQ(service.state(), networking::NetworkState::running);
    EXPECT_EQ(wifi.starts, 1);
    EXPECT_EQ(mqtt.starts, 1);
    EXPECT_TRUE(trace.empty());
}

TEST_F(NetworkServiceTest, ReportsAreDroppedOfflineAndForwardedOnline) {
    service.report({});
    EXPECT_EQ(mqtt.drops, 1);
    wifi.online = true;
    service.report({});
    EXPECT_EQ(mqtt.reports, 1);
}

} // namespace
