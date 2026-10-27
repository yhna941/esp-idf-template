#include "application/output_quiesce.hpp"
#include "application/workers.hpp"
#include "domain/command.hpp"
#include "drivers/simulated_output.hpp"

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

class OffFails final : public application::OutputSink {
    public:
    domain::OutputError set_enabled(bool enabled) noexcept override {
        return enabled ? domain::OutputError::none : domain::OutputError::hardware_fault;
    }
};

template <std::size_t Commands, std::size_t Results> struct Path {
    static constexpr domain::CommandLimits limits{1000, domain::command_defaults::max_ttl_ms};
    MemoryFifo<application::QueuedCommand, Commands> commands;
    MemoryFifo<domain::CommandResult, Results> results;
    MemorySnapshot<application::StopNotice> stops;
    application::CommandIngress ingress;
    application::CommandRequests requests;

    Path()
        : ingress(commands, results, stops, limits, false),
          requests(commands, results, stops, limits.max_run_ms) {
    }
};

domain::Command enable_for(std::uint64_t id, std::uint32_t sequence, domain::Milliseconds now) {
    domain::Command command{};
    command.id = id;
    command.sequence = sequence;
    command.identity = 7;
    command.type = domain::CommandType::set_output;
    command.source = domain::CommandSource::local;
    command.enabled = true;
    command.expires_at = now + 5000;
    command.max_duration_ms = 800;
    return command;
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

struct FailingLoop {
    FakeClock clock;
    MemorySnapshot<domain::Reading> readings;
    MemorySnapshot<domain::Telemetry> telemetry;
    OffFails output;
    domain::TimedOutput policy;
    application::ControlWorker worker;

    explicit FailingLoop(application::CommandRequests& requests)
        : policy({1000, 10000}), worker(policy, clock, readings, requests, output, telemetry) {
    }
};

void fresh(FakeClock& clock, MemorySnapshot<domain::Reading>& readings, domain::Milliseconds now) {
    clock.time = now;
    readings.publish({42.0F, now, true});
}

std::vector<domain::CommandResult> drain(application::BoundedFifo<domain::CommandResult>& results) {
    std::vector<domain::CommandResult> records;
    domain::CommandResult result{};
    while (results.try_pop(result)) {
        records.push_back(result);
    }
    return records;
}

domain::Telemetry sample(domain::Milliseconds uptime, domain::AppliedOutput applied) {
    domain::Telemetry telemetry{};
    telemetry.uptime = uptime;
    telemetry.applied_output = applied;
    return telemetry;
}

} // namespace

TEST(OutputQuiesce, ConfirmsOnlyASampleNewerThanTheStop) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(enable_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 0);
    loop.worker.step();
    ASSERT_TRUE(loop.output.enabled());
    ASSERT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::on);

    application::OutputQuiesce quiesce(path.ingress, loop.telemetry);
    EXPECT_EQ(quiesce.submit(2, 2, 7, 0, 5000), domain::CommandStatus::accepted);
    application::StopNotice notice{};
    ASSERT_TRUE(path.stops.latest(notice));
    EXPECT_EQ(notice.source, domain::CommandSource::local);
    EXPECT_EQ(notice.id, 2U);
    EXPECT_EQ(quiesce.poll(0, 1000), application::OutputQuiesceStatus::pending);

    fresh(loop.clock, loop.readings, 100);
    loop.worker.step();
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_FALSE(loop.policy.fault());
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::off);
    EXPECT_EQ(quiesce.poll(100, 1000), application::OutputQuiesceStatus::confirmed_off);
    EXPECT_EQ(quiesce.poll(150, 1000), application::OutputQuiesceStatus::confirmed_off);

    const auto records = drain(path.results);
    ASSERT_GE(records.size(), 2U);
    EXPECT_EQ(records.back().id, 2U);
    EXPECT_EQ(records.back().type, domain::CommandType::stop);
    EXPECT_EQ(records.back().status, domain::CommandStatus::applied);
}

