#include "application/workers.hpp"

namespace application {

void SensorWorker::step() noexcept {
    readings_.publish(sensor_.read(clock_.now()));
}

void ControlWorker::step() noexcept {
    domain::Reading reading{};
    if (!readings_.latest(reading)) {
        reading.valid = false;
    }
    const auto now = clock_.now();
    const bool requested = requests_.enable_requested(now);
    const bool enabled = policy_.update(requested, reading, now);
    auto error = output_.set_enabled(enabled);
    auto applied = enabled ? domain::AppliedOutput::on : domain::AppliedOutput::off;
    if (error != domain::OutputError::none) {
        policy_.trip_fault();
        // Best-effort stop. Failure must never be reported as a confirmed OFF.
        const auto stop_error = output_.set_enabled(false);
        applied = stop_error == domain::OutputError::none ? domain::AppliedOutput::off
                                                          : domain::AppliedOutput::unknown;
        if (stop_error != domain::OutputError::none) {
            error = stop_error;
        }
    }
    const bool output_on = enabled && error == domain::OutputError::none;
    // The boolean level is false for both a successful OFF and a failed OFF.
    // The applied acknowledgement is what keeps a failed OFF unconfirmed.
    requests_.note_outcome(output_on, policy_.fault(), now, applied);
    telemetry_.publish({now, requested, applied, policy_.fault(), error, reading});
}

void ReporterWorker::step() noexcept {
    domain::Telemetry telemetry{};
    if (telemetry_.latest(telemetry)) {
        sink_.report(telemetry);
    }
}

} // namespace application
