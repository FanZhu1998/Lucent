#include "lucent/core/log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <iterator>
#include <new>

#include "lucent/core/text.hpp"
#include "lucent/core/win32_error.hpp"

#include "win32.hpp"

namespace lucent::core {

namespace {

// Set while this thread is dispatching to sinks; a sink that logs would otherwise deadlock
// on the logger's (non-recursive) mutex.
thread_local bool t_dispatching = false;

class DispatchScope {
public:
    DispatchScope() noexcept { t_dispatching = true; }
    ~DispatchScope() { t_dispatching = false; }
    DispatchScope(const DispatchScope&) = delete;
    DispatchScope& operator=(const DispatchScope&) = delete;
};

constexpr std::string_view level_tag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warn:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?????";
}

// Appends `text` with malformed UTF-8 replaced and control characters escaped, so a record
// always occupies exactly one line. The escaping is for readability, not reversible.
void append_line_safe(std::string& out, std::string_view text) {
    std::string sanitized;
    if (!is_valid_utf8(text)) {
        sanitized = sanitize_utf8(text);
        text = sanitized;
    }
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20 && byte != 0x7F) {
            out.push_back(c);
            continue;
        }
        switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                std::format_to(std::back_inserter(out), "\\x{:02x}", byte);
                break;
        }
    }
}

char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ascii_nocase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return ascii_lower(x) == ascii_lower(y);
           });
}

}  // namespace

// ---- Levels and formatting ------------------------------------------------------------------

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Error:
            return "error";
    }
    return "unknown";
}

Expected<LogLevel> parse_log_level(std::string_view text) {
    constexpr std::array kLevels = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn,
                                    LogLevel::Error};
    for (const LogLevel level : kLevels) {
        if (equals_ascii_nocase(text, to_string(level))) {
            return level;
        }
    }
    if (equals_ascii_nocase(text, "warning")) {
        return LogLevel::Warn;
    }
    return fail(
        ErrorCode::InvalidArgument,
        "unknown log level '" + sanitize_utf8(text) + "' (expected trace, debug, info, warn or error)",
        "core.log.parse_level");
}

std::string format_log_time(LogTime time) {
    // For a millisecond time_point, %S prints seconds with three fractional digits.
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", time);
}

std::string format_log_line(const LogRecord& record) {
    std::string line = format_log_time(record.time);
    line.reserve(line.size() + record.category.size() + record.message.size() + 24);
    std::format_to(std::back_inserter(line), " {} [{}] ", level_tag(record.level), record.thread_id);
    append_line_safe(line, record.category);
    line += ": ";
    append_line_safe(line, record.message);
    return line;
}

// ---- DebugOutputSink ------------------------------------------------------------------------

bool DebugOutputSink::write(const LogRecord& record) {
    std::wstring text = utf8_to_wide_lossy(format_log_line(record));
    text.push_back(L'\n');
    ::OutputDebugStringW(text.c_str());
    return true;
}

// ---- FileSink -------------------------------------------------------------------------------

FileSink::FileSink(FileSinkOptions options) noexcept : options_(std::move(options)) {}

FileSink::~FileSink() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
}

Expected<std::shared_ptr<FileSink>> FileSink::open(FileSinkOptions options) {
    constexpr const char* kWhere = "core.log.file_sink.open";
    if (options.path.empty()) {
        return fail(ErrorCode::InvalidArgument, "log file path is empty", kWhere);
    }
    if (options.max_file_bytes == 0) {
        return fail(ErrorCode::InvalidArgument, "max_file_bytes must be positive", kWhere);
    }
    if (options.max_files < 1 || options.max_files > 100) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("max_files must be 1..100, got {}", options.max_files), kWhere);
    }
    std::shared_ptr<FileSink> sink;
    try {
        sink.reset(new FileSink(std::move(options)));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the file sink", kWhere);
    }
    {
        std::lock_guard lock(sink->mutex_);
        if (auto status = sink->open_active(); !status) {
            return Unexpected(std::move(status.error()));
        }
    }
    return sink;
}