TEST(OutputQuiesce, PreStopOffDoesNotConfirmAndTheDeadlineDoes) {
    Path<4, 8> path;
    MemorySnapshot<domain::Telemetry> telemetry;
    telemetry.publish(sample(5000, domain::AppliedOutput::off));
    application::OutputQuiesce quiesce(path.ingress, telemetry);
    EXPECT_EQ(quiesce.submit(1, 1, 7, 1000, 6000), domain::CommandStatus::accepted);
    EXPECT_EQ(quiesce.poll(1000, 1500), application::OutputQuiesceStatus::pending);
    EXPECT_EQ(quiesce.poll(1499, 1500), application::OutputQuiesceStatus::pending);
    telemetry.publish(sample(5000, domain::AppliedOutput::off));
    EXPECT_EQ(quiesce.poll(1499, 1500), application::OutputQuiesceStatus::pending);
    EXPECT_EQ(quiesce.poll(1500, 1500), application::OutputQuiesceStatus::deadline);
    telemetry.publish(sample(5001, domain::AppliedOutput::off));
    EXPECT_EQ(quiesce.poll(1501, 1500), application::OutputQuiesceStatus::deadline);
}

TEST(OutputQuiesce, UnknownAndStillOnStopTheAttempt) {
    Path<4, 8> path;
    MemorySnapshot<domain::Telemetry> telemetry;
    application::OutputQuiesce unknown_attempt(path.ingress, telemetry);
    EXPECT_EQ(unknown_attempt.submit(1, 1, 7, 0, 5000), domain::CommandStatus::accepted);
    telemetry.publish(sample(1, domain::AppliedOutput::unknown));
    EXPECT_EQ(unknown_attempt.poll(1, 1000), application::OutputQuiesceStatus::unknown);
    telemetry.publish(sample(2, domain::AppliedOutput::off));
    EXPECT_EQ(unknown_attempt.poll(2, 1000), application::OutputQuiesceStatus::unknown);

    application::OutputQuiesce still_on(path.ingress, telemetry);
    EXPECT_EQ(still_on.submit(2, 2, 7, 10, 5000), domain::CommandStatus::accepted);
    telemetry.publish(sample(11, domain::AppliedOutput::on));
    EXPECT_EQ(still_on.poll(11, 1000), application::OutputQuiesceStatus::still_on);
    telemetry.publish(sample(12, domain::AppliedOutput::off));
    EXPECT_EQ(still_on.poll(12, 1000), application::OutputQuiesceStatus::still_on);
}

TEST(OutputQuiesce, RejectedStopIsNotAdmittedEvenIfOutputIsOff) {
    Path<4, 8> path;
    MemorySnapshot<domain::Telemetry> telemetry;
    telemetry.publish(sample(50, domain::AppliedOutput::off));
    application::OutputQuiesce quiesce(path.ingress, telemetry);
    EXPECT_EQ(quiesce.poll(0, 1000), application::OutputQuiesceStatus::not_admitted);
    EXPECT_EQ(quiesce.submit(0, 1, 7, 0, 5000), domain::CommandStatus::rejected_invalid);
    telemetry.publish(sample(60, domain::AppliedOutput::off));
    EXPECT_EQ(quiesce.poll(10, 1000), application::OutputQuiesceStatus::not_admitted);
    EXPECT_EQ(quiesce.submit(1, 1, 7, 10, 10), domain::CommandStatus::rejected_expired);
    EXPECT_EQ(quiesce.poll(10, 1000), application::OutputQuiesceStatus::not_admitted);
    EXPECT_EQ(quiesce.submit(1, 1, 7, 10, 5000), domain::CommandStatus::accepted);
    EXPECT_EQ(quiesce.poll(10, 1000), application::OutputQuiesceStatus::pending);
}

TEST(OutputQuiesce, SecondSubmitDoesNotReplaceAnAcceptedStop) {
    Path<4, 8> path;
    MemorySnapshot<domain::Telemetry> telemetry;
    application::OutputQuiesce quiesce(path.ingress, telemetry);
    EXPECT_EQ(quiesce.submit(4, 1, 7, 0, 5000), domain::CommandStatus::accepted);
    EXPECT_EQ(quiesce.submit(5, 2, 7, 0, 5000), domain::CommandStatus::rejected_invalid);
    application::StopNotice notice{};
    ASSERT_TRUE(path.stops.latest(notice));
    EXPECT_EQ(notice.id, 4U);
    EXPECT_EQ(notice.generation, 1U);
    EXPECT_EQ(notice.source, domain::CommandSource::local);
}

TEST(OutputQuiesce, FailedOffStaysUnconfirmed) {
    Path<4, 8> path;
    FailingLoop loop(path.requests);
    EXPECT_EQ(path.ingress.submit(enable_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 0);
    loop.worker.step();
    ASSERT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::on);

    application::OutputQuiesce quiesce(path.ingress, loop.telemetry);
    EXPECT_EQ(quiesce.submit(2, 2, 7, 0, 5000), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 100);
    loop.worker.step();
    EXPECT_TRUE(loop.policy.fault());
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::unknown);
    EXPECT_EQ(quiesce.poll(100, 1000), application::OutputQuiesceStatus::unknown);
    const auto records = drain(path.results);
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.back().id, 2U);
    EXPECT_EQ(records.back().status, domain::CommandStatus::not_applied);
}

