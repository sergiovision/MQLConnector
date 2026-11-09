#include "core/config.h"

#include <gtest/gtest.h>

namespace mqlc::core {
namespace {

using std::chrono::milliseconds;

TEST(Config, EmptyInputGivesDefaults) {
    for (const char* empty : {"", "   ", "{}"}) {
        const auto parsed = parse_config(empty);
        EXPECT_EQ(parsed.config.host, "127.0.0.1") << empty;
        EXPECT_EQ(parsed.config.port, 2022);
        EXPECT_EQ(parsed.config.account, "1000000");
        EXPECT_EQ(parsed.config.chart_id, 0);
        EXPECT_EQ(parsed.config.wait_timeout, milliseconds(3000));
        EXPECT_EQ(parsed.config.wait_op, milliseconds(300));
        EXPECT_FALSE(parsed.config.has_parameters);
        EXPECT_TRUE(parsed.warnings.empty());
    }
}

TEST(Config, StringValues) {  // as CBacktestDecisionClient sends them
    const auto parsed = parse_config(
        R"({"Account":"5012345","ChartId":"133000000000000001","Host":"10.0.0.5","Port":"2022",)"
        R"("Protocol":"tcp","WaitOp":"50","WaitTimeout":"30000"})");
    const Config& c = parsed.config;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_TRUE(c.has_parameters);
    EXPECT_EQ(c.account, "5012345");
    EXPECT_EQ(c.chart_id, 133000000000000001);
    EXPECT_EQ(c.host, "10.0.0.5");
    EXPECT_EQ(c.port, 2022);
    EXPECT_EQ(c.wait_op, milliseconds(50));
    EXPECT_EQ(c.wait_timeout, milliseconds(30000));
}

TEST(Config, NumberValues) {  // as MQL5Utils::SetupTcpConnection sends them
    const auto parsed = parse_config(
        R"({"Account":5012345,"ChartId":131,"Host":"localhost","Port":2023,"WaitOp":300,"WaitTimeout":5000})");
    const Config& c = parsed.config;
    EXPECT_TRUE(parsed.warnings.empty());
    EXPECT_EQ(c.account, "5012345");
    EXPECT_EQ(c.chart_id, 131);
    EXPECT_EQ(c.host, "localhost");
    EXPECT_EQ(c.port, 2023);
    EXPECT_EQ(c.wait_timeout, milliseconds(5000));
}

TEST(Config, NonPositiveTimesMeanDefault) {
    const auto parsed = parse_config(R"({"WaitOp":0,"WaitTimeout":"-1"})");
    EXPECT_EQ(parsed.config.wait_op, Config::kDefaultWaitOp);
    EXPECT_EQ(parsed.config.wait_timeout, Config::kDefaultWaitTimeout);
}

TEST(Config, BadValuesKeepDefaultsWithWarnings) {
    const auto parsed = parse_config(R"({"Port":"abc","ChartId":{},"Host":null,"WaitTimeout":70000})");
    EXPECT_EQ(parsed.config.port, 2022);
    EXPECT_EQ(parsed.config.chart_id, 0);
    EXPECT_EQ(parsed.config.host, "127.0.0.1");
    EXPECT_EQ(parsed.config.wait_timeout, milliseconds(70000));
    EXPECT_EQ(parsed.warnings.size(), 3u);
    EXPECT_EQ(parse_config(R"({"Port":70000})").warnings.size(), 1u);
}

TEST(Config, NotAnObject) {
    const auto parsed = parse_config("Host=1.2.3.4");
    EXPECT_EQ(parsed.config.host, "127.0.0.1");
    EXPECT_EQ(parsed.warnings.size(), 1u);
}

TEST(Config, DescribeIsJson) {
    EXPECT_EQ(Config{}.describe(),
              R"({"host":"127.0.0.1","port":2022,"account":"1000000","protocol":"tcp","chartId":0,)"
              R"("waittimeout":3000,"waitop":300,"profile":"fincore","routeKey":"ChartId",)"
              R"("correlationKey":"","framing":"delimiter","delimiter":26,"logPath":"","logLevel":"info"})");
}

TEST(Config, NewKeysDefaults) {
    const Config c = parse_config(R"({"ChartId":1})").config;
    EXPECT_EQ(c.profile, ProfileKind::fincore);
    EXPECT_EQ(c.route_key, "ChartId");
    EXPECT_EQ(c.correlation_key, "");
    EXPECT_EQ(c.framing, FramingKind::delimiter);
    EXPECT_EQ(c.delimiter, '\x1A');
    EXPECT_EQ(c.log_path, "");
    EXPECT_EQ(c.log_level, LogLevel::info);
}

TEST(Config, NewKeysValidForms) {
#ifdef _WIN32
    const std::string log_path = R"(C:\\logs\\mqlc.log)";  // JSON text of C:\logs\mqlc.log
    const std::string log_path_decoded = R"(C:\logs\mqlc.log)";
#else
    const std::string log_path = "/tmp/mqlc.log";
    const std::string log_path_decoded = log_path;
#endif
    const auto parsed = parse_config(R"({"Profile":"generic","RouteKey":"Client","CorrelationKey":"ReqId",)"
                                     R"("Framing":"length","Delimiter":10,"LogPath":")" +
                                     log_path + R"(","LogLevel":"error"})");
    EXPECT_TRUE(parsed.warnings.empty()) << parsed.warnings.front();
    const Config& c = parsed.config;
    EXPECT_EQ(c.profile, ProfileKind::generic);
    EXPECT_EQ(c.route_key, "Client");
    EXPECT_EQ(c.correlation_key, "ReqId");
    EXPECT_EQ(c.framing, FramingKind::length);
    EXPECT_EQ(c.delimiter, '\n');
    EXPECT_EQ(c.log_path, log_path_decoded);
    EXPECT_EQ(c.log_level, LogLevel::error);
    EXPECT_EQ(c.framing_options().kind, FramingKind::length);
    EXPECT_EQ(c.framing_options().max_frame_bytes, kMaxFrameBytes);

