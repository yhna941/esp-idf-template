#pragma once

#include "networking/connections.hpp"
#include "networking/lifecycle.hpp"

namespace networking {

// Wall clock for TLS certificate checks. Local safety does not read this clock.
// begin() and stop() belong to the network worker. stop() leaves the phase
// started when the session is still enabled. ready() does not apply the
// bring-up deadline; NetworkService does that on the monotonic clock.
class SntpTime final : public NetworkTime {
    public:
    void begin() noexcept override;
    void stop() noexcept override;
    bool ready() const noexcept override;

    private:
    SntpPhase phase_{SntpPhase::idle};
};

} // namespace networking
