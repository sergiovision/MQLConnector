#include "core/signal_json.h"

#include "core/utf.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace mqlc::core::json {
namespace {

constexpr bool is_ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Open brackets of the container being skipped, one bit each (1 = object, 0 = array), so nesting
/// costs no recursion and no allocation.
class BracketStack {
public:
    [[nodiscard]] bool push(bool is_object) noexcept {
        if (depth_ == kMaxDepth) return false;
        const std::uint64_t mask = std::uint64_t{1} << (depth_ % 64);
        std::uint64_t& word = bits_[depth_ / 64];
        word = is_object ? (word | mask) : (word & ~mask);
        ++depth_;
        return true;
    }
    void pop() noexcept { --depth_; }
    [[nodiscard]] bool empty() const noexcept { return depth_ == 0; }
    [[nodiscard]] bool in_object() const noexcept {
        const std::size_t top = depth_ - 1;
        return ((bits_[top / 64] >> (top % 64)) & 1u) != 0;
    }

private:
    std::array<std::uint64_t, kMaxDepth / 64> bits_{};
    std::size_t depth_ = 0;
};

/// Forward-only validating scanner over one document.
class Cursor {
public:
    explicit Cursor(std::string_view text) noexcept : s_(text) {}

    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= s_.size(); }
    [[nodiscard]] char peek() const noexcept { return at_end() ? '\0' : s_[pos_]; }

    void skip_ws() noexcept {
        while (!at_end() && is_ws(s_[pos_])) ++pos_;
    }

    bool consume(char c) noexcept {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    /// At an opening quote: advances past the closing one. Raw control characters are accepted
    /// (Newtonsoft accepts them too); escapes must be well-formed.
    bool skip_string(bool* has_escape = nullptr) noexcept {
        if (!consume('"')) return false;
        while (!at_end()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') continue;
            if (has_escape != nullptr) *has_escape = true;
            if (at_end()) return false;
            const char e = s_[pos_++];
            if (e == 'u') {
                for (int k = 0; k < 4; ++k) {
                    if (at_end() || hex_value(s_[pos_]) < 0) return false;
                    ++pos_;
                }
            } else if (e != '"' && e != '\\' && e != '/' && e != 'b' && e != 'f' && e != 'n' &&
                       e != 'r' && e != 't') {
                return false;
            }
        }
        return false;
    }

    /// `-? (0 | [1-9][0-9]*) (.[0-9]+)? ([eE][+-]?[0-9]+)?`
    bool skip_number() noexcept {
        consume('-');
        if (consume('0')) {
        } else if (is_digit(peek())) {
            while (is_digit(peek())) ++pos_;
        } else {
            return false;
        }
        if (consume('.')) {
            if (!is_digit(peek())) return false;
            while (is_digit(peek())) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!is_digit(peek())) return false;
            while (is_digit(peek())) ++pos_;
        }
        return true;
    }

