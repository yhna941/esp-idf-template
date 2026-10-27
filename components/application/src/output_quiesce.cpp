#include "application/output_quiesce.hpp"

namespace application {

OutputQuiesce::OutputQuiesce(CommandIngress& ingress,
                             const SnapshotReader<domain::Telemetry>& telemetry) noexcept
    : ingress_(ingress), telemetry_(telemetry) {
}

domain::CommandStatus OutputQuiesce::submit(std::uint64_t id, std::uint32_t sequence,
                                            std::uint32_t identity, domain::Milliseconds now,
                                            domain::Milliseconds expires_at) noexcept {
    if (armed_) {
        return domain::CommandStatus::rejected_invalid;
    }
    domain::Command command{};
    command.id = id;
    command.sequence = sequence;
    command.identity = identity;
    command.type = domain::CommandType::stop;
    command.source = domain::CommandSource::local;
    command.enabled = false;
    command.expires_at = expires_at;
    command.max_duration_ms = 0;
    const auto status = ingress_.submit(command, now);
    if (status != domain::CommandStatus::accepted) {
        return status;
    }
    armed_ = true;
    submitted_at_ = now;
    terminal_ = OutputQuiesceStatus::pending;
    domain::Telemetry existing{};
    had_sample_ = telemetry_.latest(existing);
    baseline_uptime_ = had_sample_ ? existing.uptime : 0;
    return status;
}

OutputQuiesceStatus OutputQuiesce::poll(domain::Milliseconds now,
                                        domain::Milliseconds deadline) noexcept {
    if (!armed_) {
        return OutputQuiesceStatus::not_admitted;
    }
    if (terminal_ != OutputQuiesceStatus::pending) {
        return terminal_;
    }
    domain::Telemetry telemetry{};
    if (telemetry_.latest(telemetry) && sample_is_new(telemetry)) {
        switch (telemetry.applied_output) {
        case domain::AppliedOutput::off:
            terminal_ = OutputQuiesceStatus::confirmed_off;
            break;
        case domain::AppliedOutput::unknown:
            terminal_ = OutputQuiesceStatus::unknown;
            break;
        case domain::AppliedOutput::on:
            terminal_ = OutputQuiesceStatus::still_on;
            break;
        }
        return terminal_;
    }
    if (now >= deadline) {
        terminal_ = OutputQuiesceStatus::deadline;
        return terminal_;
    }
    return OutputQuiesceStatus::pending;
}

bool OutputQuiesce::sample_is_new(const domain::Telemetry& telemetry) const noexcept {
    if (telemetry.uptime <= submitted_at_) {
        return false;
    }
    if (had_sample_ && telemetry.uptime <= baseline_uptime_) {
        return false;
    }
    return true;
}

} // namespace application
