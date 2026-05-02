#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// PNG writer for one-shot screenshots.
//
// Stateless — the worker thread instantiates one and calls Write per slot.
// We use stb_image_write (already in dep/) for portability and to keep the
// dependency graph small (libpng would also work, but ffmpeg is already the
// heavy dep we needed for video — adding libpng on top is gratuitous).
class StillEncoder {
public:
	// Writes a PNG to `path`. Pixel buffer is RGBA, tightly packed,
	// `width * 4` stride. The buffer may be modified in-place (BGRA swizzle is
	// already done by CaptureSystem before we get here). Returns true on success.
	bool Write(const std::filesystem::path& path,
	           uint8_t* rgbaPixels,
	           uint32_t width,
	           uint32_t height) const;
};
