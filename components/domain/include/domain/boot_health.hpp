#pragma once

#include "domain/types.hpp"

namespace domain {

// Local decision for a pending boot. This does not select a slot or parse an image.
enum class BootHealth {
    hold,
    confirm,
    refuse
};

// Confirm only after control has completed a step and the applied output is off.
// Hold while that evidence is missing. Refuse a completed step whose output is on
// or unknown. broker_reachable is accepted and ignored: broker reachability is not
// a health signal.
BootHealth decide_boot_health(bool control_step_completed, AppliedOutput applied_output,
                              bool broker_reachable) noexcept;

} // namespace domain
