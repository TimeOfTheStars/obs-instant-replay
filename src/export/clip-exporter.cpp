/*
Instant Replay for OBS
Copyright (C) 2026 Trinity AEM

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "clip-exporter.hpp"

#include "core/program-capture.hpp"
#include "export-path.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/* Frames still missing after this many attempts at the clip start mean the head is gone for good. */
constexpr double kMaxLeadingLossSec = 1.0;

std::string ffmpeg_error(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}

/*
 * Pick an encoder that does not fight the broadcast: if the stream already runs on NVENC the CPU
 * is free for x264, otherwise NVENC (when the FFmpeg build has it) keeps x264 threads off the
 * cores the stream encoder needs.
 */
std::string resolve_encoder(const std::string &requested)
{
	const bool have_nvenc = avcodec_find_encoder_by_name("h264_nvenc") != nullptr;

	if (requested == "x264")
		return "libx264";
	if (requested == "nvenc")
		return have_nvenc ? "h264_nvenc" : "libx264";

	bool stream_uses_nvenc = false;
	if (config_t *profile = obs_frontend_get_profile_config()) {
		const char *mode = config_get_string(profile, "Output", "Mode");
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;
		const char *encoder = advanced ? config_get_string(profile, "AdvOut", "Encoder")
					       : config_get_string(profile, "SimpleOutput", "StreamEncoder");
		stream_uses_nvenc = encoder && strstr(encoder, "nvenc") != nullptr;
	}

	if (stream_uses_nvenc || !have_nvenc)
		return "libx264";
	return "h264_nvenc";
}

AVPixelFormat pixel_format_for(video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		return AV_PIX_FMT_NV12;
	case VIDEO_FORMAT_I420:
		return AV_PIX_FMT_YUV420P;
	case VIDEO_FORMAT_I444:
		return AV_PIX_FMT_YUV444P;
	default:
		return AV_PIX_FMT_NONE;
	}
}

void apply_colour(AVCodecContext *context, const obs_video_info &ovi)
{
	switch (ovi.colorspace) {
	case VIDEO_CS_601:
		context->colorspace = AVCOL_SPC_SMPTE170M;
		context->color_primaries = AVCOL_PRI_SMPTE170M;
		context->color_trc = AVCOL_TRC_SMPTE170M;
		break;
	case VIDEO_CS_SRGB:
		context->colorspace = AVCOL_SPC_BT709;
		context->color_primaries = AVCOL_PRI_BT709;
		context->color_trc = AVCOL_TRC_IEC61966_2_1;
		break;
	default:
		context->colorspace = AVCOL_SPC_BT709;
		context->color_primaries = AVCOL_PRI_BT709;
		context->color_trc = AVCOL_TRC_BT709;
		break;
	}
	context->color_range = ovi.range == VIDEO_RANGE_FULL ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

void lower_thread_priority()
{
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

/* Everything FFmpeg allocates for one file, released together whatever path we exit through. */
struct EncodeSession {
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVStream *stream = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;
	bool header_written = false;

	~EncodeSession()
	{
		if (format && header_written)
			av_write_trailer(format);
		if (format && !(format->oformat->flags & AVFMT_NOFILE))
			avio_closep(&format->pb);
		av_packet_free(&packet);
		av_frame_free(&frame);
		avcodec_free_context(&codec);
		avformat_free_context(format);
	}
};

} // namespace

ClipExporter &ClipExporter::instance()
{
	static ClipExporter exporter;
	return exporter;
}

ClipExporter::~ClipExporter()
{
	shutdown();
}

int ClipExporter::enqueue(const Clip &clip, const std::string &event_name, const ExportOptions &options)
{
	Job job;
	job.clip = clip;
	job.event_name = event_name;
	job.encoder = resolve_encoder(options.encoder);
	job.crf = std::clamp(options.crf, 10, 30);
	job.base_dir = export_base_directory(options.base_dir);

	{
		std::lock_guard<std::mutex> lock(mutex_);
		job.id = next_id_++;
		ExportStatus status;
		status.frames_total = clip.seq_out - clip.seq_in;
		statuses_[job.id] = status;
		queue_.push_back(job);
	}

	ensure_worker();
	wake_.notify_one();
	return job.id;
}

ExportStatus ClipExporter::status(int job_id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto found = statuses_.find(job_id);
	return found == statuses_.end() ? ExportStatus{} : found->second;
}

void ClipExporter::ensure_worker()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (worker_.joinable())
		return;

	stop_.store(false, std::memory_order_release);
	worker_ = std::thread([this] { worker_loop(); });
}

void ClipExporter::shutdown()
{
	stop_.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.clear();
	}
	wake_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

