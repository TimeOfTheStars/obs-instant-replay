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

#include "core/angle-manager.hpp"

#include <media-io/video-io.h>
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
#include <mutex>

#ifdef _WIN32
/* windows.h defines min/max as macros, which breaks std::max below. */
#define NOMINMAX
#include <windows.h>
#endif

namespace {

/*
 * FFmpeg explains failures only through av_log, and OBS already routes that callback into its own
 * log (obs-ffmpeg), so the detailed reason for an encoder failure is in the OBS log as "[ffmpeg]"
 * lines right before our "export #N" warning.
 */
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
/*
 * "Compiled in" is not "usable": FFmpeg reports h264_nvenc / h264_amf / h264_qsv whenever it was
 * built with them, and on a machine without that vendor's driver opening them fails with a bare
 * -1 from the dynamic loader ("Operation not permitted" on Windows). OBS itself only registers a
 * hardware encoder after probing the GPU, so its registry is the reliable signal.
 */
bool hardware_encoder_available(const char *ffmpeg_name)
{
	if (!avcodec_find_encoder_by_name(ffmpeg_name))
		return false;

	static const char *nvenc_ids[] = {"obs_nvenc_h264_tex", "obs_nvenc_h264_cuda", "jim_nvenc", "ffmpeg_nvenc"};
	static const char *amf_ids[] = {"h264_texture_amf", "h264_fallback_amf"};
	static const char *qsv_ids[] = {"obs_qsv11_v2", "obs_qsv11", "obs_qsv11_soft"};

	const char *const *ids = nullptr;
	size_t count = 0;
	if (strcmp(ffmpeg_name, "h264_nvenc") == 0) {
		ids = nvenc_ids;
		count = sizeof(nvenc_ids) / sizeof(nvenc_ids[0]);
	} else if (strcmp(ffmpeg_name, "h264_amf") == 0) {
		ids = amf_ids;
		count = sizeof(amf_ids) / sizeof(amf_ids[0]);
	} else if (strcmp(ffmpeg_name, "h264_qsv") == 0) {
		ids = qsv_ids;
		count = sizeof(qsv_ids) / sizeof(qsv_ids[0]);
	} else {
		return true; /* software encoders need no hardware */
	}

	for (size_t i = 0; i < count; ++i) {
		if (obs_get_encoder_codec(ids[i]))
			return true;
	}
	return false;
}

/* FFmpeg encoder for a panel choice (auto | x264 | nvenc | amf | qsv). */
std::string resolve_encoder(const std::string &requested)
{
	if (requested == "x264")
		return "libx264";
	if (requested == "nvenc")
		return hardware_encoder_available("h264_nvenc") ? "h264_nvenc" : "libx264";
	if (requested == "amf")
		return hardware_encoder_available("h264_amf") ? "h264_amf" : "libx264";
	if (requested == "qsv")
		return hardware_encoder_available("h264_qsv") ? "h264_qsv" : "libx264";

	/*
	 * Auto: stay off whatever the stream is using. A GPU stream leaves the CPU free for x264;
	 * an x264 stream leaves the GPU free, so take the vendor's encoder OBS has actually probed
	 * (NVIDIA or AMD or Intel — whichever this machine has).
	 */
	bool stream_on_gpu = false;
	if (config_t *profile = obs_frontend_get_profile_config()) {
		const char *mode = config_get_string(profile, "Output", "Mode");
		const bool advanced = mode && strcmp(mode, "Advanced") == 0;
		const char *encoder = advanced ? config_get_string(profile, "AdvOut", "Encoder")
					       : config_get_string(profile, "SimpleOutput", "StreamEncoder");
		/* Simple mode: "nvenc", "amd", "qsv"; advanced mode: "obs_nvenc_*", "h264_texture_amf", "obs_qsv11*". */
		stream_on_gpu = encoder && (strstr(encoder, "nvenc") || strstr(encoder, "amd") ||
					    strstr(encoder, "amf") || strstr(encoder, "qsv"));
	}

	if (stream_on_gpu)
		return "libx264";

	for (const char *gpu : {"h264_nvenc", "h264_amf", "h264_qsv"}) {
		if (hardware_encoder_available(gpu))
			return gpu;
	}
	return "libx264";
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

int ClipExporter::reserve_sequence()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return ++sequence_;
}

int ClipExporter::enqueue(int angle, const Clip &clip, const std::string &path, const ExportOptions &options)
{
	Job job;
	job.angle = angle;
	job.clip = clip;
	job.path = path;
	job.encoder = resolve_encoder(options.encoder);
	job.crf = std::clamp(options.crf, 10, 30);
	job.thread_budget = options.thread_budget;

	{
		std::lock_guard<std::mutex> lock(mutex_);
		job.id = next_id_++;
		ExportStatus status;
		status.path = path;
		status.media.angle = angle;
		statuses_[job.id] = status;
		queue_.push_back(job);
	}

	ensure_workers();
	wake_.notify_one();
	return job.id;
}

ExportStatus ClipExporter::status(int job_id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	const auto found = statuses_.find(job_id);
	return found == statuses_.end() ? ExportStatus{} : found->second;
}

void ClipExporter::ensure_workers()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (stop_.load(std::memory_order_acquire))
		return;