Status FileSink::open_active() {
    constexpr const char* kWhere = "core.log.file_sink.open";
    auto api_path = detail::win32_api_path(options_.path, kWhere);
    if (!api_path) {
        return Unexpected(std::move(api_path.error()));
    }
    // Append-only access: every WriteFile lands at the current end of file. Readers and
    // deletion are shared so logs can be viewed and cleaned up while Lucent runs; writers are
    // not, so two sinks never interleave within one file.
    detail::UniqueHandle handle(::CreateFileW(api_path->c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle) {
        const DWORD code = ::GetLastError();
        return Unexpected(win32_error(code, kWhere, "cannot open log file " + detail::quoted(options_.path)));
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle.get(), &size)) {
        const DWORD code = ::GetLastError();
        return Unexpected(
            win32_error(code, kWhere, "cannot query the size of log file " + detail::quoted(options_.path)));
    }
    handle_ = handle.release();
    active_bytes_ = static_cast<std::uint64_t>(size.QuadPart);
    next_rotation_attempt_ = 0;
    return ok();
}

std::filesystem::path FileSink::rotated_path(std::uint32_t index) const {
    std::filesystem::path path = options_.path;
    std::wstring name = path.stem().native();
    name += L'.';
    name += std::to_wstring(index);
    name += path.extension().native();
    path.replace_filename(name);
    return path;
}

bool FileSink::rotate() {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;

    bool succeeded = true;
    const auto api = [](const std::filesystem::path& path) {
        auto converted = detail::win32_api_path(path, "core.log.file_sink.rotate");
        return converted ? std::move(*converted) : std::wstring();
    };
    const auto remove_file = [&](const std::filesystem::path& path) {
        const std::wstring target = api(path);
        if (target.empty() || (!::DeleteFileW(target.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)) {
            succeeded = false;
        }
    };
    const auto move_file = [&](const std::filesystem::path& from, const std::filesystem::path& to) {
        const std::wstring source = api(from);
        const std::wstring target = api(to);
        if (source.empty() || target.empty() ||
            (!::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) &&
             ::GetLastError() != ERROR_FILE_NOT_FOUND)) {
            succeeded = false;
        }
    };

    if (options_.max_files == 1) {
        remove_file(options_.path);  // restart the only file
    } else {
        remove_file(rotated_path(options_.max_files - 1));  // drop the oldest
        for (std::uint32_t index = options_.max_files - 2; index >= 1; --index) {
            move_file(rotated_path(index), rotated_path(index + 1));
        }
        move_file(options_.path, rotated_path(1));
    }

    // Reopen the active file even after a failed step: if the rename failed this appends to
    // the old file, which is better than losing records. A failed reopen leaves handle_ null
    // and write() retries on the next record.
    if (!open_active()) {
        succeeded = false;
    }
    if (!succeeded) {
        next_rotation_attempt_ = active_bytes_ + std::max<std::uint64_t>(1, options_.max_file_bytes / 8);
    }
    return succeeded;
}

bool FileSink::write(const LogRecord& record) {
    std::string line = format_log_line(record);
    line.push_back('\n');
    if (line.size() > MAXDWORD) {
        return false;
    }

    std::lock_guard lock(mutex_);
    if (handle_ == nullptr && !open_active()) {
        return false;
    }
    bool succeeded = true;
    if (active_bytes_ > 0 && active_bytes_ + line.size() > options_.max_file_bytes &&
        active_bytes_ >= next_rotation_attempt_) {
        succeeded = rotate();
        if (handle_ == nullptr) {
            return false;
        }
    }
    DWORD written = 0;
    if (!::WriteFile(static_cast<HANDLE>(handle_), line.data(), static_cast<DWORD>(line.size()), &written,
                     nullptr)) {
        return false;
    }
    active_bytes_ += written;
    return succeeded && written == line.size();
}

void FileSink::flush() {
    std::lock_guard lock(mutex_);
    if (handle_ != nullptr) {
        ::FlushFileBuffers(static_cast<HANDLE>(handle_));
    }
}

// ---- MemorySink -----------------------------------------------------------------------------

