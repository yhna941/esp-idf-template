#include "application/command_path.hpp"
#include "configuration/network_config.hpp"
#include "domain/policy_defaults.hpp"
#include "networking/command_envelope.hpp"
#include "networking/telemetry.hpp"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <string_view>

namespace {

configuration::NetworkConfig config() {
    configuration::NetworkConfig value{};
    std::strcpy(value.wifi_ssid, "lab");
    std::strcpy(value.wifi_password, "test-password");
    std::strcpy(value.mqtt_uri, "mqtts://broker.example.com:8883");
    std::strcpy(value.device_id, "node-01");
    std::strcpy(value.site_id, "farm");
    return value;
}

TEST(NetworkConfig, RequiresTlsUnlessDevelopmentModeIsExplicit) {
    auto c = config();
    EXPECT_TRUE(configuration::valid_network_config(c));
    std::strcpy(c.mqtt_uri, "mqtt://localhost:1883");
    EXPECT_FALSE(configuration::valid_network_config(c));
    EXPECT_TRUE(configuration::valid_network_config(c, true));
}

TEST(NetworkConfig, RejectsTopicInjectionCredentialsInUriAndUnterminatedFields) {
    auto c = config();
    std::strcpy(c.device_id, "node/commands");
    EXPECT_FALSE(configuration::valid_network_config(c));
    c = config();
    std::strcpy(c.mqtt_uri, "mqtts://user:secret@host");
    EXPECT_FALSE(configuration::valid_network_config(c));
    c = config();
    std::memset(c.wifi_ssid, 'x', sizeof(c.wifi_ssid));
    EXPECT_FALSE(configuration::valid_network_config(c));
    c = config();
    c.wifi_password[0] = '\0';
    EXPECT_FALSE(configuration::valid_network_config(c));
}

TEST(NetworkConfig, RejectsMissingHostBadPortAndPaths) {
    for (const auto* uri : {"mqtts:///", "mqtts://:8883", "mqtts://host:0", "mqtts://host:99999",
                            "mqtts://host/path"}) {
        auto c = config();
        std::strcpy(c.mqtt_uri, uri);
        EXPECT_FALSE(configuration::valid_network_config(c)) << uri;
    }
}

TEST(TelemetryEncoding, CarriesMeasurementUnitTimestampAndFault) {
    domain::Telemetry status{1000,
                             false,
                             domain::AppliedOutput::off,
                             false,
                             domain::OutputError::none,
                             {25.5F, 900, true, domain::MeasurementUnit::celsius}};
    std::array<char, 512> buffer{};
    const auto size = networking::encode_telemetry(status, buffer);
    ASSERT_GT(size, 0U);
    const std::string_view json(buffer.data(), size);
    EXPECT_NE(json.find("\"value\":25.500"), json.npos);
    EXPECT_NE(json.find("\"unit\":\"celsius\""), json.npos);
    EXPECT_NE(json.find("\"sampled_at_ms\":900"), json.npos);
    EXPECT_NE(json.find("\"applied\":\"off\""), json.npos);
}

TEST(TelemetryEncoding, InvalidNumbersBecomeNullAndSmallBuffersFail) {
    domain::Telemetry status{1000,
                             false,
                             domain::AppliedOutput::unknown,
                             true,
                             domain::OutputError::hardware_fault,
                             {std::numeric_limits<float>::quiet_NaN(), 900, true}};
    std::array<char, 512> buffer{};
    ASSERT_GT(networking::encode_telemetry(status, buffer), 0U);
    EXPECT_NE(std::string_view(buffer.data()).find("\"value\":null"), std::string_view::npos);
    EXPECT_NE(std::string_view(buffer.data()).find("\"valid\":false"), std::string_view::npos);
    std::array<char, 8> small{};
    EXPECT_EQ(networking::encode_telemetry(status, small), 0U);
}

template <typename T>
class MemorySnapshot final : public application::SnapshotReader<T>,
                             public application::SnapshotWriter<T> {
    public:
    bool present{false};
    T value{};

    bool latest(T& destination) const noexcept override {
        if (!present) {
            return false;
        }
        destination = value;
        return true;
    }

    void publish(const T& source) noexcept override {
        value = source;
        present = true;
    }
};

template <typename T, std::size_t Capacity>
class MemoryFifo final : public application::BoundedFifo<T> {
    public:
    bool try_push(const T& value) noexcept override {
        if (size_ >= Capacity) {
            return false;
        }
        data_[(head_ + size_) % Capacity] = value;
        ++size_;
        return true;
    }

