// The generic profile: plain routed JSON frames and nothing else. No handshake, so InitLib
// returns as soon as TCP is up; FinCore's GetLevels/GetSignalCache are not available.
#include "profiles/generic/generic_profile.h"

#include <atomic>

namespace mqlc::profiles::generic {
namespace {

class GenericProfile final : public Profile {
public:
    [[nodiscard]] core::ProfileKind kind() const noexcept override { return core::ProfileKind::generic; }

    std::filesystem::path on_connected(Channel&, const core::Config&) override { return {}; }

    std::string levels(Channel& channel, std::string_view, std::int64_t) override {
        not_available(channel, "GetLevels");
        return {};
    }

    std::string signal_cache(Channel& channel, std::string_view, std::string_view, std::int64_t) override {
        not_available(channel, "GetSignalCache");
        return {};
    }

    [[nodiscard]] std::string id_name(std::int64_t id) const override { return std::to_string(id); }

private:
    void not_available(Channel& channel, std::string_view function) {
        if (warned_.exchange(true, std::memory_order_relaxed)) return;
        channel.log(core::LogLevel::error, std::string(function) +
                                               ": not available in the generic profile, returning \"\" "
                                               "(logged once)");
    }

    std::atomic<bool> warned_{false};  // any MQL thread; only gates one log line
};

}  // namespace

std::unique_ptr<Profile> make_profile() { return std::make_unique<GenericProfile>(); }

}  // namespace mqlc::profiles::generic
