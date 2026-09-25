#include "lucent/core/log.hpp"

#include <chrono>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <windows.h>

#include "test_support.hpp"

// Formattable probe that counts how often it is formatted, and optionally throws while being
// formatted (a runtime formatting failure that std::format_string cannot catch at compile time).
struct FormatProbe {
    int* calls = nullptr;
    bool fail = false;
};

namespace std {
template <>
struct formatter<FormatProbe> : formatter<int> {
    template <class FormatContext>
    auto format(const FormatProbe& probe, FormatContext& context) const {
        ++*probe.calls;
        if (probe.fail) {
            throw runtime_error("probe formatting failure");
        }
        return formatter<int>::format(*probe.calls, context);
    }
};
}  // namespace std

namespace lucent::core {
namespace {

using namespace std::chrono_literals;
using test_support::read_text;
using test_support::TempDir;

// 2026-09-24T07:05:09.042Z
const LogTime kFixedTime = std::chrono::sys_days{std::chrono::year{2026} / 9 / 24} + 7h + 5min + 9s + 42ms;

LogRecord make_record(std::string message, LogLevel level = LogLevel::Info,
                      std::string category = "core.test") {
    LogRecord record;
    record.time = kFixedTime;
    record.thread_id = 1234;
    record.level = level;
    record.category = std::move(category);
    record.message = std::move(message);
    return record;
}

// Sink that fails or throws, for error-accounting tests.
class FailingSink final : public LogSink {
public:
    explicit FailingSink(bool throws) : throws_(throws) {}
    bool write(const LogRecord&) override {
        if (throws_) {
            throw std::runtime_error("sink failure");
        }
        return false;
    }
    void flush() override {
        if (throws_) {
            throw std::runtime_error("flush failure");
        }
    }

private:
    bool throws_;
};

// Sink that logs from inside write(), which the logger must refuse without deadlocking.
class ReentrantSink final : public LogSink {
public:
    explicit ReentrantSink(Logger& logger) : logger_(logger) {}
    bool write(const LogRecord&) override {
        logger_.write(LogLevel::Error, "core.test", "from inside a sink");
        return true;
    }

private:
    Logger& logger_;
};

// ---- Levels and formatting ------------------------------------------------------------------

TEST(Logging, LevelNamesRoundTrip) {
    for (const LogLevel level :
         {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error}) {
        const auto parsed = parse_log_level(to_string(level));
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, level);
    }
    EXPECT_EQ(*parse_log_level("WARNING"), LogLevel::Warn);
    EXPECT_EQ(*parse_log_level("Info"), LogLevel::Info);
    for (const std::string_view bad : {"", "verbose", "info ", "err"}) {
        const auto parsed = parse_log_level(bad);
        ASSERT_FALSE(parsed) << bad;
        EXPECT_EQ(parsed.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(Logging, TimestampIsIso8601UtcWithMilliseconds) {
    EXPECT_EQ(format_log_time(kFixedTime), "2026-09-24T07:05:09.042Z");
    EXPECT_EQ(format_log_time(LogTime{}), "1970-01-01T00:00:00.000Z");
}

TEST(Logging, LineLayout) {
    EXPECT_EQ(format_log_line(make_record("hello")),
              "2026-09-24T07:05:09.042Z INFO  [1234] core.test: hello");
    EXPECT_EQ(format_log_line(make_record("x", LogLevel::Trace)),
              "2026-09-24T07:05:09.042Z TRACE [1234] core.test: x");
    EXPECT_EQ(format_log_line(make_record("x", LogLevel::Warn)),
              "2026-09-24T07:05:09.042Z WARN  [1234] core.test: x");
    EXPECT_EQ(format_log_line(make_record("x", LogLevel::Error)),
              "2026-09-24T07:05:09.042Z ERROR [1234] core.test: x");
}

TEST(Logging, LineEscapesControlCharactersAndReplacesInvalidUtf8) {
    const std::string line = format_log_line(make_record(std::string("a\nb\r\tc\x01", 7) + "\xFF"
                                                                                           "\xE5\x86\x99",
                                                         LogLevel::Debug, "cat\negory"));
    EXPECT_EQ(line,
              "2026-09-24T07:05:09.042Z DEBUG [1234] cat\\negory: a\\nb\\r\\tc\\x01\xEF\xBF\xBD\xE5\x86\x99");
    EXPECT_EQ(line.find('\n'), std::string::npos);
}

// ---- Logger ---------------------------------------------------------------------------------

TEST(Logging, FiltersBeforeFormatting) {
    Logger logger;
    const auto sink = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(sink));
    EXPECT_EQ(logger.level(), LogLevel::Info);  // default

    int calls = 0;
    logger.set_level(LogLevel::Warn);
    EXPECT_FALSE(logger.should_log(LogLevel::Info));
    EXPECT_TRUE(logger.should_log(LogLevel::Error));
    logger.log(LogLevel::Info, "core.test", "value {}", FormatProbe{&calls});
    EXPECT_EQ(calls, 0) << "a filtered record must not be formatted";
    EXPECT_EQ(sink->size(), 0u);

    logger.log(LogLevel::Warn, "core.test", "value {}", FormatProbe{&calls});
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(sink->size(), 1u);
    EXPECT_EQ(sink->records()[0].message, "value 1");
}

TEST(Logging, NoSinksMeansNoFormatting) {
    Logger logger;
    logger.set_level(LogLevel::Trace);
    int calls = 0;
    EXPECT_FALSE(logger.should_log(LogLevel::Error));
    logger.log(LogLevel::Error, "core.test", "{}", FormatProbe{&calls});
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(logger.stats().records_dispatched, 0u);
}

TEST(Logging, RecordsCarryMetadata) {
    Logger logger;
    const auto sink = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(sink));
    const auto before = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    logger.log(LogLevel::Error, "raw.libraw", "failed to unpack {} ({})", "IMG_0001.CR3", -4);
    const auto after = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());

