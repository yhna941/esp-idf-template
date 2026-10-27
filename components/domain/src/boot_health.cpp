#include "domain/boot_health.hpp"

namespace domain {

BootHealth decide_boot_health(bool control_step_completed, AppliedOutput applied_output,
                              bool broker_reachable) noexcept {
    // The flag is part of the signature so a caller cannot drop it. It does not
    // select hold, confirm, or refuse.
    (void)broker_reachable;
    if (!control_step_completed) {
        return BootHealth::hold;
    }
    switch (applied_output) {
    case AppliedOutput::off:
        return BootHealth::confirm;
    case AppliedOutput::on:
    case AppliedOutput::unknown:
        return BootHealth::refuse;
    }
    return BootHealth::refuse;
}

} // namespace domain
