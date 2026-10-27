#include "networking/command_envelope.hpp"

#include "application/command_path.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <string_view>

namespace networking {

namespace {

constexpr int max_json_depth = 8;

enum class ParseStatus {
    ok,
    malformed,
    unknown_version,
};

enum class ValueKind {
    absent,
    integer,
    string,
    boolean,
    null_value,
    nested,
    non_integer,
};

enum class Field : std::uint8_t {
    schema,
    command_id,
    type,
    enabled,
    issued_at,
    expires_at,
    max_duration,
    count
};

struct FieldValue {
    bool present{false};
    ValueKind kind{ValueKind::absent};
    std::int64_t integer{0};
    bool boolean{false};
    std::string_view text{};
};

struct Parsed {
    std::uint64_t id{0};
    domain::CommandType type{domain::CommandType::set_output};
    bool enabled{false};
    std::int64_t issued_at{0};
    std::int64_t expires_at{0};
    domain::Milliseconds max_duration_ms{0};
};

class Cursor {
    public:
    explicit Cursor(std::string_view payload) noexcept : in_(payload) {
    }

    bool done() const noexcept {
        return index_ >= in_.size();
    }

    char peek() const noexcept {
        return in_[index_];
    }

    bool eat(char expected) noexcept {
        if (done() || in_[index_] != expected) {
            return false;
        }
        ++index_;
        return true;
    }

    bool consume(std::string_view literal) noexcept {
        if (in_.size() - index_ < literal.size() || in_.substr(index_, literal.size()) != literal) {
            return false;
        }
        index_ += literal.size();
        return true;
    }

    void skip_ws() noexcept {
        while (!done()) {
            const char c = in_[index_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++index_;
        }
    }

    bool parse_string(std::string_view& out) noexcept {
        if (!eat('"')) {
            return false;
        }
        const std::size_t begin = index_;
        while (!done()) {
            const auto c = static_cast<unsigned char>(in_[index_]);
            if (c == '"') {
                out = in_.substr(begin, index_ - begin);
                ++index_;
                return true;
            }
            if (c < 0x20 || c == '\\') {
                return false;
            }
            ++index_;
        }
        return false;
    }

    bool parse_bool(FieldValue& value) noexcept {
        if (consume("true")) {
            value.present = true;
            value.kind = ValueKind::boolean;
            value.boolean = true;
            return true;
        }
        if (consume("false")) {
            value.present = true;
            value.kind = ValueKind::boolean;
            value.boolean = false;
            return true;
        }
        return false;
    }

    bool parse_number(FieldValue& value) noexcept {
        bool negative = false;
        if (!done() && peek() == '-') {
            negative = true;
            ++index_;
        }
        if (done() || !std::isdigit(static_cast<unsigned char>(peek()))) {
            return false;
        }
        std::uint64_t magnitude = 0;
        bool overflow = false;
        if (peek() == '0') {
            ++index_;
            if (!done() && std::isdigit(static_cast<unsigned char>(peek()))) {
                return false;
            }
        } else {
            while (!done() && std::isdigit(static_cast<unsigned char>(peek()))) {
                const auto digit = static_cast<unsigned>(peek() - '0');
                if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                    overflow = true;
                } else {
                    magnitude = magnitude * 10 + digit;
                }
                ++index_;
            }
        }
        bool pure_integer = true;
        if (!done() && peek() == '.') {
            pure_integer = false;
            ++index_;
            if (done() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return false;
            }
            while (!done() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++index_;
            }
        }
        if (!done() && (peek() == 'e' || peek() == 'E')) {
            pure_integer = false;
            ++index_;
            if (!done() && (peek() == '+' || peek() == '-')) {
                ++index_;
            }
            if (done() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                return false;
            }
            while (!done() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++index_;
            }
        }
        value.present = true;
        if (!pure_integer || overflow || !store_integer(negative, magnitude, value)) {
            value.kind = ValueKind::non_integer;
            return true;
        }
        value.kind = ValueKind::integer;
        return true;
    }

    bool parse_field_value(FieldValue& value) noexcept {
        if (done()) {
            return false;
        }
        const char c = peek();
        if (c == '"') {
            value.present = true;
            value.kind = ValueKind::string;
            return parse_string(value.text);
        }
        if (c == 't' || c == 'f') {
            return parse_bool(value);
        }
        if (c == 'n') {
            if (!consume("null")) {
                return false;
            }
            value.present = true;
            value.kind = ValueKind::null_value;
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number(value);
        }
        if (c == '{' || c == '[') {
            value.present = true;
            value.kind = ValueKind::nested;
            return skip_value(2);
        }
        return false;
    }

    private:
    static bool store_integer(bool negative, std::uint64_t magnitude, FieldValue& value) noexcept {
        const auto max_magnitude =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (negative) {
            if (magnitude > max_magnitude + 1) {
                return false;
            }
            if (magnitude == max_magnitude + 1) {
                value.integer = std::numeric_limits<std::int64_t>::min();
                return true;
            }
            value.integer = -static_cast<std::int64_t>(magnitude);
            return true;
        }
        if (magnitude > max_magnitude) {
            return false;
        }
        value.integer = static_cast<std::int64_t>(magnitude);
        return true;
    }