void ClipExporter::update(int job_id, ExportState state, uint64_t done, uint64_t total, const std::string &path,
			  const std::string &error)
{
	std::lock_guard<std::mutex> lock(mutex_);
	ExportStatus &status = statuses_[job_id];
	status.state = state;
	status.frames_done = done;
	status.frames_total = total;
	if (!path.empty())
		status.path = path;
	if (!error.empty())
		status.error = error;
}

void ClipExporter::worker_loop()
{
	os_set_thread_name("instant-replay-export");
	lower_thread_priority();

	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) || !queue_.empty(); });
			if (stop_.load(std::memory_order_acquire))
				return;
			job = queue_.front();
			queue_.pop_front();
		}
		run(job);
	}
}

void ClipExporter::run(const Job &job)
{
	const uint64_t total = job.clip.seq_out - job.clip.seq_in;
	update(job.id, ExportState::Encoding, 0, total, "", "");

	ProgramCapture &capture = ProgramCapture::instance();
	if (!capture.running()) {
		update(job.id, ExportState::Failed, 0, total, "", "buffer is not running");
		return;
	}

	const FrameRing &ring = capture.ring();
	const RingConfig config = ring.config();
	const AVPixelFormat pixel_format = pixel_format_for(config.format);
	if (pixel_format == AV_PIX_FMT_NONE) {
		update(job.id, ExportState::Failed, 0, total, "", "buffer colour format cannot be exported");
		return;
	}

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const uint32_t divisor = std::max<uint32_t>(1, capture.settings().frame_rate_divisor);
	const AVRational time_base = {static_cast<int>(ovi.fps_den * divisor), static_cast<int>(ovi.fps_num)};

	std::string path;
	std::string error;
	int sequence = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sequence = ++sequence_;
	}
	if (!build_export_path(job.base_dir, job.event_name, sequence, path, error)) {
		update(job.id, ExportState::Failed, 0, total, "", error);
		return;
	}

	const uint64_t started = os_gettime_ns();
	obs_log(LOG_INFO, "export #%d: %s, %llu frames, encoder %s", job.id, path.c_str(),
		static_cast<unsigned long long>(total), job.encoder.c_str());

	EncodeSession session;
	int result = avformat_alloc_output_context2(&session.format, nullptr, "mp4", path.c_str());
	if (result < 0 || !session.format) {
		update(job.id, ExportState::Failed, 0, total, path, "mp4 muxer unavailable: " + ffmpeg_error(result));
		return;
	}

	const AVCodec *codec = avcodec_find_encoder_by_name(job.encoder.c_str());
	if (!codec)
		codec = avcodec_find_encoder_by_name("libx264");
	if (!codec) {
		update(job.id, ExportState::Failed, 0, total, path, "no H.264 encoder in this FFmpeg build");
		return;
	}

	session.codec = avcodec_alloc_context3(codec);
	session.codec->width = static_cast<int>(config.width);
	session.codec->height = static_cast<int>(config.height);
	session.codec->pix_fmt = pixel_format;
	session.codec->time_base = time_base;
	session.codec->framerate = {time_base.den, time_base.num};
	session.codec->gop_size = static_cast<int>(2.0 * config.fps);
	session.codec->max_b_frames = 2;
	apply_colour(session.codec, ovi);
	if (session.format->oformat->flags & AVFMT_GLOBALHEADER)
		session.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (strcmp(codec->name, "libx264") == 0) {
		/* Leave most cores to the stream encoder; veryfast still beats 50 fps on a modest CPU. */
		session.codec->thread_count = std::clamp(os_get_logical_cores() / 2, 2, 6);
		av_opt_set(session.codec->priv_data, "preset", "veryfast", 0);
		av_opt_set(session.codec->priv_data, "profile", "high", 0);
		av_opt_set_int(session.codec->priv_data, "crf", job.crf, 0);
	} else {
		av_opt_set(session.codec->priv_data, "preset", "p5", 0);
		av_opt_set(session.codec->priv_data, "rc", "constqp", 0);
		av_opt_set_int(session.codec->priv_data, "qp", job.crf + 2, 0);
	}

	result = avcodec_open2(session.codec, codec, nullptr);
	if (result < 0) {
		update(job.id, ExportState::Failed, 0, total, path, "encoder open failed: " + ffmpeg_error(result));
		return;
	}

	session.stream = avformat_new_stream(session.format, nullptr);
	session.stream->time_base = time_base;
	avcodec_parameters_from_context(session.stream->codecpar, session.codec);

	result = avio_open(&session.format->pb, path.c_str(), AVIO_FLAG_WRITE);
	if (result < 0) {
		update(job.id, ExportState::Failed, 0, total, path, "cannot open file: " + ffmpeg_error(result));
		return;
	}

	result = avformat_write_header(session.format, nullptr);
	if (result < 0) {
		update(job.id, ExportState::Failed, 0, total, path, "cannot write header: " + ffmpeg_error(result));
		return;
	}
	session.header_written = true;

	session.frame = av_frame_alloc();
	session.frame->format = pixel_format;
	session.frame->width = session.codec->width;
	session.frame->height = session.codec->height;
	if (av_frame_get_buffer(session.frame, 0) < 0) {
		update(job.id, ExportState::Failed, 0, total, path, "cannot allocate frame");
		return;
	}
	session.packet = av_packet_alloc();

	size_t plane_count = 0;
	size_t plane_rows[MAX_AV_PLANES] = {};
	frame_plane_layout(config.format, config.height, plane_count, plane_rows);

	auto drain = [&](bool flushing) -> bool {
		while (true) {
			const int received = avcodec_receive_packet(session.codec, session.packet);
			if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
				return true;
			if (received < 0) {
				error = "encoder error: " + ffmpeg_error(received);
				return false;
			}
			av_packet_rescale_ts(session.packet, session.codec->time_base, session.stream->time_base);
			session.packet->stream_index = session.stream->index;
			const int written = av_interleaved_write_frame(session.format, session.packet);
			av_packet_unref(session.packet);
			if (written < 0) {
				error = "write error: " + ffmpeg_error(written);
				return false;
			}
			if (flushing && received == AVERROR_EOF)
				return true;
		}
	};

	uint64_t written_frames = 0;
	uint64_t lost_frames = 0;
	bool truncated = false;
	const uint64_t max_leading_loss = static_cast<uint64_t>(kMaxLeadingLossSec * config.fps);

	for (uint64_t seq = job.clip.seq_in; seq < job.clip.seq_out; ++seq) {
		if (stop_.load(std::memory_order_acquire)) {
			truncated = true;
			break;
		}

		bool valid = false;
		{
			/* Per frame, not per clip: a profile switch must not wait seconds for us. */
			const auto guard = capture.reader_guard();
			FrameMeta meta;
			const uint8_t *planes[MAX_AV_PLANES] = {};
			if (capture.running() && ring.read(seq, meta, planes) &&
			    av_frame_make_writable(session.frame) >= 0) {
				for (size_t plane = 0; plane < plane_count; ++plane) {
					const size_t stride =
						std::min<size_t>(meta.linesize[plane],
								 static_cast<size_t>(session.frame->linesize[plane]));
					for (size_t row = 0; row < plane_rows[plane]; ++row)
						memcpy(session.frame->data[plane] +
							       row * session.frame->linesize[plane],
						       planes[plane] + row * meta.linesize[plane], stride);
				}
				/*
				 * The writer may have caught up while we were copying; the ring may even have been
				 * rebuilt (head reset). Trust the frame only if it is still in place and in range.
				 */
				valid = seq >= ring.oldest() && meta.timestamp >= job.clip.ts_in &&
					meta.timestamp <= job.clip.ts_out;
			}
		}

		if (!valid) {
			++lost_frames;
			if (written_frames == 0 && lost_frames <= max_leading_loss)
				continue; /* the head was overwritten before we started; begin later */
			truncated = true;
			break;
		}

		session.frame->pts = static_cast<int64_t>(written_frames);
		result = avcodec_send_frame(session.codec, session.frame);
		if (result < 0) {
			error = "encoder rejected frame: " + ffmpeg_error(result);
			break;
		}
		if (!drain(false))
			break;

		++written_frames;
		if ((written_frames & 15) == 0)
			update(job.id, ExportState::Encoding, written_frames, total, path, "");
	}

	if (error.empty() && written_frames > 0) {
		avcodec_send_frame(session.codec, nullptr);
		drain(true);
	}

	const double seconds = static_cast<double>(os_gettime_ns() - started) / 1e9;

	if (!error.empty()) {
		obs_log(LOG_WARNING, "export #%d failed: %s", job.id, error.c_str());
		update(job.id, ExportState::Failed, written_frames, total, path, error);
		return;
	}

	if (written_frames == 0) {
		obs_log(LOG_WARNING, "export #%d: nothing written, the clip was overwritten before export started",
			job.id);
		update(job.id, ExportState::Failed, 0, total, path, "clip was overwritten before export started");
		return;
	}

	if (truncated || lost_frames > 0) {
		obs_log(LOG_WARNING,
			"export #%d: clip truncated to %llu of %llu frames (ring overwritten) — lengthen the buffer or "
			"shorten the clip; %.1f s",
			job.id, static_cast<unsigned long long>(written_frames), static_cast<unsigned long long>(total),
			seconds);
		update(job.id, ExportState::DoneTruncated, written_frames, total, path, "");
		return;
	}

	obs_log(LOG_INFO, "export #%d: done, %llu frames in %.1f s", job.id,
		static_cast<unsigned long long>(written_frames), seconds);
	update(job.id, ExportState::Done, written_frames, total, path, "");
}
