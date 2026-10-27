#include "application/command_path.hpp"
#include "application/workers.hpp"
#include "domain/command.hpp"
#include "drivers/simulated_output.hpp"

#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <vector>

namespace {

class FakeClock final : public application::Clock {
    public:
    domain::Milliseconds time{0};

    domain::Milliseconds now() const noexcept override {
        return time;
    }
};

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
    static_assert(Capacity > 0);

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

class FailingOutput final : public application::OutputSink {
    public:
    domain::OutputError set_enabled(bool enabled) noexcept override {
        return enabled ? domain::OutputError::hardware_fault : domain::OutputError::none;
    }
};

template <std::size_t Commands, std::size_t Results> struct Path {
    static constexpr domain::CommandLimits limits{1000, domain::command_defaults::max_ttl_ms};
    MemoryFifo<application::QueuedCommand, Commands> commands;
    MemoryFifo<domain::CommandResult, Results> results;
    MemorySnapshot<application::StopNotice> stops;
    application::CommandIngress ingress;
    application::CommandRequests requests;

    explicit Path(bool remote = false)
        : ingress(commands, results, stops, limits, remote),
          requests(commands, results, stops, limits.max_run_ms) {
    }
};

domain::Command command_for(std::uint64_t id, std::uint32_t sequence, domain::Milliseconds now,
                            domain::Milliseconds duration = 300) {
    domain::Command command{};
    command.id = id;
    command.sequence = sequence;
    command.identity = 7;
    command.type = domain::CommandType::set_output;
    command.source = domain::CommandSource::local;
    command.enabled = true;
    command.expires_at = now + 5000;
    command.max_duration_ms = duration;
    return command;
}

std::vector<domain::CommandStatus> drain(application::BoundedFifo<domain::CommandResult>& results) {
    std::vector<domain::CommandStatus> statuses;
    domain::CommandResult result{};
    while (results.try_pop(result)) {
        statuses.push_back(result.status);
    }
    return statuses;
}

struct Loop {
    FakeClock clock;
    MemorySnapshot<domain::Reading> readings;
    MemorySnapshot<domain::Telemetry> telemetry;
    drivers::SimulatedOutput output;
    domain::TimedOutput policy;
    application::ControlWorker worker;

    Loop(application::CommandRequests& requests, domain::OutputPolicy output_policy)
        : policy(output_policy), worker(policy, clock, readings, requests, output, telemetry) {
    }
};

void fresh(Loop& loop, domain::Milliseconds now) {
    loop.clock.time = now;
    loop.readings.publish({42.0F, now, true});
}

} // namespace

TEST(CommandValidation, RejectsRangeExpiryAndRemoteSource) {
    const domain::CommandLimits limits{1000, 15000};
    auto command = command_for(1, 1, 0, 999);
    EXPECT_EQ(domain::validate_command(command, 0, limits, false), domain::CommandValidation::ok);
    command.max_duration_ms = 1000;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::invalid);
    command.max_duration_ms = 999;
    command.expires_at = 0;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::expired);
    command.expires_at = 15001;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::invalid);
    command.expires_at = 15000;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false), domain::CommandValidation::ok);
    command.id = 0;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::invalid);
    command.id = 1;
    command.sequence = 0;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::invalid);
    command.sequence = 1;
    command.identity = 0;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::invalid);
    command.identity = 7;
    command.source = domain::CommandSource::remote;
    EXPECT_EQ(domain::validate_command(command, 0, limits, false),
              domain::CommandValidation::unauthorized);
    EXPECT_EQ(domain::validate_command(command, 0, limits, true), domain::CommandValidation::ok);
}

TEST(CommandIngress, HappyPathAppliesThroughTimedOutputAndThenReleases) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 300), 0), domain::CommandStatus::accepted);
    for (domain::Milliseconds now = 0; now <= 300; now += 100) {
        fresh(loop, now);
        loop.worker.step();
        EXPECT_EQ(loop.output.enabled(), now < 300);
    }
    EXPECT_FALSE(loop.policy.fault());
    EXPECT_EQ(drain(path.results),
              (std::vector<domain::CommandStatus>{domain::CommandStatus::accepted,
                                                  domain::CommandStatus::applied}));
}

