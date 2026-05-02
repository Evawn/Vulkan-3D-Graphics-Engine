#include "VideoEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <spdlog/spdlog.h>

namespace {
	// Encoder-pick policy: prefer hardware (h264_videotoolbox on macOS) for the
	// real-time recording use case, fall back to libx264 (CPU) if the platform
	// doesn't have a hardware encoder available. Both produce stream-compatible
	// h264; the only practical differences are CPU cost and quality knobs.
	const AVCodec* PickEncoder() {
#ifdef __APPLE__
		if (const AVCodec* hw = avcodec_find_encoder_by_name("h264_videotoolbox"); hw) {
			return hw;
		}
#endif
		return avcodec_find_encoder(AV_CODEC_ID_H264);
	}
}

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() {
	Close();
}

bool VideoEncoder::Open(const std::filesystem::path& path,
                        uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrateKbps) {
	auto logger = spdlog::get("App");

	if (m_open) Close();
	m_width  = width;
	m_height = height;
	m_fps    = fps;

	std::filesystem::create_directories(path.parent_path());
	const std::string pathStr = path.string();

	if (avformat_alloc_output_context2(&m_format, nullptr, "mp4", pathStr.c_str()) < 0 || !m_format) {
		logger->error("VideoEncoder: avformat_alloc_output_context2 failed");
		Close();
		return false;
	}

	const AVCodec* codec = PickEncoder();
	if (!codec) {
		logger->error("VideoEncoder: no h264 encoder found (neither h264_videotoolbox nor libx264)");
		Close();
		return false;
	}
	logger->info("VideoEncoder: using codec '{}'", codec->name);

	m_stream = avformat_new_stream(m_format, codec);
	if (!m_stream) {
		logger->error("VideoEncoder: avformat_new_stream failed");
		Close();
		return false;
	}
	m_stream->id = static_cast<int>(m_format->nb_streams - 1);

	m_codec = avcodec_alloc_context3(codec);
	if (!m_codec) {
		logger->error("VideoEncoder: avcodec_alloc_context3 failed");
		Close();
		return false;
	}

	m_codec->codec_id   = AV_CODEC_ID_H264;
	m_codec->codec_type = AVMEDIA_TYPE_VIDEO;
	m_codec->width      = static_cast<int>(width);
	m_codec->height     = static_cast<int>(height);
	m_codec->pix_fmt    = AV_PIX_FMT_YUV420P;
	m_codec->bit_rate   = static_cast<int64_t>(bitrateKbps) * 1000;
	m_codec->time_base  = AVRational{1, static_cast<int>(fps)};
	m_codec->framerate  = AVRational{static_cast<int>(fps), 1};
	m_codec->gop_size   = static_cast<int>(fps);   // one keyframe per second
	m_codec->max_b_frames = 2;
	m_codec->profile      = AV_PROFILE_H264_HIGH;
	// Explicit color metadata: BT.709 is the HD standard; MPEG range (16-235) is
	// the broadcast convention every player understands. Without these,
	// h264_videotoolbox emits a "Color range not set for yuv420p" warning AND
	// some players guess wrong, producing washed-out colors. Setting them
	// explicitly silences the warning AND nails down the playback intent.
	m_codec->color_range     = AVCOL_RANGE_MPEG;
	m_codec->colorspace      = AVCOL_SPC_BT709;
	m_codec->color_primaries = AVCOL_PRI_BT709;
	m_codec->color_trc       = AVCOL_TRC_BT709;
	m_stream->time_base   = m_codec->time_base;

	// Some MP4 muxers want a global header on the codec context — set the flag.
	if (m_format->oformat->flags & AVFMT_GLOBALHEADER) {
		m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	// libx264-only quality knob; videotoolbox ignores it (uses bitrate / quality).
	if (codec->name == std::string("libx264")) {
		av_opt_set(m_codec->priv_data, "preset", "veryfast", 0);
	}

	if (avcodec_open2(m_codec, codec, nullptr) < 0) {
		logger->error("VideoEncoder: avcodec_open2 failed");
		Close();
		return false;
	}

	if (avcodec_parameters_from_context(m_stream->codecpar, m_codec) < 0) {
		logger->error("VideoEncoder: avcodec_parameters_from_context failed");
		Close();
		return false;
	}

	if (!(m_format->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&m_format->pb, pathStr.c_str(), AVIO_FLAG_WRITE) < 0) {
			logger->error("VideoEncoder: avio_open failed for {}", pathStr);
			Close();
			return false;
		}
	}

	if (avformat_write_header(m_format, nullptr) < 0) {
		logger->error("VideoEncoder: avformat_write_header failed");
		Close();
		return false;
	}

	m_frame = av_frame_alloc();
	if (!m_frame) { Close(); return false; }
	m_frame->format = AV_PIX_FMT_YUV420P;
	m_frame->width  = static_cast<int>(width);
	m_frame->height = static_cast<int>(height);
	if (av_frame_get_buffer(m_frame, 0) < 0) {
		logger->error("VideoEncoder: av_frame_get_buffer failed");
		Close();
		return false;
	}

	m_packet = av_packet_alloc();
	if (!m_packet) { Close(); return false; }

	m_sws = sws_getContext(
		static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
		static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_YUV420P,
		SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!m_sws) {
		logger->error("VideoEncoder: sws_getContext failed");
		Close();
		return false;
	}

	m_open = true;
	logger->info("VideoEncoder: opened {} ({}x{} @ {} fps, {} kbps)",
	             pathStr, width, height, fps, bitrateKbps);
	return true;
}

