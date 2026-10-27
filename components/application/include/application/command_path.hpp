#pragma once

#include "application/ports/command.hpp"
#include "application/ports/device.hpp"
#include "domain/command.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace application {

// Latest stop. A full command FIFO must not delay this notice.
struct StopNotice {
    std::uint64_t id{0};
    std::uint32_t sequence{0};
    std::uint32_t identity{0};
    domain::CommandSource source{domain::CommandSource::local};
    std::uint32_t generation{0};
    domain::Milliseconds received_at{0};
    domain::Milliseconds expires_at{0};
};

// Command plus the stop generation observed at admission.
struct QueuedCommand {
    domain::Command command{};
    domain::Milliseconds received_at{0};
    std::uint32_t stop_generation{0};
};

static_assert(std::is_trivially_copyable_v<StopNotice>);
static_assert(std::is_trivially_copyable_v<QueuedCommand>);
static_assert(std::is_trivially_copyable_v<domain::CommandResult>);

// Single-threaded admission. Reassemble a remote payload on that same worker
// before calling submit. The FIFO and stop mailbox are the cross-task boundary.
// A full result FIFO rejects a new enable. Duplicate memory and sequence cursors
// reset on reboot; they are not stored.
class CommandIngress {
    public:
    static constexpr std::size_t duplicate_history = 8;

    CommandIngress(BoundedFifo<QueuedCommand>& commands,
                   BoundedFifo<domain::CommandResult>& results, SnapshotWriter<StopNotice>& stops,
                   domain::CommandLimits limits, bool remote_commands_enabled) noexcept;

    domain::CommandStatus submit(const domain::Command& command, domain::Milliseconds now) noexcept;
    bool remote_commands_enabled() const noexcept;
    std::uint32_t dropped_results() const noexcept;

    private:
    bool duplicate(std::uint64_t id) const noexcept;
    void remember(std::uint64_t id) noexcept;
    void push_result(const domain::Command& command, domain::CommandStatus status,
                     domain::Milliseconds now) noexcept;

    BoundedFifo<QueuedCommand>& commands_;
    BoundedFifo<domain::CommandResult>& results_;
    SnapshotWriter<StopNotice>& stops_;
    domain::CommandLimits limits_;
    bool remote_commands_enabled_;
    std::array<std::uint64_t, duplicate_history> seen_ids_{};
    std::size_t seen_count_{0};
    std::size_t seen_next_{0};
    std::uint32_t last_local_sequence_{0};
    std::uint32_t last_remote_sequence_{0};
    std::uint32_t stop_generation_{0};
    std::uint32_t dropped_results_{0};
};

// Turns admitted commands into the boolean TimedOutput already consumes.
// enable_requested and note_outcome are one pair per control step.
// A latched stop is applied only when that step observed AppliedOutput::off.
class CommandRequests final : public RequestSource {
    public:
    CommandRequests(BoundedFifo<QueuedCommand>& commands,
                    BoundedFifo<domain::CommandResult>& results,
                    const SnapshotReader<StopNotice>& stops,
                    domain::Milliseconds max_run_ms) noexcept;

    bool enable_requested(domain::Milliseconds now) noexcept override;
    void
    note_outcome(bool enabled, bool fault, domain::Milliseconds now,
                 domain::AppliedOutput applied = domain::AppliedOutput::unknown) noexcept override;
    std::uint32_t dropped_results() const noexcept;

    private:
    struct Lease {
        bool present{false};
        domain::Milliseconds started_at{0};
        domain::Milliseconds duration_ms{0};
        domain::Milliseconds expires_at{0};
    };

    struct Pending {
        bool present{false};
        bool wanted_on{false};
        domain::Command command{};
    };

    bool lease_is_current(domain::Milliseconds now) const noexcept;
    void release_expired_lease(domain::Milliseconds now) noexcept;
    void observe_stop(domain::Milliseconds now) noexcept;
    void adopt(const domain::Command& command, domain::Milliseconds now) noexcept;
    void arm(const domain::Command& command, bool wanted_on) noexcept;
    void finish_pending(domain::CommandStatus status, domain::Milliseconds now) noexcept;
    void push_result(const domain::Command& command, domain::CommandStatus status,
                     domain::Milliseconds now) noexcept;

    BoundedFifo<QueuedCommand>& commands_;
    BoundedFifo<domain::CommandResult>& results_;
    const SnapshotReader<StopNotice>& stops_;
    domain::Milliseconds max_run_ms_;
    Lease active_{};
    Pending pending_{};
    StopNotice stop_ack_{};
    bool stop_ack_pending_{false};
    std::uint32_t observed_stop_generation_{0};
    std::uint32_t dropped_results_{0};
};

} // namespace application
