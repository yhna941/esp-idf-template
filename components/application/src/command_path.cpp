#include "application/command_path.hpp"

#include <limits>

namespace application {

namespace {

domain::CommandStatus rejection_status(domain::CommandValidation validation) noexcept {
    switch (validation) {
    case domain::CommandValidation::invalid:
        return domain::CommandStatus::rejected_invalid;
    case domain::CommandValidation::expired:
        return domain::CommandStatus::rejected_expired;
    case domain::CommandValidation::unauthorized:
        return domain::CommandStatus::rejected_unauthorized;
    case domain::CommandValidation::ok:
        return domain::CommandStatus::accepted;
    }
    return domain::CommandStatus::rejected_invalid;
}

domain::CommandResult make_result(const domain::Command& command, domain::CommandStatus status,
                                  domain::Milliseconds at) noexcept {
    return {command.id, command.sequence, command.identity, command.type, command.source, status,
            at};
}

// applied only for an observed OFF. A failed write leaves the boolean level false and
// the acknowledgement unknown; that pair must not be recorded as applied.
domain::CommandStatus stop_status(bool output_on, bool fault,
                                  domain::AppliedOutput applied) noexcept {
    if (fault && applied != domain::AppliedOutput::off) {
        return domain::CommandStatus::not_applied;
    }
    if (applied == domain::AppliedOutput::off && !output_on) {
        return domain::CommandStatus::applied;
    }
    return domain::CommandStatus::not_applied;
}

} // namespace

CommandIngress::CommandIngress(BoundedFifo<QueuedCommand>& commands,
                               BoundedFifo<domain::CommandResult>& results,
                               SnapshotWriter<StopNotice>& stops, domain::CommandLimits limits,
                               bool remote_commands_enabled) noexcept
    : commands_(commands), results_(results), stops_(stops), limits_(limits),
      remote_commands_enabled_(remote_commands_enabled) {
}

domain::CommandStatus CommandIngress::submit(const domain::Command& command,
                                             domain::Milliseconds now) noexcept {
    const auto validation =
        domain::validate_command(command, now, limits_, remote_commands_enabled_);
    if (validation != domain::CommandValidation::ok) {
        const auto status = rejection_status(validation);
        push_result(command, status, now);
        return status;
    }
    if (duplicate(command.id)) {
        push_result(command, domain::CommandStatus::rejected_duplicate, now);
        return domain::CommandStatus::rejected_duplicate;
    }
    auto& last_sequence = command.source == domain::CommandSource::remote ? last_remote_sequence_
                                                                          : last_local_sequence_;
    if (command.sequence <= last_sequence) {
        push_result(command, domain::CommandStatus::rejected_stale_sequence, now);
        return domain::CommandStatus::rejected_stale_sequence;
    }

    if (command.type == domain::CommandType::stop) {
        if (stop_generation_ == std::numeric_limits<std::uint32_t>::max()) {
            push_result(command, domain::CommandStatus::rejected_invalid, now);
            return domain::CommandStatus::rejected_invalid;
        }
        ++stop_generation_;
        // One slot. A newer stop replaces this notice before the control loop sees it.
        // The replaced command stays accepted and gets no completion record: this mailbox
        // does not keep the replaced id, and a second completion write would race the
        // control task. accepted is not a confirmed OFF. See ADR 0008.
        stops_.publish({command.id, command.sequence, command.identity, command.source,
                        stop_generation_, now, command.expires_at});
        remember(command.id);
        last_sequence = command.sequence;
        push_result(command, domain::CommandStatus::accepted, now);
        return domain::CommandStatus::accepted;
    }

    if (results_.size() >= results_.capacity() || commands_.size() >= commands_.capacity()) {
        push_result(command, domain::CommandStatus::rejected_full, now);
        return domain::CommandStatus::rejected_full;
    }
    const QueuedCommand queued{command, now, stop_generation_};
    if (!commands_.try_push(queued)) {
        push_result(command, domain::CommandStatus::rejected_full, now);
        return domain::CommandStatus::rejected_full;
    }
    remember(command.id);
    last_sequence = command.sequence;
    push_result(command, domain::CommandStatus::accepted, now);
    return domain::CommandStatus::accepted;
}

bool CommandIngress::remote_commands_enabled() const noexcept {
    return remote_commands_enabled_;
}

std::uint32_t CommandIngress::dropped_results() const noexcept {
    return dropped_results_;
}

bool CommandIngress::duplicate(std::uint64_t id) const noexcept {
    for (std::size_t index = 0; index < seen_count_; ++index) {
        if (seen_ids_[index] == id) {
            return true;
        }
    }
    return false;
}

void CommandIngress::remember(std::uint64_t id) noexcept {
    if (seen_count_ < seen_ids_.size()) {
        seen_ids_[seen_count_++] = id;
        return;
    }
    seen_ids_[seen_next_] = id;
    seen_next_ = (seen_next_ + 1) % seen_ids_.size();
}

void CommandIngress::push_result(const domain::Command& command, domain::CommandStatus status,
                                 domain::Milliseconds now) noexcept {
    if (!results_.try_push(make_result(command, status, now))) {
        ++dropped_results_;
    }
}

CommandRequests::CommandRequests(BoundedFifo<QueuedCommand>& commands,
                                 BoundedFifo<domain::CommandResult>& results,
                                 const SnapshotReader<StopNotice>& stops,
                                 domain::Milliseconds max_run_ms) noexcept
    : commands_(commands), results_(results), stops_(stops), max_run_ms_(max_run_ms) {
}

bool CommandRequests::enable_requested(domain::Milliseconds now) noexcept {
    if (pending_.present) {
        finish_pending(domain::CommandStatus::superseded, now);
    }
    observe_stop(now);
    release_expired_lease(now);

    QueuedCommand queued{};
    while (commands_.try_pop(queued)) {
        if (queued.stop_generation < observed_stop_generation_) {
            push_result(queued.command, domain::CommandStatus::dropped_by_stop, now);
            continue;
        }
        if (queued.command.expires_at <= now) {
            push_result(queued.command, domain::CommandStatus::expired_before_apply, now);
            continue;
        }
        if (queued.command.type != domain::CommandType::set_output) {
            push_result(queued.command, domain::CommandStatus::rejected_invalid, now);
            continue;
        }
        if (pending_.present) {
            finish_pending(domain::CommandStatus::superseded, now);
        }
        if (!queued.command.enabled) {
            active_.present = false;
            arm(queued.command, false);
            continue;
        }
        adopt(queued.command, now);
        arm(queued.command, lease_is_current(now));
    }

    release_expired_lease(now);
    if (pending_.present && pending_.wanted_on && !lease_is_current(now)) {
        pending_.wanted_on = false;
    }
    return lease_is_current(now);
}

void CommandRequests::note_outcome(bool enabled, bool fault, domain::Milliseconds now,
                                   domain::AppliedOutput applied) noexcept {
    if (stop_ack_pending_) {
        domain::Command stop{};
        stop.id = stop_ack_.id;
        stop.sequence = stop_ack_.sequence;
        stop.identity = stop_ack_.identity;
        stop.type = domain::CommandType::stop;
        stop.source = stop_ack_.source;
        push_result(stop, stop_status(enabled, fault, applied), now);
        stop_ack_pending_ = false;
    }
    if (!pending_.present) {
        return;
    }
    domain::CommandStatus status = domain::CommandStatus::applied;
    if (pending_.wanted_on) {
        status = enabled ? domain::CommandStatus::applied : domain::CommandStatus::not_applied;
        if (!enabled) {
            active_.present = false;
        }
    } else if (enabled) {
        status = domain::CommandStatus::not_applied;
    }
    finish_pending(status, now);
}

std::uint32_t CommandRequests::dropped_results() const noexcept {
    return dropped_results_;
}

bool CommandRequests::lease_is_current(domain::Milliseconds now) const noexcept {
    if (!active_.present || now < active_.started_at || now >= active_.expires_at) {
        return false;
    }
    const auto elapsed = now - active_.started_at;
    if (elapsed >= active_.duration_ms || max_run_ms_ <= 1 || elapsed >= max_run_ms_ - 1) {
        return false;
    }
    return true;
}

void CommandRequests::release_expired_lease(domain::Milliseconds now) noexcept {
    if (active_.present && !lease_is_current(now)) {
        active_.present = false;
    }
}

void CommandRequests::observe_stop(domain::Milliseconds now) noexcept {
    StopNotice notice{};
    if (!stops_.latest(notice) || notice.generation == observed_stop_generation_) {
        return;
    }
    observed_stop_generation_ = notice.generation;
    active_.present = false;
    if (pending_.present) {
        finish_pending(domain::CommandStatus::dropped_by_stop, now);
    }
    stop_ack_ = notice;
    stop_ack_pending_ = true;
}

void CommandRequests::adopt(const domain::Command& command, domain::Milliseconds now) noexcept {
    if (!active_.present || now < active_.started_at) {
        active_.present = true;
        active_.started_at = now;
        active_.duration_ms = command.max_duration_ms;
        active_.expires_at = command.expires_at;
        return;
    }
    const auto current_end = active_.started_at + active_.duration_ms;
    const auto proposed_end = now + command.max_duration_ms;
    if (proposed_end < current_end) {
        active_.duration_ms = proposed_end - active_.started_at;
    }
    if (command.expires_at < active_.expires_at) {
        active_.expires_at = command.expires_at;
    }
}

void CommandRequests::arm(const domain::Command& command, bool wanted_on) noexcept {
    pending_.present = true;
    pending_.wanted_on = wanted_on;
    pending_.command = command;
}

void CommandRequests::finish_pending(domain::CommandStatus status,
                                     domain::Milliseconds now) noexcept {
    if (!pending_.present) {
        return;
    }
    push_result(pending_.command, status, now);
    pending_.present = false;
}

void CommandRequests::push_result(const domain::Command& command, domain::CommandStatus status,
                                  domain::Milliseconds now) noexcept {
    if (!results_.try_push(make_result(command, status, now))) {
        ++dropped_results_;
    }
}

} // namespace application
