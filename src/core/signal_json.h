// Just enough JSON for FinCore's SignalInfo frames.
//
// The connector only routes messages, so it never builds a DOM: the reader validates one document
// and hands back source slices for the few top-level members it needs (`Id`, `ChartId`, `Sym`,
// `Data`, ...). Everything else, including arbitrarily nested `Data`, is validated and skipped
// without allocating. The writer emits flat objects with correct string escaping; it always
// escapes 0x1A, so a written frame can never contain the wire delimiter.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mqlc::core::json {

enum class Kind : std::uint8_t { missing, null, boolean, number, string, object, array };

/// A top-level member value as an exact slice of the source document (strings keep their quotes).
struct Value {
    Kind kind = Kind::missing;
    std::string_view text;
};

/// Scans `document`, which must be exactly one well-formed JSON object (surrounding whitespace
/// allowed). For each name in `keys` stores the value of its last occurrence (as Newtonsoft does)
/// into the matching slot of `values`; absent names get `Kind::missing`. Returns false, leaving
/// `values` unspecified, if the document is not a well-formed object. Nesting deeper than
/// `kMaxDepth` is rejected rather than risking the stack.
inline constexpr std::size_t kMaxDepth = 4096;
[[nodiscard]] bool read_members(std::string_view document, std::span<const std::string_view> keys,
                                std::span<Value> values);

/// Number (fraction truncated) or numeric string, as Newtonsoft's `Value<long>` converts it.
[[nodiscard]] std::optional<std::int64_t> to_int64(const Value& value);

/// Decodes a string value (escapes resolved, `\u` surrogate pairs joined, lone ones -> U+FFFD).
/// Numbers and booleans yield their source text; null, missing, objects and arrays yield nullopt.
[[nodiscard]] std::optional<std::string> to_string(const Value& value);

/// The payload text MQL expects from `Data`: a string decoded, an object/array/number/boolean as
/// its raw JSON text, null or missing as "".
[[nodiscard]] std::string payload_text(const Value& value);

/// The routing header every frame carries. Missing `Id`/`ChartId` read as 0 (Newtonsoft default).
struct SignalHeader {
    std::int64_t id = 0;
    std::int64_t chart_id = 0;
    Value sym;
};
[[nodiscard]] std::optional<SignalHeader> read_signal_header(std::string_view document);

/// Makes a document that passed `read_members` safe to frame with `delimiter` (a byte in 1..31):
/// a raw delimiter inside a string becomes `\u00XX`; outside strings, where only JSON whitespace
/// can be (so only for `\t`, `\n`, `\r`), it becomes a space. Returns `document` itself, without
/// copying, when the delimiter does not occur; otherwise the result is built in `storage`.
[[nodiscard]] std::string_view protect_delimiter(std::string_view document, char delimiter,
                                                 std::string& storage);

/// Appends `utf8` to `out` as the body of a JSON string (no surrounding quotes).
void append_escaped(std::string& out, std::string_view utf8);

/// Builds one flat JSON object, e.g. `ObjectWriter().add_int("Id", 1023).finish()`.
class ObjectWriter {
public:
    ObjectWriter() { out_.push_back('{'); }

    ObjectWriter& add_int(std::string_view key, std::int64_t value);
    ObjectWriter& add_string(std::string_view key, std::string_view utf8);

    /// Closes the object and hands over the text; the writer is empty afterwards.
    [[nodiscard]] std::string finish();

private:
    void begin_member(std::string_view key);

    std::string out_;
    bool first_ = true;
};

}  // namespace mqlc::core::json
