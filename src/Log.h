#pragma once

#include <spdlog/spdlog.h>
#include <memory>
#include "ImGuiLogSink.h"

namespace Log {
    void Init();

    std::shared_ptr<ImGuiLogSink> GetImGuiSink();
}