    bool skip_value(int depth) noexcept {
        if (depth > max_json_depth || done()) {
            return false;
        }
        const char c = peek();
        if (c == '"') {
            std::string_view ignored;
            return parse_string(ignored);
        }
        if (c == '{') {
            return skip_container(depth, '{', '}');
        }
        if (c == '[') {
            return skip_container(depth, '[', ']');
        }
        if (c == 't') {
            return consume("true");
        }
        if (c == 'f') {
            return consume("false");
        }
        if (c == 'n') {
            return consume("null");
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            FieldValue ignored;
            return parse_number(ignored);
        }
        return false;
    }

    bool skip_container(int depth, char open, char close) noexcept {
        if (!eat(open)) {
            return false;
        }
        skip_ws();
        if (eat(close)) {
            return true;
        }
        const bool object = open == '{';
        for (;;) {
            if (object) {
                std::string_view key;
                if (!parse_string(key)) {
                    return false;
                }
                skip_ws();
                if (!eat(':')) {
                    return false;
                }
                skip_ws();
            }
            if (!skip_value(depth + 1)) {
                return false;
            }
            skip_ws();
            if (eat(',')) {
                skip_ws();
                continue;
            }
            return eat(close);
        }
    }

    std::string_view in_;
    std::size_t index_{0};
};

Field lookup(std::string_view key) noexcept {
    if (key == "schema_version") {
        return Field::schema;
    }
    if (key == "command_id") {
        return Field::command_id;
    }
    if (key == "type") {
        return Field::type;
    }
    if (key == "enabled") {
        return Field::enabled;
    }
    if (key == "issued_at") {
        return Field::issued_at;
    }
    if (key == "expires_at") {
        return Field::expires_at;
    }
    if (key == "max_duration_ms") {
        return Field::max_duration;
    }
    return Field::count;
}

bool parse_hex_id(std::string_view text, std::uint64_t& id) noexcept {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    id = value;
    return true;
}

bool read_digits(std::string_view text, std::size_t at, int count, int& value) noexcept {
    value = 0;
    for (int index = 0; index < count; ++index) {
        const char c = text[at + static_cast<std::size_t>(index)];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    return true;
}

bool leap_year(int year) noexcept {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

// Days since 1970-01-01. Howard Hinnant's civil-from-days companion.
std::int64_t days_from_civil(int year, int month, int day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(year - era * 400);
    const auto day_of_year =
        static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

bool parse_utc(std::string_view text, std::int64_t& wall_ms) noexcept {
    int fraction_digits = 0;
    if (text.size() == 20) {
        fraction_digits = 0;
    } else if (text.size() >= 22 && text.size() <= 24 && text[19] == '.') {
        fraction_digits = static_cast<int>(text.size() - 21);
    } else {
        return false;
    }
    if (text.back() != 'Z' || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':') {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int fraction = 0;
    if (!read_digits(text, 0, 4, year) || !read_digits(text, 5, 2, month) ||
        !read_digits(text, 8, 2, day) || !read_digits(text, 11, 2, hour) ||
        !read_digits(text, 14, 2, minute) || !read_digits(text, 17, 2, second)) {
        return false;
    }
    if (fraction_digits > 0 && !read_digits(text, 20, fraction_digits, fraction)) {
        return false;
    }
    if (year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || hour > 23 ||
        minute > 59 || second > 59) {
        return false;
    }
    constexpr unsigned days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned day_limit = days_in_month[month];
    if (month == 2 && leap_year(year)) {
        day_limit = 29;
    }
    if (static_cast<unsigned>(day) > day_limit) {
        return false;
    }
    int millis = fraction;
    if (fraction_digits == 1) {
        millis = fraction * 100;
    } else if (fraction_digits == 2) {
        millis = fraction * 10;
    }
    const auto days = days_from_civil(year, month, day);
    wall_ms = days * 86400000 + static_cast<std::int64_t>(hour) * 3600000 +
              static_cast<std::int64_t>(minute) * 60000 + static_cast<std::int64_t>(second) * 1000 +
              millis;
    return true;
}

ParseStatus interpret(const FieldValue* fields, bool unknown_key, Parsed& out) noexcept {
    const FieldValue& schema = fields[static_cast<std::size_t>(Field::schema)];
    if (!schema.present || schema.kind != ValueKind::integer) {
        return ParseStatus::malformed;
    }
    if (schema.integer != 1) {
        return ParseStatus::unknown_version;
    }
    if (unknown_key) {
        return ParseStatus::malformed;
    }
    const FieldValue& id = fields[static_cast<std::size_t>(Field::command_id)];
    const FieldValue& type = fields[static_cast<std::size_t>(Field::type)];
    const FieldValue& enabled = fields[static_cast<std::size_t>(Field::enabled)];
    const FieldValue& issued = fields[static_cast<std::size_t>(Field::issued_at)];
    const FieldValue& expires = fields[static_cast<std::size_t>(Field::expires_at)];
    const FieldValue& duration = fields[static_cast<std::size_t>(Field::max_duration)];
    if (!id.present || id.kind != ValueKind::string || !type.present ||
        type.kind != ValueKind::string || !issued.present || issued.kind != ValueKind::string ||
        !expires.present || expires.kind != ValueKind::string || !parse_hex_id(id.text, out.id)) {
        return ParseStatus::malformed;
    }
    if (type.text == "set_output") {
        out.type = domain::CommandType::set_output;
        if (!enabled.present || enabled.kind != ValueKind::boolean || !duration.present ||
            duration.kind != ValueKind::integer || duration.integer < 0) {
            return ParseStatus::malformed;
        }
        out.enabled = enabled.boolean;
        out.max_duration_ms = duration.integer;
    } else if (type.text == "stop") {
        out.type = domain::CommandType::stop;
        if (enabled.present || duration.present) {
            return ParseStatus::malformed;
        }
        out.enabled = false;
        out.max_duration_ms = 0;
    } else {
        return ParseStatus::malformed;
    }
    if (!parse_utc(issued.text, out.issued_at) || !parse_utc(expires.text, out.expires_at) ||
        out.issued_at > out.expires_at) {
        return ParseStatus::malformed;
    }
    return ParseStatus::ok;
}

bool parse_envelope(std::string_view payload, Parsed& parsed, ParseStatus& status) noexcept {
    Cursor cursor(payload);
    cursor.skip_ws();
    if (!cursor.eat('{')) {
        return false;
    }
    FieldValue fields[static_cast<std::size_t>(Field::count)]{};
    bool unknown_key = false;
    cursor.skip_ws();
    if (!cursor.eat('}')) {
        for (;;) {
            cursor.skip_ws();
            std::string_view key;
            if (!cursor.parse_string(key)) {
                return false;
            }
            cursor.skip_ws();
            if (!cursor.eat(':')) {
                return false;
            }
            cursor.skip_ws();
            FieldValue value;
            if (!cursor.parse_field_value(value)) {
                return false;
            }
            const Field field = lookup(key);
            if (field == Field::count) {
                unknown_key = true;
            } else {
                FieldValue& slot = fields[static_cast<std::size_t>(field)];
                if (slot.present) {
                    return false;
                }
                slot = value;
            }
            cursor.skip_ws();
            if (cursor.eat(',')) {
                continue;
            }
            if (!cursor.eat('}')) {
                return false;
            }
            break;
        }
    }
    cursor.skip_ws();
    if (!cursor.done()) {
        return false;
    }
    status = interpret(fields, unknown_key, parsed);
    return true;
}

bool to_monotonic(std::int64_t wall_ms, std::int64_t offset_ms, std::int64_t& monotonic) noexcept {
    // monotonic = wall_ms - offset_ms. The comparisons keep the subtraction in range.
    if (offset_ms >= 0) {
        if (wall_ms < std::numeric_limits<std::int64_t>::min() + offset_ms) {
            return false;
        }
    } else if (wall_ms > std::numeric_limits<std::int64_t>::max() + offset_ms) {
        return false;
    }
    monotonic = wall_ms - offset_ms;
    return true;
}

} // namespace

CommandEnvelopeResult decode_command_envelope(std::string_view payload, domain::Milliseconds now,
                                              domain::Milliseconds wall_offset_ms,
                                              std::uint32_t identity, std::uint32_t sequence,
                                              application::CommandIngress& ingress) noexcept {
    CommandEnvelopeResult result{};
    if (payload.size() > command_envelope_max_bytes) {
        result.reason = CommandEnvelopeReason::rejected_oversize;
        return result;
    }
    Parsed parsed{};
    ParseStatus status = ParseStatus::malformed;
    if (!parse_envelope(payload, parsed, status)) {
        result.reason = CommandEnvelopeReason::rejected_malformed;
        return result;
    }
    if (status == ParseStatus::unknown_version) {
        result.reason = CommandEnvelopeReason::rejected_unknown_version;
        return result;
    }
    if (status != ParseStatus::ok) {
        result.reason = CommandEnvelopeReason::rejected_malformed;
        return result;
    }
    domain::Milliseconds monotonic_expires = 0;
    if (!to_monotonic(parsed.expires_at, wall_offset_ms, monotonic_expires)) {
        result.reason = CommandEnvelopeReason::rejected_malformed;
        return result;
    }
    if (monotonic_expires <= now) {
        result.reason = CommandEnvelopeReason::rejected_expired;
        return result;
    }
    domain::Command command{};
    command.id = parsed.id;
    command.sequence = sequence;
    command.identity = identity;
    command.type = parsed.type;
    command.source = domain::CommandSource::remote;
    command.enabled = parsed.enabled;
    command.expires_at = monotonic_expires;
    command.max_duration_ms = parsed.max_duration_ms;
    result.reason = CommandEnvelopeReason::submitted;
    result.admission = ingress.submit(command, now);
    return result;
}

} // namespace networking
