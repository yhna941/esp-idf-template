#pragma once

#include "application/command_path.hpp"

#include <cstdint>

namespace application {

// Result of one local stop followed by polling applied output.
// confirmed_off is the only safe result. Every other status is unsuccessful.
enum class OutputQuiesceStatus {
    pending,
    confirmed_off,
    still_on,
    unknown,
    deadline,
    not_admitted
};

// Submits one local stop, then polls Telemetry.applied_output.
// Does not stop the network and does not call OTA. The caller polls; this type does not sleep.
class OutputQuiesce {
    public:
    OutputQuiesce(CommandIngress& ingress,
                  const SnapshotReader<domain::Telemetry>& telemetry) noexcept;

    // Arms one local stop through the command path. Source is local.
    // After acceptance, another submit does not publish a second stop.
    // A rejected attempt can be retried on this object until one stop is accepted.
    domain::CommandStatus submit(std::uint64_t id, std::uint32_t sequence, std::uint32_t identity,
                                 domain::Milliseconds now,
                                 domain::Milliseconds expires_at) noexcept;

    // pending while no sample is newer than this attempt and now is before deadline.
    // A qualifying off is confirmed_off. unknown and on are terminal and unsuccessful.
    // A sample published at or before submit, including a pre-stop off, does not count.
    OutputQuiesceStatus poll(domain::Milliseconds now, domain::Milliseconds deadline) noexcept;

    private:
    bool sample_is_new(const domain::Telemetry& telemetry) const noexcept;

    CommandIngress& ingress_;
    const SnapshotReader<domain::Telemetry>& telemetry_;
    domain::Milliseconds submitted_at_{0};
    domain::Milliseconds baseline_uptime_{0};
    bool had_sample_{false};
    bool armed_{false};
    OutputQuiesceStatus terminal_{OutputQuiesceStatus::pending};
};

} // namespace application
