#include "EnginePaths.h"

#include "config.h"

#include <spdlog/spdlog.h>
#include <system_error>

namespace engine_paths {

std::filesystem::path GetEngineCacheDir() {
    std::filesystem::path dir(config::CACHE_DIR);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        // Non-fatal — callers will hit a clearer error when they try to write.
        // Logged at warn so it doesn't get lost under info noise.
        if (auto log = spdlog::get("App")) {
            log->warn("EnginePaths: failed to create cache dir {} ({})",
                      dir.string(), ec.message());
        }
    }
    return dir;
}

std::filesystem::path GetPromotedFoliagePath() {
    return GetEngineCacheDir() / "promoted-foliage.vxa";
}

} // namespace engine_paths