	/*
	 * One worker per angle: a single queue would encode the angles one after another, and on a
	 * short ring every angle but the first would find its head already overwritten. Workers read
	 * different rings, so they do not race each other.
	 */
	const size_t wanted = safe_mode() ? 1u : static_cast<size_t>(kAngleCount);
	while (workers_.size() < wanted && workers_.size() < queue_.size() + workers_.size())
		workers_.emplace_back([this] { worker_loop(); });
}

void ClipExporter::shutdown()
{
	stop_.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		queue_.clear();
	}
	wake_.notify_all();
	for (std::thread &worker : workers_) {
		if (worker.joinable())
			worker.join();
	}
	workers_.clear();
}

bool ClipExporter::take_job(Job &job)
{
	std::unique_lock<std::mutex> lock(mutex_);
	wake_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) || !queue_.empty(); });
	if (stop_.load(std::memory_order_acquire))
		return false;

	/* The programme is what goes on air first, so it never waits behind a camera. */
	auto chosen = queue_.begin();
	for (auto candidate = queue_.begin(); candidate != queue_.end(); ++candidate) {
		if (candidate->angle == kProgramAngle) {
			chosen = candidate;
			break;
		}
	}

	job = *chosen;
	queue_.erase(chosen);
	return true;
}

void ClipExporter::note_job_started()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_jobs_++ == 0) {
		/* Snapshot once per burst of exports, so the cost is attributed to the MARK as a whole. */
		skipped_at_start_ = video_output_get_skipped_frames(obs_get_video());
		lagged_at_start_ = obs_get_lagged_frames();
	}
}

void ClipExporter::note_job_finished()
{
	uint32_t skipped = 0;
	uint32_t lagged = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (--running_jobs_ > 0)
			return;

		const uint32_t skipped_now = video_output_get_skipped_frames(obs_get_video());
		const uint32_t lagged_now = obs_get_lagged_frames();
		skipped = skipped_now > skipped_at_start_ ? skipped_now - skipped_at_start_ : 0;
		lagged = lagged_now > lagged_at_start_ ? lagged_now - lagged_at_start_ : 0;
	}

	if (skipped == 0 && lagged == 0)
		return;

	obs_log(LOG_WARNING, "export cost the broadcast %u encoded and %u rendered frames", skipped, lagged);

	/* Losing frames on air is not worth a faster export: stay conservative for the rest of the session. */
	if (skipped + lagged > 2 && !safe_mode_.exchange(true, std::memory_order_acq_rel))
		obs_log(LOG_WARNING, "export switched to safe mode: one angle at a time, faster preset");
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

void ClipExporter::set_media(int job_id, const ExportMedia &media)
{
	std::lock_guard<std::mutex> lock(mutex_);
	statuses_[job_id].media = media;
}

