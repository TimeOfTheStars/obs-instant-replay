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

#include "angle-capture.hpp"

#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <mutex>

namespace {

/*
 * The video-io thread walks every subscriber in sequence before handing the frame to the
 * encoders, so anything slower than this shows up as encoder lag for the user.
 */
constexpr uint64_t kCopyBudgetNs = 1500000; /* 1.5 ms */

} // namespace

bool plane_linesizes(video_format format, uint32_t width, uint32_t linesize[MAX_AV_PLANES])
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		linesize[0] = width;
		linesize[1] = width; /* interleaved UV, half the rows, full width */
		return true;
	case VIDEO_FORMAT_I420:
		linesize[0] = width;
		linesize[1] = (width + 1) / 2;
		linesize[2] = (width + 1) / 2;
		return true;
	case VIDEO_FORMAT_I444:
		linesize[0] = width;
		linesize[1] = width;
		linesize[2] = width;
		return true;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
		linesize[0] = width * 4;
		return true;
	default:
		return false;
	}
}

const char *video_format_name(video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
		return "NV12";
	case VIDEO_FORMAT_I420:
		return "I420";
	case VIDEO_FORMAT_I444:
		return "I444";
	case VIDEO_FORMAT_BGRA:
		return "BGRA";
	case VIDEO_FORMAT_BGRX:
		return "BGRX";
	case VIDEO_FORMAT_RGBA:
		return "RGBA";
	default:
		return "unsupported";
	}
}

AngleCapture::AngleCapture(std::shared_mutex &teardown) : teardown_(teardown) {}

AngleCapture::~AngleCapture()
{
	stop();
}

bool AngleCapture::start(std::unique_ptr<CaptureTap> tap, const TapRequest &request, double duration_sec,
			 uint64_t max_bytes)
{
	stop();
	error_.clear();

	if (!tap) {
		error_ = "no frame source";
		return false;
	}

	/*
	 * The tap starts delivering frames before the ring exists, so the sink stays inert until
	 * `running_` flips: on_frame drops everything while the ring has no capacity.
	 */
	TapInfo info;
	if (!tap->start(request, *this, info, error_))
		return false;

	RingConfig config;
	config.width = info.width;
	config.height = info.height;
	config.format = info.format;
	if (!plane_linesizes(config.format, config.width, config.linesize)) {
		tap->stop();
		error_ = std::string("unsupported colour format ") + video_format_name(config.format);
		return false;
	}

	if (info.fps <= 0.0) {
		tap->stop();
		error_ = "no frame rate";
		return false;
	}

	divisor_ = std::max<uint32_t>(1, request.frame_rate_divisor);
	skip_counter_ = 0;
	config.fps = info.divisor_applied ? info.fps : info.fps / divisor_;
	config.duration_sec = duration_sec;
	config.max_bytes = max_bytes;

	bool allocated;
	{
		std::unique_lock<std::shared_mutex> lock(teardown_);
		allocated = ring_.reconfigure(config);
	}
	if (!allocated) {
		tap->stop();
		error_ = "not enough memory for the requested buffer";
		return false;
	}

	frames_written_.store(0, std::memory_order_relaxed);
	frames_dropped_.store(0, std::memory_order_relaxed);
	copy_ns_total_.store(0, std::memory_order_relaxed);
	copy_ns_max_.store(0, std::memory_order_relaxed);
	slow_frames_.store(0, std::memory_order_relaxed);
	pending_gap_.store(false, std::memory_order_release);

	info_ = info;
	duration_sec_ = duration_sec;
	tap_ = std::move(tap);
	running_.store(true, std::memory_order_release);
	return true;
}

void AngleCapture::stop()
{
	if (tap_) {
		/* Silence the tap first: after this no on_frame() can be in flight. */
		tap_->stop();
		tap_.reset();
	}
	running_.store(false, std::memory_order_release);

	std::unique_lock<std::shared_mutex> lock(teardown_);
	ring_.release();
}

void AngleCapture::set_paused(bool paused)
{
	const bool was_paused = paused_.exchange(paused, std::memory_order_acq_rel);
	if (was_paused && !paused)
		pending_gap_.store(true, std::memory_order_release);
}

void AngleCapture::on_frame(video_data *frame)
{
	if (!frame || !frame->data[0] || !running_.load(std::memory_order_acquire) ||
	    paused_.load(std::memory_order_acquire))
		return;

	/* Outputs deliver every frame; decimate here when the tap could not. */
	if (!info_.divisor_applied && divisor_ > 1) {
		if (skip_counter_++ % divisor_ != 0)
			return;
	}

	const bool starts_gap = pending_gap_.exchange(false, std::memory_order_acq_rel);

	const uint64_t started = os_gettime_ns();
	ring_.write(frame->data, frame->linesize, frame->timestamp, starts_gap);
	const uint64_t elapsed = os_gettime_ns() - started;

	frames_written_.fetch_add(1, std::memory_order_relaxed);
	copy_ns_total_.fetch_add(elapsed, std::memory_order_relaxed);

	uint64_t previous_max = copy_ns_max_.load(std::memory_order_relaxed);
	while (elapsed > previous_max &&
	       !copy_ns_max_.compare_exchange_weak(previous_max, elapsed, std::memory_order_relaxed))
		;

	if (elapsed > kCopyBudgetNs)
		slow_frames_.fetch_add(1, std::memory_order_relaxed);
}

bool AngleCapture::live_timestamp(uint64_t &timestamp) const
{
	if (!running())
		return false;
	const uint64_t head = ring_.head();
	return head > 0 && ring_.timestamp_at(head - 1, timestamp);
}

bool AngleCapture::covers(uint64_t timestamp) const
{
	if (!running())
		return false;

	const uint64_t head = ring_.head();
	if (head == 0)
		return false;

	/*
	 * Deliberately not clamped to the gap marker. That marker exists so MARK never stitches a new
	 * clip across a recording pause; frames recorded *before* the pause are still in the ring and
	 * are perfectly good to play back. Clamping here made every camera unavailable for clips
	 * marked before the first replay, because finishing a replay moves the marker past them.
	 */
	const uint64_t first = ring_.oldest();
	uint64_t first_ts = 0;
	uint64_t last_ts = 0;
	if (!ring_.timestamp_at(first, first_ts) || !ring_.timestamp_at(head - 1, last_ts))
		return false;

	return timestamp >= first_ts && timestamp <= last_ts;
}

CaptureStatus AngleCapture::status() const
{
	CaptureStatus status;
	status.running = running();
	status.paused = paused();
	status.error = error_;
	if (!status.running)
		return status;

	const RingConfig &config = ring_.config();
	status.width = config.width;
	status.height = config.height;
	status.fps = config.fps;
	status.buffered_sec = ring_.buffered_seconds();
	status.capacity_sec = config.fps > 0.0 ? static_cast<double>(ring_.capacity()) / config.fps : 0.0;
	status.bytes = ring_.bytes_allocated();
	status.frames_written = frames_written_.load(std::memory_order_relaxed);
	status.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
	status.slow_frames = slow_frames_.load(std::memory_order_relaxed);

	if (status.frames_written > 0) {
		status.copy_ms_avg = static_cast<double>(copy_ns_total_.load(std::memory_order_relaxed)) /
				     status.frames_written / 1000000.0;
	}
	status.copy_ms_max = static_cast<double>(copy_ns_max_.load(std::memory_order_relaxed)) / 1000000.0;
	return status;
}