    bool try_pop(T& value) noexcept override {
        if (size_ == 0) {
            return false;
        }
        value = data_[head_];
        head_ = (head_ + 1) % Capacity;
        --size_;
        return true;
    }

    std::size_t size() const noexcept override {
        return size_;
    }

    std::size_t capacity() const noexcept override {
        return Capacity;
    }

    private:
    std::array<T, Capacity> data_{};
    std::size_t head_{0};
    std::size_t size_{0};
};

template <std::size_t Commands = 4, std::size_t Results = 4> struct EnvelopePath {
    MemoryFifo<application::QueuedCommand, Commands> commands;
    MemoryFifo<domain::CommandResult, Results> results;
    MemorySnapshot<application::StopNotice> stops;
    application::CommandIngress ingress;

    explicit EnvelopePath(
        bool remote = true,
        domain::CommandLimits limits = {domain::policy_defaults::default_max_run_ms,
                                        domain::command_defaults::max_ttl_ms})
        : ingress(commands, results, stops, limits, remote) {
    }
};

constexpr domain::Milliseconds k_now = 10000000;
constexpr std::uint64_t k_sample_id = 0x8f36a7b1;

std::string enable_payload(std::string_view expires, std::string_view duration = "100",
                           std::string_view id = "8f36a7b1",
                           std::string_view issued = "1970-01-01T02:46:40Z") {
    return std::string("{\"schema_version\":1,\"command_id\":\"") + std::string(id) +
           "\",\"type\":\"set_output\",\"enabled\":true,\"issued_at\":\"" + std::string(issued) +
           "\",\"expires_at\":\"" + std::string(expires) +
           "\",\"max_duration_ms\":" + std::string(duration) + "}";
}

template <std::size_t Commands, std::size_t Results>
void expect_untouched(const EnvelopePath<Commands, Results>& path) {
    EXPECT_EQ(path.commands.size(), 0U);
    EXPECT_EQ(path.results.size(), 0U);
    EXPECT_EQ(path.ingress.dropped_results(), 0U);
    application::StopNotice notice{};
    EXPECT_FALSE(path.stops.latest(notice));
}

template <std::size_t Commands, std::size_t Results>
networking::CommandEnvelopeResult decode(EnvelopePath<Commands, Results>& path,
                                         std::string_view payload, domain::Milliseconds offset = 0,
                                         std::uint32_t sequence = 1, std::uint32_t identity = 7,
                                         domain::Milliseconds now = k_now) {
    return networking::decode_command_envelope(payload, now, offset, identity, sequence,
                                               path.ingress);
}

TEST(CommandEnvelope, ConvertsWallClockAndSubmitsRemoteEnable) {
    EnvelopePath path;
    const auto payload = enable_payload("1970-01-01T02:46:45Z", "4999");
    const auto decoded = decode(path, payload, 1000);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(decoded.admission, domain::CommandStatus::accepted);

    application::QueuedCommand queued{};
    ASSERT_TRUE(path.commands.try_pop(queued));
    EXPECT_EQ(queued.received_at, k_now);
    EXPECT_EQ(queued.command.id, k_sample_id);
    EXPECT_EQ(queued.command.sequence, 1U);
    EXPECT_EQ(queued.command.identity, 7U);
    EXPECT_EQ(queued.command.type, domain::CommandType::set_output);
    EXPECT_EQ(queued.command.source, domain::CommandSource::remote);
    EXPECT_TRUE(queued.command.enabled);
    EXPECT_EQ(queued.command.max_duration_ms, 4999);
    // Wall 10005000 minus offset 1000.
    EXPECT_EQ(queued.command.expires_at, 10004000);
    domain::CommandResult result{};
    ASSERT_TRUE(path.results.try_pop(result));
    EXPECT_EQ(result.status, domain::CommandStatus::accepted);
    EXPECT_EQ(result.source, domain::CommandSource::remote);
}

TEST(CommandEnvelope, AcceptsReorderedWhitespaceFractionalAndPaddedPayloads) {
    EnvelopePath path;
    const char* pretty = R"({
        "max_duration_ms" : 100,
        "expires_at" : "1970-01-01T00:00:01.5Z",
        "issued_at" : "1970-01-01T00:00:00.050Z",
        "enabled" : true,
        "type" : "set_output",
        "command_id" : "8F36A7B1",
        "schema_version" : 1
    })";
    std::string padded(pretty);
    ASSERT_LT(padded.size(), networking::command_envelope_max_bytes);
    padded.append(networking::command_envelope_max_bytes - padded.size(), ' ');
    padded.push_back('X');
    const std::string_view exact(padded.data(), networking::command_envelope_max_bytes);
    const auto decoded = decode(path, exact, 0, 1, 7, 0);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(decoded.admission, domain::CommandStatus::accepted);
    application::QueuedCommand queued{};
    ASSERT_TRUE(path.commands.try_pop(queued));
    EXPECT_EQ(queued.command.id, k_sample_id);
    EXPECT_EQ(queued.command.expires_at, 1500);
}

