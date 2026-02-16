#include "Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

static std::shared_ptr<ImGuiLogSink> s_imgui_sink;

namespace Log {

    void Init() {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        s_imgui_sink = std::make_shared<ImGuiLogSink>();

        std::vector<spdlog::sink_ptr> sinks = { console_sink, s_imgui_sink };

        const char* logger_names[] = { "VWrap", "App", "Render", "Input", "GPU" };
        for (const auto& name : logger_names) {
            auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            logger->set_level(spdlog::level::trace);
            spdlog::register_logger(logger);
        }

        spdlog::set_default_logger(spdlog::get("App"));
        spdlog::set_level(spdlog::level::debug);
    }

    std::shared_ptr<ImGuiLogSink> GetImGuiSink() {
        return s_imgui_sink;
    }

}
