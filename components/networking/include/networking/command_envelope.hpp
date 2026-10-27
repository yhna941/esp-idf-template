#pragma once

#include "domain/command.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace application {

class CommandIngress;

} // namespace application

namespace networking {

// One reassembled command payload. The decoder does not join MQTT fragments.
inline constexpr std::size_t command_envelope_max_bytes = 512;

// Why a payload did not become a later control outcome.
// submitted means CommandIngress::submit ran; admission is that call's status.
// The other reasons do not call submit and do not use the result FIFO.
enum class CommandEnvelopeReason {
    submitted,
    rejected_oversize,
    rejected_malformed,
    rejected_unknown_version,
    rejected_expired,
};

struct CommandEnvelopeResult {
    CommandEnvelopeReason reason{CommandEnvelopeReason::rejected_malformed};
    // Meaningful only when reason is submitted.
    domain::CommandStatus admission{domain::CommandStatus::rejected_invalid};
};

// Map one schema 1 command object onto the local ingress.
// now is the device monotonic clock. wall_offset_ms is trusted wall-clock
// milliseconds minus now, so a wall instant becomes monotonic by subtraction.
// identity and sequence are chosen by the caller after authentication; they are
// not payload fields. The only effect is CommandIngress::submit with
// CommandSource::remote.
//
// Call this from the single thread that owns submit, after that worker has
// reassembled the payload. Checks run in order: oversize, malformed, unknown
// version, expired, then submit. A full result FIFO rejects a new enable inside
// submit. Call only after the wall clock is trusted; this function does not
// read a clock itself.
CommandEnvelopeResult decode_command_envelope(std::string_view payload, domain::Milliseconds now,
                                              domain::Milliseconds wall_offset_ms,
                                              std::uint32_t identity, std::uint32_t sequence,
                                              application::CommandIngress& ingress) noexcept;

} // namespace networking
