#pragma once
#include <string>
#include <vector>

namespace FileDialog {

// Opens a native file picker. Returns empty string if cancelled.
std::string OpenFile(
	const std::string& title,
	const std::vector<std::string>& filters,
	const std::string& defaultPath = "");

} // namespace FileDialog