    const auto records = sink->records();
    ASSERT_EQ(records.size(), 1u);
    const LogRecord& record = records[0];
    EXPECT_EQ(record.level, LogLevel::Error);
    EXPECT_EQ(record.category, "raw.libraw");
    EXPECT_EQ(record.message, "failed to unpack IMG_0001.CR3 (-4)");
    EXPECT_EQ(record.thread_id, ::GetCurrentThreadId());
    EXPECT_GE(record.time, before);
    EXPECT_LE(record.time, after);
    EXPECT_EQ(logger.stats().records_dispatched, 1u);
}

TEST(Logging, FormattingFailureIsCaughtAndCounted) {
    Logger logger;
    const auto sink = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(sink));
    int calls = 0;
    EXPECT_NO_THROW(logger.log(LogLevel::Error, "core.test", "{}", FormatProbe{&calls, true}));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(sink->size(), 0u);
    EXPECT_EQ(logger.stats().format_errors, 1u);
}

TEST(Logging, SinkFailuresAreCountedAndDoNotStopOtherSinks) {
    Logger logger;
    const auto good = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(std::make_shared<FailingSink>(false)));
    ASSERT_TRUE(logger.add_sink(std::make_shared<FailingSink>(true)));
    ASSERT_TRUE(logger.add_sink(good));

    EXPECT_NO_THROW(logger.write(LogLevel::Info, "core.test", "one"));
    EXPECT_NO_THROW(logger.write(LogLevel::Info, "core.test", "two"));
    EXPECT_EQ(good->size(), 2u);
    EXPECT_EQ(logger.stats().sink_errors, 4u);  // two failing sinks x two records

    EXPECT_NO_THROW(logger.flush());
    EXPECT_EQ(logger.stats().sink_errors, 5u);  // the throwing sink's flush
}

TEST(Logging, ReentrantLoggingIsDroppedWithoutDeadlock) {
    Logger logger;
    const auto memory = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(std::make_shared<ReentrantSink>(logger)));
    ASSERT_TRUE(logger.add_sink(memory));
    logger.write(LogLevel::Info, "core.test", "outer");
    ASSERT_EQ(memory->size(), 1u);
    EXPECT_EQ(memory->records()[0].message, "outer");
    EXPECT_EQ(logger.stats().sink_errors, 1u);
}

TEST(Logging, SinkManagement) {
    Logger logger;
    const Status null_sink = logger.add_sink(nullptr);
    ASSERT_FALSE(null_sink);
    EXPECT_EQ(null_sink.error().code, ErrorCode::InvalidArgument);

    const auto sink = std::make_shared<MemorySink>();
    ASSERT_TRUE(logger.add_sink(sink));
    ASSERT_TRUE(logger.add_sink(sink));  // duplicate: no-op
    logger.write(LogLevel::Info, "core.test", "once");
    EXPECT_EQ(sink->size(), 1u);

    EXPECT_TRUE(logger.remove_sink(sink.get()));
    EXPECT_FALSE(logger.remove_sink(sink.get()));
    logger.write(LogLevel::Info, "core.test", "not delivered");
    EXPECT_EQ(sink->size(), 1u);

    ASSERT_TRUE(logger.add_sink(sink));
    logger.clear_sinks();
    EXPECT_FALSE(logger.should_log(LogLevel::Error));
}

