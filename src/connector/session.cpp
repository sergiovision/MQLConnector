#include "connector/session.h"

#include "core/no_destructor.h"

#include <chrono>
#include <filesystem>
#include <utility>

namespace mqlc::connector {
namespace {

using core::LogLevel;

std::unique_ptr<net::Transport> make_tcp(const core::Config& config,
                                         net::Transport::FrameHandler on_frame, net::LogFn log) {
    net::TcpOptions options;
    options.host = config.host;
    options.port = config.port;
    options.framing = config.framing_options();
    return net::make_tcp_transport(std::move(options), std::move(on_frame), std::move(log));
}

bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return {std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size())};
}

std::string join(const std::vector<std::string>& items) {
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) text += ", ";
        text += item;
    }
    return text;
}

}  // namespace

/// The session as a profile sees it: requests on one Link.
class Session::LinkChannel final : public profiles::Channel {
public:
    LinkChannel(Session& session, Link& link) noexcept : session_(session), link_(link) {}

    std::optional<std::string> request(std::string_view frame) override {
        const auto envelope = link_.codec->inspect(frame);
        if (!envelope) {
            session_.log_error("Profile request is not a valid message, not sent");
            return std::nullopt;
        }
        session_.mailboxes_.subscribe(envelope->route);
        return session_.request(link_, frame, *envelope);
    }

    void log(LogLevel level, std::string_view message) override { session_.logger_.log(level, message); }

private:
    Session& session_;
    Link& link_;
};

Session::Session() : Session(TransportFactory(make_tcp)) {}

Session::Session(TransportFactory factory, core::Logger::Options log_options)
    : factory_(std::move(factory)), logger_(log_options) {}

Session::~Session() {
    if (const auto current = link()) {
        current->stop.request_stop();
        current->transport->stop();
    }
}

std::shared_ptr<Session::Link> Session::link() const {
    const std::scoped_lock lock(link_mutex_);
    return link_;
}

int Session::init(std::string_view json) {
    const std::scoped_lock lock(lifecycle_mutex_);
    const core::ParsedConfig parsed = core::parse_config(json);
    int result = 0;
    if (const auto current = link()) {
        result = current->transport->connected() ? 1 : 0;
        // Configuration is process-wide: say so when a later program asked for something else.
        if (const auto ignored = core::ignored_keys(current->config, parsed); !ignored.empty()) {
            log_error("InitLib(ChartId=" + std::to_string(parsed.config.chart_id) +
                      "): the connection is already configured by the first InitLib; ignored: " + join(ignored));
        }
    } else {
        start(parsed);
        result = connected() ? 1 : 0;
    }
    ++references_;
    mailboxes_.subscribe(parsed.config.chart_id);
    return result;
}

void Session::start(const core::ParsedConfig& parsed) {
    const core::Config& config = parsed.config;
    logger_.set_account(config.account);
    logger_.set_level(config.log_level);
    for (const auto& warning : parsed.warnings) log_error("InitLib: " + warning);
    if (config.protocol == "fbe") {
        log_error("InitLib: Protocol \"fbe\" is not supported yet, using tcp");
    } else if (config.protocol != "tcp") {
        log_error("InitLib: Protocol \"" + config.protocol + "\" ignored, using tcp");
    }

    auto fresh = std::make_shared<Link>();
    fresh->config = config;
    fresh->profile = profiles::make_profile(config.profile);
    fresh->codec = std::make_unique<core::JsonCodec>(config.route_key, config.correlation_key);
    if (config.framing == core::FramingKind::delimiter) fresh->delimiter = config.delimiter;
    const core::Codec* codec = fresh->codec.get();  // outlives the transport: see Link
    fresh->transport = factory_(
        config, [this, codec](std::string_view frame) { on_frame(*codec, frame); },
        [this](LogLevel level, std::string_view message) { logger_.log(level, message); });
    {
        const std::scoped_lock lock(link_mutex_);
        link_ = fresh;
    }

    std::filesystem::path server_log;
    if (fresh->transport->connect(config.wait_timeout)) {
        LinkChannel channel(*this, *fresh);
        server_log = fresh->profile->on_connected(channel, config);  // fincore: log-path handshake
    }
    // An explicit LogPath wins; then the server's file, which is shared with log4net and so
    // never rolled by us; then our own default file.
    if (!config.log_path.empty()) {
        logger_.open(path_from_utf8(config.log_path), true);
    } else if (!server_log.empty()) {
        logger_.open(server_log, false);
    } else {
        logger_.open(core::Logger::default_path(), true);
    }
    log("MQLConnector.InitLib(" + config.describe() + ")");
    log("WaitOp=" + std::to_string(config.wait_op.count()) +
        " ms ignored: replies wake the waiting call directly");
}