TEST(CommandEnvelope, SamplePayloadIsLimitedByDefaultRuntime) {
    constexpr domain::Milliseconds issued = 1792897200000;
    constexpr domain::Milliseconds expires = 1792897215000;
    constexpr domain::Milliseconds offset = issued - k_now;
    const auto sample =
        R"({"schema_version":1,"command_id":"8f36a7b1","type":"set_output","enabled":true,"issued_at":"2026-10-25T03:00:00Z","expires_at":"2026-10-25T03:00:15Z","max_duration_ms":5000})";
    EnvelopePath rejected;
    const auto too_long = decode(rejected, sample, offset);
    EXPECT_EQ(too_long.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(too_long.admission, domain::CommandStatus::rejected_invalid);
    EXPECT_EQ(rejected.commands.size(), 0U);

    auto shortened = std::string(sample);
    const auto at = shortened.rfind("5000");
    ASSERT_NE(at, std::string::npos);
    shortened.replace(at, 4, "4999");
    EnvelopePath accepted;
    const auto decoded = decode(accepted, shortened, offset);
    EXPECT_EQ(decoded.admission, domain::CommandStatus::accepted);
    application::QueuedCommand queued{};
    ASSERT_TRUE(accepted.commands.try_pop(queued));
    EXPECT_EQ(queued.command.expires_at, k_now + (expires - issued));
}

TEST(CommandEnvelope, StopBypassesTheCommandFifo) {
    EnvelopePath path;
    const auto payload =
        R"({"schema_version":1,"command_id":"a1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})";
    const auto decoded = decode(path, payload);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(decoded.admission, domain::CommandStatus::accepted);
    EXPECT_EQ(path.commands.size(), 0U);
    application::StopNotice notice{};
    ASSERT_TRUE(path.stops.latest(notice));
    EXPECT_EQ(notice.id, 0xa1U);
    EXPECT_EQ(notice.source, domain::CommandSource::remote);
    EXPECT_EQ(notice.expires_at, 10005000);
}