TEST(CommandIngress, EmptyQueueMatchesDisabledRequest) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    fresh(loop, 0);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
    EXPECT_FALSE(loop.telemetry.value.output_requested);
    EXPECT_TRUE(drain(path.results).empty());
}

TEST(CommandIngress, RejectsFullQueueWithoutRememberingTheCommand) {
    Path<2, 8> path;
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(3, 3, 0), 0), domain::CommandStatus::rejected_full);
    EXPECT_EQ(path.commands.size(), 2U);
    path.requests.enable_requested(0);
    path.requests.note_outcome(true, false, 0, domain::AppliedOutput::on);
    EXPECT_EQ(path.commands.size(), 0U);
    EXPECT_EQ(path.ingress.submit(command_for(3, 3, 0), 0), domain::CommandStatus::accepted);
}

TEST(CommandIngress, RejectsWhenResultQueueCannotRecordAcceptance) {
    Path<2, 1> path;
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 0), 0), domain::CommandStatus::rejected_full);
    EXPECT_EQ(path.commands.size(), 1U);
    EXPECT_GE(path.ingress.dropped_results(), 1U);
}

TEST(CommandIngress, RejectsInvalidExpiredDuplicateAndStaleSequence) {
    Path<4, 8> path;
    auto invalid = command_for(1, 1, 0, 0);
    EXPECT_EQ(path.ingress.submit(invalid, 0), domain::CommandStatus::rejected_invalid);
    EXPECT_EQ(path.commands.size(), 0U);
    auto expired = command_for(2, 1, 0);
    expired.expires_at = 0;
    EXPECT_EQ(path.ingress.submit(expired, 0), domain::CommandStatus::rejected_expired);
    EXPECT_EQ(path.ingress.submit(command_for(3, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(3, 2, 0), 0),
              domain::CommandStatus::rejected_duplicate);
    auto replay = command_for(4, 1, 0);
    EXPECT_EQ(path.ingress.submit(replay, 0), domain::CommandStatus::rejected_stale_sequence);
    EXPECT_EQ(path.ingress.submit(command_for(5, 2, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.commands.size(), 2U);
}

TEST(CommandIngress, InvalidCommandDoesNotConsumeSequence) {
    Path<4, 8> path;
    auto broken = command_for(0, 9, 0);
    EXPECT_EQ(path.ingress.submit(broken, 0), domain::CommandStatus::rejected_invalid);
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
}

TEST(CommandIngress, RejectsRemoteUntilExplicitlyEnabled) {
    Path<4, 8> closed;
    auto remote = command_for(1, 1, 0);
    remote.source = domain::CommandSource::remote;
    EXPECT_FALSE(closed.ingress.remote_commands_enabled());
    EXPECT_EQ(closed.ingress.submit(remote, 0), domain::CommandStatus::rejected_unauthorized);
    EXPECT_EQ(closed.commands.size(), 0U);

    Path<4, 8> open(true);
    EXPECT_TRUE(open.ingress.remote_commands_enabled());
    EXPECT_EQ(open.ingress.submit(remote, 0), domain::CommandStatus::accepted);
    Loop loop(open.requests, {1000, 10000});
    fresh(loop, 0);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
}

TEST(CommandIngress, LocalAndRemoteSequencesAreIndependent) {
    Path<4, 8> path(true);
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    auto remote = command_for(2, 1, 0);
    remote.source = domain::CommandSource::remote;
    EXPECT_EQ(path.ingress.submit(remote, 0), domain::CommandStatus::accepted);
}

TEST(CommandIngress, ForgetsOnlyTheOldestDuplicateAfterHistoryFills) {
    Path<16, 16> path;
    for (std::uint32_t index = 1; index <= application::CommandIngress::duplicate_history + 1;
         ++index) {
        EXPECT_EQ(path.ingress.submit(command_for(index, index, 0), 0),
                  domain::CommandStatus::accepted);
    }
    EXPECT_EQ(path.ingress.submit(command_for(1, 20, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(9, 21, 0), 0),
              domain::CommandStatus::rejected_duplicate);
}

TEST(CommandIngress, DuplicateDoesNotRestartTheRuntime) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 300), 0), domain::CommandStatus::accepted);
    fresh(loop, 0);
    loop.worker.step();
    ASSERT_TRUE(loop.output.enabled());
    fresh(loop, 100);
    EXPECT_EQ(path.ingress.submit(command_for(1, 2, 100, 300), 100),
              domain::CommandStatus::rejected_duplicate);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    fresh(loop, 300);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
    fresh(loop, 400);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
}