TEST(Logging, MemorySinkKeepsMostRecentRecords) {
    MemorySink sink(3);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(sink.write(make_record(std::to_string(i))));
    }
    const auto records = sink.records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].message, "2");
    EXPECT_EQ(records[2].message, "4");
    sink.clear();
    EXPECT_EQ(sink.size(), 0u);
}

TEST(Logging, ConcurrentLoggingDeliversEveryRecord) {
    Logger logger;
    const auto sink = std::make_shared<MemorySink>(100'000);
    ASSERT_TRUE(logger.add_sink(sink));
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&logger, t] {
            for (int i = 0; i < kPerThread; ++i) {
                logger.log(LogLevel::Info, "core.test", "thread {} record {}", t, i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(sink->size(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(logger.stats().records_dispatched, static_cast<std::uint64_t>(kThreads * kPerThread));
}

TEST(Logging, GlobalLoggerAndHelpers) {
    Logger& global = logger();
    EXPECT_EQ(&global, &logger());
    const LogLevel saved_level = global.level();
    const auto sink = std::make_shared<MemorySink>();
    ASSERT_TRUE(global.add_sink(sink));
    global.set_level(LogLevel::Info);

    log_trace("core.test", "trace {}", 1);
    log_debug("core.test", "debug {}", 2);
    log_info("core.test", "info {}", 3);
    log_warn("core.test", "warn {}", 4);
    log_error("core.test", "error {}", 5);

    global.remove_sink(sink.get());
    global.set_level(saved_level);

    const auto records = sink->records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].level, LogLevel::Info);
    EXPECT_EQ(records[0].message, "info 3");
    EXPECT_EQ(records[1].level, LogLevel::Warn);
    EXPECT_EQ(records[2].level, LogLevel::Error);
    EXPECT_EQ(records[2].message, "error 5");
}

TEST(Logging, DebugOutputSinkAcceptsRecords) {
    // OutputDebugStringW has no observable result without a debugger; this checks the sink
    // handles Unicode and malformed text without failing.
    DebugOutputSink sink;
    EXPECT_TRUE(sink.write(make_record("\xE5\x86\x99\xE7\x9C\x9F \xF0\x9F\x93\xB7")));
    EXPECT_TRUE(sink.write(make_record("bad \xFF utf-8")));
}

// ---- FileSink -------------------------------------------------------------------------------

TEST(FileSink, WritesUtf8LinesWithoutBom) {
    const TempDir dir;
    const auto path = dir / L"Lucent 日志.log";
    auto sink = FileSink::open({path, 1 << 20, 3});
    ASSERT_TRUE(sink) << sink.error().describe();
    const LogRecord first = make_record("\xE5\x86\x99\xE7\x9C\x9F \xF0\x9F\x93\xB7");
    const LogRecord second = make_record("second", LogLevel::Warn);
    EXPECT_TRUE((*sink)->write(first));
    EXPECT_TRUE((*sink)->write(second));
    (*sink)->flush();
    EXPECT_EQ(read_text(path), format_log_line(first) + "\n" + format_log_line(second) + "\n");
}

TEST(FileSink, AppendsToExistingFile) {
    const TempDir dir;
    const auto path = dir / L"lucent.log";
    {
        auto sink = FileSink::open({path, 1 << 20, 3});
        ASSERT_TRUE(sink);
        EXPECT_TRUE((*sink)->write(make_record("first run")));
    }
    auto sink = FileSink::open({path, 1 << 20, 3});
    ASSERT_TRUE(sink);
    EXPECT_TRUE((*sink)->write(make_record("second run")));
    EXPECT_EQ(read_text(path), format_log_line(make_record("first run")) + "\n" +
                                   format_log_line(make_record("second run")) + "\n");
}

TEST(FileSink, RotatesAndKeepsConfiguredNumberOfFiles) {
    const TempDir dir;
    const auto path = dir / L"lucent.log";
    const std::size_t line_bytes = format_log_line(make_record("record 00")).size() + 1;
    const std::uint64_t cap = 2 * line_bytes;  // two records per file
    auto opened = FileSink::open({path, cap, 3});
    ASSERT_TRUE(opened) << opened.error().describe();
    FileSink& sink = **opened;
    EXPECT_EQ(sink.rotated_path(1), dir / L"lucent.1.log");
    EXPECT_EQ(sink.rotated_path(2), dir / L"lucent.2.log");

    std::vector<std::string> lines;
    for (int i = 0; i < 20; ++i) {
        const LogRecord record = make_record(std::format("record {:02}", i));
        lines.push_back(format_log_line(record) + "\n");
        EXPECT_TRUE(sink.write(record)) << i;
    }

    // Three files: the active one and two rotated ones; nothing older.
    EXPECT_FALSE(std::filesystem::exists(dir / L"lucent.3.log"));
    const std::string newest = read_text(path);
    const std::string middle = read_text(sink.rotated_path(1));
    const std::string oldest = read_text(sink.rotated_path(2));
    for (const std::string* text : {&newest, &middle, &oldest}) {
        EXPECT_LE(text->size(), cap);
    }
    // Oldest to newest, the files hold the last six records in order.
    EXPECT_EQ(oldest + middle + newest,
              lines[14] + lines[15] + lines[16] + lines[17] + lines[18] + lines[19]);
}

TEST(FileSink, SingleFileRestartsWhenFull) {
    const TempDir dir;
    const auto path = dir / L"only.log";
    const std::size_t line_bytes = format_log_line(make_record("record 00")).size() + 1;
    auto sink = FileSink::open({path, 3 * line_bytes, 1});
    ASSERT_TRUE(sink);
    std::string last;
    for (int i = 0; i < 10; ++i) {
        const LogRecord record = make_record(std::format("record {:02}", i));
        EXPECT_TRUE((*sink)->write(record));
        last = format_log_line(record) + "\n";
    }
    EXPECT_FALSE(std::filesystem::exists(dir / L"only.1.log"));
    const std::string text = read_text(path);
    EXPECT_LE(text.size(), 3 * line_bytes);
    EXPECT_TRUE(text.ends_with(last));
}

TEST(FileSink, OversizedRecordIsStillWritten) {
    const TempDir dir;
    const auto path = dir / L"tiny.log";
    auto sink = FileSink::open({path, 10, 2});
    ASSERT_TRUE(sink);
    const LogRecord big = make_record(std::string(100, 'x'));
    EXPECT_TRUE((*sink)->write(big));
    EXPECT_EQ(read_text(path), format_log_line(big) + "\n");
    const LogRecord next = make_record("next");
    EXPECT_TRUE((*sink)->write(next));  // rotates the oversized file away first
    EXPECT_EQ(read_text(path), format_log_line(next) + "\n");
    EXPECT_EQ(read_text((*sink)->rotated_path(1)), format_log_line(big) + "\n");
}

TEST(FileSink, RejectsInvalidOptions) {
    const TempDir dir;
    const FileSinkOptions cases[] = {
        {std::filesystem::path(), 1024, 3},
        {dir / L"a.log", 0, 3},
        {dir / L"a.log", 1024, 0},
        {dir / L"a.log", 1024, 101},
    };
    for (const FileSinkOptions& options : cases) {
        const auto sink = FileSink::open(options);
        ASSERT_FALSE(sink);
        EXPECT_EQ(sink.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(FileSink, MissingDirectoryIsNotFound) {
    const TempDir dir;
    const auto sink = FileSink::open({dir / L"no-such-dir" / L"a.log", 1024, 3});
    ASSERT_FALSE(sink);
    EXPECT_EQ(sink.error().code, ErrorCode::NotFound);
}

TEST(FileSink, SecondWriterOnSameFileIsRefused) {
    const TempDir dir;
    const auto path = dir / L"a.log";
    const auto first = FileSink::open({path, 1024, 3});
    ASSERT_TRUE(first);
    const auto second = FileSink::open({path, 1024, 3});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, ErrorCode::IoError);
    EXPECT_EQ(second.error().native_code, ERROR_SHARING_VIOLATION);
}

TEST(FileSink, WorksThroughTheLogger) {
    const TempDir dir;
    const auto path = dir / L"lucent.log";
    auto sink = FileSink::open({path, 1 << 20, 2});
    ASSERT_TRUE(sink);
    Logger logger;
    ASSERT_TRUE(logger.add_sink(*sink));
    logger.log(LogLevel::Info, "core.test", "{} + {} = {}", 1, 2, 3);
    logger.flush();
    const std::string text = read_text(path);
    EXPECT_TRUE(text.ends_with(" core.test: 1 + 2 = 3\n")) << text;
    EXPECT_EQ(logger.stats().sink_errors, 0u);
}

}  // namespace
}  // namespace lucent::core
