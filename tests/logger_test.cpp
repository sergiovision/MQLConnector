#include "core/logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace mqlc::core {
namespace {

std::filesystem::path temp_file(const char* name) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

TEST(Logger, Log4netLineFormat) {
    const auto path = temp_file("mqlc_logger_format.log");
    {
        Logger logger;
        logger.set_account("5012345");
        logger.open(path, false);
        logger.log("hello");
        logger.flush();
    }
    const std::string text = read_all(path);
    const std::regex line(R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3} \(\d+\) \[5012345\]: hello\r\n$)");
    EXPECT_TRUE(std::regex_match(text, line)) << text;
}

TEST(Logger, LinesBeforeOpenAreKept) {
    const auto path = temp_file("mqlc_logger_early.log");
    {
        Logger logger;
        logger.log("early");
        logger.open(path, false);
        logger.log("late");
    }  // the destructor drains the queue
    const std::string text = read_all(path);
    EXPECT_NE(text.find("early"), std::string::npos);
    EXPECT_LT(text.find("early"), text.find("late"));
}

TEST(Logger, FullQueueDropsInsteadOfBlocking) {
    Logger::Options options;
    options.queue_capacity = 3;
    Logger logger(options);  // never opened: nothing is written, the queue only fills
    for (int i = 0; i < 10; ++i) logger.log("x");
    EXPECT_EQ(logger.dropped(), 7u);
}

TEST(Logger, RollsOwnFileBySize) {
    const auto path = temp_file("mqlc_logger_roll.log");
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path) += ".1", ec);
    Logger::Options options;
    options.roll_bytes = 200;
    {
        Logger logger(options);
        logger.open(path, true);
        for (int i = 0; i < 10; ++i) {
            logger.log(std::string(60, 'a'));
            logger.flush();
        }
    }
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(path) += ".1"));
    EXPECT_LE(std::filesystem::file_size(path), 200u);
}

}  // namespace
}  // namespace mqlc::core
