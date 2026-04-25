#include "ShaderCompiler.h"
#include "config.h"
#include <spdlog/spdlog.h>
#include <cstdio>
#include <array>
#include <filesystem>

namespace ShaderCompiler {

std::string SpvToSourcePath(const std::string& spvPath) {
	// SPV path: <SHADER_DIR>/shader_dda.frag.spv
	// Source:   <SHADER_SRC_DIR>/shader_dda.frag
	std::filesystem::path p(spvPath);
	std::string stem = p.stem().string(); // "shader_dda.frag" (strips .spv)
	return std::string(config::SHADER_SRC_DIR) + "/" + stem;
}

CompileResult Compile(const std::string& sourcePath, const std::string& outputPath) {
	CompileResult result{};
	result.spvPath = outputPath;

	std::string includeDir = std::string(config::SHADER_SRC_DIR) + "/include";
	std::string cmd = std::string(config::GLSLC_PATH) + " -I \"" + includeDir + "\" \"" + sourcePath + "\" -o \"" + outputPath + "\" 2>&1";

	auto logger = spdlog::get("Render");
	logger->info("Compiling shader: {}", sourcePath);

	FILE* pipe = popen(cmd.c_str(), "r");
	if (!pipe) {
		result.success = false;
		result.output = "Failed to run glslc";
		logger->error("Failed to launch glslc for {}", sourcePath);
		return result;
	}

	std::array<char, 256> buffer;
	std::string output;
	while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
		output += buffer.data();
	}

	int exitCode = pclose(pipe);
	result.success = (exitCode == 0);
	result.output = output;

	if (result.success) {
		logger->info("Compiled: {} -> {}", sourcePath, outputPath);
	} else {
		logger->error("Failed to compile {}:\n{}", sourcePath, output);
	}

	return result;
}

std::vector<CompileResult> CompileAll(const std::vector<std::string>& spvPaths) {
	std::vector<CompileResult> results;
	results.reserve(spvPaths.size());

	for (const auto& spvPath : spvPaths) {
		std::string sourcePath = SpvToSourcePath(spvPath);
		results.push_back(Compile(sourcePath, spvPath));
	}

	return results;
}

} // namespace ShaderCompiler
