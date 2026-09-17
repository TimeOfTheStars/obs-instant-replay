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

#pragma once

#include "capture-tap.hpp"
#include "frame-ring.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>

struct CaptureStatus {
	bool running = false;
	bool paused = false;
	uint32_t width = 0;
	uint32_t height = 0;
	double fps = 0.0;
	double buffered_sec = 0.0;
	double capacity_sec = 0.0;
	uint64_t bytes = 0;
	uint64_t frames_written = 0;
	uint64_t frames_dropped = 0;
	double copy_ms_avg = 0.0;
	double copy_ms_max = 0.0;
	uint64_t slow_frames = 0; /* callbacks that took longer than the budget */
	std::string error;
};

/*
 * One angle: a ring fed by a tap. The frame callback runs on a libobs video-io thread and must
 * never allocate, lock or log — a slow callback shows up as encoder lag on air.
 */
class AngleCapture : public FrameSink {
public:
	explicit AngleCapture(std::shared_mutex &teardown);
	~AngleCapture() override;

	AngleCapture(const AngleCapture &) = delete;
	AngleCapture &operator=(const AngleCapture &) = delete;

	bool start(std::unique_ptr<CaptureTap> tap, const TapRequest &request, double duration_sec, uint64_t max_bytes);
	void stop();
	bool running() const { return running_.load(std::memory_order_acquire); }

	void set_paused(bool paused);
	bool paused() const { return paused_.load(std::memory_order_acquire); }

	CaptureStatus status() const;
	FrameRing &ring() { return ring_; }
	const FrameRing &ring() const { return ring_; }
	const std::string &error() const { return error_; }
	double requested_seconds() const { return duration_sec_; }

	/* Newest frame timestamp, false if the ring is empty. */
	bool live_timestamp(uint64_t &timestamp) const;

	/* True if the ring still holds footage at `timestamp` (past the last gap). */
	bool covers(uint64_t timestamp) const;

	void on_frame(video_data *frame) override;

private:
	std::shared_mutex &teardown_;
	FrameRing ring_;
	std::unique_ptr<CaptureTap> tap_;
	TapInfo info_;
	std::string error_;
	double duration_sec_ = 0.0;
	uint32_t divisor_ = 1;
	uint32_t skip_counter_ = 0;

	std::atomic<bool> running_{false};
	std::atomic<bool> paused_{false};
	std::atomic<bool> pending_gap_{false};

	std::atomic<uint64_t> frames_written_{0};
	std::atomic<uint64_t> frames_dropped_{0};
	std::atomic<uint64_t> copy_ns_total_{0};
	std::atomic<uint64_t> copy_ns_max_{0};
	std::atomic<uint64_t> slow_frames_{0};
};

/* Tightly packed destination strides per plane; false for unsupported formats. */
bool plane_linesizes(video_format format, uint32_t width, uint32_t linesize[MAX_AV_PLANES]);
const char *video_format_name(video_format format);
