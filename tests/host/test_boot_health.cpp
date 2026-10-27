#include "domain/boot_health.hpp"

#include <gtest/gtest.h>

namespace {

domain::BootHealth decide(bool control_step_completed, domain::AppliedOutput applied_output,
                          bool broker_reachable) {
    return domain::decide_boot_health(control_step_completed, applied_output, broker_reachable);
}

} // namespace

TEST(BootHealth, HoldsUntilControlCompletesAStep) {
    for (const bool broker_reachable : {false, true}) {
        EXPECT_EQ(decide(false, domain::AppliedOutput::off, broker_reachable),
                  domain::BootHealth::hold);
        EXPECT_EQ(decide(false, domain::AppliedOutput::on, broker_reachable),
                  domain::BootHealth::hold);
        EXPECT_EQ(decide(false, domain::AppliedOutput::unknown, broker_reachable),
                  domain::BootHealth::hold);
    }
}

TEST(BootHealth, ConfirmsACompletedStepOnlyWhenTheAppliedOutputIsOff) {
    EXPECT_EQ(decide(true, domain::AppliedOutput::off, false), domain::BootHealth::confirm);
    EXPECT_EQ(decide(true, domain::AppliedOutput::off, true), domain::BootHealth::confirm);
}

TEST(BootHealth, RefusesACompletedStepWhenTheOutputIsNotOff) {
    for (const bool broker_reachable : {false, true}) {
        EXPECT_EQ(decide(true, domain::AppliedOutput::on, broker_reachable),
                  domain::BootHealth::refuse);
        EXPECT_EQ(decide(true, domain::AppliedOutput::unknown, broker_reachable),
                  domain::BootHealth::refuse);
    }
}

TEST(BootHealth, IgnoresBrokerReachability) {
    const domain::AppliedOutput outputs[] = {
        domain::AppliedOutput::off,
        domain::AppliedOutput::on,
        domain::AppliedOutput::unknown,
    };
    for (const bool control_step_completed : {false, true}) {
        for (const domain::AppliedOutput applied_output : outputs) {
            EXPECT_EQ(decide(control_step_completed, applied_output, false),
                      decide(control_step_completed, applied_output, true));
        }
    }
}
