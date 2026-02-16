#pragma once

#include <spdlog/sinks/base_sink.h>
#include <mutex>
#include <deque>
#include <string>

struct LogEntry {
    spdlog::level::level_enum level;
    std::string logger_name;
    std::string message;
};

class ImGuiLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr size_t MAX_ENTRIES = 2000;

    const std::deque<LogEntry>& GetEntries() const { return m_entries; }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        m_entries.clear();
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
    }

    void flush_() override {}

private:
    std::deque<LogEntry> m_entries;
};
