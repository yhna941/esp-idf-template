#include "networking/sntp_time.hpp"

#include "esp_log.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

#include <ctime>

namespace networking {

#if CONFIG_ESPKIT_NETWORK_ENABLED
namespace {

constexpr char tag[] = "sntp";

} // namespace
#endif

void SntpTime::begin() noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    // A session left started by a failed stop must not be initialized again.
    if (sntp_is_started(phase_)) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    if (!esp_sntp_enabled()) {
        ESP_LOGW(tag, "esp_sntp_init left the session disabled");
        return;
    }
    phase_ = SntpPhase::started;
#endif
}

void SntpTime::stop() noexcept {
#if CONFIG_ESPKIT_NETWORK_ENABLED
    if (!sntp_is_started(phase_)) {
        return;
    }
    esp_sntp_stop();
    phase_ = sntp_phase_after_stop(phase_, esp_sntp_enabled());
    if (sntp_is_started(phase_)) {
        ESP_LOGW(tag, "esp_sntp_stop left the session enabled");
    }
#endif
}

bool SntpTime::ready() const noexcept {
    // Certificate checks use wall time; local safety uses the monotonic clock.
    constexpr std::time_t minimum_tls_time = 1704067200; // 2024-01-01 UTC
    return std::time(nullptr) >= minimum_tls_time;
}

} // namespace networking
