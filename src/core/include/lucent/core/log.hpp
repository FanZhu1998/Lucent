#pragma once

// Small thread-safe logger.
//
//     lucent::core::log_info("core.import", "imported {} files from {}", count, folder_utf8);
//
// Design:
//  - A Logger filters by minimum level and dispatches LogRecords to LogSinks. The level check
//    happens before any formatting, so disabled log calls cost one relaxed atomic load (the
//    call arguments themselves are still evaluated by the caller).
//  - Format strings are std::format_string, checked at compile time.
//  - Logging never throws and never fails the caller: formatting errors and sink failures
//    (including exceptions thrown by custom sinks) are caught and counted in LogStats.
//  - The process-wide logger() is created on first use and never destroyed, so it can be used
//    from any static initialiser or destructor without initialisation-order hazards.
//  - Sinks: DebugOutputSink (debugger / DebugView), FileSink (UTF-8, size-capped rotation) and
//    MemorySink (tests and diagnostics views).
//
// Log text is developer-facing and UTF-8. Never log photograph pixel data, and treat paths as
// user data (support diagnostics may redact them).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lucent/core/error.hpp"

namespace lucent::core {

// Severity, lowest first. Numeric values are stable (they may appear in settings files).
enum class LogLevel : std::uint8_t {
    Trace = 0,  // very detailed flow, normally disabled
    Debug = 1,  // diagnostic detail useful to developers
    Info = 2,   // normal milestones (import finished, export published)
    Warn = 3,   // unexpected but handled (fallback taken, retry scheduled)
    Error = 4,  // an operation failed
};

// "trace", "debug", "info", "warn", "error".
[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// Inverse of to_string(LogLevel), ASCII case-insensitive; "warning" is accepted for Warn.
// Anything else is InvalidArgument.
[[nodiscard]] Expected<LogLevel> parse_log_level(std::string_view text);

// Log timestamps: UTC with millisecond precision.
using LogTime = std::chrono::sys_time<std::chrono::milliseconds>;

struct LogRecord {
    LogTime time{};               // UTC
    std::uint32_t thread_id = 0;  // Win32 thread id of the logging thread
    LogLevel level = LogLevel::Info;
    std::string category;  // stable dotted component name, e.g. "raw.libraw"
    std::string message;   // UTF-8 (not validated; line sinks sanitise it)
};

// ISO-8601 UTC with milliseconds: "2026-09-24T07:05:09.042Z".
[[nodiscard]] std::string format_log_time(LogTime time);

// One line without a terminator, as written by DebugOutputSink and FileSink:
//     "2026-09-24T07:05:09.042Z INFO  [1234] core.file: message"
// Control characters in category and message are escaped (\n, \r, \t, \xHH) so a record never
// spans lines, and malformed UTF-8 is replaced with U+FFFD.
[[nodiscard]] std::string format_log_line(const LogRecord& record);

// Destination for log records.
//
// A Logger never calls write() or flush() of one sink concurrently: it dispatches under its
// own lock, in the order records are logged. Implementations must not log from inside write()
// (re-entrant records are dropped and counted as sink errors). A sink shared by several
// Loggers, or used directly by other threads, must synchronise itself; the sinks below do.
class LogSink {
public:
    virtual ~LogSink() = default;

    // Delivers one record. Returns false on failure (counted by the Logger). May throw; the
    // Logger catches and counts exceptions.
    virtual bool write(const LogRecord& record) = 0;

    // Pushes buffered records to their destination. Default: nothing to do.
    virtual void flush() {}

protected:
    LogSink() = default;
    LogSink(const LogSink&) = default;
    LogSink& operator=(const LogSink&) = default;
};

// Sends each record to OutputDebugStringW (visible in a debugger or Sysinternals DebugView).
class DebugOutputSink final : public LogSink {
public:
    bool write(const LogRecord& record) override;
};

struct FileSinkOptions {
    // Active log file, e.g. %LOCALAPPDATA%\Lucent\logs\lucent.log. The directory must exist.
    std::filesystem::path path;
    // Rotation threshold. A record that would take the active file past this size first
    // rotates it (unless the file is empty, so an oversized record is still written).
    std::uint64_t max_file_bytes = std::uint64_t{8} << 20;  // 8 MiB
    // Files kept in total, including the active one: "lucent.log", "lucent.1.log" (newest
    // rotated) ... "lucent.<max_files-1>.log" (oldest). 1 means the active file is simply
    // restarted when full. Must be 1..100.
    std::uint32_t max_files = 5;
};

// Appends UTF-8 lines ("\n"-terminated, no BOM) to a size-capped set of rotated files.
//
// Each record is written straight to the OS with one WriteFile call (no user-space buffer), so
// records survive a crash of the process. The active file is opened for append-only access and
// shared for reading and deletion, so it can be viewed while Lucent runs; a second writer
// (another process using the same file) is refused at open(), so give each process its own
// file name. If a rotation step fails (for example another program holds a rotated file open
// without delete sharing), writing continues in the active file, the failure is counted, and
// rotation is retried after a further max_file_bytes / 8 bytes.
//
// Thread-safety: all members are safe to call concurrently.
class FileSink final : public LogSink {
public:
    // Opens (creating if needed) the active file and appends to its existing content.
    // InvalidArgument for an empty path or out-of-range options; Win32 failures otherwise.
    [[nodiscard]] static Expected<std::shared_ptr<FileSink>> open(FileSinkOptions options);

    ~FileSink() override;
    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    bool write(const LogRecord& record) override;
    void flush() override;  // FlushFileBuffers: forces the OS cache to disk (slow; rarely needed)

    [[nodiscard]] const FileSinkOptions& options() const noexcept { return options_; }

    // Path of rotated file `index` (1 = newest), e.g. "lucent.1.log" for "lucent.log".
    [[nodiscard]] std::filesystem::path rotated_path(std::uint32_t index) const;

private:
    explicit FileSink(FileSinkOptions options) noexcept;
    Status open_active();
    bool rotate();  // requires mutex_ held

    FileSinkOptions options_;
    mutable std::mutex mutex_;
    void* handle_ = nullptr;  // Win32 HANDLE of the active file, nullptr when closed
    std::uint64_t active_bytes_ = 0;
    std::uint64_t next_rotation_attempt_ = 0;  // retry threshold after a failed rotation
};

// Keeps the most recent records in memory. Intended for tests and in-app diagnostics views.
// Thread-safety: all members are safe to call concurrently.
class MemorySink final : public LogSink {
public:
    // Keeps at most `capacity` records (at least 1); older records are discarded first.
    explicit MemorySink(std::size_t capacity = 10'000);

    bool write(const LogRecord& record) override;

    [[nodiscard]] std::vector<LogRecord> records() const;  // oldest first (a snapshot copy)
    [[nodiscard]] std::size_t size() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::size_t capacity_;
    std::deque<LogRecord> records_;
};

struct LogStats {
    std::uint64_t records_dispatched = 0;  // records that passed the filter and reached dispatch
    std::uint64_t format_errors = 0;       // std::format failures (for example std::bad_alloc)
    std::uint64_t sink_errors = 0;         // sink write/flush failures, exceptions and re-entry
};

// Filters and dispatches records to sinks. Thread-safety: all members are safe to call
// concurrently. Configuration changes (set_level, add/remove sinks) take effect for records
// logged after the call returns.
class Logger {
public:
    Logger() noexcept;  // minimum level Info, no sinks
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel minimum) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;

    // True when a record at `level` would be dispatched: level >= minimum and at least one sink
    // is attached. This is the pre-formatting check used by log().
    [[nodiscard]] bool should_log(LogLevel level) const noexcept;

    // Attaches a sink (InvalidArgument for nullptr; OutOfMemory if the list cannot grow).
    // Attaching the same sink twice is a no-op.
    [[nodiscard]] Status add_sink(std::shared_ptr<LogSink> sink);
    // Detaches a sink; returns false when it was not attached.
    bool remove_sink(const LogSink* sink) noexcept;
    void clear_sinks() noexcept;

    // Dispatches an already formatted message. Never throws.
    void write(LogLevel level, std::string_view category, std::string_view message) noexcept;

    // Formats and dispatches, skipping formatting when should_log(level) is false. Never throws.
    template <class... Args>
    void log(LogLevel level, std::string_view category, std::format_string<Args...> fmt,
             Args&&... args) noexcept {
        if (!should_log(level)) {
            return;
        }
        std::string message;
        try {
            message = std::format(fmt, std::forward<Args>(args)...);
        } catch (...) {
            note_format_error();
            return;
        }
        write(level, category, message);
    }

    // Flushes every sink. Never throws.
    void flush() noexcept;

    [[nodiscard]] LogStats stats() const noexcept;

private:
    void note_format_error() noexcept;

    std::atomic<std::uint8_t> level_;
    std::atomic<std::size_t> sink_count_{0};  // mirrors sinks_.size() for the lock-free filter
    std::atomic<std::uint64_t> records_dispatched_{0};
    std::atomic<std::uint64_t> format_errors_{0};
    std::atomic<std::uint64_t> sink_errors_{0};
    mutable std::mutex mutex_;  // guards sinks_ and serialises dispatch
    std::vector<std::shared_ptr<LogSink>> sinks_;
};

// The process-wide logger: created on first call, never destroyed.
[[nodiscard]] Logger& logger() noexcept;

template <class... Args>
void log_trace(std::string_view category, std::format_string<Args...> fmt, Args&&... args) noexcept {
    logger().log(LogLevel::Trace, category, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void log_debug(std::string_view category, std::format_string<Args...> fmt, Args&&... args) noexcept {
    logger().log(LogLevel::Debug, category, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void log_info(std::string_view category, std::format_string<Args...> fmt, Args&&... args) noexcept {
    logger().log(LogLevel::Info, category, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void log_warn(std::string_view category, std::format_string<Args...> fmt, Args&&... args) noexcept {
    logger().log(LogLevel::Warn, category, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void log_error(std::string_view category, std::format_string<Args...> fmt, Args&&... args) noexcept {
    logger().log(LogLevel::Error, category, fmt, std::forward<Args>(args)...);
}

}  // namespace lucent::core
