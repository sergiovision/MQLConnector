// A profile is what a particular server expects beyond plain routed JSON frames (InitLib key
// `Profile`, chosen once per connection lifetime by the first InitLib):
//
//   fincore  (default) FinCore's extras: the log-path handshake after connecting, GetLevels and
//            GetSignalCache. Byte-for-byte what MQLConnector 1.0 did.
//   generic  nothing: InitLib returns as soon as TCP is up; GetLevels and GetSignalCache are not
//            available and return "".
//
// Profiles never touch the transport or the mailboxes directly: they talk through a Channel the
// session hands them, so a later multi-session API needs no change here.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "core/config.h"
#include "core/log_level.h"

namespace mqlc::profiles {

/// What a profile may do with the session it serves.
class Channel {
public:
    /// Sends `frame` (a JSON object) and waits up to WaitTimeout for the frame routed back to
    /// the route in it. nullopt on timeout, a bad frame or no connection (already logged).
    [[nodiscard]] virtual std::optional<std::string> request(std::string_view frame) = 0;
    virtual void log(core::LogLevel level, std::string_view message) = 0;

protected:
    ~Channel() = default;
};

class Profile {
public:
    virtual ~Profile() = default;

    [[nodiscard]] virtual core::ProfileKind kind() const noexcept = 0;

    /// Runs once per connection lifetime, after the first InitLib connected and before it
    /// returns. Returns the log file the server asks for, or an empty path.
    [[nodiscard]] virtual std::filesystem::path on_connected(Channel& channel, const core::Config& config) = 0;

    /// GetLevels and GetSignalCache.
    [[nodiscard]] virtual std::string levels(Channel& channel, std::string_view sym, std::int64_t chart_id) = 0;
    [[nodiscard]] virtual std::string signal_cache(Channel& channel, std::string_view key,
                                                   std::string_view sym, std::int64_t chart_id) = 0;

    /// A message id as log lines name it.
    [[nodiscard]] virtual std::string id_name(std::int64_t id) const = 0;
};

[[nodiscard]] std::unique_ptr<Profile> make_profile(core::ProfileKind kind);

}  // namespace mqlc::profiles