MemorySink::MemorySink(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {}

bool MemorySink::write(const LogRecord& record) {
    std::lock_guard lock(mutex_);
    if (records_.size() == capacity_) {
        records_.pop_front();
    }
    records_.push_back(record);
    return true;
}

std::vector<LogRecord> MemorySink::records() const {
    std::lock_guard lock(mutex_);
    return {records_.begin(), records_.end()};
}

std::size_t MemorySink::size() const {
    std::lock_guard lock(mutex_);
    return records_.size();
}

void MemorySink::clear() {
    std::lock_guard lock(mutex_);
    records_.clear();
}

// ---- Logger ---------------------------------------------------------------------------------

// Level and sink count are read with relaxed ordering: the filter is advisory, and dispatch
// re-reads the sink list under the mutex.

Logger::Logger() noexcept : level_(static_cast<std::uint8_t>(LogLevel::Info)) {}

Logger::~Logger() = default;

void Logger::set_level(LogLevel minimum) noexcept {
    level_.store(static_cast<std::uint8_t>(minimum), std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
}

bool Logger::should_log(LogLevel level) const noexcept {
    return static_cast<std::uint8_t>(level) >= level_.load(std::memory_order_relaxed) &&
           sink_count_.load(std::memory_order_relaxed) > 0;
}

Status Logger::add_sink(std::shared_ptr<LogSink> sink) {
    if (sink == nullptr) {
        return fail(ErrorCode::InvalidArgument, "cannot add a null log sink", "core.log.add_sink");
    }
    std::lock_guard lock(mutex_);
    if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) {
        return ok();
    }
    try {
        sinks_.push_back(std::move(sink));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "cannot grow the log sink list", "core.log.add_sink");
    }
    sink_count_.store(sinks_.size(), std::memory_order_relaxed);
    return ok();
}

bool Logger::remove_sink(const LogSink* sink) noexcept {
    std::shared_ptr<LogSink> removed;  // released after unlocking, in case its destructor logs
    {
        std::lock_guard lock(mutex_);
        const auto it =
            std::find_if(sinks_.begin(), sinks_.end(), [sink](const auto& s) { return s.get() == sink; });
        if (it == sinks_.end()) {
            return false;
        }
        removed = std::move(*it);
        sinks_.erase(it);
        sink_count_.store(sinks_.size(), std::memory_order_relaxed);
    }
    return true;
}

void Logger::clear_sinks() noexcept {
    std::vector<std::shared_ptr<LogSink>> removed;  // released after unlocking
    {
        std::lock_guard lock(mutex_);
        removed.swap(sinks_);
        sink_count_.store(0, std::memory_order_relaxed);
    }
}

void Logger::note_format_error() noexcept {
    format_errors_.fetch_add(1, std::memory_order_relaxed);
}

void Logger::write(LogLevel level, std::string_view category, std::string_view message) noexcept {
    if (!should_log(level)) {
        return;
    }
    if (t_dispatching) {
        sink_errors_.fetch_add(1, std::memory_order_relaxed);  // re-entrant log from a sink
        return;
    }
    LogRecord record;
    try {
        record.time = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
        record.thread_id = static_cast<std::uint32_t>(::GetCurrentThreadId());
        record.level = level;
        record.category.assign(category);
        record.message.assign(message);
    } catch (...) {
        note_format_error();
        return;
    }
    records_dispatched_.fetch_add(1, std::memory_order_relaxed);

    try {
        std::lock_guard lock(mutex_);
        const DispatchScope scope;
        for (const auto& sink : sinks_) {
            try {
                if (!sink->write(record)) {
                    sink_errors_.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        sink_errors_.fetch_add(1, std::memory_order_relaxed);  // mutex failure (system_error)
    }
}

void Logger::flush() noexcept {
    if (t_dispatching) {
        sink_errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    try {
        std::lock_guard lock(mutex_);
        const DispatchScope scope;
        for (const auto& sink : sinks_) {
            try {
                sink->flush();
            } catch (...) {
                sink_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        sink_errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

LogStats Logger::stats() const noexcept {
    LogStats stats;
    stats.records_dispatched = records_dispatched_.load(std::memory_order_relaxed);
    stats.format_errors = format_errors_.load(std::memory_order_relaxed);
    stats.sink_errors = sink_errors_.load(std::memory_order_relaxed);
    return stats;
}

Logger& logger() noexcept {
    // Constructed in static storage on first use and never destroyed, so logging stays valid
    // during static initialisation and destruction of other objects. Logger() is noexcept.
    alignas(Logger) static std::byte storage[sizeof(Logger)];
    static Logger* const instance = ::new (static_cast<void*>(storage)) Logger();
    return *instance;
}

}  // namespace lucent::core