    bool skip_literal(std::string_view word) noexcept {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    /// Skips any value; reports its kind.
    bool skip_value(Kind& kind) noexcept {
        switch (peek()) {
            case '"': kind = Kind::string; return skip_string();
            case '{': kind = Kind::object; return skip_container();
            case '[': kind = Kind::array; return skip_container();
            case 't': kind = Kind::boolean; return skip_literal("true");
            case 'f': kind = Kind::boolean; return skip_literal("false");
            case 'n': kind = Kind::null; return skip_literal("null");
            default: kind = Kind::number; return skip_number();
        }
    }

private:
    bool skip_scalar() noexcept {
        Kind ignored{};
        return skip_value(ignored);  // never a container here: callers check for brackets first
    }

    enum class Opened : std::uint8_t { error, has_content, closed };

    /// At '{' or '[': pushes it and, if the container is empty, closes it straight away.
    Opened open(BracketStack& stack) noexcept {
        if (!stack.push(peek() == '{')) return Opened::error;
        ++pos_;
        skip_ws();
        if (consume(stack.in_object() ? '}' : ']')) {
            stack.pop();
            return Opened::closed;
        }
        return Opened::has_content;
    }

    /// After a value: closes every container that ends here, then consumes the separating comma
    /// if one follows. Leaves the stack empty when the outermost container closed.
    bool after_value(BracketStack& stack) noexcept {
        while (!stack.empty()) {
            skip_ws();
            if (consume(',')) {
                skip_ws();
                return true;
            }
            if (!consume(stack.in_object() ? '}' : ']')) return false;
            stack.pop();
        }
        return true;
    }

    bool skip_container() noexcept {
        BracketStack stack;
        Opened opened = open(stack);
        if (opened != Opened::has_content) return opened == Opened::closed;
        while (true) {
            if (stack.in_object()) {
                if (!skip_string()) return false;
                skip_ws();
                if (!consume(':')) return false;
                skip_ws();
            }
            if (peek() == '{' || peek() == '[') {
                opened = open(stack);
                if (opened == Opened::error) return false;
                if (opened == Opened::has_content) continue;
            } else if (!skip_scalar()) {
                return false;
            }
            if (!after_value(stack)) return false;
            if (stack.empty()) return true;
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

/// Decodes the inside of a validated string literal (no surrounding quotes).
void unescape_into(std::string_view body, std::string& out) {
    out.reserve(out.size() + body.size());
    auto read_hex4 = [&](std::size_t at) -> char32_t {
        char32_t v = 0;
        for (std::size_t k = 0; k < 4; ++k)
            v = (v << 4) | static_cast<char32_t>(hex_value(body[at + k]));
        return v;
    };
    for (std::size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        const char e = body[++i];
        switch (e) {
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                char32_t cp = read_hex4(i + 1);
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < body.size() && body[i + 1] == '\\' &&
                    body[i + 2] == 'u') {
                    const char32_t low = read_hex4(i + 3);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        i += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = utf::kReplacement;
                utf::detail::append_utf8(out, cp);
                break;
            }
            default: out.push_back(e); break;  // '"', '\\', '/'
        }
    }
}

bool key_equals(std::string_view raw_key, bool has_escape, std::string_view wanted) {
    if (!has_escape) return raw_key == wanted;
    std::string decoded;
    unescape_into(raw_key, decoded);
    return decoded == wanted;
}

std::optional<std::int64_t> parse_int64(std::string_view text) {
    std::int64_t integral = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto [end, ec] = std::from_chars(first, last, integral);
    if (ec == std::errc{} && end == last) return integral;

    // A fraction or exponent: convert like Newtonsoft does (truncate toward zero), if in range.
    if (text.empty() || text.size() > 64) return std::nullopt;
    std::array<char, 65> buffer{};
    text.copy(buffer.data(), text.size());
    char* parsed_end = nullptr;
    const double d = std::strtod(buffer.data(), &parsed_end);
    if (parsed_end != buffer.data() + text.size() || !std::isfinite(d)) return std::nullopt;
    constexpr double kLimit = 9223372036854775808.0;  // 2^63
    if (d >= kLimit || d < -kLimit) return std::nullopt;
    return static_cast<std::int64_t>(d);
}

}  // namespace

bool read_members(std::string_view document, std::span<const std::string_view> keys,
                  std::span<Value> values) {
    for (Value& v : values) v = Value{};
    Cursor cur(document);
    cur.skip_ws();
    if (!cur.consume('{')) return false;
    cur.skip_ws();
    if (!cur.consume('}')) {
        while (true) {
            const std::size_t key_start = cur.pos();
            bool has_escape = false;
            if (!cur.skip_string(&has_escape)) return false;
            const std::string_view raw_key = document.substr(key_start + 1, cur.pos() - key_start - 2);
            cur.skip_ws();
            if (!cur.consume(':')) return false;
            cur.skip_ws();
            const std::size_t value_start = cur.pos();
            Kind kind{};
            if (!cur.skip_value(kind)) return false;
            const Value value{kind, document.substr(value_start, cur.pos() - value_start)};
            for (std::size_t k = 0; k < keys.size() && k < values.size(); ++k) {
                if (key_equals(raw_key, has_escape, keys[k])) values[k] = value;
            }
            cur.skip_ws();
            if (cur.consume(',')) {
                cur.skip_ws();
                continue;
            }
            if (cur.consume('}')) break;
            return false;
        }
    }
    cur.skip_ws();
    return cur.at_end();
}

std::optional<std::int64_t> to_int64(const Value& value) {
    if (value.kind == Kind::number) return parse_int64(value.text);
    if (value.kind == Kind::string) {
        const auto decoded = to_string(value);
        return decoded ? parse_int64(*decoded) : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> to_string(const Value& value) {
    switch (value.kind) {
        case Kind::string: {
            std::string out;
            unescape_into(value.text.substr(1, value.text.size() - 2), out);
            return out;
        }
        case Kind::number:
        case Kind::boolean: return std::string(value.text);
        default: return std::nullopt;
    }
}

std::string payload_text(const Value& value) {
    switch (value.kind) {
        case Kind::string: return *to_string(value);
        case Kind::missing:
        case Kind::null: return {};
        default: return std::string(value.text);
    }
}

std::optional<SignalHeader> read_signal_header(std::string_view document) {
    static constexpr std::array<std::string_view, 3> kKeys{"Id", "ChartId", "Sym"};
    std::array<Value, kKeys.size()> values{};
    if (!read_members(document, kKeys, values)) return std::nullopt;
    SignalHeader header;
    header.id = to_int64(values[0]).value_or(0);
    header.chart_id = to_int64(values[1]).value_or(0);
    header.sym = values[2];
    return header;
}

std::string_view protect_delimiter(std::string_view document, char delimiter, std::string& storage) {
    if (document.find(delimiter) == std::string_view::npos) return document;
    static constexpr char kHex[] = "0123456789abcdef";
    const auto code = static_cast<unsigned char>(delimiter);
    storage.clear();
    storage.reserve(document.size() + 16);
    bool in_string = false;
    for (std::size_t i = 0; i < document.size(); ++i) {
        const char c = document[i];
        if (c == delimiter) {
            if (in_string) {
                storage += "\\u00";
                storage.push_back(kHex[code >> 4]);
                storage.push_back(kHex[code & 0x0F]);
            } else {
                storage.push_back(' ');  // whitespace between tokens: any whitespace will do
            }
            continue;
        }
        storage.push_back(c);
        if (c == '"') {
            in_string = !in_string;
        } else if (c == '\\' && in_string && i + 1 < document.size()) {
            // The escaped character is never the delimiter: a validated document has no
            // backslash before a control character.
            storage.push_back(document[++i]);
        }
    }
    return storage;
}

void append_escaped(std::string& out, std::string_view utf8) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const char c : utf8) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {  // includes 0x1A, the frame delimiter
                    out += "\\u00";
                    out.push_back(kHex[u >> 4]);
                    out.push_back(kHex[u & 0x0F]);
                } else {
                    out.push_back(c);
                }
        }
    }
}

void ObjectWriter::begin_member(std::string_view key) {
    if (!first_) out_.push_back(',');
    first_ = false;
    out_.push_back('"');
    append_escaped(out_, key);
    out_ += "\":";
}

ObjectWriter& ObjectWriter::add_int(std::string_view key, std::int64_t value) {
    begin_member(key);
    std::array<char, 24> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), value);
    out_.append(digits.data(), result.ptr);
    return *this;
}

ObjectWriter& ObjectWriter::add_string(std::string_view key, std::string_view utf8) {
    begin_member(key);
    out_.push_back('"');
    append_escaped(out_, utf8);
    out_.push_back('"');
    return *this;
}

std::string ObjectWriter::finish() {
    out_.push_back('}');
    return std::move(out_);
}

}  // namespace mqlc::core::json
