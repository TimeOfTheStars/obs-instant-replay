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

#include "clip-file-source.hpp"

#include <obs.h>
#include <plugin-support.h>
#include <util/threading.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <algorithm>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

/* Read-ahead: a fifth of a second for the angle on screen, a couple of frames for the rest. */
constexpr size_t kActiveQueue = 12;
constexpr size_t kIdleQueue = 4;

/* Decoders share the machine with the stream encoder, so they get two threads each, not "auto". */
constexpr int kDecoderThreads = 2;

video_format format_from_av(AVPixelFormat format)
{
	switch (format) {
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

video_colorspace colorspace_from_av(AVColorSpace space)
{
	switch (space) {
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT709:
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_DEFAULT;
	}
}

std::string ffmpeg_error(int code)
{
	char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
	av_strerror(code, buffer, sizeof(buffer));
	return buffer;
}

void lower_thread_priority()
{
#ifdef _WIN32
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

} // namespace

ClipFileSource::ClipFileSource(FileClipInfo info) : info_(std::move(info)) {}

ClipFileSource::~ClipFileSource()
{
	stop();
}

void ClipFileSource::start(uint64_t start_ts)
{
	if (thread_.joinable())
		return;

	wanted_ts_.store(start_ts, std::memory_order_release);
	thread_ = std::thread([this, start_ts] { run(start_ts); });
}

void ClipFileSource::stop()
{
	stop_.store(true, std::memory_order_release);
	wake_.notify_all();
	if (thread_.joinable())
		thread_.join();

	std::lock_guard<std::mutex> lock(mutex_);
	queue_.clear();
}

size_t ClipFileSource::queue_limit() const
{
	return active_.load(std::memory_order_acquire) ? kActiveQueue : kIdleQueue;
}

bool ClipFileSource::pick(uint64_t wanted_ts, DecodedFrame &out)
{
	wanted_ts_.store(wanted_ts, std::memory_order_release);

	bool found = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);

		/*
		 * Playing the same event again rewinds the position; the decoder cannot walk backwards,
		 * so ask it to seek instead of starving the queue forever.
		 */
		if (!queue_.empty() && wanted_ts + 1000000 < queue_.front().timestamp) {
			queue_.clear();
			seek_requested_.store(true, std::memory_order_release);
		}

		/* Drop everything already in the past, keeping the newest such frame to show. */
		while (!queue_.empty() && queue_.front().timestamp <= wanted_ts) {
			if (queue_.size() > 1 && queue_[1].timestamp <= wanted_ts) {
				queue_.pop_front();
				continue;
			}
			out = queue_.front();
			found = true;
			break;
		}
	}

	wake_.notify_one();
	return found;
}

