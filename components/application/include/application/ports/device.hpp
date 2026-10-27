#pragma once

#include "domain/types.hpp"

namespace application {

class Clock {
    public:
    virtual ~Clock() = default;
    virtual domain::Milliseconds now() const noexcept = 0;
};

class SensorSource {
    public:
    virtual ~SensorSource() = default;
    virtual domain::Reading read(domain::Milliseconds now) noexcept = 0;
};

class OutputSink {
    public:
    virtual ~OutputSink() = default;
    virtual domain::OutputError set_enabled(bool enabled) noexcept = 0;
};

// Polled by the control worker. Must be non-blocking and must not bypass local safety rules.
// note_outcome runs after TimedOutput and the output adapter, once per control step.
// `applied` is that step's acknowledgement. A stop is confirmed only when it is off.
// The default unknown does not confirm a stop.
class RequestSource {
    public:
    virtual ~RequestSource() = default;
    virtual bool enable_requested(domain::Milliseconds now) noexcept = 0;

    virtual void
    note_outcome(bool enabled, bool fault, domain::Milliseconds now,
                 domain::AppliedOutput applied = domain::AppliedOutput::unknown) noexcept {
        (void)enabled;
        (void)fault;
        (void)now;
        (void)applied;
    }
};

template <typename T> class SnapshotReader {
    public:
    virtual ~SnapshotReader() = default;
    virtual bool latest(T& value) const noexcept = 0;
};

template <typename T> class SnapshotWriter {
    public:
    virtual ~SnapshotWriter() = default;
    virtual void publish(const T& value) noexcept = 0;
};

class TelemetrySink {
    public:
    virtual ~TelemetrySink() = default;
    virtual void report(const domain::Telemetry& value) noexcept = 0;
};

class Work {
    public:
    virtual ~Work() = default;
    virtual void step() noexcept = 0;
};

} // namespace application
