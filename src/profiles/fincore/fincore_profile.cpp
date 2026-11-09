// The fincore profile: what MQLConnector 1.0 did for FinCore, unchanged. Its frames always name
// the route `ChartId`, as FinCore's server expects; tests/fincore_compat_test.cpp pins their bytes.
#include "profiles/fincore/fincore_profile.h"

#include "core/signal_json.h"
#include "profiles/fincore/signal_ids.h"

#include <array>
#include <utility>

namespace mqlc::profiles::fincore {
namespace {

using core::json::ObjectWriter;

bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::string member_text(std::string_view frame, std::string_view key) {
    const std::array<std::string_view, 1> keys{key};
    std::array<core::json::Value, 1> values{};
    if (!core::json::read_members(frame, keys, values)) return {};
    return core::json::payload_text(values[0]);
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return {std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())};
}

class FinCoreProfile final : public Profile {
public:
    [[nodiscard]] core::ProfileKind kind() const noexcept override { return core::ProfileKind::fincore; }

    std::filesystem::path on_connected(Channel& channel, const core::Config& config) override {
        if (!config.has_parameters) return {};
        // Ask FinCore where its main log is, so the connector's lines land next to the server's.
        const std::string ask = ObjectWriter()
                                    .add_int("ChartId", config.chart_id)
                                    .add_int("Id", signal::kGetMainLogPath)
                                    .add_string("Sym", "")
                                    .finish();
        const auto reply = channel.request(ask);
        if (!reply) return {};
        const std::string path = member_text(*reply, "Data");
        if (is_blank(path)) return {};
        // Only an absolute path: a relative one would land in whatever the host's working
        // directory happens to be.
        auto candidate = path_from_utf8(path);
        if (candidate.is_absolute()) return candidate;
        channel.log(core::LogLevel::error, "InitLib: log path \"" + path + "\" is not absolute, using the default");
        return {};
    }

    std::string levels(Channel& channel, std::string_view sym, std::int64_t chart_id) override {
        const std::string ask = ObjectWriter()
                                    .add_int("ChartId", chart_id)
                                    .add_int("Id", signal::kLevelsForSymbol)
                                    .add_string("Sym", sym)
                                    .finish();
        const auto reply = channel.request(ask);
        return reply ? member_text(*reply, "Data") : std::string();
    }

    std::string signal_cache(Channel& channel, std::string_view key, std::string_view sym,
                             std::int64_t chart_id) override {
        const std::string ask = ObjectWriter()
                                    .add_int("ChartId", chart_id)
                                    .add_int("Id", signal::kGetFromCache)
                                    .add_string("Sym", sym)
                                    .add_string("Key", key)
                                    .finish();
        return channel.request(ask).value_or(std::string());
    }

    [[nodiscard]] std::string id_name(std::int64_t id) const override { return signal::name(id); }
};

}  // namespace

std::unique_ptr<Profile> make_profile() { return std::make_unique<FinCoreProfile>(); }

}  // namespace mqlc::profiles::fincore
