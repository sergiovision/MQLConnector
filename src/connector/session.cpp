#include "connector/session.h"

#include "core/no_destructor.h"
#include "core/signal_ids.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <utility>

namespace mqlc::connector {
namespace {

using core::json::ObjectWriter;

std::unique_ptr<net::Transport> make_tcp(const core::Config& config,
                                         net::Transport::FrameHandler on_frame, net::LogFn log) {
    net::TcpOptions options;
    options.host = config.host;
    options.port = config.port;
    return net::make_tcp_transport(std::move(options), std::move(on_frame), std::move(log));
}

bool is_blank(std::string_view text) {
    return text.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

/// A raw 0x1A can only sit inside a JSON string of a frame that passed validation (the reader
/// accepts raw control characters there, as Newtonsoft does); re-escape it so the frame cannot
/// be cut short on the wire. The common case returns `json` untouched, without copying.
std::string_view escape_delimiters(std::string_view json, std::string& storage) {
    if (json.find(core::kFrameDelimiter) == std::string_view::npos) return json;
    storage.clear();
    for (const char c : json) {
        if (c == core::kFrameDelimiter) {
            storage += "\\u001a";
        } else {
            storage.push_back(c);
        }
    }
    return storage;
}

std::string member_text(std::string_view frame, std::string_view key) {
    const std::array<std::string_view, 1> keys{key};
    std::array<core::json::Value, 1> values{};
    if (!core::json::read_members(frame, keys, values)) return {};
    return core::json::payload_text(values[0]);
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

}  // namespace

Session::Session() : Session(TransportFactory(make_tcp)) {}

Session::Session(TransportFactory factory, core::Logger::Options log_options)
    : factory_(std::move(factory)), logger_(log_options) {}

Session::~Session() {
    if (const auto current = link()) current->transport->stop();
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
    for (const auto& warning : parsed.warnings) log("InitLib: " + warning);
    if (config.protocol != "tcp") log("InitLib: Protocol \"" + config.protocol + "\" ignored, using tcp");

    auto fresh = std::make_shared<Link>();
    fresh->config = config;
    fresh->transport = factory_(
        config, [this](std::string_view frame) { on_frame(frame); },
        [this](std::string_view message) { log(message); });
    {
        const std::scoped_lock lock(link_mutex_);
        link_ = fresh;
    }

    std::filesystem::path log_file;
    const bool up = fresh->transport->connect(config.wait_timeout);
    if (up && config.has_parameters) {
        // Ask FinCore where its main log is, so the connector's lines land next to the server's.
        mailboxes_.subscribe(config.chart_id);
        const std::string ask = ObjectWriter()
                                    .add_int("ChartId", config.chart_id)
                                    .add_int("Id", core::signal::kGetMainLogPath)
                                    .add_string("Sym", "")
                                    .finish();
        core::json::SignalHeader header;
        header.id = core::signal::kGetMainLogPath;
        header.chart_id = config.chart_id;
        if (const auto reply = request(*fresh, ask, header)) {
            const std::string path = member_text(*reply, "Data");
            // Only an absolute path: a relative one would land in whatever the host's working
            // directory happens to be.
            if (!is_blank(path)) {
                auto candidate = path_from_utf8(path);
                if (candidate.is_absolute()) {
                    log_file = std::move(candidate);
                } else {
                    log("InitLib: log path \"" + path + "\" is not absolute, using the default");
                }
            }
        }
    }
    // The server's file is shared with log4net, so only our own default file is ever rolled.
    const bool own_file = log_file.empty();
    logger_.open(own_file ? core::Logger::default_path() : log_file, own_file);
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
            // Bounded (see Transport::stop); in-flight requests keep the Link alive and fail.
            if (last) last->transport->stop();
        }
    }
    log("MQLConnector.DestroyLib(ChartId=" + std::to_string(chart_id) + "))");
}

std::string Session::send_and_wait(std::string_view json) {
    if (json.empty()) return {};
    const auto current = link();
    if (!current) {
        log("Fincore Disconnected");
        return {};
    }
    const auto header = core::json::read_signal_header(json);
    if (!header) {
        log("DoSendMessage: message is not a JSON object, not sent");
        return {};
    }
    mailboxes_.subscribe(header->chart_id);
    return request(*current, json, *header).value_or(std::string());
}

void Session::post(std::string_view json) {
    if (json.empty()) return;
    const auto current = link();
    if (!current) {
        log("Fincore Disconnected");
        return;
    }
    if (!core::json::read_signal_header(json)) {
        log("DoPostMessage: message is not a JSON object, not sent");
        return;
    }
    std::string storage;
    if (!current->transport->send(escape_delimiters(json, storage)))
        log("DoPostMessage: Fincore Disconnected, message dropped");
}

std::string Session::listen(std::int64_t chart_id) {
    return mailboxes_.try_pop(chart_id).value_or(std::string());
}

std::string Session::get_levels(std::string_view sym, std::int64_t chart_id) {
    const auto current = link();
    if (!current) {
        log(kNotInitialized);
        return std::string(kNotInitialized);
    }
    const std::string ask = ObjectWriter()
                                .add_int("ChartId", chart_id)
                                .add_int("Id", core::signal::kLevelsForSymbol)
                                .add_string("Sym", sym)
                                .finish();
    const auto header = core::json::read_signal_header(ask);
    mailboxes_.subscribe(chart_id);
    const auto reply = request(*current, ask, *header);
    return reply ? member_text(*reply, "Data") : std::string();
}

std::string Session::get_signal_cache(std::string_view key, std::string_view sym,
                                      std::int64_t chart_id) {
    const auto current = link();
    if (!current) {
        log(kNotInitialized);
        return std::string(kNotInitialized);
    }
    const std::string ask = ObjectWriter()
                                .add_int("ChartId", chart_id)
                                .add_int("Id", core::signal::kGetFromCache)
                                .add_string("Sym", sym)
                                .add_string("Key", key)
                                .finish();
    const auto header = core::json::read_signal_header(ask);
    mailboxes_.subscribe(chart_id);
    return request(*current, ask, *header).value_or(std::string());
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

std::optional<std::string> Session::request(Link& link, std::string_view frame,
                                            const core::json::SignalHeader& header) {
    const auto started = std::chrono::steady_clock::now();
    const auto budget = link.config.wait_timeout;
    net::Transport& transport = *link.transport;
    std::string storage;
    if (!(transport.connected() || transport.connect(budget)) ||
        !transport.send(escape_delimiters(frame, storage))) {
        log("Fincore Disconnected");
        return std::nullopt;
    }
    auto reply = mailboxes_.wait_pop(header.chart_id, budget);
    if (!reply) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        log("Failed to wait for " + core::signal::name(header.id) + "(" +
            core::json::to_string(header.sym).value_or(std::string()) + "," +
            std::to_string(header.chart_id) + "), timeout elapsed ms(" +
            std::to_string(elapsed.count()) + ")");
    }
    return reply;
}

void Session::on_frame(std::string_view frame) {
    if (is_blank(frame)) return;
    const auto header = core::json::read_signal_header(frame);
    if (!header) {
        log("OnReceived: bad frame: " + std::string(frame.substr(0, 200)));
        return;
    }
    if (mailboxes_.deliver(header->chart_id, frame) == core::Delivery::overflowed) {
        log("Mailbox of ChartId " + std::to_string(header->chart_id) +
            " is full: oldest message dropped");
    }
}

Session& process_session() {
    static core::NoDestructor<Session> session;
    return session.get();
}

}  // namespace mqlc::connector