TEST(CommandEnvelope, RejectsOversizeMalformedUnknownVersionAndExpiredWithoutSubmit) {
    EXPECT_EQ(networking::command_envelope_max_bytes, 512U);
    EnvelopePath path;
    expect_untouched(path);

    const std::string oversize(networking::command_envelope_max_bytes + 1, '{');
    auto decoded = decode(path, oversize);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_oversize);
    expect_untouched(path);

    for (
        const char* payload :
        {"",
         "null",
         "[]",
         "{",
         "{]",
         R"({"schema_version":1,})",
         R"({"schema_version":"1"})",
         R"({"schema_version":1.0})",
         R"({"schema_version":01})",
         R"({"schema_version":true})",
         R"({"schema_version":1})",
         R"({"schema_version":1,"schema_version":1})",
         R"({"schema_version":1,"command_id":"zz","type":"set_output","enabled":true,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z","max_duration_ms":100})",
         R"({"schema_version":1,"command_id":"1234567890abcdef0","type":"set_output","enabled":true,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z","max_duration_ms":100})",
         R"({"schema_version":1,"command_id":"","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"start","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","enabled":false,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"set_output","enabled":true,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"set_output","enabled":true,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z","max_duration_ms":-1})",
         R"({"schema_version":1,"command_id":"1","type":"set_output","enabled":"true","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z","max_duration_ms":100})",
         R"({"schema_version":1,"note":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:50Z","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40+00:00","expires_at":"1970-01-01T02:46:45Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"2023-02-29T00:00:00Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"2100-02-29T00:00:00Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T24:00:00Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:60Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1969-12-31T23:59:59Z"})",
         R"({"schema_version":1,"command_id":"1","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45.1234Z"})"}) {
        decoded = decode(path, payload);
        EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_malformed) << payload;
        expect_untouched(path);
    }

    std::string deep = R"({"schema_version":2,"v":)";
    deep.append(8, '{');
    deep.append(8, '}');
    deep.push_back('}');
    decoded = decode(path, deep);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_malformed);
    expect_untouched(path);

    for (const char* payload :
         {R"({"schema_version":0})", R"({"schema_version":2})", R"({"schema_version":-1})",
          R"({"schema_version":2,"command_id":5,"extra":{"ok":true}})"}) {
        decoded = decode(path, payload);
        EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_unknown_version)
            << payload;
        expect_untouched(path);
    }

    decoded = decode(path, enable_payload("1970-01-01T02:46:40Z"));
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_expired);
    expect_untouched(path);
    decoded = decode(path, enable_payload("1970-01-01T02:46:45Z"), 10005000);
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_expired);
    expect_untouched(path);
    decoded = decode(path, enable_payload("1970-01-01T02:46:45Z"),
                     std::numeric_limits<domain::Milliseconds>::min());
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::rejected_malformed);
    expect_untouched(path);
}

TEST(CommandEnvelope, LeapDaysAndOneMillisecondPastNowAreAccepted) {
    constexpr domain::Milliseconds deadline = k_now + 5000;
    const auto leap_2024 =
        enable_payload("2024-02-29T00:00:00Z", "100", "b", "2024-02-29T00:00:00Z");
    EnvelopePath leap;
    const auto decoded_2024 = decode(leap, leap_2024, 1709164800000 - deadline);
    EXPECT_EQ(decoded_2024.admission, domain::CommandStatus::accepted);
    application::QueuedCommand queued{};
    ASSERT_TRUE(leap.commands.try_pop(queued));
    EXPECT_EQ(queued.command.expires_at, deadline);
    EXPECT_EQ(queued.command.id, 0xbU);

    const auto leap_2000 =
        enable_payload("2000-02-29T00:00:00Z", "100", "c", "2000-02-29T00:00:00Z");
    EnvelopePath earlier;
    const auto decoded_2000 = decode(earlier, leap_2000, 951782400000 - deadline);
    EXPECT_EQ(decoded_2000.admission, domain::CommandStatus::accepted);
    ASSERT_TRUE(earlier.commands.try_pop(queued));
    EXPECT_EQ(queued.command.expires_at, deadline);

    EnvelopePath next;
    const auto open = decode(
        next, enable_payload("1970-01-01T02:46:40.001Z", "100", "d", "1970-01-01T02:46:40Z"));
    EXPECT_EQ(open.admission, domain::CommandStatus::accepted);
    ASSERT_TRUE(next.commands.try_pop(queued));
    EXPECT_EQ(queued.command.expires_at, k_now + 1);
}