void ClipFileSource::run(uint64_t start_ts)
{
	os_set_thread_name("instant-replay-decode");
	lower_thread_priority();

	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	AVStream *stream = nullptr;

	auto cleanup = [&] {
		av_packet_free(&packet);
		avcodec_free_context(&codec);
		if (format)
			avformat_close_input(&format);
	};

	auto fail = [&](const std::string &reason) {
		error_ = reason;
		failed_.store(true, std::memory_order_release);
		obs_log(LOG_WARNING, "replay file '%s': %s", info_.path.c_str(), reason.c_str());
		cleanup();
	};

	int result = avformat_open_input(&format, info_.path.c_str(), nullptr, nullptr);
	if (result < 0) {
		fail("cannot open: " + ffmpeg_error(result));
		return;
	}

	result = avformat_find_stream_info(format, nullptr);
	if (result < 0) {
		fail("no stream info: " + ffmpeg_error(result));
		return;
	}

	const AVCodec *decoder = nullptr;
	const int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (index < 0 || !decoder) {
		fail("no video stream");
		return;
	}

	stream = format->streams[index];
	codec = avcodec_alloc_context3(decoder);
	if (!codec || avcodec_parameters_to_context(codec, stream->codecpar) < 0) {
		fail("cannot set up the decoder");
		return;
	}

	/* Without this the decoder cannot fill best_effort_timestamp, and positioning falls apart. */
	codec->pkt_timebase = stream->time_base;
	codec->thread_count = kDecoderThreads;

	result = avcodec_open2(codec, decoder, nullptr);
	if (result < 0) {
		fail("cannot open the decoder: " + ffmpeg_error(result));
		return;
	}

	packet = av_packet_alloc();
	if (!packet) {
		fail("out of memory");
		return;
	}

	uint64_t seek_target = start_ts;
	bool need_seek = true;
	bool at_end = false;

	while (!stop_.load(std::memory_order_acquire)) {
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [&] {
				return stop_.load(std::memory_order_acquire) ||
				       seek_requested_.load(std::memory_order_acquire) ||
				       (!at_end && queue_.size() < queue_limit());
			});
			if (stop_.load(std::memory_order_acquire))
				break;
		}

		if (seek_requested_.exchange(false, std::memory_order_acq_rel)) {
			seek_target = wanted_ts_.load(std::memory_order_acquire);
			need_seek = true;
			at_end = false;
		}

		if (need_seek) {
			const int64_t target = av_rescale_q(
				static_cast<int64_t>(seek_target > info_.ts_origin ? seek_target - info_.ts_origin : 0),
				AVRational{1, 1000000000}, stream->time_base);
			av_seek_frame(format, index, target, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(codec);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				queue_.clear();
			}
			need_seek = false;
		}

		/* Read one packet and drain whatever it produced; the mutex is never held across FFmpeg. */
		result = av_read_frame(format, packet);
		if (result < 0) {
			avcodec_send_packet(codec, nullptr); /* flush the tail */
			at_end = true;
		} else if (packet->stream_index != index) {
			av_packet_unref(packet);
			continue;
		} else {
			result = avcodec_send_packet(codec, packet);
			av_packet_unref(packet);
			if (result < 0 && result != AVERROR(EAGAIN))
				continue;
		}

		for (;;) {
			AVFrame *frame = av_frame_alloc();
			if (!frame)
				break;

			result = avcodec_receive_frame(codec, frame);
			if (result < 0) {
				av_frame_free(&frame);
				break;
			}

			const video_format format_id = format_from_av(static_cast<AVPixelFormat>(frame->format));
			if (format_id == VIDEO_FORMAT_NONE || frame->linesize[0] < 0) {
				av_frame_free(&frame);
				fail("unsupported pixel format in the saved clip");
				return;
			}

			DecodedFrame decoded;
			decoded.keepalive = std::shared_ptr<void>(frame, [](void *ptr) {
				AVFrame *owned = static_cast<AVFrame *>(ptr);
				av_frame_free(&owned);
			});
			for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane) {
				decoded.planes[plane] = frame->data[plane];
				decoded.linesize[plane] = static_cast<uint32_t>(std::max(0, frame->linesize[plane]));
			}
			decoded.width = static_cast<uint32_t>(frame->width);
			decoded.height = static_cast<uint32_t>(frame->height);
			decoded.format = format_id;
			decoded.colorspace = colorspace_from_av(frame->colorspace);
			decoded.range = frame->color_range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
			decoded.id = frame->best_effort_timestamp;
			decoded.timestamp = info_.ts_origin +
					    static_cast<uint64_t>(std::max<int64_t>(
						    0, av_rescale_q(frame->best_effort_timestamp, stream->time_base,
								    AVRational{1, 1000000000})));

			{
				std::lock_guard<std::mutex> lock(mutex_);
				queue_.push_back(std::move(decoded));
			}
			ready_.store(true, std::memory_order_release);
		}

		if (at_end) {
			/* Nothing more to read: idle until someone rewinds us. */
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [&] {
				return stop_.load(std::memory_order_acquire) ||
				       seek_requested_.load(std::memory_order_acquire);
			});
		}
	}

	cleanup();
}