void VideoEncoder::PushFrame(const uint8_t* rgbaPixels, int64_t frameIndex) {
	if (!m_open) return;

	if (av_frame_make_writable(m_frame) < 0) {
		spdlog::get("App")->error("VideoEncoder: av_frame_make_writable failed");
		return;
	}

	const uint8_t* srcSlices[1] = { rgbaPixels };
	const int      srcStride[1] = { static_cast<int>(m_width * 4) };
	sws_scale(m_sws, srcSlices, srcStride, 0, static_cast<int>(m_height),
	          m_frame->data, m_frame->linesize);

	m_frame->pts = frameIndex;

	if (avcodec_send_frame(m_codec, m_frame) < 0) {
		spdlog::get("App")->error("VideoEncoder: avcodec_send_frame failed");
		return;
	}

	while (true) {
		const int r = avcodec_receive_packet(m_codec, m_packet);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
		if (r < 0) {
			spdlog::get("App")->error("VideoEncoder: avcodec_receive_packet failed");
			return;
		}
		av_packet_rescale_ts(m_packet, m_codec->time_base, m_stream->time_base);
		m_packet->stream_index = m_stream->index;
		av_interleaved_write_frame(m_format, m_packet);
		av_packet_unref(m_packet);
	}
}

void VideoEncoder::FlushEncoder() {
	if (!m_codec) return;
	avcodec_send_frame(m_codec, nullptr);
	while (true) {
		const int r = avcodec_receive_packet(m_codec, m_packet);
		if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
		if (r < 0) break;
		av_packet_rescale_ts(m_packet, m_codec->time_base, m_stream->time_base);
		m_packet->stream_index = m_stream->index;
		av_interleaved_write_frame(m_format, m_packet);
		av_packet_unref(m_packet);
	}
}

void VideoEncoder::Close() {
	if (m_open) {
		FlushEncoder();
		av_write_trailer(m_format);
		spdlog::get("App")->info("VideoEncoder: closed");
	}
	m_open = false;

	if (m_sws)    { sws_freeContext(m_sws);  m_sws = nullptr; }
	if (m_packet) { av_packet_free(&m_packet); }
	if (m_frame)  { av_frame_free(&m_frame); }
	if (m_codec)  { avcodec_free_context(&m_codec); }
	if (m_format) {
		if (m_format->pb && !(m_format->oformat->flags & AVFMT_NOFILE)) {
			avio_closep(&m_format->pb);
		}
		avformat_free_context(m_format);
		m_format = nullptr;
	}
	m_stream = nullptr;
	m_width = m_height = 0;
}
