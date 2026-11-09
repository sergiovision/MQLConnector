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
              R"("waittimeout":3000,"waitop":300})");
}

}  // namespace
}  // namespace mqlc::core