void Session::destroy(std::int64_t chart_id) {
    {
        const std::scoped_lock lock(lifecycle_mutex_);
        mailboxes_.unsubscribe(chart_id);
        if (references_ > 0) --references_;
        if (references_ == 0) {
            std::shared_ptr<Link> last;
            {
                const std::scoped_lock link_lock(link_mutex_);
                last = std::exchange(link_, nullptr);
            }
            if (last) {
                // Wakes every request still waiting on this link, then stops the threads
                // (bounded, see Transport::stop). In-flight requests keep the Link alive and fail.
                last->stop.request_stop();
                last->transport->stop();
            }
        }
    }
    log("MQLConnector.DestroyLib(ChartId=" + std::to_string(chart_id) + "))");
}

std::string Session::send_and_wait(std::string_view json) {
    if (json.empty()) return {};
    const auto current = link();
    if (!current) {
        log_error("Fincore Disconnected");
        return {};
    }
    const auto envelope = current->codec->inspect(json);
    if (!envelope) {
        log_error("DoSendMessage: message is not a JSON object, not sent");
        return {};
    }
    mailboxes_.subscribe(envelope->route);
    return request(*current, json, *envelope).value_or(std::string());
}

void Session::post(std::string_view json) {
    if (json.empty()) return;
    const auto current = link();
    if (!current) {
        log_error("Fincore Disconnected");
        return;
    }
    if (!current->codec->inspect(json)) {
        log_error("DoPostMessage: message is not a JSON object, not sent");
        return;
    }
    std::string storage;
    if (!current->transport->send(current->codec->protect(json, current->delimiter, storage)))
        log_error("DoPostMessage: Fincore Disconnected, message dropped");
}

std::string Session::listen(std::int64_t route) { return mailboxes_.try_pop(route).value_or(std::string()); }

std::string Session::get_levels(std::string_view sym, std::int64_t chart_id) {
    const auto current = link();
    if (!current) {
        log_error(kNotInitialized);
        return std::string(kNotInitialized);
    }
    LinkChannel channel(*this, *current);
    return current->profile->levels(channel, sym, chart_id);
}

std::string Session::get_signal_cache(std::string_view key, std::string_view sym, std::int64_t chart_id) {
    const auto current = link();
    if (!current) {
        log_error(kNotInitialized);
        return std::string(kNotInitialized);
    }
    LinkChannel channel(*this, *current);
    return current->profile->signal_cache(channel, key, sym, chart_id);
}

bool Session::initialized() const { return link() != nullptr; }

bool Session::connected() const {
    const auto current = link();
    return current && current->transport->connected();
}

int Session::references() const {
    const std::scoped_lock lock(lifecycle_mutex_);
    return references_;
}

std::optional<std::string> Session::request(Link& link, std::string_view frame, const core::Envelope& envelope) {
    const auto started = std::chrono::steady_clock::now();
    const auto budget = link.config.wait_timeout;
    net::Transport& transport = *link.transport;

    // With correlation the reply is reserved before the request leaves, so neither
    // DoListenMessage nor another DoSendMessage on the same route can take it first.
    std::optional<core::CorrelatedWait> correlated;
    if (envelope.corr) {
        correlated.emplace(mailboxes_.find(envelope.route), *envelope.corr);
    } else if (link.codec->correlates() && !uncorrelated_warned_.exchange(true, std::memory_order_relaxed)) {
        log_error("DoSendMessage: CorrelationKey \"" + link.config.correlation_key +
                  "\" is set but a request lacks it: such requests get the first frame queued for their "
                  "route (logged once)");
    }

    std::string storage;
    if (!(transport.connected() || transport.connect(budget)) ||
        !transport.send(link.codec->protect(frame, link.delimiter, storage))) {
        log_error("Fincore Disconnected");
        return std::nullopt;
    }
    const std::stop_token stop = link.stop.get_token();
    auto reply = correlated ? correlated->wait(budget, stop) : mailboxes_.wait_pop(envelope.route, budget, stop);
    if (!reply) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        log_error("Failed to wait for " + link.profile->id_name(envelope.id) + "(" +
                  core::json::to_string(envelope.sym).value_or(std::string()) + "," +
                  std::to_string(envelope.route) +
                  (envelope.corr ? "," + core::describe(*envelope.corr) : std::string()) +
                  "), timeout elapsed ms(" + std::to_string(elapsed.count()) + ")");
    }
    return reply;
}

void Session::on_frame(const core::Codec& codec, std::string_view frame) {
    if (is_blank(frame)) return;
    auto envelope = codec.inspect(frame);
    if (!envelope) {
        log_error("OnReceived: bad frame: " + std::string(frame.substr(0, 200)));
        return;
    }
    if (mailboxes_.deliver(envelope->route, frame, std::move(envelope->corr)) == core::Delivery::overflowed) {
        log_error("Mailbox of route " + std::to_string(envelope->route) + " is full: oldest message dropped");
    }
}

Session& process_session() {
    static core::NoDestructor<Session> session;
    return session.get();
}

}  // namespace mqlc::connector
