#pragma once
#include <string>
#include <vector>

namespace FileDialog {

// Opens a native file picker. Returns empty string if cancelled.
std::string OpenFile(
	const std::string& title,
	const std::vector<std::string>& filters,
	const std::string& defaultPath = "");

// Opens a native save-file dialog. The user picks a destination filename;
// the returned path is the chosen absolute path (with the suggested filename
// pre-filled in the dialog). Returns empty string on cancel. `filters` is
// used the same way as OpenFile (e.g. {"vxa"}); macOS appends the extension
// automatically when the filter is set.
std::string SaveFile(
	const std::string& title,
	const std::vector<std::string>& filters,
	const std::string& suggestedFileName,
	const std::string& defaultDirectory = "");

// Opens a native directory picker. Used by the bake-save flow where we emit
// N+1 files (manifest + per-frame .vox) into a single output folder. Returns
// empty string on cancel.
std::string PickDirectory(
	const std::string& title,
	const std::string& defaultPath = "");

} // namespace FileDialog
