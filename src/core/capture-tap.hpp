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

#include <obs.h>

#include <cstdint>
#include <string>

/* Receives raw frames on whatever thread the tap runs on. Must not allocate, lock or log. */
class FrameSink {
public:
	virtual ~FrameSink() = default;
	virtual void on_frame(video_data *frame) = 0;
};

struct TapRequest {
	uint32_t output_width = 0; /* 0 = same as the programme mix */
	uint32_t output_height = 0;
	uint32_t frame_rate_divisor = 1;
	obs_weak_source_t *scene = nullptr; /* view taps only */
};

struct TapInfo {
	uint32_t width = 0;
	uint32_t height = 0;
	video_format format = VIDEO_FORMAT_NONE;
	double fps = 0.0;             /* frame rate the sink will actually see */
	bool divisor_applied = false; /* true if the tap already dropped frames for the divisor */
};

/*
 * A source of raw frames: the programme mix, or a private view rendering one scene. The tap owns
 * the libobs plumbing; the ring and bookkeeping live in AngleCapture.
 */
class CaptureTap {
public:
	virtual ~CaptureTap() = default;

	virtual bool start(const TapRequest &request, FrameSink &sink, TapInfo &info, std::string &error) = 0;

	/* Returns only once no further on_frame() calls can happen. */
	virtual void stop() = 0;
};
