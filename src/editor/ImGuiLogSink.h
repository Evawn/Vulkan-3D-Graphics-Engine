#pragma once

#include <spdlog/sinks/base_sink.h>
#include <mutex>
#include <atomic>
#include <deque>
#include <string>

struct LogEntry {
    spdlog::level::level_enum level;
    std::string logger_name;
    std::string message;
};

class ImGuiLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr size_t MAX_ENTRIES = 5000;

    const std::deque<LogEntry>& GetEntries() const { return m_entries; }

    size_t GetWarnCount()  const { return m_warn_count.load(std::memory_order_relaxed); }
    size_t GetErrorCount() const { return m_err_count.load(std::memory_order_relaxed); }

    // Monotonic version counter — bumps on every new log line. Panels read this
    // to detect "new content arrived since last frame" without comparing the
    // entire deque.
    uint64_t GetVersion() const { return m_version.load(std::memory_order_relaxed); }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        m_entries.clear();
        m_warn_count.store(0, std::memory_order_relaxed);
        m_err_count.store(0, std::memory_order_relaxed);
        m_version.fetch_add(1, std::memory_order_relaxed);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);

        LogEntry entry;
        entry.level = msg.level;
        entry.logger_name = std::string(msg.logger_name.data(), msg.logger_name.size());
        entry.message = std::string(msg.payload.data(), msg.payload.size());

        m_entries.push_back(std::move(entry));
        if (m_entries.size() > MAX_ENTRIES) {
            m_entries.pop_front();
        }

        if (msg.level == spdlog::level::warn) {
            m_warn_count.fetch_add(1, std::memory_order_relaxed);
        } else if (msg.level == spdlog::level::err || msg.level == spdlog::level::critical) {
            m_err_count.fetch_add(1, std::memory_order_relaxed);
        }
        m_version.fetch_add(1, std::memory_order_relaxed);
    }

    void flush_() override {}

private:
    std::deque<LogEntry> m_entries;
    std::atomic<size_t> m_warn_count{0};
    std::atomic<size_t> m_err_count{0};
    std::atomic<uint64_t> m_version{0};
};