void ClipExporter::worker_loop()
{
	os_set_thread_name("instant-replay-export");
	lower_thread_priority();

	while (true) {
		Job job;
		if (!take_job(job))
			return;

		note_job_started();
		run(job);
		note_job_finished();
	}
}

void ClipExporter::run(const Job &job)
{
	AngleManager &angles = AngleManager::instance();
	const AngleCapture &capture = angles.angle(job.angle);
	const std::string &path = job.path;

	if (!capture.running()) {
		update(job.id, ExportState::Failed, 0, 0, path, "buffer is not running");
		return;
	}

	const FrameRing &ring = capture.ring();
	const RingConfig config = ring.config();

	/* Frames are addressed by time, so the expected count follows from the clip, not from seq. */
	const uint64_t total = static_cast<uint64_t>(job.clip.duration_sec() * config.fps);

	const AVPixelFormat pixel_format = pixel_format_for(config.format);
	if (pixel_format == AV_PIX_FMT_NONE) {
		update(job.id, ExportState::Failed, 0, total, path, "buffer colour format cannot be exported");
		return;
	}

	update(job.id, ExportState::Encoding, 0, total, path, "");

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	/* The ring's own rate already accounts for any frame decimation. */
	const AVRational time_base = av_d2q(1.0 / std::max(config.fps, 1.0), 100000);

	std::string error;
	const uint64_t started = os_gettime_ns();
	obs_log(LOG_INFO, "export #%d: angle %d -> %s, ~%llu frames, encoder %s", job.id, job.angle, path.c_str(),
		static_cast<unsigned long long>(total), job.encoder.c_str());

	EncodeSession session;
	int result = avformat_alloc_output_context2(&session.format, nullptr, "mp4", path.c_str());
	if (result < 0 || !session.format) {
		update(job.id, ExportState::Failed, 0, total, path, "mp4 muxer unavailable: " + ffmpeg_error(result));
		return;
	}

	/*
	 * Try the preferred encoder first, then whatever else this FFmpeg build can offer. Hardware
	 * encoders fail to open for all sorts of machine-specific reasons (driver, session limits,
	 * missing runtime), and one bad encoder must not cost the operator the clip.
	 */
	const char *candidates[] = {job.encoder.c_str(), "libx264",  "h264_nvenc",
				    "h264_amf",          "h264_qsv", "libopenh264"};
	std::string open_errors;
	const AVCodec *codec = nullptr;
	bool holds_hardware_slot = false;

	for (const char *name : candidates) {
		const AVCodec *candidate = avcodec_find_encoder_by_name(name);
		if (!candidate)
			continue;
		if (!hardware_encoder_available(name))
			continue; /* no point probing a vendor's encoder on a machine without that GPU */

		const bool hardware = strcmp(candidate->name, "libx264") != 0 &&
				      strcmp(candidate->name, "libopenh264") != 0;
		if (hardware && !holds_hardware_slot) {
			/*
			 * Consumer GPUs cap concurrent encoder sessions and the broadcast already holds one,
			 * so only one export at a time may use the hardware; the rest fall back to x264.
			 */
			if (hardware_jobs_.fetch_add(1, std::memory_order_acq_rel) != 0) {
				hardware_jobs_.fetch_sub(1, std::memory_order_acq_rel);
				continue;
			}
			holds_hardware_slot = true;
		}

		bool already_tried = false;
		for (const char *earlier : candidates) {
			if (earlier == name)
				break;
			if (strcmp(earlier, name) == 0)
				already_tried = true;
		}
		if (already_tried)
			continue;

		avcodec_free_context(&session.codec);
		session.codec = avcodec_alloc_context3(candidate);
		session.codec->width = static_cast<int>(config.width);
		session.codec->height = static_cast<int>(config.height);
		session.codec->pix_fmt = pixel_format;
		session.codec->time_base = time_base;
		session.codec->framerate = {time_base.den, time_base.num};
		/* One keyframe per second: halves the worst-case seek when the clip is played back. */
		session.codec->gop_size = static_cast<int>(config.fps);
		session.codec->max_b_frames = 2;
		apply_colour(session.codec, ovi);
		if (session.format->oformat->flags & AVFMT_GLOBALHEADER)
			session.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

		if (strcmp(candidate->name, "libx264") == 0) {
			/* Leave most cores to the stream encoder; veryfast still beats 50 fps on a modest CPU. */
			session.codec->thread_count = job.thread_budget > 0
							      ? job.thread_budget
							      : std::clamp(os_get_logical_cores() / 2, 2, 6);
			av_opt_set(session.codec->priv_data, "preset", safe_mode() ? "superfast" : "veryfast", 0);
			av_opt_set_int(session.codec->priv_data, "crf", job.crf, 0);
		} else if (strcmp(candidate->name, "h264_nvenc") == 0) {
			av_opt_set(session.codec->priv_data, "preset", "p5", 0);
			av_opt_set(session.codec->priv_data, "rc", "constqp", 0);
			av_opt_set_int(session.codec->priv_data, "qp", job.crf + 2, 0);
		} else if (strcmp(candidate->name, "h264_amf") == 0) {
			av_opt_set(session.codec->priv_data, "rc", "cqp", 0);
			av_opt_set_int(session.codec->priv_data, "qp_i", job.crf + 2, 0);
			av_opt_set_int(session.codec->priv_data, "qp_p", job.crf + 2, 0);
		} else if (strcmp(candidate->name, "h264_qsv") == 0) {
			session.codec->global_quality = job.crf + 2;
		} else {
			session.codec->bit_rate = 12000000;
		}

		result = avcodec_open2(session.codec, candidate, nullptr);
		if (result >= 0) {
			codec = candidate;
			break;
		}

		const std::string reason = ffmpeg_error(result);
		obs_log(LOG_WARNING, "export #%d: encoder %s failed to open: %s", job.id, candidate->name,
			reason.c_str());
		if (!open_errors.empty())
			open_errors += "; ";
		open_errors += std::string(candidate->name) + ": " + reason;
	}

	if (!codec) {
		update(job.id, ExportState::Failed, 0, total, path,
		       open_errors.empty() ? std::string("no H.264 encoder in this FFmpeg build")
					   : "encoder open failed — " + open_errors);
		return;
	}

	if (strcmp(codec->name, job.encoder.c_str()) != 0)
		obs_log(LOG_INFO, "export #%d: using %s instead of %s", job.id, codec->name, job.encoder.c_str());

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
	uint64_t ts_origin = 0;
	uint64_t ts_last = 0;
	int64_t last_pts = -1;
	bool truncated = false;

	/* Frames are found by timestamp: clip sequence numbers only mean something in the programme ring. */
	uint64_t seq = 0;
	{
		const auto guard = angles.reader_guard();
		const uint64_t head = ring.head();
		if (head == 0 || !ring.find_by_timestamp(job.clip.ts_in, ring.oldest(), head - 1, seq)) {
			update(job.id, ExportState::Failed, 0, total, path, "clip is no longer in the buffer");
			return;
		}
	}

	/* Guards against spinning if the writer keeps outrunning us while nothing has been written yet. */
	int skips = 0;
	constexpr int kMaxSkips = 400;

	for (;;) {
		if (stop_.load(std::memory_order_acquire)) {
			truncated = true;
			break;
		}

		bool valid = false;
		bool at_end = false;
		bool lost = false;
		uint64_t frame_ts = 0;
		uint64_t oldest = 0;

		{
			/* Per frame, not per clip: a profile switch must not wait seconds for us. */
			const auto guard = angles.reader_guard();
			if (!capture.running()) {
				truncated = true;
				break;
			}

			oldest = ring.oldest();
			const uint64_t head = ring.head();

			if (seq >= head) {
				at_end = true; /* caught up with the live edge */
			} else if (seq < oldest) {
				lost = true; /* the writer got here first */
			} else {
				FrameMeta meta;
				const uint8_t *planes[MAX_AV_PLANES] = {};
				if (!ring.read(seq, meta, planes)) {
					lost = true;
				} else if (meta.timestamp > job.clip.ts_out) {
					at_end = true;
				} else if (meta.width != config.width || meta.height != config.height) {
					/* The ring was rebuilt under us; the plane layout we cached no longer fits. */
					lost = true;
				} else if (av_frame_make_writable(session.frame) >= 0) {
					for (size_t plane = 0; plane < plane_count; ++plane) {
						const size_t stride = std::min<size_t>(
							meta.linesize[plane],
							static_cast<size_t>(session.frame->linesize[plane]));
						for (size_t row = 0; row < plane_rows[plane]; ++row)
							memcpy(session.frame->data[plane] +
								       row * session.frame->linesize[plane],
							       planes[plane] + row * meta.linesize[plane], stride);
					}
					/*
					 * The writer may have caught up while we were copying; trust the frame
					 * only if it is still in place and inside the clip.
					 */
					valid = seq >= ring.oldest() && meta.timestamp >= job.clip.ts_in;
					frame_ts = meta.timestamp;
				}
			}
		}

		if (at_end)
			break;

		if (lost || !valid) {
			++lost_frames;
			truncated = true;

			if (written_frames == 0 && ++skips < kMaxSkips) {
				/*
				 * Nothing written yet: jump straight to the oldest frame still alive instead of
				 * stepping one sequence at a time, which used to burn the whole budget in
				 * microseconds and fail the export outright.
				 */
				seq = std::max(seq + 1, oldest);
				continue;
			}
			break;
		}

		if (written_frames == 0)
			ts_origin = frame_ts;

		/*
		 * pts follows the frame's own timestamp, not a counter: frames in the ring are not evenly
		 * spaced, and counting them would turn every dropped frame into a permanent time shift.
		 */
		int64_t pts =
			av_rescale_q(static_cast<int64_t>(frame_ts - ts_origin), AVRational{1, 1000000000}, time_base);
		if (written_frames > 0 && pts <= last_pts)
			pts = last_pts + 1;
		session.frame->pts = pts;
		last_pts = pts;
		ts_last = frame_ts;

		result = avcodec_send_frame(session.codec, session.frame);
		if (result < 0) {
			error = "encoder rejected frame: " + ffmpeg_error(result);
			break;
		}
		if (!drain(false))
			break;

		++written_frames;
		++seq;
		if ((written_frames & 15) == 0)
			update(job.id, ExportState::Encoding, written_frames, total, path, "");
	}

	if (error.empty() && written_frames > 0) {
		avcodec_send_frame(session.codec, nullptr);
		drain(true);
	}

	if (holds_hardware_slot)
		hardware_jobs_.fetch_sub(1, std::memory_order_acq_rel);

	const double seconds = static_cast<double>(os_gettime_ns() - started) / 1e9;
	/* Encoding speed decides whether several angles can be exported at once; log it to tune that. */
	const double encoded_fps = seconds > 0.0 ? static_cast<double>(written_frames) / seconds : 0.0;

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

	ExportMedia media;
	media.angle = job.angle;
	media.ts_origin = ts_origin;
	media.ts_last = ts_last;
	media.frames = written_frames;
	media.fps = config.fps;
	media.width = config.width;
	media.height = config.height;
	set_media(job.id, media);

	if (truncated || lost_frames > 0) {
		obs_log(LOG_WARNING,
			"export #%d: clip truncated to %llu of %llu frames (ring overwritten) — lengthen the buffer or "
			"shorten the clip; %.1f s = %.0f fps",
			job.id, static_cast<unsigned long long>(written_frames), static_cast<unsigned long long>(total),
			seconds, encoded_fps);
		update(job.id, ExportState::DoneTruncated, written_frames, total, path, "");
		return;
	}

	obs_log(LOG_INFO, "export #%d: done, %llu frames in %.1f s = %.0f fps", job.id,
		static_cast<unsigned long long>(written_frames), seconds, encoded_fps);
	update(job.id, ExportState::Done, written_frames, total, path, "");
}
