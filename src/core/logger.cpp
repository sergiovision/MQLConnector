#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mqlc::core {
namespace {

std::uint64_t current_thread_id() noexcept {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id()) % 100'000;
#endif
}

/// "yyyy-MM-dd HH:mm:ss,fff (tid) [account]: "
std::string line_prefix(std::string_view account) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[64];
    const int n = std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d,%03d (%llu) [",
                                local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                                local.tm_min, local.tm_sec, static_cast<int>(millis),
                                static_cast<unsigned long long>(current_thread_id()));
    std::string prefix(buffer, n > 0 ? static_cast<std::size_t>(n) : 0);
    prefix.append(account);
    prefix.append("]: ");
    return prefix;
}

/// Appends to `file`, opened only for this call and shared with other writers.
bool append_shared(const std::filesystem::path& file, std::string_view text) {
#ifdef _WIN32
    const HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    while (ok && !text.empty()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(text.size(), 1u << 20));
        DWORD written = 0;
        ok = WriteFile(h, text.data(), chunk, &written, nullptr) != FALSE && written > 0;
        text.remove_prefix(written);
    }
    CloseHandle(h);
    return ok;
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    bool ok = true;
    while (ok && !text.empty()) {
        const ssize_t written = ::write(fd, text.data(), text.size());
        ok = written > 0;
        if (ok) text.remove_prefix(static_cast<std::size_t>(written));
    }
    ::close(fd);
    return ok;
#endif
}

void roll(const std::filesystem::path& file, int backups) {
    std::error_code ec;
    auto backup = [&](int index) {
        auto name = file;
        name += "." + std::to_string(index);
        return name;
    };
    std::filesystem::remove(backup(backups), ec);
    for (int i = backups - 1; i >= 1; --i) std::filesystem::rename(backup(i), backup(i + 1), ec);
    std::filesystem::rename(file, backup(1), ec);
}

}  // namespace

Logger::Logger(Options options)
    : options_(options), writer_([this](std::stop_token stop) { run(std::move(stop)); }) {}

Logger::~Logger() {
    writer_.request_stop();
    wake_.notify_all();
    // jthread joins here; run() writes the remaining lines before returning.
}

std::filesystem::path Logger::default_path() {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    return dir / "MQLConnector.log";
}

void Logger::open(std::filesystem::path file, bool rolling) {
    {
        const std::scoped_lock lock(mutex_);
        file_ = std::move(file);
        rolling_ = rolling;
    }
    wake_.notify_all();
}

void Logger::set_account(std::string account) {
    const std::scoped_lock lock(mutex_);
    account_ = std::move(account);
}

void Logger::log(LogLevel level, std::string_view message) noexcept {
    if (!enabled(level)) return;
    try {
        std::unique_lock lock(mutex_);
        if (queue_.size() >= options_.queue_capacity) {
            ++dropped_;
            ++dropped_unreported_;
            return;
        }
        std::string line = line_prefix(account_);
        line.append(message);
        line.append("\r\n");
        queue_.push_back(std::move(line));
        lock.unlock();
        wake_.notify_one();
    } catch (...) {
        // Out of memory while logging: the line is lost, the caller is not.
    }
}

void Logger::flush() {
    std::unique_lock lock(mutex_);
    wake_.notify_all();
    idle_.wait(lock, [this] { return (queue_.empty() || file_.empty()) && !writing_; });
}

std::uint64_t Logger::dropped() const {
    const std::scoped_lock lock(mutex_);
    return dropped_;
}

void Logger::run(std::stop_token stop) {
    std::vector<std::string> batch;
    std::string text;
    while (true) {
        std::filesystem::path file;
        bool rolling = false;
        std::uint64_t unreported = 0;
        {
            std::unique_lock lock(mutex_);
            (void)wake_.wait(lock, stop, [this] { return !queue_.empty() && !file_.empty(); });
            if (queue_.empty() || file_.empty()) {  // stopping with nothing writable
                idle_.notify_all();
                return;
            }
            batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
            queue_.clear();
            file = file_;
            rolling = rolling_;
            unreported = std::exchange(dropped_unreported_, 0);
            writing_ = true;
        }

        text.clear();
        if (unreported > 0) {
            text += line_prefix("MQLConnector");
            text += std::to_string(unreported) + " log lines dropped: logger queue full\r\n";
        }
        for (const auto& line : batch) text += line;
        write_lines(file, rolling, text);
        batch.clear();

        {
            const std::scoped_lock lock(mutex_);
            writing_ = false;
        }
        idle_.notify_all();
    }
}

void Logger::write_lines(const std::filesystem::path& file, bool rolling, std::string_view text) const {
    if (rolling) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (!ec && size + text.size() > options_.roll_bytes) roll(file, options_.roll_backups);
    }
    // A failed write (file locked exclusively, folder gone) loses this batch; logging must
    // never take the connector down.
    (void)append_shared(file, text);
}

}  // namespace mqlc::core
