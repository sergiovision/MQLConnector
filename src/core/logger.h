// Asynchronous, bounded file logger.
//
// Callers (MQL threads, the socket reader) only format a line and enqueue it; a single writer
// thread appends batches to the file. A full queue drops lines instead of blocking the caller.
// The file is opened per batch with shared read/write access, like log4net's MinimalLock, so the
// connector can append to FinCore's own main log while the server writes to it too.
//
// Line format, matching the .NET connector's log4net layout:
//   yyyy-MM-dd HH:mm:ss,fff (threadId) [Account]: message
//
// Lines above the configured level (InitLib `LogLevel`) are rejected after one relaxed atomic
// load, before any formatting or locking.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/log_level.h"

namespace mqlc::core {

class Logger {
public:
    struct Options {
        std::size_t queue_capacity = 10'000;
        std::uintmax_t roll_bytes = 10u * 1024u * 1024u;  // only for files opened with rolling
        int roll_backups = 5;
    };

    Logger() : Logger(Options{}) {}
    explicit Logger(Options options);
    ~Logger();  // writes what is queued, then joins the writer thread

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// `%TEMP%\MQLConnector.log` (or `$TMPDIR/MQLConnector.log`).
    [[nodiscard]] static std::filesystem::path default_path();

    /// Starts (or redirects) writing to `file`. Lines logged before the first open are kept
    /// (within the queue bound) and written there. `rolling` enables size-based rolling, meant
    /// only for the connector's own default file, never for a file another process owns.
    void open(std::filesystem::path file, bool rolling);

    void set_account(std::string account);

    /// Lines above `level` are discarded from now on (LogLevel::off discards everything).
    void set_level(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return level != LogLevel::off && level <= level_.load(std::memory_order_relaxed);
    }

    /// Never blocks on I/O and never throws. The one-argument form logs at LogLevel::info.
    void log(LogLevel level, std::string_view message) noexcept;
    void log(std::string_view message) noexcept { log(LogLevel::info, message); }

    /// Blocks until everything logged so far is written (or the logger has no file yet).
    void flush();

    [[nodiscard]] std::uint64_t dropped() const;

private:
    void run(std::stop_token stop);
    void write_lines(const std::filesystem::path& file, bool rolling, std::string_view text) const;

    const Options options_;
    std::atomic<LogLevel> level_{LogLevel::info};  // any thread; a filter, so relaxed is enough

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;  // writer: lines queued, file opened, or stop
    std::condition_variable idle_;      // flush(): the writer finished a batch
    // All guarded by mutex_:
    std::deque<std::string> queue_;
    std::filesystem::path file_;
    std::string account_;
    std::uint64_t dropped_ = 0;
    std::uint64_t dropped_unreported_ = 0;
    bool rolling_ = false;
    bool writing_ = false;

    std::jthread writer_;  // declared last: starts after, and stops before, the state it uses
};

}  // namespace mqlc::core
