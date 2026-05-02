#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// Forward-declare the ffmpeg structs so this header doesn't pull in libav* into
// every translation unit. The full ffmpeg headers only matter inside the .cpp.
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// =============================================================================
// VideoEncoder
// =============================================================================
//
// h264-in-MP4 encoder. RGBA → YUV420p via libswscale → h264 NAL via libavcodec
// (h264_videotoolbox on macOS, libx264 fallback) → MP4 mux via libavformat.
//
// Worker-thread only. CaptureSystem owns one of these for the lifetime of an
// active recording; constructed empty, opened on the first recording frame
// (so we know the dimensions), closed on EndRecording.
//
// Lifecycle:
//   VideoEncoder enc;
//   enc.Open(path, w, h, fps, bitrateKbps);          // once
//   enc.PushFrame(rgbaPixels, frameIndex);            // per frame
//   ...
//   enc.Close();                                      // once
//
// Errors are logged and surfaced via IsOpen() — a failed Open leaves the
// encoder closed; subsequent PushFrame calls are no-ops. We don't throw
// because the worker thread can't unwind into anything useful.
class VideoEncoder {
public:
	VideoEncoder();
	~VideoEncoder();

	// Configure and open output file. fps is the *playback* framerate of the
	// resulting MP4. width/height must match every frame pushed.
	bool Open(const std::filesystem::path& path,
	          uint32_t width,
	          uint32_t height,
	          uint32_t fps,
	          uint32_t bitrateKbps);

	// Push one RGBA frame. `frameIndex` is the PTS in encoder timebase units
	// (one tick = 1/fps seconds). The encoder converts to YUV internally.
	// `pixels` is read-only from this call's perspective.
	void PushFrame(const uint8_t* rgbaPixels, int64_t frameIndex);

	// Flush remaining packets, write trailer, close file. Safe to call on a
	// closed encoder.
	void Close();

	bool IsOpen() const { return m_open; }
	uint32_t GetWidth()  const { return m_width;  }
	uint32_t GetHeight() const { return m_height; }

private:
	void FlushEncoder();

	bool m_open = false;
	uint32_t m_width  = 0;
	uint32_t m_height = 0;
	uint32_t m_fps    = 60;

	AVFormatContext* m_format = nullptr;
	AVCodecContext*  m_codec  = nullptr;
	AVStream*        m_stream = nullptr;
	AVFrame*         m_frame  = nullptr;
	AVPacket*        m_packet = nullptr;
	SwsContext*      m_sws    = nullptr;
};