TEST(CommandIngress, LaterEnableCannotExtendTheOriginalLease) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 800), 0), domain::CommandStatus::accepted);
    fresh(loop, 0);
    loop.worker.step();
    fresh(loop, 400);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 400, 800), 400),
              domain::CommandStatus::accepted);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    fresh(loop, 800);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
}

TEST(CommandIngress, LaterEnableMayShortenTheLease) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 800), 0), domain::CommandStatus::accepted);
    fresh(loop, 0);
    loop.worker.step();
    fresh(loop, 400);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 400, 100), 400),
              domain::CommandStatus::accepted);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    fresh(loop, 500);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
}

TEST(CommandIngress, ExpiresBeforeApplyAndExplicitOffStayInsidePolicy) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    auto expiring = command_for(1, 1, 0, 400);
    expiring.expires_at = 100;
    EXPECT_EQ(path.ingress.submit(expiring, 0), domain::CommandStatus::accepted);
    fresh(loop, 100);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 100, 400), 100),
              domain::CommandStatus::accepted);
    fresh(loop, 100);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    auto off = command_for(3, 3, 150, 0);
    off.enabled = false;
    EXPECT_EQ(path.ingress.submit(off, 150), domain::CommandStatus::accepted);
    fresh(loop, 150);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
}

TEST(CommandIngress, StopIsHonoredWhenTheCommandQueueIsFull) {
    Path<2, 16> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 0), 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(3, 3, 0), 0), domain::CommandStatus::rejected_full);
    auto stop = command_for(4, 4, 0, 0);
    stop.type = domain::CommandType::stop;
    stop.enabled = false;
    EXPECT_EQ(path.ingress.submit(stop, 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.commands.size(), 2U);
    fresh(loop, 0);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
    EXPECT_EQ(path.commands.size(), 0U);
    const auto statuses = drain(path.results);
    EXPECT_EQ(statuses.front(), domain::CommandStatus::accepted);
    EXPECT_NE(std::find(statuses.begin(), statuses.end(), domain::CommandStatus::dropped_by_stop),
              statuses.end());
    EXPECT_EQ(statuses.back(), domain::CommandStatus::applied);
    fresh(loop, 50);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
}

TEST(CommandIngress, RetransmittedStoppedCommandCannotStartTheOutput) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 400), 0), domain::CommandStatus::accepted);
    auto stop = command_for(2, 2, 0, 0);
    stop.type = domain::CommandType::stop;
    stop.enabled = false;
    EXPECT_EQ(path.ingress.submit(stop, 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(command_for(1, 3, 0, 400), 0),
              domain::CommandStatus::rejected_duplicate);
    fresh(loop, 0);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
}

TEST(CommandIngress, StaleSampleFaultsInsteadOfBypassingTimedOutput) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 200});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 800), 0), domain::CommandStatus::accepted);
    loop.clock.time = 0;
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_TRUE(loop.policy.fault());
    EXPECT_EQ(drain(path.results),
              (std::vector<domain::CommandStatus>{domain::CommandStatus::accepted,
                                                  domain::CommandStatus::not_applied}));
    fresh(loop, 10);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 10, 400), 10), domain::CommandStatus::accepted);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_TRUE(loop.policy.fault());
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::off);
}

TEST(CommandIngress, FreshCommandThenStaleSampleStillForcesOff) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 200});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 800), 0), domain::CommandStatus::accepted);
    fresh(loop, 0);
    loop.worker.step();
    ASSERT_TRUE(loop.output.enabled());
    ASSERT_FALSE(loop.policy.fault());
    loop.clock.time = 201;
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_TRUE(loop.policy.fault());
    EXPECT_EQ(loop.telemetry.value.output_requested, true);
}

