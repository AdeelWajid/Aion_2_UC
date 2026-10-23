// Logger.hpp — leveled logger with console + file sinks, written from a background
// thread. Log calls never block the render loop: a console in QuickEdit mode (or a
// slow/redirected stdout) can otherwise stall a write for seconds.
//
// The console sink is coloured per level, the file sink never is: escape sequences in a
// log file are noise, and the file is what gets read back later. Colour is enabled only
// when stdout is a real console and virtual terminal processing is on (Windows Terminal
// has it on by default; classic conhost is asked to turn it on here) - when output is
// redirected, the codes would otherwise be printed literally.
//
// The startup banner goes through Raw(), which puts it in the same queue as the log
// lines, so the welcome screen cannot interleave with the first messages.
#pragma once
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// windows.h for the console mode and the wall clock. NOMINMAX before it: this header is
// included from everywhere, and the min/max macros would break std::min in the callers.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace aion {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Off };

// SGR codes for the console sink. Dim marks the parts that repeat on every line
// (timestamp, banner labels) so the message itself carries the contrast.
namespace ansi {
constexpr const char* Reset  = "\x1b[0m";
constexpr const char* Bold   = "\x1b[1m";
constexpr const char* Dim    = "\x1b[90m";
constexpr const char* Red    = "\x1b[91m";
constexpr const char* Green  = "\x1b[32m";
constexpr const char* Yellow = "\x1b[33m";
constexpr const char* Blue   = "\x1b[94m";
constexpr const char* Cyan   = "\x1b[36m";
constexpr const char* White  = "\x1b[97m";
}  // namespace ansi

// The level's own name: used by the header and by --log-level.
inline const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
        case LogLevel::Off:   return "off";
    }
    return "info";
}

// Accepts the names LogLevelName prints, case-insensitively.
inline bool ParseLogLevel(std::string_view name, LogLevel& out) {
    const LogLevel all[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info,
                            LogLevel::Warn,  LogLevel::Error, LogLevel::Off};
    for (LogLevel level : all) {
        const std::string_view candidate = LogLevelName(level);
        if (name.size() != candidate.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(name[i])) == candidate[i];
        if (same) {
            out = level;
            return true;
        }
    }
    return false;
}

