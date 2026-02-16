#pragma once

#include <string>
#include <vector>

struct CompileResult {
	bool success;
	std::string output;
	std::string spvPath;
};

namespace ShaderCompiler {
	CompileResult Compile(const std::string& sourcePath, const std::string& outputPath);
	std::vector<CompileResult> CompileAll(const std::vector<std::string>& spvPaths);
	std::string SpvToSourcePath(const std::string& spvPath);
}