TEST(OutputQuiesce, LatchedFaultDoesNotVetoAnObservedOff) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 200});
    EXPECT_EQ(path.ingress.submit(enable_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 0);
    loop.worker.step();
    ASSERT_TRUE(loop.output.enabled());

    application::OutputQuiesce quiesce(path.ingress, loop.telemetry);
    EXPECT_EQ(quiesce.submit(2, 2, 7, 0, 5000), domain::CommandStatus::accepted);
    loop.clock.time = 250;
    loop.worker.step();
    EXPECT_TRUE(loop.policy.fault());
    EXPECT_FALSE(loop.output.enabled());
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::off);
    EXPECT_EQ(quiesce.poll(250, 1000), application::OutputQuiesceStatus::confirmed_off);
    const auto records = drain(path.results);
    ASSERT_FALSE(records.empty());
    EXPECT_EQ(records.back().id, 2U);
    EXPECT_EQ(records.back().status, domain::CommandStatus::applied);
}

TEST(OutputQuiesce, ConfirmsOffWhenTheCompletionRecordIsDropped) {
    Path<2, 1> path;
    Loop loop(path.requests, {1000, 10000});
    EXPECT_EQ(path.ingress.submit(enable_for(1, 1, 0), 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 0);
    loop.worker.step();
    application::OutputQuiesce quiesce(path.ingress, loop.telemetry);
    EXPECT_EQ(quiesce.submit(2, 2, 7, 0, 5000), domain::CommandStatus::accepted);
    EXPECT_GE(path.ingress.dropped_results(), 1U);
    fresh(loop.clock, loop.readings, 100);
    loop.worker.step();
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::off);
    EXPECT_EQ(quiesce.poll(100, 1000), application::OutputQuiesceStatus::confirmed_off);
    const auto records = drain(path.results);
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].status, domain::CommandStatus::accepted);
    EXPECT_EQ(records[0].id, 1U);
}

TEST(OutputQuiesce, LaterEnableLeavesTheStopUnconfirmed) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    application::OutputQuiesce quiesce(path.ingress, loop.telemetry);
    EXPECT_EQ(quiesce.submit(1, 1, 7, 0, 5000), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(enable_for(2, 2, 0), 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 100);
    loop.worker.step();
    EXPECT_TRUE(loop.output.enabled());
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::on);
    EXPECT_EQ(quiesce.poll(100, 1000), application::OutputQuiesceStatus::still_on);
    bool saw_acceptance = false;
    bool saw_not_applied = false;
    for (const auto& record : drain(path.results)) {
        if (record.id != 1) {
            continue;
        }
        EXPECT_NE(record.status, domain::CommandStatus::applied);
        if (record.status == domain::CommandStatus::accepted) {
            saw_acceptance = true;
        }
        if (record.status == domain::CommandStatus::not_applied) {
            saw_not_applied = true;
        }
    }
    EXPECT_TRUE(saw_acceptance);
    EXPECT_TRUE(saw_not_applied);
}

TEST(OutputQuiesce, OverwrittenStopStaysAcceptedWithoutACompletionRecord) {
    Path<4, 8> path;
    Loop loop(path.requests, {1000, 10000});
    auto first = enable_for(1, 1, 0);
    first.type = domain::CommandType::stop;
    first.enabled = false;
    first.max_duration_ms = 0;
    auto second = first;
    second.id = 2;
    second.sequence = 2;
    EXPECT_EQ(path.ingress.submit(first, 0), domain::CommandStatus::accepted);
    EXPECT_EQ(path.ingress.submit(second, 0), domain::CommandStatus::accepted);
    fresh(loop.clock, loop.readings, 100);
    loop.worker.step();
    EXPECT_EQ(loop.telemetry.value.applied_output, domain::AppliedOutput::off);

    const auto records = drain(path.results);
    int first_records = 0;
    int second_applied = 0;
    for (const auto& record : records) {
        if (record.id == 1) {
            ++first_records;
            EXPECT_EQ(record.status, domain::CommandStatus::accepted);
        }
        if (record.id == 2 && record.status == domain::CommandStatus::applied) {
            ++second_applied;
        }
    }
    EXPECT_EQ(first_records, 1);
    EXPECT_EQ(second_applied, 1);
}