// What the console wants coloured, the file wants plain.
inline std::string StripAnsi(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

class Logger {
public:
    static Logger& Instance() {
        static Logger l;
        return l;
    }

    Logger() {
        EnableConsoleColours();
        worker_ = std::thread([this] { Run(); });
    }
    ~Logger() { Shutdown(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void SetLevel(LogLevel l) { level_.store(l, std::memory_order_relaxed); }
    LogLevel Level() const { return level_.load(std::memory_order_relaxed); }
    // Whether the console sink is emitting colour. Tools that print with printf rather than through
    // the log queue (src/recon.cpp) ask this so their own headings match the log's.
    bool ConsoleColour() const { return vt_; }

    // Waits for the queue to drain and for the worker to have written what it took (used before
    // mixing printf with the log, before exit, and by tests).
    void Flush() {
        const std::uint64_t target = pushed_.load(std::memory_order_relaxed);
        for (int spin = 0; spin < 250; ++spin) {
            if (written_.load(std::memory_order_relaxed) >= target) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Optional file sink; empty path disables. Returns false if it could not open.
    bool OpenFile(const std::string& path) {
        std::lock_guard<std::mutex> lk(sink_mu_);
        if (file_) { std::fclose(file_); file_ = nullptr; }
        if (path.empty()) return true;
        file_ = std::fopen(path.c_str(), "ab");
        return file_ != nullptr;
    }

    void Log(LogLevel lv, const char* fmt, ...) {
        if (lv < Level()) return;
        char body[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof(body), fmt, ap);
        va_end(ap);
        Push(lv, body, false);
    }

    // Verbatim multi-line text: the startup banner. Never level-filtered, no timestamp and
    // no tag - it is a picture, not a log record. Colour, if any, is embedded as ANSI: the
    // console keeps it, the file sink strips it.
    void Raw(const std::string& text) { Push(LogLevel::Info, text, true); }

    std::size_t dropped() const {
        std::lock_guard<std::mutex> lk(mu_);
        return dropped_;
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(sink_mu_);
        if (file_) { std::fclose(file_); file_ = nullptr; }
    }

private:
    struct Entry {
        LogLevel Level = LogLevel::Info;
        std::string Body;
        bool Raw = false;
    };

    static constexpr std::size_t kMaxQueued = 8192;

    static const char* Tag(LogLevel lv) {
        switch (lv) {
            case LogLevel::Trace: return "TRACE";
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            case LogLevel::Error: return "ERROR";
            default:              return "?";
        }
    }

    static const char* LevelColour(LogLevel lv) {
        switch (lv) {
            case LogLevel::Trace: return ansi::Dim;
            case LogLevel::Debug: return ansi::Blue;
            case LogLevel::Info:  return ansi::Green;
            case LogLevel::Warn:  return ansi::Yellow;
            case LogLevel::Error: return ansi::Red;
            default:              return ansi::White;
        }
    }

    // One line, with or without colour. The no-colour form is also what the file gets, so
    // a log file reads exactly like the console minus the escape sequences.
    static std::string Format(LogLevel lv, const std::string& body, bool colour) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%02u:%02u:%02u.%03u",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        char tag[8];
        std::snprintf(tag, sizeof(tag), "%-5s", Tag(lv));
        if (!colour) return "[" + std::string(ts) + "][" + tag + "] " + body + "\n";

        std::string out = ansi::Dim;
        out += "["; out += ts; out += "][";
        out += ansi::Reset;
        out += LevelColour(lv);
        out += tag;
        out += ansi::Reset;
        out += ansi::Dim;
        out += "] ";
        out += ansi::Reset;
        // A warning or an error says so in its own colour, not just in its tag.
        if (lv >= LogLevel::Warn) out += LevelColour(lv);
        out += body;
        out += ansi::Reset;
        out += "\n";
        return out;
    }

    void EnableConsoleColours() {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
            vt_ = SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

    void Push(LogLevel lv, const std::string& body, bool raw) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Bounded: never let logging grow without limit (or block) if the sink
            // stalls. Dropped lines are counted, not queued.
            if (queue_.size() >= kMaxQueued) { ++dropped_; return; }
            queue_.push_back(Entry{lv, body, raw});
            pushed_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    void Run() {
        for (;;) {
            Entry e;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stop_) return;
                    continue;
                }
                e = std::move(queue_.front());
                queue_.pop_front();
            }
            std::string console, plain;
            if (e.Raw) {
                console = vt_ ? e.Body : StripAnsi(e.Body);
                plain = StripAnsi(e.Body);
            } else {
                console = Format(e.Level, e.Body, vt_);
                plain = Format(e.Level, e.Body, false);
            }
            std::lock_guard<std::mutex> lk(sink_mu_);
            std::fputs(console.c_str(), stdout);
            std::fflush(stdout);
            if (file_) {
                std::fputs(plain.c_str(), file_);
                std::fflush(file_);
            }
            written_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<std::uint64_t> pushed_{0};   // entries handed to the worker
    std::atomic<std::uint64_t> written_{0};  // entries the worker has written
    mutable std::mutex mu_;
    mutable std::mutex sink_mu_;
    std::condition_variable cv_;
    std::deque<Entry> queue_;
    bool stop_ = false;
    bool vt_ = false;
    std::size_t dropped_ = 0;
    std::thread worker_;
    std::FILE* file_ = nullptr;
};

// ---------------------------------------------------------------- welcome screen ---
// One label/value pair of the banner's detail block.
struct BannerField {
    std::string Label;
    std::string Value;
};

// A phase heading for the console: one accent-coloured line, no rules. Used at Info so a session
// reads as phases (target, initialization, overlay) rather than as one undifferentiated stream.
inline std::string MakeSection(const std::string& title) {
    std::string out = "\n";
    out += ansi::Cyan;
    out += ansi::Bold;
    out += title;
    out += ansi::Reset;
    out += "\n";
    return out;
}

// A section heading for tools that print with printf instead of through the log queue: the same
// look as MakeSection, and plain text when the console would not interpret the codes.
inline std::string SectionForStdio(const std::string& title) {
    // A tool that prints with printf cannot be ordered against the worker thread. Draining the
    // queue first is what keeps the log's own lines above the block that follows, instead of
    // letting the last of them land in the middle of it.
    Logger::Instance().Flush();
    return Logger::Instance().ConsoleColour() ? MakeSection(title) : ("\n" + title + "\n");
}

// The startup screen: who wrote it, and the handful of facts that answer
// "which build is this and which config is it reading" before the first log line. The
// caller supplies the facts, so nothing here has to know about offsets or the client.
// Built with ANSI colour; Logger::Raw() strips it for the file sink.
inline std::string MakeBanner(const std::vector<BannerField>& fields) {
    std::string out;
    out += "\n";
    out += "  ";
    out += ansi::Cyan;
    out += ansi::Bold;
    out += "external overlay";
    out += ansi::Reset;
    out += ansi::Dim;
    out += "   AION2 (UE5)   read-only: no injection, no writes to the client";
    out += ansi::Reset;
    out += "\n";
    out += "  ";
    out += ansi::Dim;
    out += "by ";
    out += ansi::Cyan;
    out += ansi::Bold;
    out += "ZixMatrix";
    out += ansi::Reset;
    out += "\n";
    // The label column is as wide as the widest label plus a gap: a fixed width glued a longer
    // label straight onto its value ("launched asC3Y2u1JUASKLzt8X.exe").
    std::size_t label_w = 0;
    for (const BannerField& f : fields)
        if (f.Label.size() > label_w) label_w = f.Label.size();
    for (const BannerField& f : fields) {
        std::string label = "  " + f.Label;
        label.append(label_w - f.Label.size() + 2, ' ');
        out += ansi::Dim;
        out += label;
        out += ansi::Reset;
        out += f.Value;
        out += "\n";
    }
    out += "\n";
    return out;
}

}  // namespace aion

#define AION_LOG(lv, ...) ::aion::Logger::Instance().Log(lv, __VA_ARGS__)
#define AION_TRACE(...) AION_LOG(::aion::LogLevel::Trace, __VA_ARGS__)
#define AION_DEBUG(...) AION_LOG(::aion::LogLevel::Debug, __VA_ARGS__)
#define AION_INFO(...)  AION_LOG(::aion::LogLevel::Info, __VA_ARGS__)
#define AION_WARN(...)  AION_LOG(::aion::LogLevel::Warn, __VA_ARGS__)
#define AION_ERROR(...) AION_LOG(::aion::LogLevel::Error, __VA_ARGS__)
// Verbatim block for the console (and the file, minus the colour).
#define AION_RAW(text)  ::aion::Logger::Instance().Raw(text)
// A phase heading, printed only when the level would show the lines that follow it: an error-only
// session should not be a list of empty headings.
#define AION_SECTION(title)                                                  \
    do {                                                                     \
        if (::aion::Logger::Instance().Level() <= ::aion::LogLevel::Info)    \
            ::aion::Logger::Instance().Raw(::aion::MakeSection(title));      \
    } while (0)