TEST(CommandEnvelope, RemoteAdmissionStaysOffUntilTheIngressAllowsIt) {
    EnvelopePath closed(false);
    const auto payload = enable_payload("1970-01-01T02:46:45Z");
    const auto decoded = decode(closed, payload);
    EXPECT_FALSE(closed.ingress.remote_commands_enabled());
    EXPECT_EQ(decoded.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(decoded.admission, domain::CommandStatus::rejected_unauthorized);
    EXPECT_EQ(closed.commands.size(), 0U);
    domain::CommandResult result{};
    ASSERT_TRUE(closed.results.try_pop(result));
    EXPECT_EQ(result.source, domain::CommandSource::remote);
    EXPECT_EQ(result.status, domain::CommandStatus::rejected_unauthorized);
}

TEST(CommandEnvelope, FullResultFifoRejectsNewEnableAndStillArmsStop) {
    EnvelopePath<4, 1> path;
    domain::CommandResult filler{};
    ASSERT_TRUE(path.results.try_push(filler));
    const auto enable = decode(path, enable_payload("1970-01-01T02:46:45Z"));
    EXPECT_EQ(enable.reason, networking::CommandEnvelopeReason::submitted);
    EXPECT_EQ(enable.admission, domain::CommandStatus::rejected_full);
    EXPECT_EQ(path.commands.size(), 0U);
    EXPECT_EQ(path.ingress.dropped_results(), 1U);

    const auto stop =
        R"({"schema_version":1,"command_id":"a2","type":"stop","issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z"})";
    const auto stopped = decode(path, stop, 0, 2);
    EXPECT_EQ(stopped.admission, domain::CommandStatus::accepted);
    EXPECT_EQ(path.commands.size(), 0U);
    application::StopNotice notice{};
    ASSERT_TRUE(path.stops.latest(notice));
    EXPECT_EQ(notice.id, 0xa2U);
    EXPECT_EQ(path.ingress.dropped_results(), 2U);
}

TEST(CommandEnvelope, DuplicateStaleSequenceAndZeroIdentityUseTheIngress) {
    EnvelopePath<4, 8> path;
    const auto payload = enable_payload("1970-01-01T02:46:45Z");
    EXPECT_EQ(decode(path, payload, 0, 1, 0).admission, domain::CommandStatus::rejected_invalid);
    EXPECT_EQ(path.commands.size(), 0U);

    EXPECT_EQ(decode(path, enable_payload("1970-01-01T02:46:45Z", "100", "0"), 0, 1).admission,
              domain::CommandStatus::rejected_invalid);

    EXPECT_EQ(decode(path, payload).admission, domain::CommandStatus::accepted);
    EXPECT_EQ(decode(path, payload, 0, 2).admission, domain::CommandStatus::rejected_duplicate);
    const auto second = enable_payload("1970-01-01T02:46:45Z", "100", "2");
    EXPECT_EQ(decode(path, second, 0, 1).admission, domain::CommandStatus::rejected_stale_sequence);
    EXPECT_EQ(decode(path, second, 0, 2).admission, domain::CommandStatus::accepted);

    EnvelopePath<1, 4> limited;
    EXPECT_EQ(decode(limited, payload).admission, domain::CommandStatus::accepted);
    const auto third = enable_payload("1970-01-01T02:46:45Z", "100", "3");
    EXPECT_EQ(decode(limited, third, 0, 2).admission, domain::CommandStatus::rejected_full);
    EXPECT_EQ(limited.commands.size(), 1U);
    domain::CommandResult result{};
    ASSERT_TRUE(limited.results.try_pop(result));
    EXPECT_EQ(result.status, domain::CommandStatus::accepted);
    ASSERT_TRUE(limited.results.try_pop(result));
    EXPECT_EQ(result.status, domain::CommandStatus::rejected_full);
}

TEST(CommandEnvelope, DisableAndTtlBoundaryStayInsideAdmission) {
    EnvelopePath path;
    const auto off =
        R"({"schema_version":1,"command_id":"10","type":"set_output","enabled":false,"issued_at":"1970-01-01T02:46:40Z","expires_at":"1970-01-01T02:46:45Z","max_duration_ms":0})";
    const auto disabled = decode(path, off);
    EXPECT_EQ(disabled.admission, domain::CommandStatus::accepted);
    application::QueuedCommand queued{};
    ASSERT_TRUE(path.commands.try_pop(queued));
    EXPECT_FALSE(queued.command.enabled);
    EXPECT_EQ(queued.command.max_duration_ms, 0);

    const auto edge = enable_payload("1970-01-01T02:46:55Z", "100", "11");
    EXPECT_EQ(decode(path, edge, 0, 2).admission, domain::CommandStatus::accepted);
    const auto past_ttl = enable_payload("1970-01-01T02:46:55.001Z", "100", "12");
    EXPECT_EQ(decode(path, past_ttl, 0, 3).admission, domain::CommandStatus::rejected_invalid);
}

TEST(ReconnectBackoff, IsBoundedAddsJitterAndResetsAfterRecovery) {
    networking::ReconnectBackoff retry;
    EXPECT_EQ(retry.next_delay(0), 500U);
    EXPECT_EQ(retry.next_delay(0), 1000U);
    for (unsigned i = 0; i < 30; ++i) {
        EXPECT_LE(retry.next_delay(12345), 30000U);
    }
    retry.reset();
    EXPECT_GT(retry.next_delay(10), 500U);
}

} // namespace