    // Case-insensitive words, the delimiter as a numeric string, an explicit empty CorrelationKey.
    const auto other = parse_config(R"({"Profile":"FinCore","Framing":"Delimiter","Delimiter":"31",)"
                                    R"("LogLevel":"OFF","CorrelationKey":""})");
    EXPECT_TRUE(other.warnings.empty());
    EXPECT_EQ(other.config.profile, ProfileKind::fincore);
    EXPECT_EQ(other.config.framing, FramingKind::delimiter);
    EXPECT_EQ(other.config.delimiter, '\x1F');
    EXPECT_EQ(other.config.log_level, LogLevel::off);
    EXPECT_EQ(other.config.correlation_key, "");
}

TEST(Config, NewKeysBadValuesWarnAndKeepDefaults) {
    const auto parsed = parse_config(R"({"Profile":"mt4","RouteKey":"","CorrelationKey":7,"Framing":"json",)"
                                     R"("Delimiter":0,"LogPath":"relative/x.log","LogLevel":"debug"})");
    EXPECT_EQ(parsed.warnings.size(), 7u);
    const Config& c = parsed.config;
    EXPECT_EQ(c.profile, ProfileKind::fincore);
    EXPECT_EQ(c.route_key, "ChartId");
    EXPECT_EQ(c.correlation_key, "");
    EXPECT_EQ(c.framing, FramingKind::delimiter);
    EXPECT_EQ(c.delimiter, '\x1A');
    EXPECT_EQ(c.log_path, "");
    EXPECT_EQ(c.log_level, LogLevel::info);

    for (const char* bad : {R"({"Delimiter":32})", R"({"Delimiter":-1})", R"({"Delimiter":"x"})",
                            R"({"Delimiter":true})", R"({"RouteKey":null})", R"({"Profile":1})",
                            R"({"LogLevel":["info"]})"}) {
        EXPECT_EQ(parse_config(bad).warnings.size(), 1u) << bad;
    }
}

TEST(Config, IgnoredKeysListsOnlyGivenDifferences) {
    const Config active = parse_config(R"({"Port":2022,"ChartId":1,"WaitTimeout":3000})").config;
    EXPECT_TRUE(ignored_keys(active, parse_config(R"({"ChartId":2})")).empty());
    EXPECT_TRUE(ignored_keys(active, parse_config(R"({"ChartId":2,"Port":"2022","WaitTimeout":3000})")).empty());
    EXPECT_EQ(ignored_keys(active, parse_config(R"({"ChartId":2,"Port":2023,"Profile":"generic","Host":"127.0.0.1"})")),
              (std::vector<std::string>{"Port", "Profile"}));
}

}  // namespace
}  // namespace mqlc::core