TEST(CommandIngress, OutputFaultRejectsTheCommandWithoutAResetPath) {
    Path<4, 8> path;
    FakeClock clock;
    MemorySnapshot<domain::Reading> readings;
    MemorySnapshot<domain::Telemetry> telemetry;
    FailingOutput output;
    domain::TimedOutput policy({1000, 10000});
    application::ControlWorker worker(policy, clock, readings, path.requests, output, telemetry);
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0, 300), 0), domain::CommandStatus::accepted);
    readings.publish({42.0F, 0, true});
    worker.step();
    EXPECT_TRUE(policy.fault());
    EXPECT_EQ(telemetry.value.applied_output, domain::AppliedOutput::off);
    EXPECT_EQ(drain(path.results).back(), domain::CommandStatus::not_applied);
    EXPECT_EQ(path.ingress.submit(command_for(2, 2, 0, 300), 0), domain::CommandStatus::accepted);
    worker.step();
    EXPECT_TRUE(policy.fault());
    EXPECT_EQ(telemetry.value.applied_output, domain::AppliedOutput::off);
}

TEST(CommandIngress, ClockRegressionFaultsWhileALeaseIsActive) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 100, 400), 100),
              domain::CommandStatus::accepted);
    fresh(loop, 100);
    loop.worker.step();
    ASSERT_TRUE(loop.output.enabled());
    fresh(loop, 50);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_TRUE(loop.policy.fault());
}

TEST(CommandIngress, StopStillAppliesWhenResultQueueIsFull) {
    Path<2, 1> path;
    EXPECT_EQ(path.ingress.submit(command_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    auto stop = command_for(2, 2, 0, 0);
    stop.type = domain::CommandType::stop;
    stop.enabled = false;
    EXPECT_EQ(path.ingress.submit(stop, 0), domain::CommandStatus::accepted);
    application::StopNotice notice{};
    ASSERT_TRUE(path.stops.latest(notice));
    EXPECT_EQ(notice.generation, 1U);
    EXPECT_EQ(notice.id, 2U);
}

domain::Command stop_command(std::uint64_t id, std::uint32_t sequence, domain::Milliseconds now) {
    auto command = command_for(id, sequence, now, 0);
    command.type = domain::CommandType::stop;
    command.enabled = false;
    return command;
}

std::vector<domain::CommandResult>
drain_results(application::BoundedFifo<domain::CommandResult>& results) {
    std::vector<domain::CommandResult> records;
    domain::CommandResult result{};
    while (results.try_pop(result)) {
        records.push_back(result);
    }
    return records;
}

TEST(CommandRequests, StopIsAppliedOnlyWhenOffWasObserved) {
    Path<4, 8> path;
    EXPECT_EQ(path.ingress.submit(stop_command(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_FALSE(path.requests.enable_requested(0));
    path.requests.note_outcome(false, true, 0, domain::AppliedOutput::unknown);
    const auto failed = drain_results(path.results);
    ASSERT_EQ(failed.size(), 2U);
    EXPECT_EQ(failed[0].status, domain::CommandStatus::accepted);
    EXPECT_EQ(failed[1].id, 1U);
    EXPECT_EQ(failed[1].status, domain::CommandStatus::not_applied);
}

TEST(CommandRequests, ObservedOffIsAppliedEvenWhenAFaultIsLatched) {
    Path<4, 8> path;
    EXPECT_EQ(path.ingress.submit(stop_command(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_FALSE(path.requests.enable_requested(0));
    path.requests.note_outcome(false, true, 0, domain::AppliedOutput::off);
    const auto records = drain_results(path.results);
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].status, domain::CommandStatus::applied);
    EXPECT_EQ(records[1].type, domain::CommandType::stop);
}

TEST(CommandRequests, StopLeftOnIsNotApplied) {
    Path<4, 8> path;
    EXPECT_EQ(path.ingress.submit(stop_command(1, 1, 0), 0), domain::CommandStatus::accepted);
    EXPECT_FALSE(path.requests.enable_requested(0));
    path.requests.note_outcome(true, false, 0, domain::AppliedOutput::on);
    const auto records = drain_results(path.results);
    ASSERT_EQ(records.size(), 2U);
    EXPECT_EQ(records[1].status, domain::CommandStatus::not_applied);
}
