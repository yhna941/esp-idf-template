#pragma once

#include "application/ports/device.hpp"
#include "configuration/network_config.hpp"

namespace networking {

// Station link used by the network worker. start() returns false only after
// service-owned resources are released, or leaves them recorded so stop() can
// finish that release. stop() is safe when start() never succeeded and safe to
// call again. After stop(), connected() is false.
class WifiConnection {
    public:
    virtual ~WifiConnection() = default;
    virtual bool start(const configuration::NetworkConfig& config) noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual void step() noexcept = 0;
    virtual bool connected() const noexcept = 0;
};

class MqttConnection : public application::TelemetrySink {
    public:
    virtual bool start(const configuration::NetworkConfig& config) noexcept = 0;
    // Idempotent. Drops the client so a later start() can create another one.
    // A stop or destroy the SDK refuses leaves that client in place.
    virtual void stop() noexcept = 0;
    virtual void drop_report() noexcept = 0;
    // Network worker. Publishes retained availability after a connection is
    // recorded. The MQTT task callback must not enqueue.
    virtual void publish_availability() noexcept = 0;
};

class NetworkTime {
    public:
    virtual ~NetworkTime() = default;
    // Starts a session unless one is already started. A session left running
    // because stop() did not finish is not started again.
    virtual void begin() noexcept = 0;
    // Returns to idle only when the session has actually stopped.
    virtual void stop() noexcept = 0;
    // Wall-clock readiness for TLS. This can stay true after stop(); stopping
    // SNTP does not roll the clock backwards. A clock that never becomes ready
    // is bounded by NetworkService, not by this check.
    virtual bool ready() const noexcept = 0;
};

} // namespace networking
